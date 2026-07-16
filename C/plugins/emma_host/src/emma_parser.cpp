// emma_parser.cpp — Emma 递归下降语法分析器实装
//
// 对齐 Python emma_runtime.py 的 _Parser。节点通过 ast_pool 索引式存储:
//   - kinds[id]     = node_kind
//   - lines[id]     = 定义行号
//   - children[id]  = 子节点 id 列表 (语义依 node_kind 而定)
//   - strings[id]   = 字符串 payload (ident name / string literal / binop op / attr name / fn name)
//   - ints[id]      = 整数 payload (literal_int / literal_bool 用 0/1)
//   - floats[id]    = 浮点 payload (literal_float)
//
// children 布局约定 (每个 node_kind):
//   literal_nil / literal_bool / literal_int / literal_float / literal_string / ident:
//                 无子节点
//   binop:        [left, right],  strings[id] = op
//   unaryop:      [operand],      strings[id] = op ("-" or "not")
//   call:         [callee, arg0, arg1, ...]
//   attr:         [obj],          strings[id] = attr name
//   index:        [obj, key]
//   array_lit:    [item0, item1, ...]
//   dict_lit:     [k0, v0, k1, v1, ...]
//   assign:       [target, value]
//   let:          [value] (可能是 literal_nil),  strings[id] = name
//   fn_def:       [body0, body1, ...],  strings[id] = name,
//                 (params 用 ints_children_split 存: children 前 nparam 个是 param name-only ident 节点,
//                  之后是 body statements. 但为了简单, 我们用 sub-nodes for params:
//                  param ident 节点用 node_kind::ident 且 strings[param_id] = name)
//                 → children[id] = [param_ident_ids..., BODY_SENTINEL, body_stmts...]
//                 用 ints[id] 存 param count 做拆分
//   return_stmt:  [expr] 或 [] (无 expr)
//   if_stmt:      按 branches: [cond0, then_len_sentinel, then0, then1, ..., cond1, ..., ELSE_SENTINEL, else...]
//                 简化: ints[id] = branch count; strings[id] 空; children 布局:
//                 前 branch_count 个 cond id, 之后是拼接的 branch bodies, else body 用 -1 分隔?
//                 →太复杂. 改用 node_kind::if_stmt 只存主 cond+then,
//                 elif/else 挂在 body 结束后, 用 children 分段, ints[id] 存分段偏移。
//                 更简单方案: if_stmt.children = [cond, then_len_marker_int, ...]
//                 → 干脆用 shim: if_stmt 有 pair (cond,body) 数组 + else body.
//                 用 ints[id] 编码分段: ints[id] = branch_count, 后接 branch_count 组
//                 (cond_id + then_body_len)。这需要 ints[] 存多值 → 违反 SoA。
//                 折中方案: 引入辅助 sentinel 节点 kind. 简单起见, 加个 "block" 内部 kind
//                 不在 header 里 (直接复用 expr_stmt 的 children 作 block?). 会破坏语义。
//                 最干净: if_stmt.children = [cond0, block0, cond1, block1, ..., condN, blockN, elseBlock (可能是空 block)]
//                 每个 block 是一个 node_kind::expr_stmt (作 wrapper) 或另加辅助. 但改 header 会破坏 header 契约。
//                 → 用 helper: 在 ast_pool 内加一个 "block wrapper" 用 expr_stmt kind 但 children 是 stmts 列表。
//                 语义分歧: expr_stmt.children[0] 是唯一 expr id. 复用不安全。
//                 → 引入 pseudo-block: node_kind::expr_stmt with strings[]="__block__", children = stmts.
//                 interpreter 认这个标记就知道是块。够 hacky, 但不改 header。
//                 更干净的替代: **本次不改 header**, 我们把 if 分支各自做成一个匿名 fn_def 也不合适。
//
//                 最终决定: 存 ints[id] = 2*N+1 flags (每个分支占 2 个 ids: cond, block_marker;
//                 block_marker 是伪节点 kind=expr_stmt strings="__block__" children=body_stmts;
//                 最末一位 else block 也是 __block__, 若无 else 用 nil literal 占位)。
//   while_stmt:   [cond, __block__]
//   for_stmt:     [iter_expr, __block__],  strings[id] = var name
//   break_stmt:   无
//   continue_stmt:无
//   expr_stmt:    [expr]  (普通表达式语句; 若 strings[id]=="__block__" 则 children 是 stmts, 是块 wrapper)

