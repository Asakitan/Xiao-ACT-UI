// Generic IL2CPP runtime probe.  Delivers the primitives the
// Python `il2cpp_adapter.py` uses (PE export walk to locate
// `il2cpp_domain_get`, name pointer read at klass+0x10, safe pointer
// scan) without embedding any title-specific klass name.

#include "sao/core/il2cpp_probe.h"

#include "sao/core/error.h"
#include "sao/core/memory.h"
#include "sao/core/time.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

extern HANDLE sao_core_process_native_handle(sao_core_process_handle_t process) noexcept;

namespace {

constexpr uint64_t kProbeByteBudget = 64ull * 1024ull * 1024ull;
constexpr uint32_t kProbeReadBudget = 8192u;
constexpr uint32_t kProbeCandidateBudget = 4096u;
constexpr uint64_t kProbeTimeBudgetMs = 750ull;

struct ProbeBudget {
    sao_il2cpp_discovery_budget_t local{};
    sao_il2cpp_discovery_budget_t* shared = nullptr;
    uint32_t candidates_remaining = kProbeCandidateBudget;
    bool exhausted_local = false;

    ProbeBudget()
        : local{kProbeByteBudget, kProbeReadBudget, 0u,
                sao_core_time_now_ms() + kProbeTimeBudgetMs},
          shared(&local) {}

    explicit ProbeBudget(sao_il2cpp_discovery_budget_t* budget)
        : local{kProbeByteBudget, kProbeReadBudget, 0u,
                sao_core_time_now_ms() + kProbeTimeBudgetMs},
          shared(budget != nullptr ? budget : &local) {}

    bool reserve_read(size_t bytes) {
        if (sao_core_time_now_ms() >= shared->deadline_ms || shared->reads_remaining == 0 ||
            bytes > shared->bytes_remaining) {
            exhausted_local = true;
            return false;
        }
        shared->bytes_remaining -= bytes;
        --shared->reads_remaining;
        return true;
    }

    bool reserve_candidate() {
        if (sao_core_time_now_ms() >= shared->deadline_ms || candidates_remaining == 0) {
            exhausted_local = true;
            return false;
        }
        --candidates_remaining;
        return true;
    }

    bool active() {
        if (sao_core_time_now_ms() >= shared->deadline_ms)
            exhausted_local = true;
        return !exhausted_local && shared->reads_remaining != 0 && shared->bytes_remaining != 0;
    }

    bool exhausted() {
        return !active();
    }
};

sao_status_t fail_probe(sao_status_t status, const char* message, const char* file, uint32_t line) {
    sao_core_error_set(status, "core.il2cpp_probe", message, file, line);
    return status;
}

// Convenience: try to read `count` bytes from the target; returns true
// only when the whole read succeeded.
bool read_exact(sao_core_process_handle_t handle, uint64_t address, void* buffer, size_t count,
                ProbeBudget& budget) {
    if (!budget.reserve_read(count))
        return false;
    size_t got = 0;
    if (sao_core_mem_read(handle, address, buffer, count, &got) != SAO_STATUS_OK) {
        return false;
    }
    return got == count;
}

// Read the u64 at `address`.  Returns 0 on failure.  A raw zero return is
// still meaningful downstream (null pointers exist), so callers check the
// second `ok` output.
uint64_t read_u64(sao_core_process_handle_t handle, uint64_t address, bool* ok,
                  ProbeBudget& budget) {
    uint64_t value = 0;
    *ok = read_exact(handle, address, &value, sizeof(value), budget);
    return value;
}

// Locate a named export inside a Windows PE loaded at `module_base`.
// Returns 0 on any failure.  This is a 1:1 port of the Python
// `_resolve_export` in `il2cpp_adapter.py` and handles PE32+ only (x64).
uint64_t module_size_for_base(sao_core_process_handle_t handle, uint64_t module_base) {
    SaoModuleEntry entries[512];
    char names[65536];
    size_t count = 0;
    size_t used = 0;
    if (sao_core_process_enum_modules(handle, entries, 512, &count, names, sizeof(names), &used) !=
        SAO_STATUS_OK)
        return 0;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].base_address == module_base && entries[i].module_size != 0)
            return entries[i].module_size;
    }
    return 0;
}

bool rva_span_valid(uint32_t rva, size_t length, uint64_t module_size) {
    return static_cast<uint64_t>(rva) <= module_size &&
           length <= module_size - static_cast<uint64_t>(rva);
}

bool add_rva(uint64_t module_base, uint32_t rva, uint64_t module_size, uint64_t* out_address) {
    if (out_address == nullptr || !rva_span_valid(rva, 1, module_size) ||
        module_base > UINT64_MAX - static_cast<uint64_t>(rva))
        return false;
    *out_address = module_base + static_cast<uint64_t>(rva);
    return true;
}

