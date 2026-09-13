#include "sdk_dumper_base.h"
#include "sdk_dumper_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::ai_editor::sdk_dumper {
namespace {

using namespace detail;

constexpr size_t kMaximumDtStrings = 8192;
constexpr size_t kMaximumRecvTables = 8192;
constexpr size_t kMaximumClientClassCandidates = 4096;
constexpr size_t kMaximumNetvars = 250000;
constexpr size_t kMaximumStringCandidateVisits = 262144;
constexpr size_t kMaximumRecvTableCandidateVisits = 262144;
constexpr size_t kMaximumClientClassCandidateVisits = 262144;
constexpr size_t kMaximumRecvPropVisits = 250000;
constexpr size_t kMaximumRecvTableDepth = 64;
constexpr size_t kMaximumNetvarPathBytes = 4096;
constexpr uint64_t kModuleScanLimit = 48ull * 1024ull * 1024ull;

struct SourceLayout final {
    uint8_t pointer_size;
    uint32_t client_network_name;
    uint32_t client_recv_table;
    uint32_t client_next;
    uint32_t client_class_id;
    uint32_t client_size;
    uint32_t table_props;
    uint32_t table_prop_count;
    uint32_t table_decoder;
    uint32_t table_name;
    uint32_t table_initialized;
    uint32_t table_in_main_list;
    uint32_t table_size;
    uint32_t prop_name;
    uint32_t prop_type;
    uint32_t prop_data_table;
    uint32_t prop_offset;
    uint32_t prop_element_stride;
    uint32_t prop_elements;
    uint32_t prop_size;
};

constexpr SourceLayout kSourceX64{
    8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x00, 0x08, 0x10, 0x18, 0x20, 0x21,
    0x28, 0x00, 0x08, 0x40, 0x48, 0x4c, 0x50, 0x60,
};

constexpr SourceLayout kSourceX86{
    4, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x00, 0x04, 0x08, 0x0c, 0x10, 0x11,
    0x14, 0x00, 0x04, 0x28, 0x2c, 0x30, 0x34, 0x3c,
};

struct RecvTableInfo final {
    uint64_t address = 0;
    uint64_t props = 0;
    uint32_t prop_count = 0;
    uint64_t name_pointer = 0;
    std::string name;
};

struct ClientClassInfo final {
    uint64_t address = 0;
    uint64_t next = 0;
    uint64_t recv_table = 0;
    int32_t class_id = -1;
    std::string network_name;
};

struct RecvPropInfo final {
    uint64_t address = 0;
    uint64_t data_table = 0;
    int32_t type = -1;
    int32_t offset = 0;
    std::string name;
};

struct NetvarRecord final {
    int32_t class_id = -1;
    uint32_t offset = 0;
    int32_t type = -1;
    std::string class_name;
    std::string table_name;
    std::string path;
};

struct SelectedChain final {
    std::vector<uint64_t> addresses;
    bool truncated = false;
    bool head_referenced = false;
    bool ambiguous = false;
};

const ModuleInfo* find_client_module(const std::vector<ModuleInfo>& modules, bool& ambiguous) {
    ambiguous = false;
    for (std::string_view requested : {std::string_view("client.dll"),
                                       std::string_view("client_panorama.dll")}) {
        const ModuleInfo* selected = nullptr;
        for (const ModuleInfo& module : modules) {
            if (module.lower_name == requested) {
                if (selected != nullptr)
                    ambiguous = true;
                else
                    selected = &module;
            }
        }
        if (selected != nullptr)
            return selected;
    }
    return nullptr;
}

std::vector<size_t> source_scan_sections(const PeImage& image) {
    std::vector<size_t> sections;
    sections.reserve(image.sections.size());
    for (size_t index = 0; index < image.sections.size(); ++index) {
        const PeSection& section = image.sections[index];
        if ((section.characteristics & IMAGE_SCN_MEM_READ) != 0 &&
            (section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            sections.push_back(index);
        }
    }
    std::stable_sort(sections.begin(), sections.end(), [&image](size_t left, size_t right) {
        const PeSection& a = image.sections[left];
        const PeSection& b = image.sections[right];
        const auto priority = [](const PeSection& section) {
            if (section.name == ".rdata") {
                return 0;
            }
            if (section.name == ".data") {
                return 1;
            }
            if ((section.characteristics & IMAGE_SCN_MEM_WRITE) != 0) {
                return 2;
            }
            return 3;
        };
        const int left_priority = priority(a);
        const int right_priority = priority(b);
        return left_priority != right_priority ? left_priority < right_priority : a.base < b.base;
    });
    return sections;
}

template <typename Visitor>
bool visit_pointer_slots(ReadBudget& reader, const std::vector<CachedRange>& ranges,
                         uint8_t pointer_size, Visitor&& visitor) {
    uint64_t visited = 0;
    for (const CachedRange& range : ranges) {
        const uint64_t alignment = range.base & (pointer_size - 1u);
        size_t offset = static_cast<size_t>((pointer_size - alignment) & (pointer_size - 1u));
        while (offset + pointer_size <= range.bytes.size()) {
            if ((visited++ & 0xfffu) == 0 && !reader.checkpoint()) {
                return false;
            }
            uint64_t value = 0;
            if (pointer_size == 8) {
                std::memcpy(&value, range.bytes.data() + offset, sizeof(uint64_t));
            } else {
                uint32_t narrow = 0;
                std::memcpy(&narrow, range.bytes.data() + offset, sizeof(uint32_t));
                value = narrow;
            }
            uint64_t slot = 0;
            if (!checked_add(range.base, offset, slot) || !visitor(slot, value)) {
                return false;
            }
            offset += pointer_size;
        }
    }
    return reader.checkpoint();
}

bool find_dt_strings(ReadBudget& reader, MemoryView& memory, const ModuleInfo& module,
                     const std::vector<CachedRange>& ranges,
                     std::unordered_map<uint64_t, std::string>& strings,
                     std::string& error) {
    std::unordered_set<uint64_t> visited;
    uint64_t inspected = 0;
    for (const CachedRange& range : ranges) {
        for (size_t offset = 0; offset + 3 <= range.bytes.size(); ++offset) {
            if ((inspected++ & 0x3fffu) == 0 && !reader.checkpoint()) {
                return false;
            }
            const uint8_t* candidate = range.bytes.data() + offset;
            if (candidate[0] != 'D' || candidate[1] != 'T' || candidate[2] != '_') {
                continue;
            }
            if (offset != 0) {
                const unsigned char previous = range.bytes[offset - 1];
                if ((previous >= 'a' && previous <= 'z') ||
                    (previous >= 'A' && previous <= 'Z') ||
                    (previous >= '0' && previous <= '9') || previous == '_') {
                    continue;
                }
            }
            uint64_t address = 0;
            if (!checked_add(range.base, offset, address)) {
                continue;
            }
            if (visited.find(address) != visited.end())
                continue;
            if (visited.size() == kMaximumStringCandidateVisits) {
                error = "budget: DT_ string candidate visits exceeded 262,144";
                return false;
            }
            visited.insert(address);
            std::string name;
            if (!read_bounded_module_cstring(memory, module, address, 128, name) ||
                !valid_ascii_identifier(name, 127, true)) {
                if (reader.reason() != StopReason::None) {
                    return false;
                }
                continue;
            }
            if (strings.size() == kMaximumDtStrings) {
                error = "budget: DT_ string candidate cap exceeded 8,192";
                return false;
            }
            strings.emplace(address, std::move(name));
        }
    }
    return reader.checkpoint();
}

bool validate_recv_table(MemoryView& memory, const ModuleInfo& module,
                         const SourceLayout& layout,
                         const std::unordered_map<uint64_t, std::string>& dt_strings,
                         uint64_t address, RecvTableInfo& table) {
    if (!module_contains(module, address, layout.table_size)) {
        return false;
    }
    uint64_t props = 0;
    uint64_t name_pointer = 0;
    int32_t prop_count = 0;
    uint8_t initialized = 0;
    uint8_t in_main_list = 0;
    uint64_t field_address = 0;
    if (!checked_add(address, layout.table_props, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, props) ||
        !checked_add(address, layout.table_prop_count, field_address) ||
        !memory.read_value(field_address, prop_count) ||
        !checked_add(address, layout.table_name, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, name_pointer) ||
        !checked_add(address, layout.table_initialized, field_address) ||
        !memory.read_value(field_address, initialized) ||
        !checked_add(address, layout.table_in_main_list, field_address) ||
        !memory.read_value(field_address, in_main_list)) {
        return false;
    }
    const auto name = dt_strings.find(name_pointer);
    if (name == dt_strings.end() || prop_count < 0 || prop_count > 500 || initialized > 1 ||
        in_main_list > 1) {
        return false;
    }
    if (prop_count != 0) {
        uint64_t prop_bytes = 0;
        if (!valid_pointer(props, layout.pointer_size) ||
            !checked_mul(static_cast<uint32_t>(prop_count), layout.prop_size, prop_bytes) ||
            prop_bytes > (std::numeric_limits<size_t>::max)() ||
            !module_contains(module, props, static_cast<size_t>(prop_bytes))) {
            return false;
        }
        uint64_t first_name_address = 0;
        uint64_t first_name = 0;
        int32_t first_type = -1;
        if (!checked_add(props, layout.prop_name, first_name_address) ||
            !memory.read_pointer(first_name_address, layout.pointer_size, first_name) ||
            !checked_add(props, layout.prop_type, first_name_address) ||
            !memory.read_value(first_name_address, first_type) || first_type < 0 || first_type > 7 ||
            !valid_pointer_target(first_name, layout.pointer_size)) {
            return false;
        }
        std::string first_prop_name;
        if (!read_bounded_module_cstring(memory, module, first_name, 128, first_prop_name) ||
            !valid_ascii_identifier(first_prop_name, 127)) {
            return false;
        }
    } else if (props != 0 && !valid_pointer(props, layout.pointer_size)) {
        return false;
    }
    table.address = address;
    table.props = props;
    table.prop_count = static_cast<uint32_t>(prop_count);
    table.name_pointer = name_pointer;
    table.name = name->second;
    return true;
}

bool discover_recv_tables(ReadBudget& reader, MemoryView& memory,
                          const std::vector<CachedRange>& ranges, const ModuleInfo& module,
                          const SourceLayout& layout,
                          const std::unordered_map<uint64_t, std::string>& dt_strings,
                          std::unordered_map<uint64_t, RecvTableInfo>& tables,
                          std::string& error) {
    std::unordered_set<uint64_t> visited;
    return visit_pointer_slots(
        reader, ranges, layout.pointer_size, [&](uint64_t slot, uint64_t value) {
            if (dt_strings.find(value) == dt_strings.end()) {
                return true;
            }
            uint64_t address = 0;
            if (!checked_sub(slot, layout.table_name, address)) {
                return true;
            }
            if (visited.find(address) != visited.end())
                return true;
            if (visited.size() == kMaximumRecvTableCandidateVisits) {
                error = "budget: RecvTable candidate visits exceeded 262,144";
                return false;
            }
            visited.insert(address);
            RecvTableInfo table;
            if (!validate_recv_table(memory, module, layout, dt_strings, address, table)) {
                return reader.reason() == StopReason::None;
            }
            if (tables.size() == kMaximumRecvTables) {
                error = "budget: RecvTable candidate cap exceeded 8,192";
                return false;
            }
            tables.emplace(address, std::move(table));
            return true;
        });
}

bool validate_client_class(MemoryView& memory, const ModuleInfo& module, const PeImage& image,
                           const SourceLayout& layout,
                           const std::unordered_map<uint64_t, RecvTableInfo>& tables,
                           uint64_t address, ClientClassInfo& client_class) {
    if (!module_contains(module, address, layout.client_size)) {
        return false;
    }
    uint64_t create_fn = 0;
    uint64_t create_event_fn = 0;
    uint64_t network_name_pointer = 0;
    uint64_t recv_table = 0;
    uint64_t next = 0;
    int32_t class_id = -1;
    uint64_t field_address = address;
    if (!memory.read_pointer(field_address, layout.pointer_size, create_fn) ||
        !checked_add(address, layout.pointer_size, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, create_event_fn) ||
        !checked_add(address, layout.client_network_name, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, network_name_pointer) ||
        !checked_add(address, layout.client_recv_table, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, recv_table) ||
        !checked_add(address, layout.client_next, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, next) ||
        !checked_add(address, layout.client_class_id, field_address) ||
        !memory.read_value(field_address, class_id)) {
        return false;
    }
    if (tables.find(recv_table) == tables.end() ||
        find_section(image, create_fn, IMAGE_SCN_MEM_EXECUTE) == nullptr ||
        (create_event_fn != 0 &&
         find_section(image, create_event_fn, IMAGE_SCN_MEM_EXECUTE) == nullptr) ||
        !valid_pointer_target(network_name_pointer, layout.pointer_size) ||
        (next != 0 && (!valid_pointer(next, layout.pointer_size) ||
                       !module_contains(module, next, layout.client_size))) ||
        next == address || class_id < -1 || class_id > 1000000) {
        return false;
    }
    std::string network_name;
    if (!read_bounded_module_cstring(memory, module, network_name_pointer, 128,
                                     network_name) ||
        !valid_ascii_identifier(network_name, 127)) {
        return false;
    }
    client_class.address = address;
    client_class.next = next;
    client_class.recv_table = recv_table;
    client_class.class_id = class_id;
    client_class.network_name = std::move(network_name);
    return true;
}

bool discover_client_classes(
    ReadBudget& reader, MemoryView& memory, const std::vector<CachedRange>& ranges,
    const ModuleInfo& module, const PeImage& image, const SourceLayout& layout,
    const std::unordered_map<uint64_t, RecvTableInfo>& tables,
    std::unordered_map<uint64_t, ClientClassInfo>& classes, std::string& error) {
    std::unordered_set<uint64_t> visited;
    return visit_pointer_slots(
        reader, ranges, layout.pointer_size, [&](uint64_t slot, uint64_t value) {
            if (tables.find(value) == tables.end()) {
                return true;
            }
            uint64_t address = 0;
            if (!checked_sub(slot, layout.client_recv_table, address)) {
                return true;
            }
            if (visited.find(address) != visited.end())
                return true;
            if (visited.size() == kMaximumClientClassCandidateVisits) {
                error = "budget: ClientClass candidate visits exceeded 262,144";
                return false;
            }
            visited.insert(address);
            ClientClassInfo client_class;
            if (!validate_client_class(memory, module, image, layout, tables, address,
                                       client_class)) {
                return reader.reason() == StopReason::None;
            }
            if (classes.size() == kMaximumClientClassCandidates) {
                error = "budget: ClientClass candidate cap exceeded 4,096";
                return false;
            }
            classes.emplace(address, std::move(client_class));
            return true;
        });
}

bool choose_client_class_chain(ReadBudget& reader, const std::vector<CachedRange>& ranges,
                               const SourceLayout& layout,
                               const std::unordered_map<uint64_t, ClientClassInfo>& classes,
                               uint32_t class_cap, SelectedChain& selected) {
    std::unordered_set<uint64_t> incoming;
    for (const auto& [address, client_class] : classes) {
        (void)address;
        if (client_class.next != 0 && classes.find(client_class.next) != classes.end()) {
            incoming.insert(client_class.next);
        }
    }

    std::unordered_map<uint64_t, uint32_t> reference_counts;
    if (!visit_pointer_slots(reader, ranges, layout.pointer_size,
                             [&](uint64_t, uint64_t value) {
                                 if (classes.find(value) != classes.end()) {
                                     ++reference_counts[value];
                                 }
                                 return true;
                             })) {
        return false;
    }

    std::vector<uint64_t> roots;
    for (const auto& [address, client_class] : classes) {
        (void)client_class;
        if (incoming.find(address) == incoming.end()) {
            roots.push_back(address);
        }
    }
    std::sort(roots.begin(), roots.end());

    const size_t minimum_chain = class_cap == 1 ? 1 : 2;
    for (const uint64_t root : roots) {
        std::unordered_set<uint64_t> visited;
        std::vector<uint64_t> chain;
        bool complete = false;
        bool truncated = false;
        const bool head_referenced = reference_counts[root] != 0;
        if (!head_referenced) {
            continue;
        }
        uint64_t cursor = root;
        while (cursor != 0) {
            if (!reader.checkpoint()) {
                return false;
            }
            if (!visited.insert(cursor).second) {
                chain.clear();
                break;
            }
            const auto candidate = classes.find(cursor);
            if (candidate == classes.end()) {
                chain.clear();
                break;
            }
            if (chain.size() < class_cap) {
                chain.push_back(cursor);
            } else {
                truncated = true;
            }
            cursor = candidate->second.next;
        }
        if (!chain.empty() && cursor == 0) {
            complete = true;
        }
        if (!complete || chain.size() < minimum_chain) {
            continue;
        }
        const bool better =
            chain.size() > selected.addresses.size() ||
            (chain.size() == selected.addresses.size() && !truncated && selected.truncated) ||
            (chain.size() == selected.addresses.size() && truncated == selected.truncated &&
             head_referenced && !selected.head_referenced);
        const bool equally_ranked =
            !selected.addresses.empty() && chain.size() == selected.addresses.size() &&
            truncated == selected.truncated && head_referenced == selected.head_referenced;
        if (better) {
            selected.addresses = std::move(chain);
            selected.truncated = truncated;
            selected.head_referenced = head_referenced;
            selected.ambiguous = false;
        } else if (equally_ranked && chain != selected.addresses) {
            selected.ambiguous = true;
        }
    }
    return reader.checkpoint();
}

bool read_recv_prop(MemoryView& memory, const ModuleInfo& module, const SourceLayout& layout,
                    uint64_t address, RecvPropInfo& prop) {
    if (!module_contains(module, address, layout.prop_size)) {
        return false;
    }
    uint64_t name_pointer = 0;
    uint64_t data_table = 0;
    int32_t type = -1;
    int32_t offset = 0;
    int32_t element_stride = 0;
    int32_t elements = 0;
    uint64_t field_address = 0;
    if (!checked_add(address, layout.prop_name, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, name_pointer) ||
        !checked_add(address, layout.prop_type, field_address) ||
        !memory.read_value(field_address, type) ||
        !checked_add(address, layout.prop_data_table, field_address) ||
        !memory.read_pointer(field_address, layout.pointer_size, data_table) ||
        !checked_add(address, layout.prop_offset, field_address) ||
        !memory.read_value(field_address, offset) ||
        !checked_add(address, layout.prop_element_stride, field_address) ||
        !memory.read_value(field_address, element_stride) ||
        !checked_add(address, layout.prop_elements, field_address) ||
        !memory.read_value(field_address, elements)) {
        return false;
    }
    if (!valid_pointer_target(name_pointer, layout.pointer_size) || type < 0 || type > 7 || offset < 0 ||
        offset > 0x1000000 || element_stride < 0 || element_stride > 0x100000 || elements < 0 ||
        elements > 4096 ||
        (data_table != 0 && !valid_pointer(data_table, layout.pointer_size))) {
        return false;
    }
    std::string name;
    if (!read_bounded_module_cstring(memory, module, name_pointer, 128, name) ||
        !valid_ascii_identifier(name, 127)) {
        return false;
    }
    prop.address = address;
    prop.data_table = data_table;
    prop.type = type;
    prop.offset = offset;
    prop.name = std::move(name);
    return true;
}

bool push_netvar(std::vector<NetvarRecord>& records, const ClientClassInfo& client_class,
                 const RecvTableInfo& table, const RecvPropInfo& prop, std::string path,
                 uint32_t offset, std::string& error) {
    if (records.size() == kMaximumNetvars) {
        error = "budget: netvar record cap exceeded 250,000";
        return false;
    }
    records.push_back(NetvarRecord{client_class.class_id, offset, prop.type,
                                   client_class.network_name, table.name, std::move(path)});
    return true;
}

bool enumerate_recv_table(
    ReadBudget& reader, MemoryView& memory, const ModuleInfo& module, const SourceLayout& layout,
    const std::unordered_map<uint64_t, std::string>& dt_strings,
    const std::unordered_map<uint64_t, RecvTableInfo>& tables,
    const ClientClassInfo& client_class, const RecvTableInfo& root_table,
    const RecvTableInfo& table, std::string_view path_prefix, uint64_t base_offset,
    size_t depth, std::unordered_set<uint64_t>& active_tables,
    std::unordered_set<uint64_t>& visited_tables, size_t& prop_visits,
    std::vector<NetvarRecord>& records, std::string& error) {
    if (depth >= kMaximumRecvTableDepth) {
        error = "budget: RecvTable traversal depth exceeded 64";
        return false;
    }
    if (path_prefix.size() > kMaximumNetvarPathBytes) {
        error = "budget: netvar path exceeds 4,096 bytes";
        return false;
    }
    if (visited_tables.find(table.address) == visited_tables.end()) {
        if (visited_tables.size() == kMaximumRecvTables) {
            error = "budget: RecvTable traversal visits exceeded 8,192";
            return false;
        }
        visited_tables.insert(table.address);
    }
    if (!active_tables.insert(table.address).second) {
        error = "capability: RecvTable traversal cycle detected";
        return false;
    }
    struct ActiveTableGuard final {
        std::unordered_set<uint64_t>& active;
        uint64_t address;
        ~ActiveTableGuard() { active.erase(address); }
    } active_guard{active_tables, table.address};

    for (uint32_t index = 0; index < table.prop_count; ++index) {
        if (!reader.checkpoint()) {
            set_stop_error(reader, error);
            return false;
        }
        if (prop_visits == kMaximumRecvPropVisits) {
            error = "budget: RecvProp traversal visits exceeded 250,000";
            return false;
        }
        ++prop_visits;

        uint64_t prop_delta = 0;
        uint64_t prop_address = 0;
        if (!checked_mul(index, layout.prop_size, prop_delta) ||
            !checked_add(table.props, prop_delta, prop_address)) {
            error = "capability: RecvProp address arithmetic overflow";
            return false;
        }
        RecvPropInfo prop;
        if (!read_recv_prop(memory, module, layout, prop_address, prop)) {
            if (!set_stop_error(reader, error))
                error = "capability: validated RecvTable contains an invalid RecvProp layout";
            return false;
        }

        uint64_t combined_offset = 0;
        if (!checked_add(base_offset, static_cast<uint32_t>(prop.offset), combined_offset) ||
            combined_offset > (std::numeric_limits<uint32_t>::max)()) {
            error = "capability: nested netvar offset arithmetic overflow";
            return false;
        }
        std::string path(path_prefix);
        if (prop.name.size() > kMaximumNetvarPathBytes - path.size()) {
            error = "budget: netvar path exceeds 4,096 bytes";
            return false;
        }
        path.append(prop.name);
        if (prop.name != "baseclass" &&
            !push_netvar(records, client_class, root_table, prop, path,
                         static_cast<uint32_t>(combined_offset), error)) {
            return false;
        }
        if (prop.type != 6 || prop.data_table == 0)
            continue;

        RecvTableInfo child;
        const auto known_child = tables.find(prop.data_table);
        if (known_child != tables.end()) {
            child = known_child->second;
        } else if (!validate_recv_table(memory, module, layout, dt_strings, prop.data_table,
                                        child)) {
            if (!set_stop_error(reader, error))
                error = "capability: DataTable RecvProp points to an invalid child table";
            return false;
        }
        if (active_tables.find(child.address) != active_tables.end()) {
            error = "capability: RecvTable traversal cycle detected";
            return false;
        }

        std::string child_prefix(path_prefix);
        if (prop.name != "baseclass") {
            if (path.size() == kMaximumNetvarPathBytes) {
                error = "budget: netvar path exceeds 4,096 bytes";
                return false;
            }
            child_prefix = std::move(path);
            child_prefix.push_back('.');
        }
        if (!enumerate_recv_table(reader, memory, module, layout, dt_strings, tables,
                                  client_class, root_table, child, child_prefix,
                                  combined_offset, depth + 1, active_tables, visited_tables,
                                  prop_visits, records, error)) {
            return false;
        }
    }
    return true;
}

bool enumerate_netvars(
    ReadBudget& reader, MemoryView& memory, const ModuleInfo& module, const SourceLayout& layout,
    const std::unordered_map<uint64_t, std::string>& dt_strings,
    const std::unordered_map<uint64_t, RecvTableInfo>& tables,
    const std::unordered_map<uint64_t, ClientClassInfo>& classes, const SelectedChain& chain,
    std::vector<NetvarRecord>& records, std::unordered_set<uint64_t>& visited_tables,
    std::string& error) {
    size_t prop_visits = 0;
    for (const uint64_t class_address : chain.addresses) {
        if (!reader.checkpoint()) {
            set_stop_error(reader, error);
            return false;
        }
        const ClientClassInfo& client_class = classes.at(class_address);
        const RecvTableInfo& table = tables.at(client_class.recv_table);
        std::unordered_set<uint64_t> active_tables;
        if (!enumerate_recv_table(reader, memory, module, layout, dt_strings, tables,
                                  client_class, table, table, {}, 0, 0, active_tables,
                                  visited_tables, prop_visits, records, error)) {
            return false;
        }
    }
    return true;
}

std::string recv_type_name(int32_t type) {
    static constexpr std::string_view names[] = {
        "Int", "Float", "Vector", "VectorXY", "String", "Array", "DataTable", "Int64",
    };
    return type >= 0 && type < static_cast<int32_t>(std::size(names))
               ? std::string(names[type])
               : std::string("Unknown");
}

bool build_source_output(const DumpConfig& config, const ModuleInfo& module,
                         const SourceLayout& layout,
                         const std::unordered_map<uint64_t, ClientClassInfo>& classes,
                         const std::unordered_map<uint64_t, RecvTableInfo>& tables,
                         const SelectedChain& chain, std::vector<NetvarRecord> records,
                         size_t visited_table_count, uint64_t bytes_read, std::string& output,
                         std::string& error) {
    std::vector<ClientClassInfo> ordered_classes;
    ordered_classes.reserve(chain.addresses.size());
    for (const uint64_t address : chain.addresses) {
        ordered_classes.push_back(classes.at(address));
    }
    std::sort(ordered_classes.begin(), ordered_classes.end(),
              [](const ClientClassInfo& left, const ClientClassInfo& right) {
                  if (left.class_id != right.class_id) {
                      return left.class_id < right.class_id;
                  }
                  if (left.network_name != right.network_name) {
                      return left.network_name < right.network_name;
                  }
                  return left.address < right.address;
              });
    std::sort(records.begin(), records.end(), [](const NetvarRecord& left, const NetvarRecord& right) {
        if (left.table_name != right.table_name) {
            return left.table_name < right.table_name;
        }
        if (left.path != right.path) {
            return left.path < right.path;
        }
        if (left.offset != right.offset) {
            return left.offset < right.offset;
        }
        if (left.class_name != right.class_name) {
            return left.class_name < right.class_name;
        }
        return left.type < right.type;
    });
    records.erase(std::unique(records.begin(), records.end(),
                              [](const NetvarRecord& left, const NetvarRecord& right) {
                                  return left.class_id == right.class_id &&
                                         left.offset == right.offset && left.type == right.type &&
                                         left.class_name == right.class_name &&
                                         left.table_name == right.table_name && left.path == right.path;
                              }),
                  records.end());

    const size_t address_digits = layout.pointer_size * 2u;
    if (!append_output(output, "source-netvars-v1", error) ||
        !append_output(output, "meta-v1\ttarget-pid\t" + decimal(config.target_pid), error) ||
        !append_output(output, "meta-v1\tmodule\t" + escaped_field(module.name), error) ||
        !append_output(output,
                       "meta-v1\tmodule-base\t" + hexadecimal(module.base, address_digits), error) ||
        !append_output(output,
                       "meta-v1\tpointer-size\t" + decimal(layout.pointer_size), error) ||
        !append_output(output,
                       "meta-v1\tclient-class-head\t" +
                           hexadecimal(chain.addresses.front(), address_digits),
                       error) ||
        !append_output(output,
                       "meta-v1\tclass-count\t" + decimal(ordered_classes.size()), error) ||
        !append_output(output,
                       "meta-v1\trecv-table-count\t" + decimal(visited_table_count), error) ||
        !append_output(output, "meta-v1\tnetvar-count\t" + decimal(records.size()), error) ||
        !append_output(output, "meta-v1\tread-bytes\t" + decimal(bytes_read), error) ||
        !append_output(output,
                       std::string("meta-v1\ttruncated\t") +
                           (chain.truncated ? "max-classes" : "none"),
                       error) ||
        !append_output(output,
                       std::string("meta-v1\thead-reference\t") +
                           (chain.head_referenced ? "present" : "derived-from-chain"),
                       error)) {
        return false;
    }

    for (const ClientClassInfo& client_class : ordered_classes) {
        const RecvTableInfo& table = tables.at(client_class.recv_table);
        const std::string line =
            "class-v1\t" + signed_decimal(client_class.class_id) + "\t" +
            escaped_field(client_class.network_name) + "\t" + escaped_field(table.name) + "\t" +
            hexadecimal(client_class.address, address_digits) + "\t" +
            hexadecimal(table.address, address_digits);
        if (!append_output(output, line, error)) {
            return false;
        }
    }
    for (const NetvarRecord& record : records) {
        const std::string line =
            "netvar-v1\t" + escaped_field(record.table_name) + "\t" +
            escaped_field(record.path) + "\t" + hexadecimal(record.offset, 8) + "\t" +
            recv_type_name(record.type) + "\t" + escaped_field(record.class_name);
        if (!append_output(output, line, error)) {
            return false;
        }
    }
    return true;
}

class SourceDumper final : public DumperBase {
public:
    DumpKind kind() const override { return DumpKind::Source; }

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
                                     L"Source-netvars.dt", workspace_boundary,
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
            bool module_ambiguous = false;
            const ModuleInfo* module = find_client_module(modules, module_ambiguous);
            if (module_ambiguous) {
                result.error_message =
                    "capability: multiple equally authoritative Source client modules were found";
                return result;
            }
            if (module == nullptr) {
                result.error_message = "not-found: client.dll or client_panorama.dll is not loaded";
                return result;
            }

            PeImage image;
            if (!parse_pe_image(reader, *module, image, result.error_message)) {
                return result;
            }
            const SourceLayout& layout = image.pointer_size == 8 ? kSourceX64 : kSourceX86;
            std::vector<CachedRange> ranges;
            bool incomplete_scan = false;
            if (!cache_sections(reader, image, source_scan_sections(image), kModuleScanLimit, ranges,
                                incomplete_scan)) {
                set_stop_error(reader, result.error_message);
                return result;
            }
            if (incomplete_scan) {
                result.error_message =
                    "budget: bounded client-module scan did not cover every selected data section";
                return result;
            }
            if (ranges.empty()) {
                result.error_message = "capability: client module has no readable data sections";
                return result;
            }
            MemoryView memory(reader, ranges);

            std::unordered_map<uint64_t, std::string> dt_strings;
            if (!find_dt_strings(reader, memory, *module, ranges, dt_strings,
                                 result.error_message)) {
                if (result.error_message.empty()) {
                    set_stop_error(reader, result.error_message);
                }
                return result;
            }
            if (dt_strings.empty()) {
                result.error_message = incomplete_scan
                                           ? "budget: bounded client-module scan did not cover all data sections"
                                           : "not-found: no bounded valid DT_ strings were found";
                return result;
            }

            std::unordered_map<uint64_t, RecvTableInfo> tables;
            if (!discover_recv_tables(reader, memory, ranges, *module, layout, dt_strings, tables,
                                      result.error_message)) {
                if (result.error_message.empty()) {
                    set_stop_error(reader, result.error_message);
                }
                return result;
            }
            if (tables.empty()) {
                result.error_message = incomplete_scan
                                           ? "budget: bounded data scan did not validate a RecvTable layout"
                                           : "capability: DT_ pointer references did not validate a RecvTable layout";
                return result;
            }

            std::unordered_map<uint64_t, ClientClassInfo> classes;
            if (!discover_client_classes(reader, memory, ranges, *module, image, layout, tables,
                                         classes, result.error_message)) {
                if (result.error_message.empty()) {
                    set_stop_error(reader, result.error_message);
                }
                return result;
            }
            if (classes.empty()) {
                result.error_message = incomplete_scan
                                           ? "budget: bounded data scan did not validate ClientClass nodes"
                                           : "capability: RecvTable pointer references did not validate ClientClass nodes";
                return result;
            }

            const uint32_t class_cap =
                (std::min)({config.max_classes, uint32_t{2000}, uint32_t{10000}});
            SelectedChain chain;
            if (!choose_client_class_chain(reader, ranges, layout, classes, class_cap, chain)) {
                set_stop_error(reader, result.error_message);
                return result;
            }
            if (chain.addresses.empty()) {
                result.error_message =
                    "capability: no acyclic referenced ClientClass chain passed bounded validation";
                return result;
            }
            if (chain.ambiguous) {
                result.error_message =
                    "capability: multiple equally plausible ClientClass chains were found";
                return result;
            }

            std::vector<NetvarRecord> records;
            std::unordered_set<uint64_t> visited_tables;
            if (!enumerate_netvars(reader, memory, *module, layout, dt_strings, tables, classes,
                                  chain, records, visited_tables, result.error_message)) {
                return result;
            }
            if (records.empty()) {
                result.error_message = "capability: validated ClientClass chain contains no netvars";
                return result;
            }
            if (!reader.checkpoint()) {
                set_stop_error(reader, result.error_message);
                return result;
            }

            std::string output;
            output.reserve((std::min)(kMaximumOutputBytes, records.size() * size_t{96} + 4096));
            if (!build_source_output(config, *module, layout, classes, tables, chain,
                                     std::move(records), visited_tables.size(), reader.bytes_used(),
                                     output, result.error_message)) {
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
            result.class_count = static_cast<uint32_t>(chain.addresses.size());
            return result;
        } catch (const std::bad_alloc&) {
            result.error_message = "budget: allocation failed within bounded Source dump";
            return result;
        } catch (...) {
            result.error_message = "capability: unexpected Source dump failure";
            return result;
        }
    }
};

} // namespace

DumperBase* create_source() { return new SourceDumper(); }

} // namespace sao::ai_editor::sdk_dumper
