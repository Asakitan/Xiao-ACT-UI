// as_sandbox.h — AngelScript 沙箱 (import 白名单)
//
// AngelScript 里没有 os / file / net 模块 (那要靠 addon), 所以沙箱主要限制
// 加载哪些 addon (scriptarray / scriptdictionary / scriptmath 默认开;
// datetime / file / net addon 按 permissions 控)。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;

namespace sao::plugins::angel_host {

struct as_sandbox_config {
    bool allow_datetime_addon = true;
    bool allow_file_addon = false;
    bool allow_math_addon = true;
    bool allow_dictionary_addon = true;
    bool allow_array_addon = true;
    bool allow_string_addon = true;
    // 未列出的 addon 一律拒
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_arm(asIScriptEngine* engine,
                               const as_sandbox_config* cfg);

} // namespace sao::plugins::angel_host
