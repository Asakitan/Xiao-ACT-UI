// as_stdlib.h — AngelScript 标准库注入 (script* addon)
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;

namespace sao::plugins::angel_host {

// 注册 AngelScript 官方 addon:
//   - scriptstdstring (string 类型)
//   - scriptarray (数组)
//   - scriptdictionary (键值 map)
//   - scriptmath (math 函数)
//   - datetime (可选, 见 as_sandbox_config)
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_install_stdlib(asIScriptEngine* engine);

} // namespace sao::plugins::angel_host
