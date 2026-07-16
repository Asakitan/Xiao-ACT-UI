// emma_error.cpp — Emma 错误 → SAO_STATUS + 格式化实装
//
// 对齐 Python emma_runtime.py 里 SyntaxError / RuntimeError 直接冒泡。C++
// 侧通过 emma_error 结构体承接, 转 SAO_STATUS + 生成可读文本。

#include "sao/plugins/emma_host/emma_error.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace sao::plugins::emma_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_error_status(const emma_error* err) {
    if (err == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    switch (err->kind) {
        case error_kind::none:          return SAO_OK;
        case error_kind::lex_error:
        case error_kind::parse_error:   return SAO_ERR_INVALID_ARGUMENT;
        case error_kind::runtime_error: return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_error_format(const emma_error* err, char** out_utf8) {
    if (err == nullptr || out_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    std::ostringstream oss;
    switch (err->kind) {
        case error_kind::none:          oss << "[ok]"; break;
        case error_kind::lex_error:     oss << "[lex] "; break;
        case error_kind::parse_error:   oss << "[parse] "; break;
        case error_kind::runtime_error: oss << "[runtime] "; break;
    }
    if (err->kind != error_kind::none && (err->line || err->column)) {
        oss << "line " << err->line << ":" << err->column << " ";
    }
    oss << err->message;
    if (err->kind == error_kind::runtime_error && !err->call_stack.empty()) {
        oss << "\n  at " << err->call_stack;
    }
    std::string s = oss.str();
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (buf == nullptr) return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *out_utf8 = buf;
    return SAO_OK;
}

} // namespace sao::plugins::emma_host
