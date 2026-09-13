#include "sdk_dumper_base.h"
#include "sdk_dumper_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::ai_editor::sdk_dumper {
namespace {

using namespace detail;

constexpr uint64_t kExecutableScanLimit = 32ull * 1024ull * 1024ull;
constexpr size_t kMaximumRipReferences = 65536;
constexpr size_t kMaximumGlobalCandidates = 32;
constexpr size_t kMaximumGlobalProbeVisits = 262144;
constexpr uint32_t kObjectChunkCapacity = 0x10000;
constexpr uint32_t kMaximumObjectChunks = 8192;
constexpr uint32_t kMaximumObjectElements = 8000000;
constexpr uint32_t kMaximumObjectVisits = 10000;
constexpr uint32_t kMaximumFNameBlocks = 8192;
constexpr uint32_t kFNameBlockBytes = 0x20000;

struct RipReference final {
    uint64_t target = 0;
    uint64_t source = 0;
};

struct UObjectHeader final {
    uint64_t vtable = 0;
    uint32_t flags = 0;
    int32_t internal_index = -1;
    uint64_t class_pointer = 0;
    uint32_t name_index = 0;
    uint32_t name_number = 0;
    uint64_t outer = 0;
};

struct ObjectSample final {
    uint32_t index = 0;
    uint64_t object = 0;
    UObjectHeader header;
};

struct ObjectArrayInfo final {
    uint64_t address = 0;
    uint64_t chunks = 0;
    uint32_t maximum_elements = 0;
    uint32_t element_count = 0;
    uint32_t maximum_chunks = 0;
    uint32_t chunk_count = 0;
    uint32_t item_stride = 0;
    uint64_t reference = 0;
    std::vector<ObjectSample> samples;
};

struct NamePoolInfo final {
    uint64_t address = 0;
    uint32_t current_block = 0;
    uint32_t current_cursor = 0;
    uint64_t reference = 0;
};

struct GlobalPair final {
    ObjectArrayInfo objects;
    NamePoolInfo names;
    size_t score = 0;
    bool ambiguous = false;
};

struct ClassRecord final {
    uint64_t object = 0;
    int32_t internal_index = -1;
    uint32_t flags = 0;
    std::string name;
    std::string meta_class;
    std::string outer;
};

enum class TruncationReason { None, MaxObjects, MaxClasses };

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

int unreal_module_priority(const ModuleInfo& module) {
    if (ends_with(module.lower_name, "-win64-shipping.exe")) {
        return 0;
    }
    if (module.lower_name == "ue4editor-coreuobject.dll" ||
        module.lower_name == "ue5editor-coreuobject.dll" ||
        module.lower_name == "unrealeditor-coreuobject.dll") {
        return 1;
    }
    if (module.lower_name == "ue4game.dll" || module.lower_name == "ue5game.dll") {
        return 2;
    }
    if (module.lower_name == "unrealengine.dll") {
        return 3;
    }
    if (module.lower_name == "ue4editor.exe" || module.lower_name == "ue5editor.exe" ||
        module.lower_name == "unrealeditor.exe") {
        return 4;
    }
    return 100;
}

bool modules_identify_unreal(const std::vector<ModuleInfo>& modules) {
    return std::any_of(modules.begin(), modules.end(), [](const ModuleInfo& module) {
        return unreal_module_priority(module) != 100;
    });
}

const ModuleInfo* find_unreal_module(const std::vector<ModuleInfo>& modules, bool& ambiguous) {
    ambiguous = false;
    const ModuleInfo* selected = nullptr;
    int selected_priority = 100;
    for (const ModuleInfo& module : modules) {
        const int priority = unreal_module_priority(module);
        if (priority == 100 || module.size < 1024 * 1024 ||
            module.size > 2ull * 1024ull * 1024ull * 1024ull) {
            continue;
        }
        if (selected == nullptr || priority < selected_priority ||
            (priority == selected_priority && module.base < selected->base)) {
            ambiguous = selected != nullptr && priority == selected_priority;
            selected = &module;
            selected_priority = priority;
        } else if (priority == selected_priority) {
            ambiguous = true;
        }
    }
    return selected;
}

std::vector<size_t> executable_sections(const PeImage& image) {
    std::vector<size_t> sections;
    for (size_t index = 0; index < image.sections.size(); ++index) {
        const PeSection& section = image.sections[index];
        if ((section.characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)) ==
            (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)) {
            sections.push_back(index);
        }
    }
    std::stable_sort(sections.begin(), sections.end(), [&image](size_t left, size_t right) {
        const PeSection& a = image.sections[left];
        const PeSection& b = image.sections[right];
        const int a_priority = a.name == ".text" ? 0 : 1;
        const int b_priority = b.name == ".text" ? 0 : 1;
        return a_priority != b_priority ? a_priority < b_priority : a.base < b.base;
    });
    return sections;
}