uint64_t resolve_export(sao_core_process_handle_t handle, uint64_t module_base,
                        const char* export_name, ProbeBudget& budget) {
    if (handle == nullptr || module_base == 0 || export_name == nullptr)
        return 0;
    const uint64_t module_size = module_size_for_base(handle, module_base);
    if (module_size < 0x40)
        return 0;
    const size_t name_len = std::strlen(export_name);
    if (name_len == 0 || name_len > 128)
        return 0;

    std::array<uint8_t, 0x40> dos{};
    if (!read_exact(handle, module_base, dos.data(), dos.size(), budget) || dos[0] != 'M' ||
        dos[1] != 'Z')
        return 0;
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, dos.data() + 0x3C, sizeof(e_lfanew));
    if (!rva_span_valid(e_lfanew, 0x90, module_size))
        return 0;
    uint64_t pe_address = 0;
    if (!add_rva(module_base, e_lfanew, module_size, &pe_address))
        return 0;
    std::array<uint8_t, 0x90> pe_hdr{};
    if (!read_exact(handle, pe_address, pe_hdr.data(), pe_hdr.size(), budget) || pe_hdr[0] != 'P' ||
        pe_hdr[1] != 'E' || pe_hdr[2] != 0 || pe_hdr[3] != 0)
        return 0;

    uint16_t size_of_optional_header = 0;
    std::memcpy(&size_of_optional_header, pe_hdr.data() + 0x14, sizeof(size_of_optional_header));
    uint16_t magic = 0;
    std::memcpy(&magic, pe_hdr.data() + 0x18, sizeof(magic));
    if (magic != 0x20B)
        return 0;
    constexpr size_t kExportDirectoryOffset = 0x70;
    constexpr size_t kExportDirectoryBytes = 8;
    if (size_of_optional_header < kExportDirectoryOffset + kExportDirectoryBytes ||
        !rva_span_valid(e_lfanew, 0x18 + static_cast<size_t>(size_of_optional_header),
                        module_size) ||
        0x18 + kExportDirectoryOffset + kExportDirectoryBytes > pe_hdr.size())
        return 0;
    uint32_t number_of_rva_and_sizes = 0;
    std::memcpy(&number_of_rva_and_sizes, pe_hdr.data() + 0x18 + 0x6C,
                sizeof(number_of_rva_and_sizes));
    if (number_of_rva_and_sizes == 0)
        return 0;

    uint32_t export_rva = 0;
    uint32_t export_size = 0;
    std::memcpy(&export_rva, pe_hdr.data() + 0x18 + kExportDirectoryOffset, 4);
    std::memcpy(&export_size, pe_hdr.data() + 0x18 + kExportDirectoryOffset + 4, 4);
    constexpr uint32_t kMaxExportDirectoryBytes = 16u * 1024u * 1024u;
    if (export_rva == 0 || export_size < 40 || export_size > kMaxExportDirectoryBytes ||
        !rva_span_valid(export_rva, export_size, module_size))
        return 0;

    uint64_t export_address = 0;
    if (!add_rva(module_base, export_rva, module_size, &export_address))
        return 0;
    std::vector<uint8_t> export_buf(export_size);
    size_t got = 0;
    if (!budget.reserve_read(export_buf.size()) ||
        sao_core_mem_read(handle, export_address, export_buf.data(), export_buf.size(), &got) !=
            SAO_STATUS_OK ||
        got < 40)
        return 0;
    uint32_t nfuncs = 0, nnames = 0, addr_rva = 0, name_rva = 0, ord_rva = 0;
    std::memcpy(&nfuncs, export_buf.data() + 0x14, 4);
    std::memcpy(&nnames, export_buf.data() + 0x18, 4);
    std::memcpy(&addr_rva, export_buf.data() + 0x1C, 4);
    std::memcpy(&name_rva, export_buf.data() + 0x20, 4);
    std::memcpy(&ord_rva, export_buf.data() + 0x24, 4);
    if (nfuncs == 0 || nfuncs > 50000 || nnames == 0 || nnames > 50000)
        return 0;
    const size_t function_bytes = static_cast<size_t>(nfuncs) * sizeof(uint32_t);
    const size_t name_bytes = static_cast<size_t>(nnames) * sizeof(uint32_t);
    const size_t ordinal_bytes = static_cast<size_t>(nnames) * sizeof(uint16_t);
    if (!rva_span_valid(addr_rva, function_bytes, module_size) ||
        !rva_span_valid(name_rva, name_bytes, module_size) ||
        !rva_span_valid(ord_rva, ordinal_bytes, module_size))
        return 0;

    uint64_t function_address = 0, name_address = 0, ordinal_address = 0;
    if (!add_rva(module_base, addr_rva, module_size, &function_address) ||
        !add_rva(module_base, name_rva, module_size, &name_address) ||
        !add_rva(module_base, ord_rva, module_size, &ordinal_address))
        return 0;
    std::vector<uint32_t> name_rvas(nnames);
    std::vector<uint16_t> ordinals(nnames);
    if (!read_exact(handle, name_address, name_rvas.data(), name_bytes, budget) ||
        !read_exact(handle, ordinal_address, ordinals.data(), ordinal_bytes, budget))
        return 0;
    std::vector<uint32_t> functions(nfuncs);
    if (!read_exact(handle, function_address, functions.data(), function_bytes, budget))
        return 0;
    std::vector<uint8_t> raw(name_len + 1);
    for (uint32_t index = 0; index < nnames; ++index) {
        if (!budget.reserve_candidate())
            return 0;
        if (!rva_span_valid(name_rvas[index], raw.size(), module_size))
            continue;
        uint64_t string_address = 0;
        if (!add_rva(module_base, name_rvas[index], module_size, &string_address) ||
            !read_exact(handle, string_address, raw.data(), raw.size(), budget))
            continue;
        if (raw[name_len] != 0 || std::memcmp(raw.data(), export_name, name_len) != 0)
            continue;
        const uint16_t ordinal = ordinals[index];
        if (ordinal >= nfuncs || functions[ordinal] == 0 ||
            !rva_span_valid(functions[ordinal], 1, module_size))
            return 0;
        uint64_t function_va = 0;
        if (!add_rva(module_base, functions[ordinal], module_size, &function_va))
            return 0;
        return function_va;
    }
    return 0;
}
// Decode either `mov rax, [rip+disp32]` (48 8B 05 xx xx xx xx) or
// `lea rax, [rip+disp32]` (48 8D 05 xx xx xx xx) starting at `code_addr`
// MOV returns the address of a global slot to load; LEA returns the address value directly.
struct DomainGetDecode {
    uint64_t target = 0;
    bool load_from_memory = false;
};