#include "sao/plugins/emma_host/emma_parser.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace sao::plugins::emma_host {

namespace {

// 内部标记: 块 wrapper 复用 expr_stmt kind, 用 strings 打这个标记
constexpr const char* BLOCK_MARK = "__block__";

class parser_impl {
public:
    parser_impl(const std::vector<token>& tokens, ast_pool* pool)
        : tokens_(&tokens), pool_(pool) {}

    int32_t parse(std::vector<node_id>& out_stmts, std::string& out_error) {
        try {
            while (peek().kind != token_kind::eof) {
                skip_semis();
                if (peek().kind == token_kind::eof) break;
                out_stmts.push_back(stmt());
            }
            return SAO_OK;
        } catch (const std::exception& e) {
            out_error = e.what();
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }

private:
    const std::vector<token>* tokens_ = nullptr;
    ast_pool* pool_ = nullptr;
    size_t pos_ = 0;

    // ── 节点池辅助 ──
    node_id new_node(node_kind k, uint32_t line = 0) {
        node_id id = static_cast<node_id>(pool_->kinds.size());
        pool_->kinds.push_back(k);
        pool_->strings.emplace_back();
        pool_->ints.push_back(0);
        pool_->floats.push_back(0.0);
        pool_->children.emplace_back();
        pool_->lines.push_back(line);
        return id;
    }

    void set_string(node_id id, std::string s) { pool_->strings[id] = std::move(s); }
    void set_int(node_id id, int64_t v)         { pool_->ints[id] = v; }
    void set_float(node_id id, double v)        { pool_->floats[id] = v; }
    void add_child(node_id id, node_id c)       { pool_->children[id].push_back(c); }

    // ── token 辅助 ──
    const token& peek() const {
        return pos_ < tokens_->size() ? (*tokens_)[pos_] : (*tokens_)[tokens_->size() - 1];
    }
    const token& advance() {
        const token& t = peek();
        if (pos_ < tokens_->size()) ++pos_;
        return t;
    }
    const token* match(token_kind k, const std::string& v = "") {
        const token& t = peek();
        if (t.kind != k) return nullptr;
        if (!v.empty() && t.value != v) return nullptr;
        advance();
        return &t;
    }
    const token& expect(token_kind k, const std::string& v = "") {
        const token& t = peek();
        if (t.kind != k || (!v.empty() && t.value != v)) {
            std::ostringstream oss;
            oss << "line " << t.line << ": expected ";
            switch (k) {
                case token_kind::ident:   oss << "IDENT"; break;
                case token_kind::keyword: oss << "KEYWORD"; break;
                case token_kind::number:  oss << "NUMBER"; break;
                case token_kind::string:  oss << "STRING"; break;
                case token_kind::op:      oss << "OP"; break;
                case token_kind::eof:     oss << "EOF"; break;
            }
            if (!v.empty()) oss << " '" << v << "'";
            oss << ", got ";
            switch (t.kind) {
                case token_kind::ident:   oss << "IDENT"; break;
                case token_kind::keyword: oss << "KEYWORD"; break;
                case token_kind::number:  oss << "NUMBER"; break;
                case token_kind::string:  oss << "STRING"; break;
                case token_kind::op:      oss << "OP"; break;
                case token_kind::eof:     oss << "EOF"; break;
            }
            oss << " '" << t.value << "'";
            throw std::runtime_error(oss.str());
        }
        advance();
        return t;
    }

    void skip_semis() {
        while (match(token_kind::op, ";")) {}
    }

    // ── stmt ──
    node_id stmt() {
        const token& t = peek();
        if (t.kind == token_kind::keyword) {
            if (t.value == "let")      return let_stmt();
            if (t.value == "fn")       return fn_def();
            if (t.value == "return")   return return_stmt();
            if (t.value == "if")       return if_stmt();
            if (t.value == "while")    return while_stmt();
            if (t.value == "for")      return for_stmt();
            if (t.value == "break")    { advance(); match(token_kind::op, ";"); return new_node(node_kind::break_stmt, t.line); }
            if (t.value == "continue") { advance(); match(token_kind::op, ";"); return new_node(node_kind::continue_stmt, t.line); }
        }
        // expr / assign
        node_id e = expr();
        if (match(token_kind::op, "=")) {
            node_id v = expr();
            match(token_kind::op, ";");
            node_id a = new_node(node_kind::assign, t.line);
            add_child(a, e);
            add_child(a, v);
            return a;
        }
        match(token_kind::op, ";");
        node_id es = new_node(node_kind::expr_stmt, t.line);
        add_child(es, e);
        return es;
    }

    node_id let_stmt() {
        const token& kw = expect(token_kind::keyword, "let");
        const token& name = expect(token_kind::ident);
        node_id id = new_node(node_kind::let, kw.line);
        set_string(id, name.value);
        node_id value_id;
        if (match(token_kind::op, "=")) {
            value_id = expr();
        } else {
            value_id = new_node(node_kind::literal_nil, kw.line);
        }
        add_child(id, value_id);
        match(token_kind::op, ";");
        return id;
    }

    node_id fn_def() {
        const token& kw = expect(token_kind::keyword, "fn");
        std::string fn_name;
        // 支持匿名 fn (lambda) 和具名 fn
        if (peek().kind == token_kind::ident) {
            fn_name = advance().value;
        }
        expect(token_kind::op, "(");
        std::vector<std::string> params;
        if (!match(token_kind::op, ")")) {
            params.push_back(expect(token_kind::ident).value);
            while (match(token_kind::op, ",")) {
                params.push_back(expect(token_kind::ident).value);
            }
            expect(token_kind::op, ")");
        }
        // body = block until "end"
        node_id body_block = parse_block({"end"});
        expect(token_kind::keyword, "end");

        node_id id = new_node(node_kind::fn_def, kw.line);
        set_string(id, fn_name);
        set_int(id, static_cast<int64_t>(params.size()));
        // children 布局: 先 nparam 个 param ident 节点, 再 body_block 一个
        for (const auto& p : params) {
            node_id pid = new_node(node_kind::ident, kw.line);
            set_string(pid, p);
            add_child(id, pid);
        }
        add_child(id, body_block);
        return id;
    }

    node_id return_stmt() {
        const token& kw = expect(token_kind::keyword, "return");
        node_id id = new_node(node_kind::return_stmt, kw.line);
        // 若下一个是 end/else/elif/;/EOF, 则无返回值
        const token& t = peek();
        bool has_expr = true;
        if (t.kind == token_kind::keyword && (t.value == "end" || t.value == "else" || t.value == "elif")) {
            has_expr = false;
        } else if (t.kind == token_kind::op && t.value == ";") {
            advance();
            has_expr = false;
        } else if (t.kind == token_kind::eof) {
            has_expr = false;
        }
        if (has_expr) {
            node_id e = expr();
            add_child(id, e);
            match(token_kind::op, ";");
        }
        return id;
    }

    node_id if_stmt() {
        const token& kw = expect(token_kind::keyword, "if");
        node_id id = new_node(node_kind::if_stmt, kw.line);
        // 结构: cond, block, [cond, block, ...], else_block (可能是空 block)
        int64_t branch_count = 1;
        node_id cond = expr();
        node_id then_body = parse_block({"elif", "else", "end"});
        add_child(id, cond);
        add_child(id, then_body);

        while (match(token_kind::keyword, "elif")) {
            node_id ec = expr();
            node_id eb = parse_block({"elif", "else", "end"});
            add_child(id, ec);
            add_child(id, eb);
            ++branch_count;
        }
        node_id else_block;
        if (match(token_kind::keyword, "else")) {
            else_block = parse_block({"end"});
        } else {
            else_block = new_node(node_kind::expr_stmt, kw.line);
            set_string(else_block, BLOCK_MARK);
        }
        add_child(id, else_block);
        expect(token_kind::keyword, "end");
        set_int(id, branch_count);
        return id;
    }

    node_id while_stmt() {
        const token& kw = expect(token_kind::keyword, "while");
        node_id cond = expr();
        node_id body = parse_block({"end"});
        expect(token_kind::keyword, "end");
        node_id id = new_node(node_kind::while_stmt, kw.line);
        add_child(id, cond);
        add_child(id, body);
        return id;
    }

    node_id for_stmt() {
        const token& kw = expect(token_kind::keyword, "for");
        const token& var = expect(token_kind::ident);
        expect(token_kind::keyword, "in");
        node_id iter = expr();
        node_id body = parse_block({"end"});
        expect(token_kind::keyword, "end");
        node_id id = new_node(node_kind::for_stmt, kw.line);
        set_string(id, var.value);
        add_child(id, iter);
        add_child(id, body);
        return id;
    }

    // 解析一个块 (statements until 遇到指定 terminator keyword)
    // 返回一个 expr_stmt 节点作 wrapper (strings="__block__", children=stmts)
    node_id parse_block(std::initializer_list<const char*> terminators) {
        node_id block = new_node(node_kind::expr_stmt, peek().line);
        set_string(block, BLOCK_MARK);
        while (true) {
            skip_semis();
            const token& t = peek();
            if (t.kind == token_kind::eof) break;
            if (t.kind == token_kind::keyword) {
                bool is_term = false;
                for (const char* term : terminators) {
                    if (t.value == term) { is_term = true; break; }
                }
                if (is_term) break;
            }
            add_child(block, stmt());
        }
        return block;
    }

    // ── expression parser (precedence climbing, 对齐 python 顺序) ──
    node_id expr()       { return or_expr(); }

    node_id or_expr() {
        node_id left = and_expr();
        while (match(token_kind::keyword, "or")) {
            node_id right = and_expr();
            node_id id = new_node(node_kind::binop, peek().line);
            set_string(id, "or");
            add_child(id, left);
            add_child(id, right);
            left = id;
        }
        return left;
    }

    node_id and_expr() {
        node_id left = not_expr();
        while (match(token_kind::keyword, "and")) {
            node_id right = not_expr();
            node_id id = new_node(node_kind::binop, peek().line);
            set_string(id, "and");
            add_child(id, left);
            add_child(id, right);
            left = id;
        }
        return left;
    }

    node_id not_expr() {
        if (match(token_kind::keyword, "not")) {
            node_id inner = not_expr();
            node_id id = new_node(node_kind::unaryop, peek().line);
            set_string(id, "not");
            add_child(id, inner);
            return id;
        }
        return cmp_expr();
    }

    node_id cmp_expr() {
        node_id left = concat_expr();
        while (true) {
            const token& t = peek();
            if (t.kind != token_kind::op) break;
            const std::string& v = t.value;
            if (v != "==" && v != "!=" && v != "<=" && v != ">=" && v != "<" && v != ">") break;
            advance();
            node_id right = concat_expr();
            node_id id = new_node(node_kind::binop, t.line);
            set_string(id, v);
            add_child(id, left);
            add_child(id, right);
            left = id;
        }
        return left;
    }

    node_id concat_expr() {
        node_id left = add_expr();
        while (peek().kind == token_kind::op && peek().value == "..") {
            const token& t = advance();
            node_id right = add_expr();
            node_id id = new_node(node_kind::binop, t.line);
            set_string(id, "..");
            add_child(id, left);
            add_child(id, right);
            left = id;
        }
        return left;
    }

    node_id add_expr() {
        node_id left = mul_expr();
        while (peek().kind == token_kind::op && (peek().value == "+" || peek().value == "-")) {
            const token& t = advance();
            node_id right = mul_expr();
            node_id id = new_node(node_kind::binop, t.line);
            set_string(id, t.value);
            add_child(id, left);
            add_child(id, right);
            left = id;
        }
        return left;
    }

    node_id mul_expr() {
        node_id left = unary_expr();
        while (peek().kind == token_kind::op && (peek().value == "*" || peek().value == "/" || peek().value == "%")) {
            const token& t = advance();
            node_id right = unary_expr();
            node_id id = new_node(node_kind::binop, t.line);
            set_string(id, t.value);
            add_child(id, left);
            add_child(id, right);
            left = id;
        }
        return left;
    }

    node_id unary_expr() {
        if (peek().kind == token_kind::op && peek().value == "-") {
            const token& t = advance();
            node_id inner = unary_expr();
            node_id id = new_node(node_kind::unaryop, t.line);
            set_string(id, "-");
            add_child(id, inner);
            return id;
        }
        if (peek().kind == token_kind::op && peek().value == "!") {
            const token& t = advance();
            node_id inner = unary_expr();
            node_id id = new_node(node_kind::unaryop, t.line);
            set_string(id, "not");
            add_child(id, inner);
            return id;
        }
        return postfix_expr();
    }

    node_id postfix_expr() {
        node_id node = primary();
        while (true) {
            const token& t = peek();
            if (t.kind == token_kind::op && t.value == ".") {
                advance();
                const token& attr = expect(token_kind::ident);
                if (peek().kind == token_kind::op && peek().value == "(") {
                    // method call: .attr(args)
                    advance();
                    std::vector<node_id> args = arglist();
                    node_id attr_node = new_node(node_kind::attr, t.line);
                    set_string(attr_node, attr.value);
                    add_child(attr_node, node);
                    node_id call = new_node(node_kind::call, t.line);
                    add_child(call, attr_node);
                    for (node_id a : args) add_child(call, a);
                    node = call;
                } else {
                    node_id attr_node = new_node(node_kind::attr, t.line);
                    set_string(attr_node, attr.value);
                    add_child(attr_node, node);
                    node = attr_node;
                }
            } else if (t.kind == token_kind::op && t.value == "(") {
                advance();
                std::vector<node_id> args = arglist();
                node_id call = new_node(node_kind::call, t.line);
                add_child(call, node);
                for (node_id a : args) add_child(call, a);
                node = call;
            } else if (t.kind == token_kind::op && t.value == "[") {
                advance();
                node_id key = expr();
                expect(token_kind::op, "]");
                node_id idx = new_node(node_kind::index, t.line);
                add_child(idx, node);
                add_child(idx, key);
                node = idx;
            } else {
                break;
            }
        }
        return node;
    }

    std::vector<node_id> arglist() {
        std::vector<node_id> args;
        if (!(peek().kind == token_kind::op && peek().value == ")")) {
            args.push_back(expr());
            while (match(token_kind::op, ",")) {
                if (peek().kind == token_kind::op && peek().value == ")") break;
                args.push_back(expr());
            }
        }
        expect(token_kind::op, ")");
        return args;
    }

    node_id primary() {
        const token& t = peek();
        if (t.kind == token_kind::number) {
            advance();
            const std::string& v = t.value;
            bool is_float = false;
            for (char c : v) {
                if (c == '.' || c == 'e' || c == 'E') { is_float = true; break; }
            }
            if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
                node_id id = new_node(node_kind::literal_int, t.line);
                set_int(id, static_cast<int64_t>(std::strtoll(v.c_str() + 2, nullptr, 16)));
                return id;
            }
            if (is_float) {
                node_id id = new_node(node_kind::literal_float, t.line);
                set_float(id, std::strtod(v.c_str(), nullptr));
                return id;
            }
            node_id id = new_node(node_kind::literal_int, t.line);
            set_int(id, static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10)));
            return id;
        }
        if (t.kind == token_kind::string) {
            advance();
            node_id id = new_node(node_kind::literal_string, t.line);
            set_string(id, t.value);   // lexer 已 unescape
            return id;
        }
        if (t.kind == token_kind::keyword) {
            if (t.value == "true") { advance(); node_id id = new_node(node_kind::literal_bool, t.line); set_int(id, 1); return id; }
            if (t.value == "false"){ advance(); node_id id = new_node(node_kind::literal_bool, t.line); set_int(id, 0); return id; }
            if (t.value == "nil")  { advance(); return new_node(node_kind::literal_nil, t.line); }
            if (t.value == "fn")   { return fn_def(); }  // lambda: fn(args) body end
        }
        if (t.kind == token_kind::ident) {
            advance();
            node_id id = new_node(node_kind::ident, t.line);
            set_string(id, t.value);
            return id;
        }
        if (t.kind == token_kind::op && t.value == "(") {
            advance();
            node_id e = expr();
            expect(token_kind::op, ")");
            return e;
        }
        if (t.kind == token_kind::op && t.value == "[") {
            return array_lit();
        }
        if (t.kind == token_kind::op && t.value == "{") {
            return dict_lit();
        }
        std::ostringstream oss;
        oss << "line " << t.line << ": unexpected token '" << t.value << "'";
        throw std::runtime_error(oss.str());
    }

    node_id array_lit() {
        const token& t = expect(token_kind::op, "[");
        node_id id = new_node(node_kind::array_lit, t.line);
        if (!(peek().kind == token_kind::op && peek().value == "]")) {
            add_child(id, expr());
            while (match(token_kind::op, ",")) {
                if (peek().kind == token_kind::op && peek().value == "]") break;
                add_child(id, expr());
            }
        }
        expect(token_kind::op, "]");
        return id;
    }

    node_id dict_lit() {
        const token& t = expect(token_kind::op, "{");
        node_id id = new_node(node_kind::dict_lit, t.line);
        if (!(peek().kind == token_kind::op && peek().value == "}")) {
            parse_dict_entry(id);
            while (match(token_kind::op, ",")) {
                if (peek().kind == token_kind::op && peek().value == "}") break;
                parse_dict_entry(id);
            }
        }
        expect(token_kind::op, "}");
        return id;
    }

    void parse_dict_entry(node_id dict_id) {
        const token& t = peek();
        node_id key_id;
        // 若下一个是 ident + ":", 视作 shorthand key
        if (t.kind == token_kind::ident && pos_ + 1 < tokens_->size()
            && (*tokens_)[pos_ + 1].kind == token_kind::op
            && (*tokens_)[pos_ + 1].value == ":") {
            advance();  // ident
            key_id = new_node(node_kind::literal_string, t.line);
            set_string(key_id, t.value);
            expect(token_kind::op, ":");
        } else if (t.kind == token_kind::string) {
            advance();
            key_id = new_node(node_kind::literal_string, t.line);
            set_string(key_id, t.value);  // lexer 已 unescape
            expect(token_kind::op, ":");
        } else {
            key_id = expr();
            expect(token_kind::op, ":");
        }
        node_id value_id = expr();
        add_child(dict_id, key_id);
        add_child(dict_id, value_id);
    }
};

} // namespace

parser::parser(const std::vector<token>& tokens, ast_pool* pool)
    : tokens_(&tokens), pool_(pool) {}

int32_t parser::parse_program(std::vector<node_id>& out_stmts, std::string& out_error) {
    out_stmts.clear();
    out_error.clear();
    parser_impl impl(*tokens_, pool_);
    return impl.parse(out_stmts, out_error);
}

} // namespace sao::plugins::emma_host
