// migration.h — 老字段废弃诊断 + 迁移建议 (不阻止加载)
//
// 老插件里可能用了已经废弃的字段 / 方法。策略:
//   1. 仍然工作 (由 py_v1_manifest / py_v1_ctx_shim 支持)
//   2. 但在 UI 打一个 warn 日志: "该字段/方法已废弃, 建议改用 X"
//   3. 未来 v3 版本可能真正移除, 先给作者时间迁移
//
// 对齐 Python 源: 主平台并没有真的 warn (老代码就是老代码, 都跑),
// C++ 版本主动加固为一个诊断维度。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::compat {

// 老字段清单 (每条: 老名, 新推荐名, 弃用理由)
struct deprecated_entry {
    const char* old_name;
    const char* new_name;
    const char* reason;
    // "manifest" / "ctx_method" / "capability_id" / "extension_kind"
    const char* category;
};

// 获取所有已知弃用条目 (静态列表, 编译期定)。
extern "C" SAO_PLUGINS_API const deprecated_entry* SAO_PLUGINS_CALL
sao_plugins_compat_deprecated_entries(size_t* out_count);

// 扫描某插件的 manifest + 已加载 ctx, 报告用到了哪些弃用条目。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_scan_deprecated(const char* plugin_id_utf8,
                                   char** out_report_json_utf8);

} // namespace sao::plugins::compat