DomainGetDecode decode_domain_get_prologue(sao_core_process_handle_t handle, uint64_t code_addr,
                                           ProbeBudget& budget) {
    std::array<uint8_t, 16> code{};
    if (!read_exact(handle, code_addr, code.data(), code.size(), budget))
        return {};
    if (code[0] != 0x48 || code[2] != 0x05)
        return {};
    const bool is_mov = code[1] == 0x8B;
    const bool is_lea = code[1] == 0x8D;
    if (!is_mov && !is_lea)
        return {};
    int32_t disp = 0;
    std::memcpy(&disp, code.data() + 3, sizeof(disp));
    if (code_addr > UINT64_MAX - 7)
        return {};
    const uint64_t rip = code_addr + 7;
    uint64_t target = 0;
    if (disp >= 0) {
        const uint64_t amount = static_cast<uint64_t>(disp);
        if (rip > UINT64_MAX - amount)
            return {};
        target = rip + amount;
    } else {
        const uint64_t amount = static_cast<uint64_t>(-(static_cast<int64_t>(disp) + 1)) + 1;
        if (rip < amount)
            return {};
        target = rip - amount;
    }
    return DomainGetDecode{target, is_mov};
}

// Heuristic upper bound for user-mode pointers on x64.  Anything above
// this is either kernel or garbage.
constexpr uint64_t kUserModeUpperBound = 0x00007FFFFFFFFFFFull;
constexpr uint64_t kUserModeLowerBound = 0x0000000000010000ull;
constexpr uint32_t kMetadataMagic = 0xFAB11BAFu;
constexpr size_t kMetadataHeaderPairs = 8u;

bool v2_metadata_version(uint32_t version) {
    return version == SAO_IL2CPP_METADATA_VERSION_24 || version == SAO_IL2CPP_METADATA_VERSION_27 ||
           version == SAO_IL2CPP_METADATA_VERSION_29 || version == SAO_IL2CPP_METADATA_VERSION_31;
}

bool metadata_header_valid(const uint8_t* bytes, size_t available, uint64_t region_remaining,
                           uint32_t* out_version) {
    constexpr size_t kHeaderBytes = 8u + kMetadataHeaderPairs * 8u;
    if (bytes == nullptr || out_version == nullptr || available < kHeaderBytes)
        return false;
    uint32_t magic = 0;
    uint32_t version = 0;
    std::memcpy(&magic, bytes, sizeof(magic));
    std::memcpy(&version, bytes + sizeof(magic), sizeof(version));
    if (magic != kMetadataMagic || !v2_metadata_version(version))
        return false;
    size_t non_empty_pairs = 0;
    for (size_t index = 0; index < kMetadataHeaderPairs; ++index) {
        uint32_t offset = 0;
        uint32_t count = 0;
        const size_t pair_offset = 8u + index * 8u;
        std::memcpy(&offset, bytes + pair_offset, sizeof(offset));
        std::memcpy(&count, bytes + pair_offset + sizeof(offset), sizeof(count));
        if (offset == 0 && count == 0)
            continue;
        if (offset < kHeaderBytes || (offset & 3u) != 0 ||
            static_cast<uint64_t>(offset) > region_remaining ||
            static_cast<uint64_t>(count) > region_remaining - static_cast<uint64_t>(offset))
            return false;
        ++non_empty_pairs;
    }
    if (non_empty_pairs < 3u)
        return false;
    *out_version = version;
    return true;
}

bool mapped_global_metadata_file(sao_core_process_handle_t handle, uint64_t address);

struct AddressRange {
    uint64_t begin = 0;
    uint64_t end = 0;
};

struct RuntimeRegistrationEvidence {
    std::vector<AddressRange> ranges;
};

bool add_registration_range(RuntimeRegistrationEvidence& evidence, uint64_t begin, uint64_t size) {
    if (begin == 0 || size == 0 || begin > UINT64_MAX - size)
        return false;
    evidence.ranges.push_back(AddressRange{begin, begin + size});
    return true;
}

bool header_in_registration_ranges(const RuntimeRegistrationEvidence& evidence,
                                   uint64_t header_address, size_t header_size) {
    if (header_address == 0 || header_address > UINT64_MAX - header_size)
        return false;
    const uint64_t header_end = header_address + header_size;
    for (const AddressRange& range : evidence.ranges) {
        if (header_address >= range.begin && header_end <= range.end)
            return true;
    }
    return false;
}

