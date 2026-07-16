// emma_error.h — Emma 错误 → SAO_STATUS + 消息
//
// Emma 层三种错误:
//   1. 词法错误 (invalid character / unterminated string) → 消息含 line/col
//   2. 语法错误 (unexpected token / mismatched end) → 消息含 line/col
//   3. 运行时 (undefined name / type mismatch / division by zero) → 含调用栈
//
// 对齐 Python 源: emma_runtime.py 里 SyntaxError / RuntimeError 直接冒泡到
// PluginManager._record_failure。C++ 侧一并抽象。
#pragma once

#include <cstdint>
#include <string>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::emma_host {

enum class error_kind : uint8_t {
    none = 0,
    lex_error,
    parse_error,
    runtime_error,
};

struct emma_error {
    error_kind kind = error_kind::none;
    std::string message;
    uint32_t line = 0;
    uint32_t column = 0;
    std::string call_stack;   // runtime_error 时的 emma-side stack trace
};

// 把 emma_error 转成 SAO_STATUS。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_error_status(const emma_error* err);

// 格式化成人可读多行文本 (含 line/col 和 stack)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_error_format(const emma_error* err, char** out_utf8);

} // namespace sao::plugins::emma_host
