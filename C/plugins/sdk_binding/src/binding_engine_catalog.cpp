// binding_engine_catalog.cpp — 六张组表聚合成一个有序目录索引。
//
// 每个 binding_engine_<group>.cpp 定义一张 const sdk_engine_group_table
// kEngineFns<Group>（descs + count + 组级 probe）；本 TU 只做聚合，不拥有
// 任何函数条目。catalog 顺序 = 组表声明顺序 = 目录枚举顺序，新增组在此
// 追加一行聚合即可。

#include "sao/plugins/sdk_binding/binding_engine.h"

namespace sao::plugins::sdk_binding {
namespace {

// 有序组表索引：mem → net → ui → gpuhunt → vt → misc。
const sdk_engine_group_table* const k_engine_groups[] = {
    &kEngineGroupMem,
    &kEngineGroupNet,
    &kEngineGroupUi,
    &kEngineGroupGpuHunt,
    &kEngineGroupVt,
    &kEngineGroupMisc,
};

} // namespace

const sdk_engine_function_desc* sdk_engine_catalog_find(std::string_view name) noexcept {
    if (name.empty()) return nullptr;
    for (const sdk_engine_group_table* group : k_engine_groups) {
        if (group == nullptr || group->descs == nullptr) continue;
        for (size_t index = 0; index < group->count; ++index) {
            const sdk_engine_function_desc& desc = group->descs[index];
            if (desc.name != nullptr && name == std::string_view(desc.name)) return &desc;
        }
    }
    return nullptr;
}

const sdk_engine_group_table* sdk_engine_catalog_group_of(
    const sdk_engine_function_desc* desc) noexcept {
    if (desc == nullptr) return nullptr;
    for (const sdk_engine_group_table* group : k_engine_groups) {
        if (group == nullptr || group->descs == nullptr) continue;
        if (desc >= group->descs && desc < group->descs + group->count) return group;
    }
    return nullptr;
}

size_t sdk_engine_catalog_size() noexcept {
    size_t total = 0;
    for (const sdk_engine_group_table* group : k_engine_groups) {
        if (group != nullptr) total += group->count;
    }
    return total;
}

const sdk_engine_function_desc* sdk_engine_catalog_at(size_t index) noexcept {
    for (const sdk_engine_group_table* group : k_engine_groups) {
        if (group == nullptr) continue;
        if (index < group->count) {
            return group->descs == nullptr ? nullptr : group->descs + index;
        }
        index -= group->count;
    }
    return nullptr;
}

bool sdk_engine_entry_available(const SaoSdkContext* ctx,
                                const sdk_engine_function_desc* desc) noexcept {
    if (desc == nullptr) return false;
    sdk_engine_probe_fn probe = desc->availability;
    if (probe == nullptr) {
        const sdk_engine_group_table* group = sdk_engine_catalog_group_of(desc);
        probe = group != nullptr ? group->probe : nullptr;
    }
    // desc.probe 与组 probe 均为空 → 恒可用。
    return probe == nullptr || probe(ctx);
}

} // namespace sao::plugins::sdk_binding