bool find_metadata_header(sao_core_process_handle_t handle, uint64_t module_base,
                          uint32_t* out_version, uint64_t* out_header_rva,
                          uint64_t* out_header_address, bool allow_runtime_binding,
                          const RuntimeRegistrationEvidence* registration, ProbeBudget& budget) {
    if (out_version == nullptr || out_header_rva == nullptr || out_header_address == nullptr)
        return false;
    *out_version = SAO_IL2CPP_METADATA_VERSION_NOT_READY;
    *out_header_rva = 0;
    *out_header_address = 0;
    SaoMemRegion regions[8192]{};
    size_t region_count = 0;
    if (sao_core_mem_enum_regions(handle, regions, 8192, &region_count) != SAO_STATUS_OK)
        return false;
    constexpr size_t kChunkBytes = 1u << 20;
    constexpr size_t kHeaderBytes = 8u + kMetadataHeaderPairs * 8u;
    const uint64_t module_size = module_size_for_base(handle, module_base);
    std::vector<uint8_t> chunk(kChunkBytes);

    for (size_t region_index = 0; region_index < region_count; ++region_index) {
        const SaoMemRegion& region = regions[region_index];
        if ((region.state & MEM_COMMIT) == 0 || (region.protect & PAGE_GUARD) != 0 ||
            region.protect == 0 || (region.protect & PAGE_NOACCESS) != 0 || region.region_size < 8u)
            continue;
        if (!allow_runtime_binding && (region.type != MEM_MAPPED ||
                                       !mapped_global_metadata_file(handle, region.base_address)))
            continue;
        uint64_t cursor = region.base_address;
        uint64_t remaining = region.region_size;
        while (remaining != 0 && budget.active()) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkBytes));
            if (!budget.reserve_read(want))
                break;
            size_t got = 0;
            const sao_status_t status = sao_core_mem_read(handle, cursor, chunk.data(), want, &got);
            if (got >= 4u) {
                for (size_t offset = 0; offset + 4u <= got; offset += 4u) {
                    uint32_t magic = 0;
                    std::memcpy(&magic, chunk.data() + offset, sizeof(magic));
                    if (magic != kMetadataMagic)
                        continue;
                    if (cursor > UINT64_MAX - static_cast<uint64_t>(offset))
                        continue;
                    const uint64_t header_address = cursor + static_cast<uint64_t>(offset);
                    if (!budget.reserve_candidate())
                        break;
                    const uint64_t consumed = header_address - region.base_address;
                    if (consumed > region.region_size)
                        continue;
                    const uint64_t region_remaining = region.region_size - consumed;
                    if (region_remaining < kHeaderBytes)
                        continue;
                    std::array<uint8_t, kHeaderBytes> header{};
                    size_t header_got = 0;
                    if (!budget.reserve_read(header.size()) ||
                        sao_core_mem_read(handle, header_address, header.data(), header.size(),
                                          &header_got) != SAO_STATUS_OK ||
                        !metadata_header_valid(header.data(), header_got, region_remaining,
                                               out_version))
                        continue;
                    if (allow_runtime_binding &&
                        (registration == nullptr ||
                         !header_in_registration_ranges(*registration, header_address,
                                                        header.size())))
                        continue;
                    if (module_size != 0 && header_address >= module_base &&
                        header_address - module_base < module_size)
                        *out_header_rva = header_address - module_base;
                    *out_header_address = header_address;
                    return true;
                }
            }
            const size_t advanced = got == 0 ? want : got;
            if (cursor > UINT64_MAX - static_cast<uint64_t>(advanced))
                break;
            cursor += static_cast<uint64_t>(advanced);
            remaining -= std::min<uint64_t>(remaining, advanced);
            if (status != SAO_STATUS_OK && got < want)
                continue;
        }
    }
    return false;
}

bool mapped_global_metadata_file(sao_core_process_handle_t handle, uint64_t address) {
    const HANDLE native_handle = sao_core_process_native_handle(handle);
    if (native_handle == nullptr)
        return false;
    using GetMappedFileNameWFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
    const HMODULE psapi = LoadLibraryW(L"psapi.dll");
    if (psapi == nullptr)
        return false;
    const auto get_name =
        reinterpret_cast<GetMappedFileNameWFn>(GetProcAddress(psapi, "GetMappedFileNameW"));
    wchar_t path[1024] = {};
    const DWORD length = get_name == nullptr
                             ? 0
                             : get_name(native_handle, reinterpret_cast<LPVOID>(address), path,
                                        static_cast<DWORD>(std::size(path)));
    FreeLibrary(psapi);
    if (length == 0 || length >= std::size(path))
        return false;
    std::wstring lowered(path, length);
    for (wchar_t& character : lowered)
        character = static_cast<wchar_t>(std::towlower(character));
    return lowered.find(L"global-metadata.dat") != std::wstring::npos ||
           lowered.find(L"global-metadata") != std::wstring::npos;
}

