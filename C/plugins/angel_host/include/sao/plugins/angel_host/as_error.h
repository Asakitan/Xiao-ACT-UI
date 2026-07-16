// as_error.h — AngelScript 错误 → SAO_STATUS
//
// asIScriptContext::GetState / GetExceptionString / GetExceptionLineNumber
// 转 sao_status + utf-8 message。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptContext;

namespace sao::plugins::angel_host {

// 从 context 拿异常 (若有), 转 SAO_STATUS。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_take_exception(asIScriptContext* ctx, char** out_utf8);

} // namespace sao::plugins::angel_host
