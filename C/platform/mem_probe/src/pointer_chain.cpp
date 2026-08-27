// pointer_chain.cpp — BFS pointer-chain backtrace + resolve.

#include "sao/mem_probe/pointer_chain.h"

#include "sao/core/memory.h"
#include "sao/core/time.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000
#endif
#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS 0x01
#endif
#ifndef PAGE_GUARD
#define PAGE_GUARD 0x100
#endif

namespace {

constexpr uint32_t kDefaultMaxDepth = 3;
constexpr uint32_t kDefaultMaxPtrPerLevel = 8192;
constexpr uint64_t kDefaultScanBudget = 256ULL * 1024 * 1024;
constexpr uint32_t kDefaultReadBudget = 20000u;
constexpr uint32_t kDefaultTimeBudgetMs = 1000u;
constexpr size_t kChunkBytes = 1u << 20;
constexpr size_t kMaxRegions = 8192;
constexpr uint32_t kMaxRepresentableDepth = 16;
constexpr uint64_t kMaxNodes = 1'000'000;
constexpr int32_t kObjectBackDeltas[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,
                                         0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88,
                                         0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0,
                                         0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100};

struct ModuleRange {
    std::string name;
    uint64_t base = 0;
    uint64_t end = 0;
};

bool add_u64_checked(uint64_t left, uint64_t right, uint64_t* out) {
    if (out == nullptr || left > std::numeric_limits<uint64_t>::max() - right)
        return false;
    *out = left + right;
    return true;
}

bool add_signed_checked(uint64_t base, int64_t delta, uint64_t* out) {
    if (out == nullptr)
        return false;
    if (delta >= 0)
        return add_u64_checked(base, static_cast<uint64_t>(delta), out);
    const uint64_t magnitude = static_cast<uint64_t>(-(delta + 1)) + 1;
    if (base < magnitude)
        return false;
    *out = base - magnitude;
    return true;
}

bool region_scannable(const SaoMemRegion& r) {
    return (r.state & MEM_COMMIT) != 0 && (r.protect & PAGE_GUARD) == 0 && r.protect != 0 &&
           (r.protect & PAGE_NOACCESS) == 0;
}

std::vector<ModuleRange> enumerate_modules_or_empty(sao_core_process_handle_t p) {
    std::vector<ModuleRange> out;
    if (p == nullptr)
        return out;
    SaoModuleEntry entries[512];
    char names[65536];
    size_t entry_count = 0;
    size_t names_used = 0;
    if (sao_core_process_enum_modules(p, entries, 512, &entry_count, names, sizeof(names),
                                      &names_used) != SAO_STATUS_OK)
        return out;
    out.reserve(entry_count);
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].name_offset >= names_used)
            continue;
        const size_t remaining = names_used - entries[i].name_offset;
        const void* terminator = std::memchr(names + entries[i].name_offset, '\0', remaining);
        if (terminator == nullptr)
            continue;
        uint64_t end = 0;
        if (!add_u64_checked(entries[i].base_address, entries[i].module_size, &end) ||
            end <= entries[i].base_address)
            continue;
        ModuleRange r;
        r.base = entries[i].base_address;
        r.end = end;
        r.name.assign(names + entries[i].name_offset,
                      static_cast<const char*>(terminator) - (names + entries[i].name_offset));
        if (r.name.empty() || r.name.size() >= 128)
            continue;
        out.push_back(std::move(r));
    }
    return out;
}

const ModuleRange* address_in_module(uint64_t addr, const std::vector<ModuleRange>& mods) {
    for (const auto& m : mods) {
        if (addr >= m.base && addr < m.end)
            return &m;
    }
    return nullptr;
}

