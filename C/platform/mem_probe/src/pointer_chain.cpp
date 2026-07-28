// pointer_chain.cpp — BFS pointer-chain backtrace + resolve.
//
// Phase 5 (Python parity closure) — port of python/mem_probe/pointer_chain.py.
// Algorithm:
//   L0 candidates = {target_addr - δ | δ ∈ [0, 0x100] step 8}
//   For depth ∈ [1..max_depth]:
//     Full-memory scan for u64 == any candidate; each hit → (source, offset)
//     If source ∈ any module's .data/.rdata section → chain found
//   Chain = { module_name, module_base_at_find, static_offset, deref_offsets[], final_offset }

#include "sao/mem_probe/pointer_chain.h"

#include "sao/core/memory.h"
#include "sao/mem_probe/scanner.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kDefaultMaxDepth = 3;
constexpr uint32_t kDefaultMaxPtrPerLevel = 8192;
constexpr uint64_t kDefaultScanBudget = 256ULL * 1024 * 1024;
constexpr int32_t kObjectBackDeltas[] = {0, 8, 16, 24, 32, 40, 48, 56, 64,
                                          72, 80, 88, 96, 104, 112, 120, 128,
                                          136, 144, 152, 160, 168, 176, 184,
                                          192, 200, 208, 216, 224, 232, 240,
                                          248, 256};

struct ModuleRange {
    std::string name;
    uint64_t base;
    uint64_t end;
};

std::vector<ModuleRange> enumerate_modules_or_empty(sao_core_process_handle_t p) {
    std::vector<ModuleRange> out;
    if (p == nullptr) return out;
    SaoModuleEntry entries[512];
    char names[65536];
    size_t entry_count = 0;
    size_t names_used = 0;
    if (sao_core_process_enum_modules(p, entries, 512, &entry_count, names,
                                       sizeof(names), &names_used) != SAO_STATUS_OK) {
        return out;
    }
    out.reserve(entry_count);
    for (size_t i = 0; i < entry_count; ++i) {
        ModuleRange r;
        r.base = entries[i].base_address;
        r.end = r.base + entries[i].module_size;
        r.name = std::string(names + entries[i].name_offset);
        out.push_back(std::move(r));
    }
    return out;
}