bool validate_runtime_registration(sao_core_process_handle_t handle, uint64_t domain,
                                   RuntimeRegistrationEvidence* evidence, ProbeBudget& budget) {
    if (evidence == nullptr)
        return false;
    evidence->ranges.clear();
    if (domain < kUserModeLowerBound || domain > kUserModeUpperBound || (domain & 7ull) != 0 ||
        domain > UINT64_MAX - 0x20ull)
        return false;
    if (!add_registration_range(*evidence, domain, 0x28u))
        return false;
    uint64_t assemblies = 0;
    uint32_t assembly_count = 0;
    if (!read_exact(handle, domain + 0x18ull, &assemblies, sizeof(assemblies), budget) ||
        !read_exact(handle, domain + 0x20ull, &assembly_count, sizeof(assembly_count), budget) ||
        assembly_count == 0 || assembly_count > 512u || assemblies < kUserModeLowerBound ||
        assemblies > kUserModeUpperBound || (assemblies & 7ull) != 0)
        return false;
    const size_t bytes = static_cast<size_t>(assembly_count) * sizeof(uint64_t);
    if (!add_registration_range(*evidence, assemblies, bytes))
        return false;
    std::vector<uint64_t> values(assembly_count);
    if (!read_exact(handle, assemblies, values.data(), bytes, budget))
        return false;
    for (uint32_t index = 0; index < assembly_count; ++index) {
        if (!budget.active())
            return false;
        const uint64_t assembly = values[index];
        if (assembly < kUserModeLowerBound || assembly > kUserModeUpperBound ||
            (assembly & 7ull) != 0 || assembly > UINT64_MAX - 0x10ull)
            continue;
        if (!add_registration_range(*evidence, assembly, 0x18u))
            return false;
        uint64_t image = 0;
        uint32_t type_count = 0;
        uint64_t type_start = 0;
        if (!read_exact(handle, assembly + 0x10ull, &image, sizeof(image), budget) ||
            image < kUserModeLowerBound || image > kUserModeUpperBound || (image & 7ull) != 0 ||
            image > UINT64_MAX - 0x28ull ||
            !read_exact(handle, image + 0x1Cull, &type_count, sizeof(type_count), budget) ||
            !read_exact(handle, image + 0x28ull, &type_start, sizeof(type_start), budget) ||
            type_count == 0 || type_count > 4096u || type_start < kUserModeLowerBound ||
            type_start > kUserModeUpperBound || (type_start & 7ull) != 0)
            continue;
        if (!add_registration_range(*evidence, image, 0x30u) ||
            !add_registration_range(*evidence, type_start,
                                    static_cast<size_t>(type_count) * sizeof(uint64_t)))
            return false;
        return true;
    }
    return false;
}
} // namespace

sao_status_t probe_metadata_v2_impl(sao_core_process_handle_t handle, uint64_t module_base,
                                    sao_il2cpp_discovery_budget_t* shared_budget,
                                    SaoIl2CppProbeV2* probe_out) {
    if (probe_out == nullptr)
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "probe_out is null", __FILE__, __LINE__);
    *probe_out = SaoIl2CppProbeV2{};
    try {
        if (handle == nullptr)
            return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID, "handle is null", __FILE__, __LINE__);
        if (module_base < kUserModeLowerBound)
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "module_base looks bogus", __FILE__,
                              __LINE__);
        ProbeBudget budget(shared_budget);
        const uint64_t domain_get_addr =
            resolve_export(handle, module_base, "il2cpp_domain_get", budget);
        if (domain_get_addr == 0 && budget.exhausted())
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "IL2CPP metadata probe budget exhausted",
                              __FILE__, __LINE__);
        if (domain_get_addr == 0)
            return fail_probe(SAO_STATUS_ERR_CAPABILITY_MISSING, "il2cpp_domain_get export missing",
                              __FILE__, __LINE__);
        if (domain_get_addr < module_base)
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                              "il2cpp_domain_get export is outside module", __FILE__, __LINE__);
        const DomainGetDecode decoded = decode_domain_get_prologue(handle, domain_get_addr, budget);
        if (decoded.target == 0)
            return fail_probe(SAO_STATUS_ERR_CAPABILITY_MISSING,
                              "il2cpp_domain_get prologue is not a supported anchor", __FILE__,
                              __LINE__);
        uint64_t domain_value = decoded.target;
        bool domain_ok = true;
        if (decoded.load_from_memory)
            domain_value = read_u64(handle, decoded.target, &domain_ok, budget);
        if (!domain_ok || domain_value < kUserModeLowerBound ||
            domain_value > kUserModeUpperBound || (domain_value & 7ull) != 0)
            return fail_probe(SAO_STATUS_ERR_CAPABILITY_MISSING,
                              "il2cpp domain anchor is not readable/valid", __FILE__, __LINE__);
        constexpr const char* kRequiredExports[] = {
            "il2cpp_domain_get_assemblies",
            "il2cpp_assembly_get_image",
            "il2cpp_image_get_class_count",
            "il2cpp_image_get_class",
        };
        for (const char* export_name : kRequiredExports) {
            if (resolve_export(handle, module_base, export_name, budget) == 0) {
                return fail_probe(
                    budget.exhausted() ? SAO_STATUS_ERR_TIMEOUT : SAO_STATUS_ERR_CAPABILITY_MISSING,
                    budget.exhausted() ? "IL2CPP metadata probe budget exhausted"
                                       : "required IL2CPP runtime export is unavailable",
                    __FILE__, __LINE__);
            }
        }
        uint32_t metadata_version = SAO_IL2CPP_METADATA_VERSION_NOT_READY;
        uint64_t metadata_header_rva = 0;
        uint64_t metadata_header_address = 0;
        const bool mapped_file_binding =
            find_metadata_header(handle, module_base, &metadata_version, &metadata_header_rva,
                                 &metadata_header_address, false, nullptr, budget);
        if (budget.exhausted())
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "IL2CPP metadata probe budget exhausted",
                              __FILE__, __LINE__);
        bool metadata_found = mapped_file_binding;
        RuntimeRegistrationEvidence registration_evidence{};
        bool runtime_registration = false;
        if (!metadata_found) {
            runtime_registration =
                validate_runtime_registration(handle, domain_value, &registration_evidence, budget);
            if (budget.exhausted())
                return fail_probe(SAO_STATUS_ERR_TIMEOUT, "IL2CPP metadata probe budget exhausted",
                                  __FILE__, __LINE__);
            if (runtime_registration) {
                metadata_found = find_metadata_header(
                    handle, module_base, &metadata_version, &metadata_header_rva,
                    &metadata_header_address, true, &registration_evidence, budget);
                if (budget.exhausted())
                    return fail_probe(SAO_STATUS_ERR_TIMEOUT,
                                      "IL2CPP metadata probe budget exhausted", __FILE__, __LINE__);
            }
        }
        if (!metadata_found)
            return fail_probe(SAO_STATUS_ERR_CAPABILITY_MISSING,
                              "IL2CPP metadata has no verifiable file or runtime binding", __FILE__,
                              __LINE__);
        SaoIl2CppProbeV2 candidate{};
        candidate.metadata_rva = domain_get_addr - module_base;
        candidate.metadata_version = metadata_version;
        candidate.domain_ptr = domain_value;
        candidate.game_assembly_base = module_base;
        candidate.struct_size = sizeof(candidate);
        candidate.abi_version = SAO_IL2CPP_PROBE_V2_ABI_VERSION;
        candidate.metadata_version_valid = 1u;
        candidate.metadata_header_rva = metadata_header_rva;
        candidate.metadata_binding = mapped_file_binding
                                         ? SAO_IL2CPP_METADATA_BINDING_MAPPED_FILE
                                         : SAO_IL2CPP_METADATA_BINDING_RUNTIME_REGISTRATION;
        *probe_out = candidate;
        return SAO_STATUS_OK;
    } catch (...) {
        *probe_out = SaoIl2CppProbeV2{};
        return fail_probe(SAO_STATUS_ERR_UNKNOWN, "unexpected exception in metadata probe",
                          __FILE__, __LINE__);
    }
}