struct ProbeBudget {
    uint64_t bytes_remaining;
    uint32_t reads_remaining;
    uint64_t deadline_ms;
    bool exhausted = false;
    ProbeBudget(uint64_t bytes, uint32_t reads, uint32_t duration_ms)
        : bytes_remaining(bytes), reads_remaining(reads),
          deadline_ms(sao_core_time_now_ms() + duration_ms) {}
    bool reserve(size_t bytes) {
        if (sao_core_time_now_ms() >= deadline_ms || reads_remaining == 0 ||
            bytes > bytes_remaining) {
            exhausted = true;
            return false;
        }
        bytes_remaining -= bytes;
        --reads_remaining;
        return true;
    }
    bool active() {
        if (sao_core_time_now_ms() >= deadline_ms)
            exhausted = true;
        return !exhausted;
    }
};
bool static_data_section_contains(sao_core_process_handle_t process, const ModuleRange& module,
                                  uint64_t address, ProbeBudget& budget) {
    std::array<uint8_t, 0x1000> header{};
    if (!budget.reserve(header.size()))
        return false;
    size_t got = 0;
    if (sao_core_mem_read(process, module.base, header.data(), header.size(), &got) !=
            SAO_STATUS_OK ||
        got < 0x40)
        return false;
    uint32_t pe_offset = 0;
    std::memcpy(&pe_offset, header.data() + 0x3C, sizeof(pe_offset));
    if (pe_offset > header.size() - 0x18 || header[0] != 'M' || header[1] != 'Z' ||
        header[pe_offset] != 'P' || header[pe_offset + 1] != 'E' || header[pe_offset + 2] != 0 ||
        header[pe_offset + 3] != 0)
        return false;
    uint16_t section_count = 0;
    uint16_t optional_size = 0;
    std::memcpy(&section_count, header.data() + pe_offset + 6, sizeof(section_count));
    std::memcpy(&optional_size, header.data() + pe_offset + 20, sizeof(optional_size));
    if (section_count == 0 || section_count > 96 || optional_size > 0x400)
        return false;
    const size_t table_offset = static_cast<size_t>(pe_offset) + 24u + optional_size;
    const size_t table_size = static_cast<size_t>(section_count) * 40u;
    if (table_offset > header.size() || table_size > header.size() - table_offset)
        return false;
    for (uint16_t index = 0; index < section_count; ++index) {
        const uint8_t* section = header.data() + table_offset + index * 40u;
        uint32_t virtual_size = 0;
        uint32_t virtual_address = 0;
        uint32_t raw_size = 0;
        uint32_t characteristics = 0;
        std::memcpy(&virtual_size, section + 8, sizeof(virtual_size));
        std::memcpy(&virtual_address, section + 12, sizeof(virtual_address));
        std::memcpy(&raw_size, section + 16, sizeof(raw_size));
        std::memcpy(&characteristics, section + 36, sizeof(characteristics));
        if ((characteristics & (0x40u | 0x80u)) == 0 || (characteristics & 0x20000000u) != 0)
            continue;
        const uint64_t span = std::max<uint32_t>(virtual_size, raw_size);
        if (span == 0 || virtual_address > module.end - module.base ||
            span > module.end - module.base - virtual_address)
            continue;
        const uint64_t start = module.base + virtual_address;
        if (address >= start && address < start + span)
            return true;
    }
    return false;
}
struct PointerHit {
    uint64_t source = 0;
    uint64_t target = 0;
};

