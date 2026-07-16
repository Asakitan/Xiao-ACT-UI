// migration.cpp — 老字段清单 (真正实装的一部分, 让 tests 有点内容)
#include "sao/plugins/compat/migration.h"

namespace sao::plugins::compat {

namespace {
// 目前已知的旧字段清单。新增弃用条目往这里加。
constexpr deprecated_entry k_deprecated_entries[] = {
    {"engine", "language", "manifest 里 'engine' 字段推荐改用 'language'",
     "manifest"},
    {"deps", "requires", "manifest 里 'deps' 数组统一到 'requires'",
     "manifest"},
    {"description_short", "description", "sao_menu 里 'description_short' 弃用",
     "manifest"},
    {"register_script", "register_ui_panel", "老 ctx.register_script 别名",
     "ctx_method"},
    {"add_hotkey", "register_hotkey", "老 ctx.add_hotkey 别名",
     "ctx_method"},
    {"add_menu_item", "register_menu_category", "老菜单添加接口",
     "ctx_method"},
};
constexpr size_t k_deprecated_count = sizeof(k_deprecated_entries) /
                                       sizeof(k_deprecated_entries[0]);
} // namespace

extern "C" SAO_PLUGINS_API const deprecated_entry* SAO_PLUGINS_CALL
sao_plugins_compat_deprecated_entries(size_t* out_count) {
    if (out_count != nullptr) *out_count = k_deprecated_count;
    return k_deprecated_entries;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_scan_deprecated(const char* /*plugin_id_utf8*/,
                                   char** out_report_json_utf8) {
    if (out_report_json_utf8 != nullptr) *out_report_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::compat
