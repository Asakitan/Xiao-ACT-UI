// emma_error.cpp — Emma 错误 → SAO_STATUS + 格式化实装
//
// 对齐 Python emma_runtime.py 里 SyntaxError / RuntimeError 直接冒泡。C++
// 侧通过 emma_error 结构体承接, 转 SAO_STATUS + 生成可读文本。

#include "sao/plugins/emma_host/emma_error.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

namespace sao::plugins::emma_host {

emma_exception::emma_exception(emma_error error) : error_(std::move(error)) {}

const char* emma_exception::what() const noexcept {
    return error_.message.c_str();
}

const emma_error& emma_exception::error() const noexcept {
    return error_;
}

bool is_valid_error_kind(error_kind kind) noexcept {
    switch (kind) {
    case error_kind::none:
    case error_kind::lex_error:
    case error_kind::parse_error:
    case error_kind::runtime_error:
        return true;
    }
    return false;
}

void set_error_location_from_message(emma_error& error) noexcept {
    if (error.line != 0 || error.message.empty())
        return;
    try {
        constexpr const char* prefix = "line ";
        const size_t start = error.message.find(prefix);
        if (start == std::string::npos)
            return;
        size_t cursor = start + std::strlen(prefix);
        size_t consumed = 0;
        const unsigned long line = std::stoul(error.message.substr(cursor), &consumed, 10);
        cursor += consumed;
        unsigned long column = 0;
        if (cursor < error.message.size() && error.message[cursor] == ':') {
            ++cursor;
            column = std::stoul(error.message.substr(cursor), nullptr, 10);
        }
        if (line <= std::numeric_limits<uint32_t>::max() &&
            column <= std::numeric_limits<uint32_t>::max()) {
            error.line = static_cast<uint32_t>(line);
            error.column = static_cast<uint32_t>(column);
        }
    } catch (...) {
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_error_status(const emma_error* err) {
    if (err == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (!is_valid_error_kind(err->kind))
        return SAO_ERR_INVALID_ARGUMENT;
    if (err->status != SAO_OK)
        return err->status;
    switch (err->kind) {
    case error_kind::none:
        return SAO_OK;
    case error_kind::lex_error:
    case error_kind::parse_error:
        return SAO_ERR_INVALID_ARGUMENT;
    case error_kind::runtime_error:
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_ERR_INVALID_ARGUMENT;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_error_format(const emma_error* err, char** out_utf8) {
    if (out_utf8 != nullptr)
        *out_utf8 = nullptr;
    if (err == nullptr || out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (!is_valid_error_kind(err->kind))
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::ostringstream oss;
        switch (err->kind) {
        case error_kind::none:
            oss << "[ok]";
            break;
        case error_kind::lex_error:
            oss << "[lex] ";
            break;
        case error_kind::parse_error:
            oss << "[parse] ";
            break;
        case error_kind::runtime_error:
            oss << "[runtime] ";
            break;
        }
        if (err->kind != error_kind::none && (err->line || err->column)) {
            oss << "line " << err->line << ":" << err->column << " ";
        }
        oss << err->message;
        if (err->kind == error_kind::runtime_error && !err->call_stack.empty()) {
            oss << "\n  at " << err->call_stack;
        }
        const std::string text = oss.str();
        auto* buffer = static_cast<char*>(std::malloc(text.size() + 1));
        if (buffer == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        std::memcpy(buffer, text.data(), text.size());
        buffer[text.size()] = '\0';
        *out_utf8 = buffer;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::emma_host