sao_status_t scan_pointer_values(sao_core_process_handle_t process,
                                 const std::vector<uint64_t>& needles, uint32_t max_hits,
                                 ProbeBudget& budget, std::vector<PointerHit>& hits) {
    if (max_hits == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (needles.empty())
        return SAO_STATUS_OK;
    const std::unordered_set<uint64_t> needle_set(needles.begin(), needles.end());
    std::vector<SaoMemRegion> regions(kMaxRegions);
    size_t region_count = 0;
    if (sao_core_mem_enum_regions(process, regions.data(), regions.size(), &region_count) !=
        SAO_STATUS_OK)
        return SAO_STATUS_ERR_READ_FAULT;
    regions.resize(region_count);
    std::vector<uint8_t> chunk(kChunkBytes);
    hits.clear();
    for (const auto& region : regions) {
        uint64_t region_end = 0;
        if (!add_u64_checked(region.base_address, region.region_size, &region_end))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        (void)region_end;
        if (!region_scannable(region))
            continue;
        uint64_t addr = region.base_address;
        uint64_t remaining = region.region_size;
        std::array<uint8_t, sizeof(uint64_t) - 1> carry{};
        size_t carry_len = 0;
        while (remaining > 0) {
            if (!budget.active())
                return SAO_STATUS_ERR_TIMEOUT;
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkBytes));
            if (!budget.reserve(want))
                return SAO_STATUS_ERR_TIMEOUT;
            size_t got = 0;
            const sao_status_t read_status =
                sao_core_mem_read(process, addr, chunk.data(), want, &got);
            if (got > want)
                return SAO_STATUS_ERR_READ_FAULT;
            if (got == 0) {
                carry_len = 0;
                uint64_t next = 0;
                if (!add_u64_checked(addr, want, &next))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                addr = next;
                remaining -= want;
                continue;
            }
            if (addr < carry_len)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            std::vector<uint8_t> window(carry_len + got);
            if (carry_len != 0)
                std::memcpy(window.data(), carry.data(), carry_len);
            std::memcpy(window.data() + carry_len, chunk.data(), got);
            const uint64_t window_base = addr - static_cast<uint64_t>(carry_len);
            const size_t last =
                window.size() >= sizeof(uint64_t) ? window.size() - sizeof(uint64_t) : 0;
            const size_t remainder = static_cast<size_t>(window_base & 7ull);
            const size_t first = remainder == 0 ? 0 : 8 - remainder;
            for (size_t off = first; window.size() >= sizeof(uint64_t) && off <= last; off += 8) {
                uint64_t current = 0;
                std::memcpy(&current, window.data() + off, sizeof(current));
                if (needle_set.find(current) == needle_set.end())
                    continue;
                uint64_t hit_address = 0;
                if (!add_u64_checked(window_base, static_cast<uint64_t>(off), &hit_address))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                hits.push_back(PointerHit{hit_address, current});
                if (hits.size() >= max_hits)
                    return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
            carry_len = std::min<size_t>(sizeof(uint64_t) - 1, window.size());
            if (carry_len != 0)
                std::memcpy(carry.data(), window.data() + window.size() - carry_len, carry_len);
            uint64_t next = 0;
            if (!add_u64_checked(addr, got, &next))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            addr = next;
            remaining -= got;
            if (read_status != SAO_STATUS_OK && got < want)
                continue;
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t module_fingerprint(sao_core_process_handle_t process, const ModuleRange& module,
                                ProbeBudget& budget, uint64_t* out_fingerprint) {
    if (out_fingerprint == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_fingerprint = 0;
    constexpr size_t kFingerprintBytes = 64u * 1024u;
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
    };
    mix(reinterpret_cast<const uint8_t*>(module.name.data()), module.name.size());
    const uint64_t module_size = module.end - module.base;
    mix(reinterpret_cast<const uint8_t*>(&module_size), sizeof(module_size));
    const size_t sample_size =
        static_cast<size_t>(std::min<uint64_t>(module_size, kFingerprintBytes));
    if (!budget.reserve(sample_size))
        return SAO_STATUS_ERR_TIMEOUT;
    std::vector<uint8_t> sample(sample_size);
    size_t got = 0;
    const sao_status_t read_status =
        sample_size == 0
            ? SAO_STATUS_OK
            : sao_core_mem_read(process, module.base, sample.data(), sample.size(), &got);
    if (got > sample_size || (read_status != SAO_STATUS_OK && got == 0))
        return SAO_STATUS_ERR_READ_FAULT;
    mix(sample.data(), got);
    *out_fingerprint = hash;
    return SAO_STATUS_OK;
}

struct NodeKey {
    uint64_t address = 0;
    int32_t delta = 0;

    bool operator==(const NodeKey& other) const {
        return address == other.address && delta == other.delta;
    }
};

struct NodeKeyHash {
    size_t operator()(const NodeKey& key) const {
        uint64_t value = key.address + 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value ^= static_cast<uint64_t>(static_cast<uint32_t>(key.delta)) + 0x94D049BB133111EBull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return static_cast<size_t>(value ^ (value >> 31));
    }
};

bool valid_chain_shape(const sao_memprobe_ptr_chain_t& chain) {
    size_t name_len = 0;
    while (name_len < sizeof(chain.module_name_utf8) && chain.module_name_utf8[name_len] != '\0')
        ++name_len;
    return name_len > 0 && name_len < sizeof(chain.module_name_utf8) && chain.deref_count <= 16;
}

bool valid_chain_shape_v2(const sao_memprobe_ptr_chain_v2_t& chain) {
    size_t name_len = 0;
    while (name_len < sizeof(chain.module_name_utf8) && chain.module_name_utf8[name_len] != '\0')
        ++name_len;
    size_t provenance_len = 0;
    while (provenance_len < sizeof(chain.provenance_utf8) &&
           chain.provenance_utf8[provenance_len] != '\0')
        ++provenance_len;
    return name_len > 0 && name_len < sizeof(chain.module_name_utf8) && chain.deref_count <= 16 &&
           chain.struct_size == sizeof(sao_memprobe_ptr_chain_v2_t) &&
           chain.abi_version == SAO_MEMPROBE_PTR_CHAIN_V2_ABI_VERSION &&
           provenance_len < sizeof(chain.provenance_utf8) &&
           (chain.validation_state == SAO_MEMPROBE_PTR_CHAIN_VALIDATION_CANDIDATE ||
            chain.validation_state == SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED);
}

struct NormalizedBacktraceConfig {
    uint32_t max_depth = 0;
    uint32_t max_ptr_per_level = 0;
    uint64_t max_scan_bytes_budget = 0;
    uint32_t max_read_count = 0;
    uint32_t max_duration_ms = 0;
};

NormalizedBacktraceConfig backtrace_config_v1(
    const sao_memprobe_backtrace_config_t* source) noexcept {
    NormalizedBacktraceConfig target{};
    if (source != nullptr) {
        target.max_depth = source->max_depth;
        target.max_ptr_per_level = source->max_ptr_per_level;
        target.max_scan_bytes_budget = source->max_scan_bytes_budget;
    }
    return target;
}

bool backtrace_config_v2_to_internal(const sao_memprobe_backtrace_config_v2_t* source,
                                     NormalizedBacktraceConfig* target) {
    if (source == nullptr || target == nullptr ||
        source->struct_size < offsetof(sao_memprobe_backtrace_config_v2_t, _reserved) ||
        source->abi_version != SAO_MEMPROBE_BACKTRACE_CONFIG_V2_ABI_VERSION)
        return false;
    target->max_depth = source->max_depth;
    target->max_ptr_per_level = source->max_ptr_per_level;
    target->max_scan_bytes_budget = source->max_scan_bytes_budget;
    target->max_read_count = source->max_read_count;
    target->max_duration_ms = source->max_duration_ms;
    return true;
}

sao_status_t resolve_impl(sao_core_process_handle_t process,
                          const sao_memprobe_ptr_chain_v2_t& chain, bool require_v2,
                          uint64_t* out_final_addr, ProbeBudget& budget) {
    if (out_final_addr == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_final_addr = 0;
    if (process == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (require_v2) {
        if (!valid_chain_shape_v2(chain))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } else {
        sao_memprobe_ptr_chain_t v1{};
        std::memcpy(&v1, &chain, sizeof(v1));
        if (!valid_chain_shape(v1))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    const auto modules = enumerate_modules_or_empty(process);
    const ModuleRange* module = nullptr;
    for (const auto& candidate : modules) {
        if (candidate.name == chain.module_name_utf8) {
            module = &candidate;
            break;
        }
    }
    if (module == nullptr)
        return SAO_STATUS_ERR_MODULE_NOT_FOUND;
    if (require_v2 && chain.module_fingerprint != 0) {
        uint64_t fingerprint = 0;
        const sao_status_t fingerprint_status =
            module_fingerprint(process, *module, budget, &fingerprint);
        if (fingerprint_status != SAO_STATUS_OK || fingerprint != chain.module_fingerprint)
            return fingerprint_status == SAO_STATUS_OK ? SAO_STATUS_ERR_NOT_FOUND
                                                       : fingerprint_status;
    }
    if (chain.static_offset < 0 ||
        static_cast<uint64_t>(chain.static_offset) >= module->end - module->base)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    uint64_t addr = 0;
    if (!add_signed_checked(module->base, chain.static_offset, &addr) || addr < module->base ||
        addr >= module->end)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < chain.deref_count; ++i) {
        uint64_t next = 0;
        if (!budget.reserve(sizeof(next)))
            return SAO_STATUS_ERR_TIMEOUT;
        if (sao_core_mem_read_ptr(process, addr, &next) != SAO_STATUS_OK)
            return SAO_STATUS_ERR_READ_FAULT;
        if (!add_signed_checked(next, chain.deref_offsets[i], &addr))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    uint64_t root = 0;
    if (!budget.reserve(sizeof(root)))
        return SAO_STATUS_ERR_TIMEOUT;
    if (sao_core_mem_read_ptr(process, addr, &root) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_READ_FAULT;
    if (!add_signed_checked(root, chain.final_offset, &addr))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_final_addr = addr;
    return SAO_STATUS_OK;
}

sao_status_t backtrace_impl(sao_core_process_handle_t process, uint64_t target_addr,
                            const NormalizedBacktraceConfig* cfg,
                            sao_memprobe_ptr_chain_v2_t* out_chain) {
    if (out_chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_chain, 0, sizeof(*out_chain));
    try {
        if (process == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const uint32_t max_depth = (cfg && cfg->max_depth) ? cfg->max_depth : kDefaultMaxDepth;
        const uint32_t max_ptr_per_level =
            (cfg && cfg->max_ptr_per_level) ? cfg->max_ptr_per_level : kDefaultMaxPtrPerLevel;
        const uint64_t configured_budget =
            (cfg && cfg->max_scan_bytes_budget) ? cfg->max_scan_bytes_budget : kDefaultScanBudget;
        const uint32_t configured_reads =
            (cfg && cfg->max_read_count) ? cfg->max_read_count : kDefaultReadBudget;
        const uint32_t configured_duration =
            (cfg && cfg->max_duration_ms) ? cfg->max_duration_ms : kDefaultTimeBudgetMs;
        if (max_depth > kMaxRepresentableDepth || max_ptr_per_level == 0 ||
            max_ptr_per_level > kMaxNodes)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        constexpr uint64_t kRootCount = sizeof(kObjectBackDeltas) / sizeof(kObjectBackDeltas[0]);
        if (static_cast<uint64_t>(max_depth) >
            std::numeric_limits<uint64_t>::max() / max_ptr_per_level)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const uint64_t layer_budget = static_cast<uint64_t>(max_depth) * max_ptr_per_level;
        uint64_t max_nodes = 0;
        if (!add_u64_checked(kRootCount, layer_budget, &max_nodes) || max_nodes > kMaxNodes ||
            max_nodes > std::numeric_limits<size_t>::max())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        const auto modules = enumerate_modules_or_empty(process);
        struct Node {
            uint64_t address = 0;
            uint32_t parent_idx = UINT32_MAX;
            int32_t back_delta = 0;
        };
        std::vector<Node> all_nodes;
        all_nodes.reserve(static_cast<size_t>(max_nodes));
        for (int32_t delta : kObjectBackDeltas) {
            if (target_addr < static_cast<uint64_t>(delta))
                continue;
            all_nodes.push_back(
                Node{target_addr - static_cast<uint64_t>(delta), UINT32_MAX, delta});
        }
        if (all_nodes.empty())
            return SAO_STATUS_ERR_NOT_FOUND;

        std::unordered_set<NodeKey, NodeKeyHash> visited_nodes;
        visited_nodes.reserve(static_cast<size_t>(max_nodes));
        for (const Node& node : all_nodes)
            visited_nodes.insert(NodeKey{node.address, node.back_delta});

        uint32_t level_begin = 0;
        uint32_t level_end = static_cast<uint32_t>(all_nodes.size());
        ProbeBudget budget(configured_budget, configured_reads, configured_duration);
        sao_memprobe_ptr_chain_v2_t best_candidate{};
        bool have_candidate = false;
        bool truncated = false;
        for (uint32_t depth = 0; depth < max_depth; ++depth) {
            const uint32_t next_level_begin = level_end;
            uint32_t next_level_count = 0;
            std::vector<uint64_t> targets;
            targets.reserve(level_end - level_begin);
            std::unordered_map<uint64_t, std::vector<uint32_t>> nodes_by_address;
            nodes_by_address.reserve(level_end - level_begin);
            for (uint32_t index = level_begin; index < level_end; ++index) {
                targets.push_back(all_nodes[index].address);
                nodes_by_address[all_nodes[index].address].push_back(index);
            }

            std::vector<PointerHit> hits;
            const sao_status_t scan_status =
                scan_pointer_values(process, targets, max_ptr_per_level, budget, hits);
            if (scan_status == SAO_STATUS_ERR_TIMEOUT)
                return scan_status;
            if (scan_status != SAO_STATUS_OK && scan_status != SAO_STATUS_ERR_BUFFER_TOO_SMALL)
                return scan_status;
            if (scan_status == SAO_STATUS_ERR_BUFFER_TOO_SMALL)
                truncated = true;

            for (const PointerHit& hit : hits) {
                const auto match = nodes_by_address.find(hit.target);
                if (match == nodes_by_address.end())
                    continue;
                const ModuleRange* module = address_in_module(hit.source, modules);
                for (uint32_t node_index : match->second) {
                    if (module != nullptr) {
                        std::vector<uint32_t> path;
                        for (uint32_t walk = node_index; walk != UINT32_MAX;
                             walk = all_nodes[walk].parent_idx)
                            path.push_back(walk);
                        if (path.empty() || path.size() > 16)
                            continue;
                        std::reverse(path.begin(), path.end());
                        const uint64_t static_offset_u64 = hit.source - module->base;
                        if (static_offset_u64 >
                                static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
                            module->name.size() >= sizeof(out_chain->module_name_utf8))
                            continue;
                        sao_memprobe_ptr_chain_v2_t candidate{};
                        std::memcpy(candidate.module_name_utf8, module->name.data(),
                                    module->name.size());
                        candidate.module_name_utf8[module->name.size()] = '\0';
                        candidate.module_base_at_find = module->base;
                        candidate.static_offset = static_cast<int64_t>(static_offset_u64);
                        candidate.deref_count = static_cast<uint32_t>(path.size() - 1);
                        std::fill(std::begin(candidate.deref_offsets),
                                  std::end(candidate.deref_offsets), 0);
                        for (size_t offset_index = 0; offset_index < path.size() - 1;
                             ++offset_index) {
                            candidate.deref_offsets[offset_index] =
                                all_nodes[path[path.size() - 1 - offset_index]].back_delta;
                        }
                        candidate.final_offset = all_nodes[path.front()].back_delta;
                        candidate.struct_size = sizeof(candidate);
                        candidate.abi_version = SAO_MEMPROBE_PTR_CHAIN_V2_ABI_VERSION;
                        candidate.anchor_address_at_find = target_addr;
                        candidate.validation_state = SAO_MEMPROBE_PTR_CHAIN_VALIDATION_CANDIDATE;
                        std::strncpy(candidate.provenance_utf8, "pointer_chain.candidate",
                                     sizeof(candidate.provenance_utf8) - 1);
                        if (!have_candidate) {
                            best_candidate = candidate;
                            have_candidate = true;
                        }
                        const bool static_source =
                            static_data_section_contains(process, *module, hit.source, budget);
                        if (budget.exhausted)
                            return SAO_STATUS_ERR_TIMEOUT;
                        if (static_source) {
                            const sao_status_t fingerprint_status = module_fingerprint(
                                process, *module, budget, &candidate.module_fingerprint);
                            if (fingerprint_status != SAO_STATUS_OK)
                                return fingerprint_status;
                        }
                        candidate.validation_state = SAO_MEMPROBE_PTR_CHAIN_VALIDATION_CANDIDATE;
                        std::strncpy(candidate.provenance_utf8, "pointer_chain.candidate",
                                     sizeof(candidate.provenance_utf8) - 1);
                        SaoProcessInfo process_info{};
                        char image_path[SAO_PROCESS_IMAGE_PATH_MAX] = {};
                        if (sao_core_process_get_info(process, &process_info, image_path,
                                                      sizeof(image_path)) == SAO_STATUS_OK)
                            candidate.process_pid_at_find = process_info.pid;
                        uint64_t resolved = 0;
                        const sao_status_t readback_status =
                            resolve_impl(process, candidate, true, &resolved, budget);
                        if (readback_status == SAO_STATUS_ERR_TIMEOUT)
                            return readback_status;
                        if (readback_status != SAO_STATUS_OK || resolved != target_addr)
                            continue;
                        uint8_t readback = 0;
                        size_t readback_size = 0;
                        if (!budget.reserve(sizeof(readback)) ||
                            sao_core_mem_read(process, target_addr, &readback, sizeof(readback),
                                              &readback_size) != SAO_STATUS_OK ||
                            readback_size != sizeof(readback))
                            continue;
                        if (!static_source)
                            continue;
                        candidate.validation_state = SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED;
                        std::strncpy(candidate.provenance_utf8, "pointer_chain.validated",
                                     sizeof(candidate.provenance_utf8) - 1);
                        *out_chain = candidate;
                        return SAO_STATUS_OK;
                    }
                    for (int32_t back_delta : kObjectBackDeltas) {
                        if (next_level_count >= max_ptr_per_level ||
                            all_nodes.size() >= static_cast<size_t>(max_nodes)) {
                            truncated = true;
                            break;
                        }
                        if (hit.source < static_cast<uint64_t>(back_delta))
                            continue;
                        Node child{hit.source - static_cast<uint64_t>(back_delta), node_index,
                                   back_delta};
                        if (!visited_nodes.insert(NodeKey{child.address, child.back_delta}).second)
                            continue;
                        all_nodes.push_back(child);
                        ++next_level_count;
                    }
                    if (truncated)
                        break;
                }
                if (truncated && next_level_count >= max_ptr_per_level)
                    break;
            }
            level_end = static_cast<uint32_t>(all_nodes.size());
            if (level_end == next_level_begin)
                break;
            level_begin = next_level_begin;
        }
        if (budget.exhausted)
            return SAO_STATUS_ERR_TIMEOUT;
        if (have_candidate) {
            *out_chain = best_candidate;
            return SAO_STATUS_ERR_CAPABILITY_MISSING;
        }
        return truncated ? SAO_STATUS_ERR_BUFFER_TOO_SMALL : SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        std::memset(out_chain, 0, sizeof(*out_chain));
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t verify_impl(sao_core_process_handle_t process,
                         const sao_memprobe_ptr_chain_v2_t& chain, bool require_v2) {
    uint64_t final_addr = 0;
    ProbeBudget budget(64u * 1024u, 256u, 250u);
    const sao_status_t status = resolve_impl(process, chain, require_v2, &final_addr, budget);
    if (status != SAO_STATUS_OK)
        return status;
    if (require_v2 && chain.anchor_address_at_find != 0 && chain.process_pid_at_find != 0) {
        SaoProcessInfo process_info{};
        char image_path[SAO_PROCESS_IMAGE_PATH_MAX] = {};
        const sao_status_t info_status =
            sao_core_process_get_info(process, &process_info, image_path, sizeof(image_path));
        if (info_status != SAO_STATUS_OK)
            return info_status;
        if (process_info.pid == chain.process_pid_at_find &&
            final_addr != chain.anchor_address_at_find)
            return SAO_STATUS_ERR_NOT_FOUND;
    }
    uint64_t sample = 0;
    if (!budget.reserve(sizeof(sample)))
        return SAO_STATUS_ERR_TIMEOUT;
    if (sao_core_mem_read_u64(process, final_addr, &sample) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_READ_FAULT;
    return SAO_STATUS_OK;
}

} // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_backtrace_v2(
    sao_core_process_handle_t process, uint64_t target_addr,
    const sao_memprobe_backtrace_config_v2_t* cfg, sao_memprobe_ptr_chain_v2_t* out_chain) {
    NormalizedBacktraceConfig normalized{};
    if (!backtrace_config_v2_to_internal(cfg, &normalized))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return backtrace_impl(process, target_addr, &normalized, out_chain);
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_backtrace(
    sao_core_process_handle_t process, uint64_t target_addr,
    const sao_memprobe_backtrace_config_t* cfg, sao_memprobe_ptr_chain_t* out_chain) {
    if (out_chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_chain, 0, sizeof(*out_chain));
    sao_memprobe_ptr_chain_v2_t v2{};
    const NormalizedBacktraceConfig normalized = backtrace_config_v1(cfg);
    const sao_status_t status = backtrace_impl(process, target_addr, &normalized, &v2);
    if (status == SAO_STATUS_OK)
        std::memcpy(out_chain, &v2, sizeof(*out_chain));
    return status;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_resolve_v2(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_v2_t* chain,
    uint64_t* out_final_addr) {
    if (chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ProbeBudget budget(64u * 1024u, 256u, 250u);
    return resolve_impl(process, *chain, true, out_final_addr, budget);
}

extern "C" sao_status_t SAO_CORE_CALL
sao_memprobe_ptr_chain_resolve(sao_core_process_handle_t process,
                               const sao_memprobe_ptr_chain_t* chain, uint64_t* out_final_addr) {
    if (chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_memprobe_ptr_chain_v2_t v2{};
    std::memcpy(&v2, chain, sizeof(*chain));
    ProbeBudget budget(64u * 1024u, 256u, 250u);
    return resolve_impl(process, v2, false, out_final_addr, budget);
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_verify_v2(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_v2_t* chain) {
    if (chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        return verify_impl(process, *chain, true);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_verify(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_t* chain) {
    if (chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        sao_memprobe_ptr_chain_v2_t v2{};
        std::memcpy(&v2, chain, sizeof(*chain));
        return verify_impl(process, v2, false);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
