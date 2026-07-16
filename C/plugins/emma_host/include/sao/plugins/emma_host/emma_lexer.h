// emma_lexer.h — Emma 词法分析器
//
// 对齐 Python 源: emma_runtime.py 的 _TOKEN_RE + _tokenize。C++ 侧因为
// std::regex 太慢, 改用手写状态机 tokenizer, 同样返回 Token 序列。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::emma_host {

enum class token_kind : uint8_t {
    eof = 0,
    ident,     // 标识符 (但 keyword 单出)
    keyword,   // let / fn / end / if / elif / else / while / for / in /
               // return / and / or / not / true / false / nil / break / continue
    number,    // 整数或浮点数字面量
    string,    // 双/单引号字符串
    op,        // 运算符 / 分隔符 ( ) { } [ ] . , ; : + - * / % = == != < > <= >= .. !
};

struct token {
    token_kind kind = token_kind::eof;
    std::string value;   // 原始文本 (或 unescape 过的 string 内容)
    uint32_t line = 0;
    uint32_t column = 0;
};

// 词法分析 utf-8 源码。返回 token 序列 + 末尾 eof。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_tokenize(const char* utf8_source,
                          size_t source_len,
                          token** out_tokens,
                          size_t* out_count);

// 释放 sao_plugins_emma_tokenize 分配的数组。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_emma_tokens_free(token* tokens, size_t count);

// C++ 便利接口 (RAII, 不跨 DLL 边界暴露 vector)
std::vector<token> tokenize_source(std::string_view source);

} // namespace sao::plugins::emma_host