sao_status_t probe_metadata_v1_impl(sao_core_process_handle_t handle, uint64_t module_base,
                                    SaoIl2CppProbe* probe_out) {
    if (probe_out == nullptr)
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "probe_out is null", __FILE__, __LINE__);
    *probe_out = SaoIl2CppProbe{};
    try {
        if (handle == nullptr)
            return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID, "handle is null", __FILE__, __LINE__);
        if (module_base < kUserModeLowerBound)
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "module_base looks bogus", __FILE__,
                              __LINE__);
        ProbeBudget budget;
        const uint64_t domain_get_addr =
            resolve_export(handle, module_base, "il2cpp_domain_get", budget);
        if (domain_get_addr == 0 && budget.exhausted())
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "V1 IL2CPP probe budget exhausted", __FILE__,
                              __LINE__);
        if (domain_get_addr == 0 || domain_get_addr < module_base)
            return fail_probe(SAO_STATUS_ERR_NOT_FOUND, "il2cpp_domain_get export missing",
                              __FILE__, __LINE__);
        SaoIl2CppProbe candidate{};
        candidate.metadata_rva = domain_get_addr - module_base;
        candidate.game_assembly_base = module_base;
        const DomainGetDecode decoded = decode_domain_get_prologue(handle, domain_get_addr, budget);
        if (decoded.target != 0) {
            uint64_t domain_value = decoded.target;
            bool domain_ok = true;
            if (decoded.load_from_memory)
                domain_value = read_u64(handle, decoded.target, &domain_ok, budget);
            if (domain_ok && domain_value >= kUserModeLowerBound &&
                domain_value <= kUserModeUpperBound && (domain_value & 7ull) == 0)
                candidate.domain_ptr = domain_value;
        }
        uint64_t metadata_header_rva = 0;
        uint64_t metadata_header_address = 0;
        uint32_t metadata_version = SAO_IL2CPP_METADATA_VERSION_NOT_READY;
        if (find_metadata_header(handle, module_base, &metadata_version, &metadata_header_rva,
                                 &metadata_header_address, false, nullptr, budget))
            candidate.metadata_version = metadata_version;
        if (budget.exhausted())
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "V1 IL2CPP probe budget exhausted", __FILE__,
                              __LINE__);
        *probe_out = candidate;
        return SAO_STATUS_OK;
    } catch (...) {
        *probe_out = SaoIl2CppProbe{};
        return fail_probe(SAO_STATUS_ERR_UNKNOWN, "unexpected exception in V1 metadata probe",
                          __FILE__, __LINE__);
    }
}

