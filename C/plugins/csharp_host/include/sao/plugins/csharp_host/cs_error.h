// cs_error.h — .NET 异常 → SAO_STATUS
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

// 从最近一次 CLR 异常拿 (Type, Message, StackTrace) → SAO_STATUS + utf-8。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_take_error(char** out_utf8);

// 编译时错误 (Roslyn diagnostics) → SAO_STATUS + 详细列表 json。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_take_compile_errors(char** out_json_utf8);

} // namespace sao::plugins::csharp_host
