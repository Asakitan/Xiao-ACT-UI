// engine/il2cpp_adapter.cpp — IL2CPP runtime class enumeration and lookup.

#include "adapter_common.h"

#include "sao/core/il2cpp_probe.h"
#include "sao/core/memory.h"
#include "sao/core/time.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sao::mem_probe::engine {

namespace {

constexpr uint64_t kUserModeLowerBound = 0x10000ull;
constexpr uint64_t kUserModeUpperBound = 0x00007FFFFFFFFFFFull;
constexpr uint32_t kMaxImageTypes = 4096;
constexpr uint32_t kMaxDomainAssemblies = 512;
constexpr size_t kMaxClasses = 100000;
constexpr uint64_t kWalkByteBudget = 32ull * 1024ull * 1024ull;
constexpr uint32_t kWalkReadBudget = 20000u;
constexpr uint64_t kWalkTimeBudgetMs = 1000ull;
// Runtime walks charge every remote read against 32 MiB, 20000 reads, and 1000 ms.

struct RuntimeLayout {
    uint32_t domain_assemblies_offset;
    uint32_t domain_assembly_count_offset;
    uint32_t assembly_image_offset;
    uint32_t image_assembly_offset;
    uint32_t image_name_offset;
    uint32_t image_name_no_ext_offset;
    uint32_t image_type_count_offset;
    uint32_t image_type_array_offset;
    uint32_t klass_image_offset;
    uint32_t klass_name_offset;
    uint32_t klass_namespace_offset;
};

const RuntimeLayout* runtime_layout_for_version(uint32_t version) {
    static constexpr RuntimeLayout kMetadata24{0x18u,
                                               0x20u,
                                               0x00u,
                                               0x10u,
                                               0x00u,
                                               0x08u,
                                               0x1Cu,
                                               0x28u,
                                               0x00u,
                                               SAO_IL2CPP_KLASS_NAME_OFFSET,
                                               SAO_IL2CPP_KLASS_NAMESPACE_OFFSET};
    static constexpr RuntimeLayout kMetadata27 = kMetadata24;
    static constexpr RuntimeLayout kMetadata29 = kMetadata24;
    static constexpr RuntimeLayout kMetadata31 = kMetadata24;
    switch (version) {
    case SAO_IL2CPP_METADATA_VERSION_24:
        return &kMetadata24;
    case SAO_IL2CPP_METADATA_VERSION_27:
        return &kMetadata27;
    case SAO_IL2CPP_METADATA_VERSION_29:
        return &kMetadata29;
    case SAO_IL2CPP_METADATA_VERSION_31:
        return &kMetadata31;
    default:
        return nullptr;
    }
}

using WalkBudget = sao_il2cpp_discovery_budget_t;

bool consume(WalkBudget& budget, size_t bytes) {
    if (sao_core_time_now_ms() >= budget.deadline_ms || budget.reads_remaining == 0 ||
        bytes > budget.bytes_remaining)
        return false;
    budget.bytes_remaining -= bytes;
    --budget.reads_remaining;
    return true;
}

bool exhausted(const WalkBudget& budget) {
    return sao_core_time_now_ms() >= budget.deadline_ms || budget.reads_remaining == 0 ||
           budget.bytes_remaining == 0;
}

bool plausible_address(uint64_t value) {
    return value >= kUserModeLowerBound && value <= kUserModeUpperBound;
}

bool plausible_pointer(uint64_t value) {
    return plausible_address(value) && (value & 7ull) == 0;
}

bool add_offset(uint64_t base, uint32_t offset, uint64_t* out) {
    if (out == nullptr || base > UINT64_MAX - static_cast<uint64_t>(offset))
        return false;
    *out = base + static_cast<uint64_t>(offset);
    return true;
}

std::string basename_lower(std::string value) {
    const size_t slash = value.find_last_of("\\/");
    if (slash != std::string::npos)
        value.erase(0, slash + 1);
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

bool read_exact(sao_core_process_handle_t process, uint64_t address, void* buffer, size_t size,
                WalkBudget& budget) {
    if (!consume(budget, size))
        return false;
    size_t got = 0;
    return sao_core_mem_read(process, address, buffer, size, &got) == SAO_STATUS_OK && got == size;
}

bool read_u64(sao_core_process_handle_t process, uint64_t address, uint64_t* out,
              WalkBudget& budget) {
    return out != nullptr && read_exact(process, address, out, sizeof(*out), budget);
}

bool read_u32(sao_core_process_handle_t process, uint64_t address, uint32_t* out,
              WalkBudget& budget) {
    return out != nullptr && read_exact(process, address, out, sizeof(*out), budget);
}

bool read_ascii(sao_core_process_handle_t process, uint64_t address, std::string& out,
                WalkBudget& budget) {
    out.clear();
    if (!plausible_address(address) || !consume(budget, 256u))
        return false;
    char buffer[257] = {};
    size_t got = 0;
    const sao_status_t status =
        sao_core_mem_read(process, address, buffer, sizeof(buffer) - 1, &got);
    if (status != SAO_STATUS_OK && got == 0)
        return false;
    size_t length = 0;
    while (length < sizeof(buffer) - 1 && buffer[length] != '\0')
        ++length;
    if (length == 0 || length >= sizeof(buffer) - 1 || length >= got)
        return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(buffer[i]);
        if (byte < 0x20 || byte > 0x7E)
            return false;
    }
    out.assign(buffer, length);
    return true;
}

struct ClassRecord {
    uint64_t address = 0;
    std::string qualified_name;
    std::string simple_name;
};

class Il2CppAdapter : public AdapterBase {
  public:
    Il2CppAdapter() {
        kind = SAO_MEMPROBE_ENGINE_IL2CPP;
    }