extern "C" void SAO_CORE_CALL
sao_core_il2cpp_discovery_budget_init(sao_il2cpp_discovery_budget_t* budget, uint64_t max_bytes,
                                      uint32_t max_reads, uint32_t max_duration_ms) {
    if (budget == nullptr)
        return;
    budget->bytes_remaining = max_bytes;
    budget->reads_remaining = max_reads;
    budget->_reserved = 0;
    budget->deadline_ms = sao_core_time_now_ms() + max_duration_ms;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata_v2(
    sao_core_process_handle_t handle, uint64_t module_base, SaoIl2CppProbeV2* probe_out) {
    return probe_metadata_v2_impl(handle, module_base, nullptr, probe_out);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata_v2_with_budget(
    sao_core_process_handle_t handle, uint64_t module_base, sao_il2cpp_discovery_budget_t* budget,
    SaoIl2CppProbeV2* probe_out) {
    return probe_metadata_v2_impl(handle, module_base, budget, probe_out);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata(
    sao_core_process_handle_t handle, uint64_t module_base, SaoIl2CppProbe* probe_out) {
    return probe_metadata_v1_impl(handle, module_base, probe_out);
}

extern "C" sao_status_t SAO_CORE_CALL
sao_core_il2cpp_resolve_export_anchor(sao_core_process_handle_t handle, uint64_t module_base,
                                      const char* export_name, uint64_t* out_rva) {
    if (out_rva == nullptr) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "out_rva is null", __FILE__, __LINE__);
    }
    *out_rva = 0;
    try {
        if (handle == nullptr) {
            return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID, "handle is null", __FILE__, __LINE__);
        }
        if (module_base < kUserModeLowerBound || export_name == nullptr || export_name[0] == '\0') {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                              "export anchor arguments are invalid", __FILE__, __LINE__);
        }
        ProbeBudget budget;
        const uint64_t export_address = resolve_export(handle, module_base, export_name, budget);
        if (export_address == 0 && budget.exhausted())
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "export anchor budget exhausted", __FILE__,
                              __LINE__);
        if (export_address == 0 || export_address < module_base) {
            return fail_probe(SAO_STATUS_ERR_NOT_FOUND, "export anchor not found", __FILE__,
                              __LINE__);
        }
        *out_rva = export_address - module_base;
        return SAO_STATUS_OK;
    } catch (...) {
        *out_rva = 0;
        return fail_probe(SAO_STATUS_ERR_UNKNOWN, "unexpected exception resolving export anchor",
                          __FILE__, __LINE__);
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_resolve_export_anchor_with_budget(
    sao_core_process_handle_t handle, uint64_t module_base, const char* export_name,
    sao_il2cpp_discovery_budget_t* shared_budget, uint64_t* out_rva) {
    if (out_rva == nullptr)
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "out_rva is null", __FILE__, __LINE__);
    *out_rva = 0;
    try {
        if (handle == nullptr)
            return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID, "handle is null", __FILE__, __LINE__);
        if (module_base < kUserModeLowerBound || export_name == nullptr || export_name[0] == '\0')
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                              "export anchor arguments are invalid", __FILE__, __LINE__);
        ProbeBudget budget(shared_budget);
        const uint64_t export_address = resolve_export(handle, module_base, export_name, budget);
        if (export_address == 0 && budget.exhausted())
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "export anchor budget exhausted", __FILE__,
                              __LINE__);
        if (export_address == 0 || export_address < module_base)
            return fail_probe(SAO_STATUS_ERR_NOT_FOUND, "export anchor not found", __FILE__,
                              __LINE__);
        *out_rva = export_address - module_base;
        return SAO_STATUS_OK;
    } catch (...) {
        *out_rva = 0;
        return fail_probe(SAO_STATUS_ERR_UNKNOWN, "unexpected exception resolving export anchor",
                          __FILE__, __LINE__);
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_scan_klass_pointers(
    sao_core_process_handle_t handle, uint64_t region_base, size_t region_size,
    sao_il2cpp_klass_callback_t callback, void* user_data, size_t* visited_out) {
    size_t visited = 0;
    if (visited_out != nullptr)
        *visited_out = 0;
    try {
        if (callback == nullptr) {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "callback is null", __FILE__,
                              __LINE__);
        }
        if (handle == nullptr) {
            return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID, "handle is null", __FILE__, __LINE__);
        }
        if (region_base > UINT64_MAX - static_cast<uint64_t>(region_size)) {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "region range overflows", __FILE__,
                              __LINE__);
        }
        if (region_size < sizeof(uint64_t))
            return SAO_STATUS_OK;

        if (region_base > UINT64_MAX - 7ull) {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "aligned region base overflows",
                              __FILE__, __LINE__);
        }
        const uint64_t aligned_base = (region_base + 7ull) & ~7ull;
        const uint64_t start_shift = aligned_base - region_base;
        if (start_shift >= region_size)
            return SAO_STATUS_OK;
        const size_t usable = region_size - static_cast<size_t>(start_shift);
        const size_t pointer_count = usable / sizeof(uint64_t);
        if (pointer_count == 0)
            return SAO_STATUS_OK;

        ProbeBudget budget;
        constexpr size_t kChunkPointers = 4096;
        std::vector<uint64_t> chunk(kChunkPointers);
        size_t remaining = pointer_count;
        uint64_t cursor = aligned_base;
        while (remaining > 0) {
            const size_t take = std::min(kChunkPointers, remaining);
            const size_t bytes = take * sizeof(uint64_t);
            if (!budget.active() || !budget.reserve_read(bytes)) {
                if (visited_out != nullptr)
                    *visited_out = visited;
                return SAO_STATUS_ERR_TIMEOUT;
            }
            size_t got = 0;
            const sao_status_t rc = sao_core_mem_read(handle, cursor, chunk.data(), bytes, &got);
            if (got > bytes) {
                if (visited_out != nullptr)
                    *visited_out = visited;
                return fail_probe(SAO_STATUS_ERR_READ_FAULT, "memory read returned too many bytes",
                                  __FILE__, __LINE__);
            }
            const size_t got_pointers = got / sizeof(uint64_t);
            for (size_t index = 0; index < got_pointers; ++index) {
                const uint64_t candidate = chunk[index];
                if (candidate < kUserModeLowerBound || candidate > kUserModeUpperBound ||
                    (candidate & 7ull) != 0)
                    continue;
                if (!budget.reserve_candidate()) {
                    if (visited_out != nullptr)
                        *visited_out = visited;
                    return SAO_STATUS_ERR_TIMEOUT;
                }
                ++visited;
                bool keep_scanning = true;
                try {
                    keep_scanning = callback(candidate, user_data);
                } catch (...) {
                    if (visited_out != nullptr)
                        *visited_out = visited;
                    return SAO_STATUS_ERR_UNKNOWN;
                }
                if (!keep_scanning) {
                    if (visited_out != nullptr)
                        *visited_out = visited;
                    return SAO_STATUS_OK;
                }
            }
            if (rc != SAO_STATUS_OK) {
                constexpr uint64_t kPage = 0x1000;
                if (bytes > UINT64_MAX - (kPage - 1) || cursor > UINT64_MAX - bytes - (kPage - 1)) {
                    if (visited_out != nullptr)
                        *visited_out = visited;
                    return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                                      "scan cursor advance overflows", __FILE__, __LINE__);
                }
                const uint64_t next = (cursor + bytes + kPage - 1) & ~(kPage - 1);
                const uint64_t hop = next - cursor;
                const size_t hop_pointers = static_cast<size_t>(hop / sizeof(uint64_t));
                if (hop_pointers >= remaining)
                    break;
                cursor = next;
                remaining -= hop_pointers;
                continue;
            }
            if (bytes > UINT64_MAX - cursor) {
                return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "scan cursor advance overflows",
                                  __FILE__, __LINE__);
            }
            cursor += bytes;
            remaining -= take;
        }
        if (visited_out != nullptr)
            *visited_out = visited;
        return SAO_STATUS_OK;
    } catch (...) {
        if (visited_out != nullptr)
            *visited_out = visited;
        return fail_probe(SAO_STATUS_ERR_UNKNOWN, "unexpected exception scanning klass pointers",
                          __FILE__, __LINE__);
    }
}
extern "C" sao_status_t SAO_CORE_CALL
sao_core_il2cpp_read_klass_name(sao_core_process_handle_t handle, uint64_t klass_ptr,
                                char* out_name_utf8, size_t name_capacity) {
    if (out_name_utf8 == nullptr || name_capacity == 0) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "output buffer is null", __FILE__,
                          __LINE__);
    }
    out_name_utf8[0] = '\0';
    try {
        if (handle == nullptr) {
            return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID, "handle is null", __FILE__, __LINE__);
        }
        if (klass_ptr < kUserModeLowerBound || (klass_ptr & 7ull) != 0) {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "klass pointer misaligned or bogus",
                              __FILE__, __LINE__);
        }
        ProbeBudget budget;
        bool ok = false;
        const uint64_t name_ptr =
            klass_ptr > UINT64_MAX - SAO_IL2CPP_KLASS_NAME_OFFSET
                ? 0
                : read_u64(handle, klass_ptr + SAO_IL2CPP_KLASS_NAME_OFFSET, &ok, budget);
        if (!ok) {
            return fail_probe(SAO_STATUS_ERR_READ_FAULT, "klass name pointer unreadable", __FILE__,
                              __LINE__);
        }
        if (name_ptr < kUserModeLowerBound || name_ptr > kUserModeUpperBound) {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "name pointer looks bogus", __FILE__,
                              __LINE__);
        }

        constexpr size_t kNameLimit = 256;
        std::array<uint8_t, kNameLimit + 1> buf{};
        size_t got = 0;
        if (!budget.reserve_read(buf.size()))
            return fail_probe(SAO_STATUS_ERR_TIMEOUT, "klass name read budget exhausted", __FILE__,
                              __LINE__);
        const sao_status_t rc = sao_core_mem_read(handle, name_ptr, buf.data(), buf.size(), &got);
        if (got > buf.size() || (rc != SAO_STATUS_OK && got == 0)) {
            return fail_probe(SAO_STATUS_ERR_READ_FAULT, "name string unreadable", __FILE__,
                              __LINE__);
        }
        size_t length = 0;
        while (length < got && buf[length] != 0)
            ++length;
        if (length == got) {
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "name string not null-terminated",
                              __FILE__, __LINE__);
        }
        for (size_t index = 0; index < length; ++index) {
            const uint8_t byte = buf[index];
            if (byte < 0x20 || byte > 0x7E) {
                return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT, "name string not ASCII",
                                  __FILE__, __LINE__);
            }
        }
        const size_t needed = length + 1;
        if (name_capacity < needed) {
            return fail_probe(SAO_STATUS_ERR_BUFFER_TOO_SMALL, "name buffer too small", __FILE__,
                              __LINE__);
        }
        std::memcpy(out_name_utf8, buf.data(), length);
        out_name_utf8[length] = '\0';
        return SAO_STATUS_OK;
    } catch (...) {
        out_name_utf8[0] = '\0';
        return fail_probe(SAO_STATUS_ERR_UNKNOWN, "unexpected exception reading klass name",
                          __FILE__, __LINE__);
    }
}