const ModuleRange* address_in_module(uint64_t addr,
                                     const std::vector<ModuleRange>& mods) {
    for (const auto& m : mods) {
        if (addr >= m.base && addr < m.end) return &m;
    }
    return nullptr;
}

} // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_backtrace(
    sao_core_process_handle_t process, uint64_t target_addr,
    const sao_memprobe_backtrace_config_t* cfg,
    sao_memprobe_ptr_chain_t* out_chain) {
    if (process == nullptr || out_chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_chain, 0, sizeof(*out_chain));
    const uint32_t max_depth = (cfg && cfg->max_depth) ? cfg->max_depth : kDefaultMaxDepth;
    const uint32_t max_ptr_lv = (cfg && cfg->max_ptr_per_level)
                                    ? cfg->max_ptr_per_level
                                    : kDefaultMaxPtrPerLevel;
    const uint64_t budget = (cfg && cfg->max_scan_bytes_budget)
                                ? cfg->max_scan_bytes_budget
                                : kDefaultScanBudget;
    (void)budget;

    auto modules = enumerate_modules_or_empty(process);

    // BFS with parent linkage for chain reconstruction.
    struct Node {
        uint64_t address;   // 该节点代表的"被指向"地址
        uint32_t depth;
        uint32_t parent_idx; // 上一层节点在 all_nodes[] 中的索引; UINT32_MAX = root
        int32_t offset_to_parent; // (source) 该 node 到 parent.address 的 offset
    };

    std::vector<Node> all_nodes;
    all_nodes.reserve(1024);

    // Root layer: 若干 target ± δ 候选地址.
    for (int32_t d : kObjectBackDeltas) {
        Node n{};
        n.address = target_addr - static_cast<uint64_t>(d);
        n.depth = 0;
        n.parent_idx = UINT32_MAX;
        n.offset_to_parent = d;
        all_nodes.push_back(n);
    }

    std::unordered_set<uint64_t> visited;
    for (const auto& n : all_nodes) visited.insert(n.address);

    // BFS: 每层扫描 all_nodes[level_begin..level_end) 找指向它们的 u64 指针。
    uint32_t level_begin = 0;
    uint32_t level_end = static_cast<uint32_t>(all_nodes.size());
    for (uint32_t depth = 0; depth < max_depth; ++depth) {
        uint32_t next_level_begin = level_end;
        for (uint32_t ni = level_begin; ni < level_end; ++ni) {
            uint64_t target = all_nodes[ni].address;
            std::vector<sao_memprobe_candidate_t> hits(max_ptr_lv);
            uint32_t hit_count = 0;
            sao_memprobe_scan_bounds_t bounds{};
            bounds.max_addrs = max_ptr_lv;
            bounds.max_duration_ms = 5000;
            bounds.align = 8;
            (void)sao_memprobe_scan_first(process, SAO_MEMPROBE_DTYPE_U64,
                                          target, &bounds, hits.data(),
                                          max_ptr_lv, &hit_count);
            for (uint32_t i = 0; i < hit_count; ++i) {
                const uint64_t src = hits[i].addr;
                if (visited.count(src)) continue;
                visited.insert(src);
                // 是否落在 module 静态段 → 找到链，回溯组装。
                const ModuleRange* m = address_in_module(src, modules);
                if (m != nullptr) {
                    // 组装 deref_offsets: 从 source 向下到 target。
                    std::vector<int32_t> derefs;
                    // 从当前 node ni 逆序回溯 (parent 链)，收集 offset_to_parent。
                    derefs.push_back(0); // final: src -> ni.address
                    uint32_t walk = ni;
                    while (walk != UINT32_MAX) {
                        derefs.push_back(all_nodes[walk].offset_to_parent);
                        walk = all_nodes[walk].parent_idx;
                    }
                    // derefs 现在是 [0, ni.offset, parent.offset, ..., root.offset(=δ)]
                    // 我们要 chain: 从 static 段的 src 开始，逐个 deref+offset 后到 target。
                    // 反转成从 top-down (static -> ... -> target) 顺序。
                    std::reverse(derefs.begin(), derefs.end());
                    // 输出.
                    std::snprintf(out_chain->module_name_utf8,
                                  sizeof(out_chain->module_name_utf8), "%s",
                                  m->name.c_str());
                    out_chain->module_base_at_find = m->base;
                    out_chain->static_offset = static_cast<int64_t>(src - m->base);
                    out_chain->deref_count = static_cast<uint32_t>(
                        std::min<size_t>(derefs.size() > 0 ? derefs.size() - 1 : 0, 16));
                    for (uint32_t j = 0; j < out_chain->deref_count; ++j) {
                        out_chain->deref_offsets[j] = derefs[j];
                    }
                    out_chain->final_offset =
                        derefs.empty() ? 0 : derefs.back();
                    return SAO_STATUS_OK;
                }
                // 非静态段：作为下一层候选 push 进 all_nodes。
                if (all_nodes.size() < max_ptr_lv * (depth + 2)) {
                    Node child{};
                    child.address = src;
                    child.depth = depth + 1;
                    child.parent_idx = ni;
                    child.offset_to_parent = 0; // src *is* pointer TO target.
                    all_nodes.push_back(child);
                }
            }
        }
        level_end = static_cast<uint32_t>(all_nodes.size());
        if (level_end == next_level_begin) break; // 无新增
        level_begin = next_level_begin;
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_resolve(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_t* chain,
    uint64_t* out_final_addr) {
    if (process == nullptr || chain == nullptr || out_final_addr == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Find module current base by name.
    auto modules = enumerate_modules_or_empty(process);
    const ModuleRange* target = nullptr;
    for (const auto& m : modules) {
        if (m.name == chain->module_name_utf8) {
            target = &m;
            break;
        }
    }
    if (target == nullptr) return SAO_STATUS_ERR_MODULE_NOT_FOUND;
    uint64_t addr = target->base + static_cast<uint64_t>(chain->static_offset);
    for (uint32_t i = 0; i < chain->deref_count; ++i) {
        uint64_t next_addr = 0;
        if (sao_core_mem_read_ptr(process, addr, &next_addr) != SAO_STATUS_OK)
            return SAO_STATUS_ERR_READ_FAULT;
        addr = next_addr + static_cast<uint64_t>(
                               static_cast<int64_t>(chain->deref_offsets[i]));
    }
    addr += static_cast<uint64_t>(static_cast<int64_t>(chain->final_offset));
    *out_final_addr = addr;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_verify(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_t* chain) {
    uint64_t final_addr = 0;
    sao_status_t st = sao_memprobe_ptr_chain_resolve(process, chain, &final_addr);
    if (st != SAO_STATUS_OK) return st;
    uint64_t sample = 0;
    return sao_core_mem_read_u64(process, final_addr, &sample);
}
