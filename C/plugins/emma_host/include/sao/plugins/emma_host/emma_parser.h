// emma_parser.h — Emma 递归下降语法分析
//
// 对齐 Python 源: emma_runtime.py 的 _Parser (parse / _stmt / _expr / _fn_def /
// _if_stmt / _while_stmt / _for_stmt / _postfix_expr / ...)。
//
// AST 用 std::variant + 索引化池 (避免 unique_ptr 分配抖动)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/emma_host/emma_lexer.h"

namespace sao::plugins::emma_host {

// AST 节点索引 (0 = 无效)
using node_id = uint32_t;

enum class node_kind : uint8_t {
    literal_nil = 0,
    literal_bool,
    literal_int,
    literal_float,
    literal_string,
    ident,
    binop,
    unaryop,
    call,
    attr,
    index,
    array_lit,
    dict_lit,
    assign,
    let,
    fn_def,
    return_stmt,
    if_stmt,
    while_stmt,
    for_stmt,
    break_stmt,
    continue_stmt,
    expr_stmt,
};

// AST 节点池
struct ast_pool {
    std::vector<node_kind> kinds;
    std::vector<std::string> strings;   // 复用池
    std::vector<int64_t> ints;
    std::vector<double> floats;
    std::vector<std::vector<node_id>> children;  // 每节点的子节点列表
    std::vector<uint32_t> lines;
};

// 递归下降 parser (对齐 python _Parser)
class parser {
public:
    parser(const std::vector<token>& tokens, ast_pool* pool);
    // 解析顶层语句序列; 返回 stmt id 列表。
    int32_t parse_program(std::vector<node_id>& out_stmts,
                          std::string& out_error);
private:
    const std::vector<token>* tokens_ = nullptr;
    ast_pool* pool_ = nullptr;
    size_t pos_ = 0;
};

} // namespace sao::plugins::emma_host