bool decode_rip_reference(const uint8_t* instruction, size_t available, uint64_t source,
                          uint64_t& target, size_t& decoded_size) {
    decoded_size = 0;
    size_t displacement_offset = 0;
    size_t instruction_size = 0;
    if (available >= 7 && instruction[0] >= 0x40 && instruction[0] <= 0x4f &&
        (instruction[1] == 0x8d || instruction[1] == 0x8b || instruction[1] == 0x89 ||
         instruction[1] == 0x3b || instruction[1] == 0x39) &&
        (instruction[2] & 0xc7u) == 0x05u) {
        displacement_offset = 3;
        instruction_size = 7;
    } else if (available >= 6 &&
               (instruction[0] == 0x8d || instruction[0] == 0x8b ||
                instruction[0] == 0x89 || instruction[0] == 0x3b ||
                instruction[0] == 0x39) &&
               (instruction[1] & 0xc7u) == 0x05u) {
        displacement_offset = 2;
        instruction_size = 6;
    } else if (available >= 7 && instruction[0] == 0xc6 && instruction[1] == 0x05u) {
        displacement_offset = 2;
        instruction_size = 7;
    } else if (available >= 10 && instruction[0] == 0xc7 && instruction[1] == 0x05u) {
        displacement_offset = 2;
        instruction_size = 10;
    } else {
        return false;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, instruction + displacement_offset, sizeof(displacement));
    uint64_t next = 0;
    if (!checked_add(source, instruction_size, next) ||
        !checked_add_signed(next, displacement, target)) {
        return false;
    }
    decoded_size = instruction_size;
    return true;
}