    uint64_t game_assembly_base() const {
        for (const auto& m : modules) {
            const std::string name = basename_lower(m.name);
            if (name == "gameassembly.dll" || name == "il2cpp.dll")
                return m.base;
        }
        return 0;
    }

    sao_status_t list_classes(uint32_t* offsets, size_t max_off, size_t* off_ct, char* names,
                              size_t names_cap, size_t* names_used) override {
        if (offsets == nullptr || off_ct == nullptr || names == nullptr || names_used == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *off_ct = 0;
        *names_used = 0;
        if (game_assembly_base() == 0)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (!walk_runtime())
            return last_status_;
        for (const ClassRecord& record : classes_) {
            const size_t required = record.qualified_name.size() + 1;
            if (*off_ct >= max_off || required > names_cap - *names_used)
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            offsets[*off_ct] = static_cast<uint32_t>(*names_used);
            std::memcpy(names + *names_used, record.qualified_name.data(),
                        record.qualified_name.size());
            names[*names_used + record.qualified_name.size()] = '\0';
            *names_used += required;
            ++*off_ct;
        }
        return *off_ct == 0 ? SAO_STATUS_ERR_NOT_FOUND : SAO_STATUS_OK;
    }

    sao_status_t find_class(const char* name, uint64_t* out) override {
        if (out != nullptr)
            *out = 0;
        if (name == nullptr || out == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (game_assembly_base() == 0)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (!walk_runtime())
            return last_status_;
        for (const ClassRecord& record : classes_) {
            if (record.qualified_name == name || record.simple_name == name) {
                *out = record.address;
                return SAO_STATUS_OK;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    }

  private:
    bool read_class(uint64_t klass, uint64_t expected_image, const RuntimeLayout& layout,
                    WalkBudget& budget) {
        if (!plausible_pointer(klass))
            return false;
        uint64_t address = 0;
        uint64_t image = 0;
        if (!add_offset(klass, layout.klass_image_offset, &address) ||
            !read_u64(process, address, &image, budget) || image != expected_image)
            return false;
        uint64_t name_ptr = 0;
        if (!add_offset(klass, layout.klass_name_offset, &address) ||
            !read_u64(process, address, &name_ptr, budget))
            return false;
        std::string name;
        if (!read_ascii(process, name_ptr, name, budget))
            return false;
        uint64_t namespace_ptr = 0;
        if (!add_offset(klass, layout.klass_namespace_offset, &address) ||
            !read_u64(process, address, &namespace_ptr, budget))
            return false;
        std::string namespace_name;
        if (namespace_ptr != 0 && !read_ascii(process, namespace_ptr, namespace_name, budget))
            return false;
        const std::string qualified = namespace_name.empty() ? name : namespace_name + "." + name;
        for (const ClassRecord& record : classes_) {
            if (record.address == klass || record.qualified_name == qualified)
                return true;
        }
        if (classes_.size() < kMaxClasses)
            classes_.push_back(ClassRecord{klass, qualified, name});
        return true;
    }

    size_t walk_image(uint64_t image, uint64_t assembly, const RuntimeLayout& layout,
                      WalkBudget& budget) {
        if (!plausible_pointer(image) || !plausible_pointer(assembly))
            return 0;
        uint64_t address = 0;
        uint64_t image_assembly = 0;
        if (!add_offset(image, layout.image_assembly_offset, &address) ||
            !read_u64(process, address, &image_assembly, budget) || image_assembly != assembly)
            return 0;
        uint64_t image_name = 0;
        uint64_t image_name_no_ext = 0;
        if (!add_offset(image, layout.image_name_offset, &address) ||
            !read_u64(process, address, &image_name, budget) ||
            !add_offset(image, layout.image_name_no_ext_offset, &address) ||
            !read_u64(process, address, &image_name_no_ext, budget))
            return 0;
        std::string image_name_text;
        std::string image_name_no_ext_text;
        if (image_name != 0 && !read_ascii(process, image_name, image_name_text, budget))
            return 0;
        if (image_name_no_ext != 0 &&
            !read_ascii(process, image_name_no_ext, image_name_no_ext_text, budget))
            return 0;
        if (image_name_text.empty() && image_name_no_ext_text.empty())
            return 0;
        uint32_t type_count = 0;
        uint64_t type_start = 0;
        if (!add_offset(image, layout.image_type_count_offset, &address) ||
            !read_u32(process, address, &type_count, budget) ||
            !add_offset(image, layout.image_type_array_offset, &address) ||
            !read_u64(process, address, &type_start, budget) || type_count == 0 ||
            type_count > kMaxImageTypes || !plausible_pointer(type_start))
            return 0;
        if (static_cast<size_t>(type_count) > std::numeric_limits<size_t>::max() / sizeof(uint64_t))
            return 0;
        const size_t bytes = static_cast<size_t>(type_count) * sizeof(uint64_t);
        std::vector<uint64_t> klasses(type_count);
        if (!read_exact(process, type_start, klasses.data(), bytes, budget))
            return 0;
        size_t added = 0;
        for (uint64_t klass : klasses) {
            const size_t before = classes_.size();
            if (read_class(klass, image, layout, budget) && classes_.size() > before)
                ++added;
        }
        return added;
    }

    size_t walk_assembly_array(uint64_t domain, const RuntimeLayout& layout, WalkBudget& budget) {
        uint64_t address = 0;
        uint64_t assembly_array = 0;
        uint32_t assembly_count = 0;
        if (!add_offset(domain, layout.domain_assemblies_offset, &address) ||
            !read_u64(process, address, &assembly_array, budget) ||
            !add_offset(domain, layout.domain_assembly_count_offset, &address) ||
            !read_u32(process, address, &assembly_count, budget) || assembly_count == 0 ||
            assembly_count > kMaxDomainAssemblies || !plausible_pointer(assembly_array))
            return 0;
        const size_t bytes = static_cast<size_t>(assembly_count) * sizeof(uint64_t);
        std::vector<uint64_t> assemblies(assembly_count);
        if (!read_exact(process, assembly_array, assemblies.data(), bytes, budget))
            return 0;
        size_t added = 0;
        for (uint64_t assembly : assemblies) {
            if (!plausible_pointer(assembly))
                continue;
            if (!add_offset(assembly, layout.assembly_image_offset, &address))
                continue;
            uint64_t image = 0;
            if (!read_u64(process, address, &image, budget))
                continue;
            added += walk_image(image, assembly, layout, budget);
        }
        return added;
    }

    bool walk_runtime() {
        if (walked_)
            return !classes_.empty();
        classes_.clear();
        last_status_ = SAO_STATUS_ERR_CAPABILITY_MISSING;
        const uint64_t base = game_assembly_base();
        if (base == 0)
            return false;
        SaoIl2CppProbeV2 probe{};
        WalkBudget budget{};
        sao_core_il2cpp_discovery_budget_init(&budget, kWalkByteBudget, kWalkReadBudget,
                                              static_cast<uint32_t>(kWalkTimeBudgetMs));
        const sao_status_t probe_status =
            sao_core_il2cpp_probe_metadata_v2_with_budget(process, base, &budget, &probe);
        if (probe_status != SAO_STATUS_OK) {
            last_status_ = probe_status;
            return false;
        }
        const RuntimeLayout* layout = probe.metadata_version_valid == 1u
                                          ? runtime_layout_for_version(probe.metadata_version)
                                          : nullptr;
        if (layout == nullptr || !plausible_pointer(probe.domain_ptr))
            return false;
        walk_assembly_array(probe.domain_ptr, *layout, budget);
        if (exhausted(budget)) {
            classes_.clear();
            walked_ = false;
            last_status_ = SAO_STATUS_ERR_TIMEOUT;
            return false;
        }
        if (classes_.empty()) {
            walked_ = false;
            last_status_ = SAO_STATUS_ERR_CAPABILITY_MISSING;
            return false;
        }
        std::sort(classes_.begin(), classes_.end(),
                  [](const ClassRecord& left, const ClassRecord& right) {
                      return left.qualified_name < right.qualified_name;
                  });
        walked_ = true;
        last_status_ = SAO_STATUS_OK;
        return true;
    }

    bool walked_ = false;
    sao_status_t last_status_ = SAO_STATUS_ERR_CAPABILITY_MISSING;
    std::vector<ClassRecord> classes_;
};

} // namespace

AdapterBase* open_il2cpp(sao_core_process_handle_t p) {
    auto* adapter = new Il2CppAdapter();
    adapter->process = p;
    populate_modules(*adapter);
    return adapter;
}

} // namespace sao::mem_probe::engine