bool scan_rip_references(ReadBudget& reader, MemoryView& memory, const PeImage& image,
                         const std::vector<CachedRange>& code_ranges,
                         std::vector<RipReference>& references, std::string& error) {
    std::unordered_map<uint64_t, uint64_t> target_sources;
    uint64_t inspected = 0;
    for (const CachedRange& range : code_ranges) {
        for (size_t offset = 0; offset < range.bytes.size(); ++offset) {
            if ((inspected++ & 0x3fffu) == 0 && !reader.checkpoint()) {
                return false;
            }
            std::array<uint8_t, 10> boundary{};
            const uint8_t* instruction = range.bytes.data() + offset;
            size_t available = range.bytes.size() - offset;
            uint64_t source = 0;
            if (!checked_add(range.base, offset, source)) {
                continue;
            }
            const PeSection* source_section =
                find_section(image, source, IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
            if (source_section == nullptr) {
                continue;
            }
            if (available < boundary.size()) {
                if (!section_contains(*source_section, source, boundary.size()) ||
                    !memory.read(source, boundary.data(), boundary.size())) {
                    if (reader.reason() != StopReason::None) {
                        return false;
                    }
                    continue;
                }
                instruction = boundary.data();
                available = boundary.size();
            }
            uint64_t target = 0;
            size_t decoded_size = 0;
            if (!decode_rip_reference(instruction, available, source, target, decoded_size) ||
                !section_contains(*source_section, source, decoded_size)) {
                continue;
            }
            const PeSection* target_section =
                find_section(image, target, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
            if (target_section == nullptr ||
                !section_contains(*target_section, target, sizeof(uint64_t))) {
                continue;
            }
            auto [entry, inserted] = target_sources.emplace(target, source);
            if (!inserted && source < entry->second) {
                entry->second = source;
            }
            if (target_sources.size() > kMaximumRipReferences) {
                error = "budget: RIP-relative writable reference cap exceeded 65,536";
                return false;
            }
        }
    }
    references.reserve(target_sources.size());
    for (const auto& [target, source] : target_sources) {
        references.push_back(RipReference{target, source});
    }
    std::sort(references.begin(), references.end(), [](const RipReference& left,
                                                       const RipReference& right) {
        return left.target != right.target ? left.target < right.target : left.source < right.source;
    });
    return reader.checkpoint();
}

bool read_uobject_header(MemoryView& memory, const std::vector<ModuleInfo>& modules,
                         uint64_t address, UObjectHeader& header) {
    std::array<uint8_t, 0x28> bytes{};
    if (!valid_pointer(address, 8) || !memory.read(address, bytes.data(), bytes.size())) {
        return false;
    }
    std::memcpy(&header.vtable, bytes.data() + 0x00, sizeof(header.vtable));
    std::memcpy(&header.flags, bytes.data() + 0x08, sizeof(header.flags));
    std::memcpy(&header.internal_index, bytes.data() + 0x0c, sizeof(header.internal_index));
    std::memcpy(&header.class_pointer, bytes.data() + 0x10, sizeof(header.class_pointer));
    std::memcpy(&header.name_index, bytes.data() + 0x18, sizeof(header.name_index));
    std::memcpy(&header.name_number, bytes.data() + 0x1c, sizeof(header.name_number));
    std::memcpy(&header.outer, bytes.data() + 0x20, sizeof(header.outer));
    uint64_t first_virtual_function = 0;
    return valid_pointer(header.vtable, 8) &&
           address_in_modules(modules, header.vtable, sizeof(uint64_t)) &&
           memory.read_value(header.vtable, first_virtual_function) &&
           valid_pointer_target(first_virtual_function, 8) &&
           address_in_modules(modules, first_virtual_function) &&
           valid_pointer(header.class_pointer, 8) &&
           (header.outer == 0 || valid_pointer(header.outer, 8));
}

bool read_fname(MemoryView& memory, const NamePoolInfo& pool, uint32_t index,
                std::string& name) {
    const uint32_t block_index = index >> 16u;
    const uint32_t entry_units = index & 0xffffu;
    if (block_index > pool.current_block || block_index >= kMaximumFNameBlocks) {
        return false;
    }
    uint64_t block_slot_delta = 0;
    uint64_t block_slot = 0;
    uint64_t block = 0;
    uint64_t blocks = 0;
    if (!checked_add(pool.address, 0x10, blocks) ||
        !checked_mul(block_index, sizeof(uint64_t), block_slot_delta) ||
        !checked_add(blocks, block_slot_delta, block_slot) ||
        !memory.read_value(block_slot, block) || !valid_pointer(block, 8)) {
        return false;
    }
    const uint32_t byte_offset = entry_units * 2u;
    if (byte_offset >= kFNameBlockBytes ||
        (block_index == pool.current_block && byte_offset >= pool.current_cursor)) {
        return false;
    }
    uint64_t entry = 0;
    uint16_t header = 0;
    if (!checked_add(block, byte_offset, entry) || !memory.read_value(entry, header)) {
        return false;
    }
    const bool wide = (header & 1u) != 0;
    const uint32_t length = header >> 6u;
    const uint32_t character_bytes = wide ? 2u : 1u;
    uint64_t payload_bytes = 0;
    if (length == 0 || length > 1023 || !checked_mul(length, character_bytes, payload_bytes) ||
        byte_offset + 2ull + payload_bytes > kFNameBlockBytes ||
        (block_index == pool.current_block &&
         byte_offset + 2ull + payload_bytes > pool.current_cursor)) {
        return false;
    }
    uint64_t payload = 0;
    if (!checked_add(entry, 2, payload)) {
        return false;
    }
    if (wide) {
        std::vector<wchar_t> characters(length);
        if (!memory.read(payload, characters.data(), static_cast<size_t>(payload_bytes))) {
            return false;
        }
        name = sao::ai_editor::native::wide_to_utf8(
            std::wstring_view(characters.data(), characters.size()));
    } else {
        name.resize(length);
        if (!memory.read(payload, name.data(), name.size())) {
            name.clear();
            return false;
        }
    }
    return valid_utf8_text(name, 4096);
}

bool probe_name_pool(MemoryView& memory, uint64_t address, uint64_t reference,
                     NamePoolInfo& pool) {
    if (!valid_pointer(address, 8)) {
        return false;
    }
    uint32_t current_block = 0;
    uint32_t current_cursor = 0;
    uint64_t block0 = 0;
    uint64_t field = 0;
    if (!checked_add(address, 0x08, field) || !memory.read_value(field, current_block) ||
        !checked_add(address, 0x0c, field) || !memory.read_value(field, current_cursor) ||
        !checked_add(address, 0x10, field) || !memory.read_value(field, block0) ||
        current_block >= kMaximumFNameBlocks || current_cursor > kFNameBlockBytes ||
        (current_cursor & 1u) != 0 || !valid_pointer(block0, 8)) {
        return false;
    }
    NamePoolInfo candidate{address, current_block, current_cursor, reference};
    std::string none;
    if (!read_fname(memory, candidate, 0, none) || none != "None") {
        return false;
    }
    pool = candidate;
    return true;
}

bool read_object_item(MemoryView& memory, const ObjectArrayInfo& objects, uint32_t index,
                      std::unordered_map<uint32_t, uint64_t>& chunk_cache,
                      uint64_t& object) {
    if (index >= objects.element_count) {
        return false;
    }
    const uint32_t chunk_index = index / kObjectChunkCapacity;
    uint64_t chunk = 0;
    const auto cached = chunk_cache.find(chunk_index);
    if (cached != chunk_cache.end()) {
        chunk = cached->second;
    } else {
        uint64_t slot_delta = 0;
        uint64_t slot = 0;
        if (!checked_mul(chunk_index, sizeof(uint64_t), slot_delta) ||
            !checked_add(objects.chunks, slot_delta, slot) || !memory.read_value(slot, chunk) ||
            !valid_pointer(chunk, 8)) {
            return false;
        }
        chunk_cache.emplace(chunk_index, chunk);
    }
    uint64_t item_delta = 0;
    uint64_t item = 0;
    if (!checked_mul(index % kObjectChunkCapacity, objects.item_stride, item_delta) ||
        !checked_add(chunk, item_delta, item) || !memory.read_value(item, object)) {
        return false;
    }
    return object == 0 || valid_pointer(object, 8);
}

bool probe_object_array(MemoryView& memory, const std::vector<ModuleInfo>& modules,
                        uint64_t address, uint64_t reference, ObjectArrayInfo& objects) {
    if (!valid_pointer(address, 8)) {
        return false;
    }
    uint64_t chunks = 0;
    uint64_t preallocated = 0;
    uint32_t maximum_elements = 0;
    uint32_t element_count = 0;
    uint32_t maximum_chunks = 0;
    uint32_t chunk_count = 0;
    uint64_t field = 0;
    if (!checked_add(address, 0x10, field) || !memory.read_value(field, chunks) ||
        !checked_add(address, 0x18, field) || !memory.read_value(field, preallocated) ||
        !checked_add(address, 0x20, field) || !memory.read_value(field, maximum_elements) ||
        !checked_add(address, 0x24, field) || !memory.read_value(field, element_count) ||
        !checked_add(address, 0x28, field) || !memory.read_value(field, maximum_chunks) ||
        !checked_add(address, 0x2c, field) || !memory.read_value(field, chunk_count)) {
        return false;
    }
    if (element_count < 16 || element_count > kMaximumObjectElements ||
        maximum_elements < element_count || maximum_elements > kMaximumObjectElements) {
        return false;
    }
    const uint32_t required_chunks = static_cast<uint32_t>(
        (static_cast<uint64_t>(element_count) + kObjectChunkCapacity - 1u) /
        kObjectChunkCapacity);
    uint64_t maximum_capacity = 0;
    if (!valid_pointer(chunks, 8) ||
        (preallocated != 0 && !valid_pointer(preallocated, 8)) || maximum_chunks == 0 ||
        maximum_chunks > kMaximumObjectChunks || chunk_count < required_chunks ||
        chunk_count > maximum_chunks ||
        !checked_mul(maximum_chunks, kObjectChunkCapacity, maximum_capacity) ||
        maximum_elements > maximum_capacity) {
        return false;
    }

    ObjectArrayInfo best;
    size_t best_valid = 0;
    bool stride_ambiguous = false;
    for (const uint32_t stride : {uint32_t{0x18}, uint32_t{0x20}}) {
        ObjectArrayInfo candidate{address, chunks, maximum_elements, element_count, maximum_chunks,
                                  chunk_count, stride, reference, {}};
        std::unordered_map<uint32_t, uint64_t> chunk_cache;
        const uint32_t sample_limit = (std::min)(element_count, uint32_t{2048});
        for (uint32_t index = 0; index < sample_limit && candidate.samples.size() < 12; ++index) {
            uint64_t object = 0;
            if (!read_object_item(memory, candidate, index, chunk_cache, object)) {
                candidate.samples.clear();
                break;
            }
            if (object == 0) {
                continue;
            }
            UObjectHeader header;
            if (!read_uobject_header(memory, modules, object, header) ||
                header.internal_index != static_cast<int32_t>(index) ||
                header.name_number > 1000000) {
                continue;
            }
            candidate.samples.push_back(ObjectSample{index, object, header});
        }
        if (candidate.samples.size() > best_valid) {
            best_valid = candidate.samples.size();
            best = std::move(candidate);
            stride_ambiguous = false;
        } else if (candidate.samples.size() == best_valid && best_valid >= 4) {
            stride_ambiguous = true;
        }
    }
    if (best_valid < 4 || stride_ambiguous) {
        return false;
    }
    objects = std::move(best);
    return true;
}

void add_candidate_address(std::vector<uint64_t>& candidates, uint64_t value) {
    if (valid_pointer(value, 8)) {
        candidates.push_back(value);
    }
}

bool discover_global_candidates(ReadBudget& reader, MemoryView& memory,
                                const std::vector<ModuleInfo>& modules,
                                const std::vector<RipReference>& references,
                                std::vector<ObjectArrayInfo>& object_candidates,
                                std::vector<NamePoolInfo>& name_candidates,
                                std::string& error) {
    std::unordered_set<uint64_t> visited_candidates;
    for (const RipReference& reference : references) {
        if (!reader.checkpoint()) {
            return false;
        }
        std::vector<uint64_t> candidates;
        candidates.reserve(16);
        uint64_t adjusted = 0;
        for (const uint32_t field_offset :
             {uint32_t{0x00}, uint32_t{0x08}, uint32_t{0x0c}, uint32_t{0x10},
              uint32_t{0x18}, uint32_t{0x20}, uint32_t{0x24}, uint32_t{0x28},
              uint32_t{0x2c}}) {
            if (checked_sub(reference.target, field_offset, adjusted)) {
                add_candidate_address(candidates, adjusted);
            }
        }
        uint64_t indirect = 0;
        if (memory.read_value(reference.target, indirect) && valid_pointer(indirect, 8)) {
            for (const uint32_t field_offset :
                 {uint32_t{0x00}, uint32_t{0x08}, uint32_t{0x10}}) {
                if (checked_sub(indirect, field_offset, adjusted)) {
                    add_candidate_address(candidates, adjusted);
                }
            }
        } else if (reader.reason() != StopReason::None) {
            return false;
        }

        for (const uint64_t candidate : candidates) {
            if (visited_candidates.find(candidate) != visited_candidates.end())
                continue;
            if (visited_candidates.size() == kMaximumGlobalProbeVisits) {
                error = "budget: Unreal global probe visits exceeded 262,144";
                return false;
            }
            visited_candidates.insert(candidate);
            if (!address_in_modules(modules, candidate, 0x30))
                continue;

            ObjectArrayInfo objects;
            if (probe_object_array(memory, modules, candidate, reference.source, objects)) {
                if (object_candidates.size() == kMaximumGlobalCandidates) {
                    error = "budget: GUObjectArray candidate cap exceeded 32";
                    return false;
                }
                object_candidates.push_back(std::move(objects));
            } else if (reader.reason() != StopReason::None) {
                return false;
            }

            NamePoolInfo names;
            if (probe_name_pool(memory, candidate, reference.source, names)) {
                if (name_candidates.size() == kMaximumGlobalCandidates) {
                    error = "budget: FNamePool candidate cap exceeded 32";
                    return false;
                }
                name_candidates.push_back(std::move(names));
            } else if (reader.reason() != StopReason::None) {
                return false;
            }
        }
    }
    return reader.checkpoint();
}

size_t pair_score(MemoryView& memory, const std::vector<ModuleInfo>& modules,
                  const ObjectArrayInfo& objects, const NamePoolInfo& names) {
    size_t object_names = 0;
    size_t class_names = 0;
    std::unordered_set<uint64_t> visited_classes;
    for (const ObjectSample& sample : objects.samples) {
        std::string object_name;
        if (read_fname(memory, names, sample.header.name_index, object_name)) {
            ++object_names;
        }
        if (!visited_classes.insert(sample.header.class_pointer).second) {
            continue;
        }
        UObjectHeader class_header;
        std::string class_name;
        if (read_uobject_header(memory, modules, sample.header.class_pointer, class_header) &&
            class_header.internal_index >= 0 &&
            static_cast<uint32_t>(class_header.internal_index) < objects.element_count &&
            read_fname(memory, names, class_header.name_index, class_name)) {
            ++class_names;
        }
    }
    return object_names >= 3 && class_names >= 2 ? object_names + class_names * 2 : 0;
}

bool select_global_pair(ReadBudget& reader, MemoryView& memory,
                        const std::vector<ModuleInfo>& modules,
                        const std::vector<ObjectArrayInfo>& object_candidates,
                        const std::vector<NamePoolInfo>& name_candidates, GlobalPair& selected) {
    for (const ObjectArrayInfo& objects : object_candidates) {
        for (const NamePoolInfo& names : name_candidates) {
            if (!reader.checkpoint()) {
                return false;
            }
            const size_t score = pair_score(memory, modules, objects, names);
            if (!reader.checkpoint()) {
                return false;
            }
            const std::pair candidate_identity{objects.address, names.address};
            const std::pair selected_identity{selected.objects.address, selected.names.address};
            if (score > selected.score) {
                selected = GlobalPair{objects, names, score, false};
            } else if (score == selected.score && score != 0 &&
                       candidate_identity != selected_identity) {
                selected.ambiguous = true;
                if (candidate_identity < selected_identity) {
                    const bool ambiguous = selected.ambiguous;
                    selected = GlobalPair{objects, names, score, ambiguous};
                }
            }
        }
    }
    return true;
}

bool collect_class_pointers(ReadBudget& reader, MemoryView& memory,
                            const std::vector<ModuleInfo>& modules,
                            const ObjectArrayInfo& objects, uint32_t class_cap,
                            std::unordered_set<uint64_t>& class_pointers,
                            uint32_t& object_visits, TruncationReason& truncation,
                            std::string& error) {
    std::unordered_set<uint64_t> visited_objects;
    visited_objects.reserve((std::min)(objects.element_count, kMaximumObjectVisits));
    class_pointers.reserve(class_cap);
    uint32_t visited_indices = 0;
    for (uint32_t chunk_index = 0;
         chunk_index < objects.chunk_count && visited_indices < objects.element_count;
         ++chunk_index) {
        uint64_t chunk_slot_delta = 0;
        uint64_t chunk_slot = 0;
        uint64_t chunk = 0;
        if (!checked_mul(chunk_index, sizeof(uint64_t), chunk_slot_delta) ||
            !checked_add(objects.chunks, chunk_slot_delta, chunk_slot) ||
            !memory.read_value(chunk_slot, chunk) || !valid_pointer(chunk, 8)) {
            if (!set_stop_error(reader, error)) {
                error = "capability: active FUObjectArray chunk pointer is invalid";
            }
            return false;
        }
        const uint32_t remaining = objects.element_count - visited_indices;
        const uint32_t chunk_elements = (std::min)(remaining, kObjectChunkCapacity);
        uint32_t local_index = 0;
        while (local_index < chunk_elements) {
            if (!reader.checkpoint()) {
                set_stop_error(reader, error);
                return false;
            }
            if (visited_indices >= kMaximumObjectVisits) {
                object_visits = visited_indices;
                truncation = TruncationReason::MaxObjects;
                return true;
            }
            const uint32_t visit_room = kMaximumObjectVisits - visited_indices;
            const uint32_t batch_count =
                (std::min)({chunk_elements - local_index, uint32_t{4096}, visit_room});
            uint64_t batch_delta = 0;
            uint64_t batch_address = 0;
            uint64_t batch_bytes = 0;
            if (batch_count == 0 || !checked_mul(local_index, objects.item_stride, batch_delta) ||
                !checked_add(chunk, batch_delta, batch_address) ||
                !checked_mul(batch_count, objects.item_stride, batch_bytes) ||
                batch_bytes > (std::numeric_limits<size_t>::max)()) {
                error = "capability: FUObjectItem batch bounds overflow";
                return false;
            }
            std::vector<uint8_t> items(static_cast<size_t>(batch_bytes));
            if (!memory.read(batch_address, items.data(), items.size())) {
                if (!set_stop_error(reader, error)) {
                    error = "capability: active FUObjectItem batch is unreadable";
                }
                return false;
            }
            for (uint32_t index = 0; index < batch_count; ++index) {
                uint64_t object = 0;
                std::memcpy(&object, items.data() + static_cast<size_t>(index) * objects.item_stride,
                            sizeof(object));
                if (!valid_pointer(object, 8) || !visited_objects.insert(object).second) {
                    continue;
                }
                UObjectHeader header;
                if (!read_uobject_header(memory, modules, object, header)) {
                    if (reader.reason() != StopReason::None) {
                        set_stop_error(reader, error);
                        return false;
                    }
                    continue;
                }
                const uint32_t object_index = visited_indices + index;
                if (header.internal_index != static_cast<int32_t>(object_index) ||
                    header.name_number > 1000000) {
                    continue;
                }
                class_pointers.insert(header.class_pointer);
                if (class_pointers.size() == class_cap) {
                    if (!reader.checkpoint()) {
                        set_stop_error(reader, error);
                        return false;
                    }
                    object_visits = object_index + 1;
                    truncation = object_visits < objects.element_count
                                     ? TruncationReason::MaxClasses
                                     : TruncationReason::None;
                    return true;
                }
            }
            local_index += batch_count;
            visited_indices += batch_count;
        }
    }
    if (!reader.checkpoint()) {
        set_stop_error(reader, error);
        return false;
    }
    object_visits = visited_indices;
    truncation = TruncationReason::None;
    return true;
}

class NameResolver final {
public:
    NameResolver(MemoryView& memory, const NamePoolInfo& pool) : memory_(memory), pool_(pool) {}

    bool resolve(uint32_t index, uint32_t number, std::string& name) {
        const auto cached = cache_.find(index);
        if (cached != cache_.end()) {
            name = cached->second;
        } else {
            if (!read_fname(memory_, pool_, index, name)) {
                return false;
            }
            cache_.emplace(index, name);
        }
        if (number != 0) {
            if (number > 1000000) {
                return false;
            }
            name += "_" + decimal(number - 1u);
        }
        return valid_utf8_text(name, 4096);
    }

private:
    MemoryView& memory_;
    const NamePoolInfo& pool_;
    std::unordered_map<uint32_t, std::string> cache_;
};

bool build_class_records(ReadBudget& reader, MemoryView& memory,
                         const std::vector<ModuleInfo>& modules, const ObjectArrayInfo& objects,
                         const NamePoolInfo& names,
                         const std::unordered_set<uint64_t>& class_pointers,
                         std::vector<ClassRecord>& records, std::string& error) {
    std::vector<uint64_t> ordered_pointers(class_pointers.begin(), class_pointers.end());
    std::sort(ordered_pointers.begin(), ordered_pointers.end());
    std::unordered_map<uint32_t, uint64_t> chunk_cache;
    std::unordered_set<uint64_t> visited_classes;
    std::unordered_map<uint64_t, std::string> outer_cache;
    NameResolver resolver(memory, names);
    for (const uint64_t class_pointer : ordered_pointers) {
        if (!reader.checkpoint()) {
            set_stop_error(reader, error);
            return false;
        }
        if (!visited_classes.insert(class_pointer).second) {
            continue;
        }
        UObjectHeader header;
        if (!read_uobject_header(memory, modules, class_pointer, header) ||
            header.internal_index < 0 ||
            static_cast<uint32_t>(header.internal_index) >= objects.element_count) {
            if (reader.reason() != StopReason::None) {
                set_stop_error(reader, error);
                return false;
            }
            continue;
        }
        uint64_t backlink = 0;
        if (!read_object_item(memory, objects, static_cast<uint32_t>(header.internal_index),
                              chunk_cache, backlink) ||
            backlink != class_pointer) {
            if (reader.reason() != StopReason::None) {
                set_stop_error(reader, error);
                return false;
            }
            continue;
        }
        std::string name;
        if (!resolver.resolve(header.name_index, header.name_number, name)) {
            if (reader.reason() != StopReason::None) {
                set_stop_error(reader, error);
                return false;
            }
            continue;
        }
        UObjectHeader meta_header;
        std::string meta_class;
        if (!read_uobject_header(memory, modules, header.class_pointer, meta_header) ||
            meta_header.internal_index < 0 ||
            static_cast<uint32_t>(meta_header.internal_index) >= objects.element_count ||
            !resolver.resolve(meta_header.name_index, meta_header.name_number, meta_class)) {
            if (reader.reason() != StopReason::None) {
                set_stop_error(reader, error);
                return false;
            }
            continue;
        }
        uint64_t meta_backlink = 0;
        if (!read_object_item(memory, objects,
                              static_cast<uint32_t>(meta_header.internal_index),
                              chunk_cache, meta_backlink) ||
            meta_backlink != header.class_pointer) {
            if (reader.reason() != StopReason::None) {
                set_stop_error(reader, error);
                return false;
            }
            continue;
        }
        std::string outer;
        if (header.outer != 0) {
            const auto cached_outer = outer_cache.find(header.outer);
            if (cached_outer != outer_cache.end()) {
                outer = cached_outer->second;
            } else {
                UObjectHeader outer_header;
                if (read_uobject_header(memory, modules, header.outer, outer_header) &&
                    resolver.resolve(outer_header.name_index, outer_header.name_number, outer)) {
                    outer_cache.emplace(header.outer, outer);
                } else if (reader.reason() != StopReason::None) {
                    set_stop_error(reader, error);
                    return false;
                } else {
                    outer.clear();
                }
            }
        }
        records.push_back(ClassRecord{class_pointer, header.internal_index, header.flags,
                                      std::move(name), std::move(meta_class), std::move(outer)});
    }
    return true;
}

bool build_unreal_output(const DumpConfig& config, const ModuleInfo& module,
                         const GlobalPair& pair, std::vector<ClassRecord> records,
                         uint32_t object_visits, TruncationReason truncation,
                         uint64_t bytes_read, std::string& output, std::string& error) {
    std::sort(records.begin(), records.end(), [](const ClassRecord& left, const ClassRecord& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        return left.object < right.object;
    });
    if (!append_output(output, "#pragma once", error) || !append_output(output, "", error) ||
        !append_output(output, "// unreal-sdk-v1", error) ||
        !append_output(output, "// metadata-v1\ttarget-pid\t" + decimal(config.target_pid), error) ||
        !append_output(output, "// metadata-v1\tmodule\t" + escaped_field(module.name), error) ||
        !append_output(output,
                       "// metadata-v1\tmodule-base\t" + hexadecimal(module.base, 16), error) ||
        !append_output(output,
                       "// metadata-v1\tguobject-array\t" +
                           hexadecimal(pair.objects.address, 16),
                       error) ||
        !append_output(output,
                       "// metadata-v1\tfname-pool\t" + hexadecimal(pair.names.address, 16),
                       error) ||
        !append_output(output,
                       "// metadata-v1\tguobject-reference\t" +
                           hexadecimal(pair.objects.reference, 16),
                       error) ||
        !append_output(output,
                       "// metadata-v1\tfname-reference\t" +
                           hexadecimal(pair.names.reference, 16),
                       error) ||
        !append_output(output,
                       "// metadata-v1\tobject-count\t" +
                           decimal(pair.objects.element_count),
                       error) ||
        !append_output(output,
                       "// metadata-v1\tobject-visits\t" + decimal(object_visits), error) ||
        !append_output(output,
                       "// metadata-v1\tclass-count\t" + decimal(records.size()), error) ||
        !append_output(output,
                       "// metadata-v1\tread-bytes\t" + decimal(bytes_read), error) ||
        !append_output(output,
                       "// metadata-v1\tlayout\tFUObjectItem=" +
                           hexadecimal(pair.objects.item_stride, 2) +
                           "\tUObject.ClassPrivate=0x10\tUObject.NamePrivate=0x18\tFNamePool.Blocks=0x10",
                       error) ||
        !append_output(output,
                       std::string("// metadata-v1\ttruncated\t") +
                           (truncation == TruncationReason::MaxObjects
                                ? "max-objects"
                                : truncation == TruncationReason::MaxClasses ? "max-classes"
                                                                            : "none"),
                       error)) {
        return false;
    }
    for (const ClassRecord& record : records) {
        const std::string line =
            "// class-v1\t" + escaped_field(record.name) + "\tobject=" +
            hexadecimal(record.object, 16) + "\tindex=" + signed_decimal(record.internal_index) +
            "\tflags=" + hexadecimal(record.flags, 8) + "\tmeta=" +
            escaped_field(record.meta_class) + "\touter=" + escaped_field(record.outer);
        if (!append_output(output, line, error)) {
            return false;
        }
    }
    return true;
}

class UnrealDumper final : public DumperBase {
public:
    DumpKind kind() const override { return DumpKind::Unreal; }

    DumpResult dump(const DumpConfig& config) override {
        DumpResult result;
        if (config.target_pid == 0) {
            result.error_message = "invalid: target_pid must be non-zero";
            return result;
        }
        if (config.max_classes == 0) {
            result.error_message = "invalid: max_classes must be non-zero";
            return result;
        }
        if (config.timeout_ms == 0) {
            result.error_message = "invalid: timeout_ms must be non-zero";
            return result;
        }

        try {
            Deadline deadline(config.timeout_ms);
            fs::path workspace_boundary;
            fs::path output_path;
            if (!prepare_output_path(config.workspace_root, config.output_dir,
                                     L"Unreal-dump.h", workspace_boundary,
                                     output_path, result.error_message)) {
                return result;
            }
            if (deadline.expired()) {
                result.error_message = "timeout: monotonic dump deadline expired";
                return result;
            }

            ProcessHandle process;
            const sao_status_t open_status = process.open(config.target_pid);
            if (deadline.expired()) {
                result.error_message = "timeout: monotonic dump deadline expired";
                return result;
            }
            if (open_status != SAO_STATUS_OK || process.get() == nullptr) {
                result.error_message =
                    open_status == SAO_STATUS_ERR_PROCESS_GONE ||
                            open_status == SAO_STATUS_ERR_NOT_FOUND
                        ? "not-found: target process does not exist"
                        : "capability: target process cannot be opened for read-only inspection";
                return result;
            }
            ReadBudget reader(process.get(), deadline);
            std::vector<ModuleInfo> modules;
            if (!enumerate_modules(process.get(), modules, result.error_message)) {
                if (!reader.checkpoint()) {
                    set_stop_error(reader, result.error_message);
                }
                return result;
            }
            if (!reader.checkpoint()) {
                set_stop_error(reader, result.error_message);
                return result;
            }
            if (!modules_identify_unreal(modules)) {
                result.error_message = "not-found: target does not validate as an Unreal process";
                return result;
            }
            bool module_ambiguous = false;
            const ModuleInfo* module = find_unreal_module(modules, module_ambiguous);
            if (module_ambiguous) {
                result.error_message =
                    "capability: multiple equally preferred Unreal modules were found";
                return result;
            }
            if (module == nullptr) {
                result.error_message = "not-found: bounded Unreal game module is not loaded";
                return result;
            }

            PeImage image;
            if (!parse_pe_image(reader, *module, image, result.error_message)) {
                return result;
            }
            if (image.pointer_size != 8) {
                result.error_message = "capability: Unreal dumper requires a validated x64 PE layout";
                return result;
            }

            std::vector<CachedRange> code_ranges;
            bool incomplete_code_scan = false;
            if (!cache_sections(reader, image, executable_sections(image), kExecutableScanLimit,
                                code_ranges, incomplete_code_scan)) {
                set_stop_error(reader, result.error_message);
                return result;
            }
            if (incomplete_code_scan) {
                result.error_message =
                    "budget: bounded Unreal scan did not cover every executable section";
                return result;
            }
            if (code_ranges.empty()) {
                result.error_message = "capability: Unreal game module has no readable executable section";
                return result;
            }
            MemoryView memory(reader, code_ranges);
            std::vector<RipReference> references;
            if (!scan_rip_references(reader, memory, image, code_ranges, references,
                                     result.error_message)) {
                if (result.error_message.empty()) {
                    set_stop_error(reader, result.error_message);
                }
                return result;
            }
            if (references.empty()) {
                result.error_message = incomplete_code_scan
                                           ? "budget: bounded Unreal executable scan did not cover all code"
                                           : "not-found: no writable RIP-relative Unreal globals were found";
                return result;
            }

            std::vector<ObjectArrayInfo> object_candidates;
            std::vector<NamePoolInfo> name_candidates;
            if (!discover_global_candidates(reader, memory, modules, references, object_candidates,
                                            name_candidates, result.error_message)) {
                if (result.error_message.empty())
                    set_stop_error(reader, result.error_message);
                return result;
            }
            if (object_candidates.empty() || name_candidates.empty()) {
                result.error_message = incomplete_code_scan
                                           ? "budget: bounded code scan did not validate GUObjectArray and FNamePool references"
                                           : "capability: plausible GUObjectArray and FNamePool references did not validate";
                return result;
            }

            GlobalPair pair;
            if (!select_global_pair(reader, memory, modules, object_candidates, name_candidates,
                                    pair)) {
                set_stop_error(reader, result.error_message);
                return result;
            }
            if (pair.score == 0) {
                result.error_message = incomplete_code_scan
                                           ? "budget: bounded code scan did not produce a cross-validated Unreal global pair"
                                           : "capability: GUObjectArray/FNamePool candidates failed cross-layout validation";
                return result;
            }
            if (pair.ambiguous) {
                result.error_message =
                    "capability: multiple equally scored GUObjectArray/FNamePool pairs were found";
                return result;
            }

            const uint32_t class_cap = (std::min)(config.max_classes, uint32_t{10000});
            std::unordered_set<uint64_t> class_pointers;
            uint32_t object_visits = 0;
            TruncationReason truncation = TruncationReason::None;
            if (!collect_class_pointers(reader, memory, modules, pair.objects, class_cap,
                                        class_pointers, object_visits, truncation,
                                        result.error_message)) {
                return result;
            }
            if (class_pointers.empty()) {
                result.error_message =
                    "capability: FUObjectItem traversal found no valid UObject class pointers";
                return result;
            }

            std::vector<ClassRecord> records;
            records.reserve(class_pointers.size());
            if (!build_class_records(reader, memory, modules, pair.objects, pair.names,
                                     class_pointers, records, result.error_message)) {
                return result;
            }
            if (records.empty()) {
                result.error_message =
                    "capability: UObject class objects failed bounded name and backlink validation";
                return result;
            }
            if (!reader.checkpoint()) {
                set_stop_error(reader, result.error_message);
                return result;
            }

            const uint32_t emitted_class_count = static_cast<uint32_t>(records.size());
            std::string output;
            output.reserve(records.size() * size_t{160} + 4096);
            if (!build_unreal_output(config, *module, pair, std::move(records), object_visits,
                                     truncation, reader.bytes_used(), output,
                                     result.error_message)) {
                return result;
            }
            std::string output_file = path_utf8(output_path);
            if (output_file.empty()) {
                result.error_message = "capability: fixed output path is not valid UTF-8";
                return result;
            }
            std::vector<std::string> output_files;
            output_files.push_back(std::move(output_file));
            if (!reader.checkpoint()) {
                set_stop_error(reader, result.error_message);
                return result;
            }
            if (!publish_output(workspace_boundary, output_path, output,
                                result.error_message)) {
                return result;
            }

            result.ok = true;
            result.output_files.swap(output_files);
            result.class_count = emitted_class_count;
            return result;
        } catch (const std::bad_alloc&) {
            result.error_message = "budget: allocation failed within bounded Unreal dump";
            return result;
        } catch (...) {
            result.error_message = "capability: unexpected Unreal dump failure";
            return result;
        }
    }
};

} // namespace

DumperBase* create_unreal() { return new UnrealDumper(); }

} // namespace sao::ai_editor::sdk_dumper
