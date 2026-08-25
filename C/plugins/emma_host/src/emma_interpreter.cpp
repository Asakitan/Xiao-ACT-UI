// emma_interpreter.cpp — Emma tree-walk 解释器实装
//
// 对齐 Python emma_runtime.py _EmmaInterpreter + _EmmaScope + _EmmaCallable。
// AST 通过 ast_pool 索引访问 (parser.cpp 里布局详见 header + parser 内注释)。
//
// 控制流用局部 enum signal + throw:
//   - _ReturnSignal   → runtime_signal{kind=Return, value=...}
//   - _BreakSignal    → runtime_signal{kind=Break}
//   - _ContinueSignal → runtime_signal{kind=Continue}
//   - 一般错误       → std::runtime_error("emma runtime: <msg>")

#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_error.h"
#include "sao/plugins/emma_host/emma_parser.h"
#include "sao/plugins/emma_host/emma_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>

namespace sao::plugins::emma_host {

// ── scope 实现 (对齐 Python _EmmaScope) ──
scope::scope(std::shared_ptr<scope> parent) : parent_(std::move(parent)) {}

emma_value scope::get(const std::string& name) const {
    auto it = vars_.find(name);
    if (it != vars_.end())
        return it->second;
    if (parent_)
        return parent_->get(name);
    return nullptr;
}

void scope::set(const std::string& name, emma_value value) {
    auto it = vars_.find(name);
    if (it != vars_.end()) {
        it->second = std::move(value);
        return;
    }
    if (parent_ && parent_->has(name)) {
        parent_->set(name, std::move(value));
        return;
    }
    vars_[name] = std::move(value);
}

void scope::define(const std::string& name, emma_value value) {
    vars_[name] = std::move(value);
}

bool scope::has(const std::string& name) const {
    if (vars_.count(name))
        return true;
    if (parent_)
        return parent_->has(name);
    return false;
}

// ── interpreter 实现 ──
namespace {

// 内部控制流信号
enum class signal_kind { none, ret, brk, cont };

struct runtime_signal {
    signal_kind kind = signal_kind::none;
    emma_value value = nullptr;
};

// 块 wrapper 标记 (parser 侧一致)
constexpr const char* BLOCK_MARK = "__block__";

bool truthy(const emma_value& v) {
    if (std::holds_alternative<std::nullptr_t>(v))
        return false;
    if (std::holds_alternative<bool>(v))
        return std::get<bool>(v);
    if (std::holds_alternative<int64_t>(v))
        return std::get<int64_t>(v) != 0;
    if (std::holds_alternative<double>(v))
        return std::get<double>(v) != 0.0;
    if (std::holds_alternative<std::string>(v))
        return !std::get<std::string>(v).empty();
    if (std::holds_alternative<std::shared_ptr<emma_list>>(v)) {
        auto& a = std::get<std::shared_ptr<emma_list>>(v);
        return a && !a->items.empty();
    }
    if (std::holds_alternative<std::shared_ptr<emma_dict>>(v)) {
        auto& d = std::get<std::shared_ptr<emma_dict>>(v);
        return d && !d->items.empty();
    }
    if (std::holds_alternative<std::shared_ptr<callable>>(v)) {
        return static_cast<bool>(std::get<std::shared_ptr<callable>>(v));
    }
    return false;
}

double to_number_double(const emma_value& v) {
    if (std::holds_alternative<int64_t>(v))
        return static_cast<double>(std::get<int64_t>(v));
    if (std::holds_alternative<double>(v))
        return std::get<double>(v);
    if (std::holds_alternative<bool>(v))
        return std::get<bool>(v) ? 1.0 : 0.0;
    if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stod(std::get<std::string>(v));
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

int64_t to_number_int(const emma_value& v) {
    if (std::holds_alternative<int64_t>(v))
        return std::get<int64_t>(v);
    if (std::holds_alternative<double>(v))
        return static_cast<int64_t>(std::get<double>(v));
    if (std::holds_alternative<bool>(v))
        return std::get<bool>(v) ? 1 : 0;
    if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stoll(std::get<std::string>(v));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

// 全 int? (两侧都是 int)
bool both_int(const emma_value& a, const emma_value& b) {
    return std::holds_alternative<int64_t>(a) && std::holds_alternative<int64_t>(b);
}

// numeric fallback (含 double)
bool is_numeric(const emma_value& v) {
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v) ||
           std::holds_alternative<bool>(v);
}

} // namespace

// 用户可访问: emma_value → 字符串 (对齐 python str())
std::string emma_value_to_string(const emma_value& v) {
    if (std::holds_alternative<std::nullptr_t>(v))
        return "nil";
    if (std::holds_alternative<bool>(v))
        return std::get<bool>(v) ? "true" : "false";
    if (std::holds_alternative<int64_t>(v))
        return std::to_string(std::get<int64_t>(v));
    if (std::holds_alternative<double>(v)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", std::get<double>(v));
        return buf;
    }
    if (std::holds_alternative<std::string>(v))
        return std::get<std::string>(v);
    if (std::holds_alternative<std::shared_ptr<emma_list>>(v)) {
        auto& a = std::get<std::shared_ptr<emma_list>>(v);
        if (!a)
            return "[]";
        std::string s = "[";
        for (size_t i = 0; i < a->items.size(); ++i) {
            if (i)
                s += ", ";
            s += emma_value_to_string(a->items[i]);
        }
        s += "]";
        return s;
    }
    if (std::holds_alternative<std::shared_ptr<emma_dict>>(v)) {
        auto& d = std::get<std::shared_ptr<emma_dict>>(v);
        if (!d)
            return "{}";
        std::string s = "{";
        bool first = true;
        for (auto& kv : d->items) {
            if (!first)
                s += ", ";
            s += kv.first;
            s += ": ";
            s += emma_value_to_string(kv.second);
            first = false;
        }
        s += "}";
        return s;
    }
    if (std::holds_alternative<std::shared_ptr<callable>>(v)) {
        auto& c = std::get<std::shared_ptr<callable>>(v);
        if (!c)
            return "<fn:nil>";
        return c->name.empty() ? "<fn:lambda>" : ("<fn:" + c->name + ">");
    }
    return "<?>";
}

// ── interpreter::impl ──
struct interpreter::impl {
    std::shared_ptr<scope> global;
    ast_pool* pool = nullptr;
    std::vector<std::string> call_stack;

    impl() : global(std::make_shared<scope>()) {}

    // ── 递归 eval / exec ──
    emma_value eval_node(node_id id, const std::shared_ptr<scope>& scp);
    void exec_node(node_id id, const std::shared_ptr<scope>& scp);
    void exec_block(node_id block_id, const std::shared_ptr<scope>& scp);
    emma_value call_fn(const std::shared_ptr<callable>& fn, std::vector<emma_value> args);
    emma_value eval_binop(const std::string& op, emma_value left, emma_value right);
    void do_assign(node_id target_id, emma_value value, const std::shared_ptr<scope>& scp);

    // 帮 exec 层从 expr 结果 continue 传播控制流
    // (throw 直接跨帧)
};

interpreter::interpreter() : pimpl_(std::make_unique<impl>()) {}
interpreter::~interpreter() {
    release_stdlib_state(this);
}

void interpreter::install_builtins() {
    // 具体 builtin 装载由 emma_stdlib.cpp 的 sao_plugins_emma_install_stdlib 完成。
    // 这里保留 API 兼容 (不同调用点可能只调本方法), 转发给 stdlib 装载。
    sao_plugins_emma_install_stdlib(this);
}

void interpreter::register_global(std::string name, emma_value value) {
    pimpl_->global->define(std::move(name), std::move(value));
}

std::shared_ptr<callable> interpreter::get_function(const std::string& name) {
    emma_value v = pimpl_->global->get(name);
    if (auto* cb = std::get_if<std::shared_ptr<callable>>(&v)) {
        return *cb;
    }
    return nullptr;
}

emma_value interpreter::get_global(const std::string& name) const {
    return pimpl_->global->get(name);
}

const ast_pool* interpreter::pool() const {
    return pimpl_->pool;
}
ast_pool* interpreter::mutable_pool() const {
    return pimpl_->pool;
}
void interpreter::set_pool(ast_pool* pool) {
    pimpl_->pool = pool;
}

int32_t interpreter::execute(const std::vector<node_id>& stmts, std::string& out_error) {
    return execute(stmts, out_error, nullptr);
}

int32_t interpreter::execute(const std::vector<node_id>& stmts, std::string& out_error,
                             emma_error* out_structured_error) {
    if (out_structured_error != nullptr)
        *out_structured_error = {};
    if (pimpl_->pool == nullptr) {
        out_error = "interpreter: no ast_pool set";
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
        }
        return SAO_ERR_NOT_INITIALIZED;
    }
    try {
        for (node_id id : stmts) {
            pimpl_->exec_node(id, pimpl_->global);
        }
        return SAO_OK;
    } catch (const emma_exception& error) {
        out_error = error.what();
        if (out_structured_error != nullptr) {
            *out_structured_error = error.error();
        }
        return sao_plugins_emma_error_status(&error.error());
    } catch (const std::exception& e) {
        out_error = e.what();
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
            set_error_location_from_message(*out_structured_error);
        }
        return SAO_ERR_OS_CALL_FAILED;
    } catch (runtime_signal&) {
        out_error = "emma: uncaught return/break/continue at top level";
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
        }
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        out_error = "emma: unknown exception";
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

emma_value interpreter::call_function(const std::shared_ptr<callable>& fn,
                                      std::vector<emma_value> args, std::string& out_error) {
    return call_function(fn, std::move(args), out_error, nullptr);
}

emma_value interpreter::call_function(const std::shared_ptr<callable>& fn,
                                      std::vector<emma_value> args, std::string& out_error,
                                      emma_error* out_structured_error) {
    if (out_structured_error != nullptr)
        *out_structured_error = {};
    if (!fn) {
        out_error = "call_function: null callable";
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
        }
        return nullptr;
    }
    try {
        return pimpl_->call_fn(fn, std::move(args));
    } catch (const emma_exception& error) {
        out_error = error.what();
        if (out_structured_error != nullptr) {
            *out_structured_error = error.error();
        }
        return nullptr;
    } catch (const std::exception& e) {
        out_error = e.what();
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
            set_error_location_from_message(*out_structured_error);
        }
        return nullptr;
    } catch (...) {
        out_error = "call_function: unknown exception";
        if (out_structured_error != nullptr) {
            out_structured_error->kind = error_kind::runtime_error;
            out_structured_error->message = out_error;
        }
        return nullptr;
    }
}

// ── impl 递归 ──
void interpreter::impl::exec_block(node_id block_id, const std::shared_ptr<scope>& scp) {
    // block 是 expr_stmt with strings=="__block__" children=stmts
    if (pool->kinds[block_id] != node_kind::expr_stmt) {
        // 单条 stmt 也走一下 exec (不太可能来这)
        exec_node(block_id, scp);
        return;
    }
    if (pool->strings[block_id] != BLOCK_MARK) {
        // 不是块 wrapper, 而是普通 expr_stmt
        exec_node(block_id, scp);
        return;
    }
    for (node_id child : pool->children[block_id]) {
        exec_node(child, scp);
    }
}

void interpreter::impl::exec_node(node_id id, const std::shared_ptr<scope>& scp) {
    node_kind k = pool->kinds[id];
    switch (k) {
    case node_kind::let: {
        emma_value v = eval_node(pool->children[id][0], scp);
        scp->define(pool->strings[id], std::move(v));
        return;
    }
    case node_kind::fn_def: {
        // 定义函数, 存入作用域
        int64_t nparam = pool->ints[id];
        std::vector<std::string> params;
        const auto& kids = pool->children[id];
        for (int64_t i = 0; i < nparam; ++i) {
            params.push_back(pool->strings[kids[static_cast<size_t>(i)]]);
        }
        node_id body_id = kids[static_cast<size_t>(nparam)];
        auto fn = std::make_shared<callable>();
        fn->name = pool->strings[id];
        fn->params = std::move(params);
        fn->body = {body_id};
        fn->closure = scp;
        // fn->interp 由 call_fn 通过 lambda 拿到 (下面的 host_impl 是可选的; 我们通过 impl
        // 状态直接调 call_fn)
        if (!fn->name.empty()) {
            scp->define(fn->name, fn);
        }
        return;
    }
    case node_kind::assign: {
        emma_value v = eval_node(pool->children[id][1], scp);
        do_assign(pool->children[id][0], std::move(v), scp);
        return;
    }
    case node_kind::return_stmt: {
        emma_value v = nullptr;
        if (!pool->children[id].empty()) {
            v = eval_node(pool->children[id][0], scp);
        }
        runtime_signal sig;
        sig.kind = signal_kind::ret;
        sig.value = std::move(v);
        throw sig;
    }
    case node_kind::break_stmt: {
        runtime_signal sig;
        sig.kind = signal_kind::brk;
        throw sig;
    }
    case node_kind::continue_stmt: {
        runtime_signal sig;
        sig.kind = signal_kind::cont;
        throw sig;
    }
    case node_kind::if_stmt: {
        // children: cond0, block0, cond1, block1, ..., condN-1, blockN-1, else_block
        const auto& kids = pool->children[id];
        int64_t branch_count = pool->ints[id];
        for (int64_t i = 0; i < branch_count; ++i) {
            node_id cond_id = kids[static_cast<size_t>(2 * i)];
            node_id body_id = kids[static_cast<size_t>(2 * i + 1)];
            emma_value cv = eval_node(cond_id, scp);
            if (truthy(cv)) {
                exec_block(body_id, scp);
                return;
            }
        }
        // else block
        if (kids.size() > static_cast<size_t>(2 * branch_count)) {
            node_id else_id = kids.back();
            exec_block(else_id, scp);
        }
        return;
    }
    case node_kind::while_stmt: {
        node_id cond_id = pool->children[id][0];
        node_id body_id = pool->children[id][1];
        int guard = 0;
        while (truthy(eval_node(cond_id, scp)) && guard < 1'000'000) {
            try {
                exec_block(body_id, scp);
            } catch (runtime_signal& sig) {
                if (sig.kind == signal_kind::brk)
                    break;
                if (sig.kind == signal_kind::cont) {
                    ++guard;
                    continue;
                }
                throw;
            }
            ++guard;
        }
        return;
    }
    case node_kind::for_stmt: {
        const std::string& var_name = pool->strings[id];
        node_id iter_id = pool->children[id][0];
        node_id body_id = pool->children[id][1];
        emma_value iter_v = eval_node(iter_id, scp);
        // 支持 array / dict / string
        if (std::holds_alternative<std::shared_ptr<emma_list>>(iter_v)) {
            auto& arr = std::get<std::shared_ptr<emma_list>>(iter_v);
            if (!arr)
                return;
            for (auto& item : arr->items) {
                scp->define(var_name, item);
                try {
                    exec_block(body_id, scp);
                } catch (runtime_signal& sig) {
                    if (sig.kind == signal_kind::brk)
                        return;
                    if (sig.kind == signal_kind::cont)
                        continue;
                    throw;
                }
            }
        } else if (std::holds_alternative<std::shared_ptr<emma_dict>>(iter_v)) {
            auto& d = std::get<std::shared_ptr<emma_dict>>(iter_v);
            if (!d)
                return;
            for (auto& kv : d->items) {
                scp->define(var_name, kv.first);
                try {
                    exec_block(body_id, scp);
                } catch (runtime_signal& sig) {
                    if (sig.kind == signal_kind::brk)
                        return;
                    if (sig.kind == signal_kind::cont)
                        continue;
                    throw;
                }
            }
        } else if (std::holds_alternative<std::string>(iter_v)) {
            const std::string& s = std::get<std::string>(iter_v);
            for (char c : s) {
                scp->define(var_name, std::string(1, c));
                try {
                    exec_block(body_id, scp);
                } catch (runtime_signal& sig) {
                    if (sig.kind == signal_kind::brk)
                        return;
                    if (sig.kind == signal_kind::cont)
                        continue;
                    throw;
                }
            }
        }
        return;
    }
    case node_kind::expr_stmt: {
        // 可能是块 wrapper (顶层 stmts 集合语义)
        if (pool->strings[id] == BLOCK_MARK) {
            for (node_id child : pool->children[id]) {
                exec_node(child, scp);
            }
            return;
        }
        // 普通表达式语句: eval 并丢弃
        if (!pool->children[id].empty()) {
            (void)eval_node(pool->children[id][0], scp);
        }
        return;
    }
    default: {
        // 其他 node_kind (literal 等) 出现在 stmt 位置: 视作表达式 eval 丢弃
        (void)eval_node(id, scp);
        return;
    }
    }
}

emma_value interpreter::impl::eval_node(node_id id, const std::shared_ptr<scope>& scp) {
    node_kind kind = pool->kinds[id];
    switch (kind) {
    case node_kind::literal_nil:
        return nullptr;
    case node_kind::literal_bool:
        return static_cast<bool>(pool->ints[id] != 0);
    case node_kind::literal_int:
        return pool->ints[id];
    case node_kind::literal_float:
        return pool->floats[id];
    case node_kind::literal_string:
        return pool->strings[id];
    case node_kind::ident: {
        return scp->get(pool->strings[id]);
    }
    case node_kind::binop: {
        const std::string& op = pool->strings[id];
        // short-circuit: and / or
        if (op == "and") {
            emma_value left = eval_node(pool->children[id][0], scp);
            if (!truthy(left))
                return left;
            return eval_node(pool->children[id][1], scp);
        }
        if (op == "or") {
            emma_value left = eval_node(pool->children[id][0], scp);
            if (truthy(left))
                return left;
            return eval_node(pool->children[id][1], scp);
        }
        emma_value left = eval_node(pool->children[id][0], scp);
        emma_value right = eval_node(pool->children[id][1], scp);
        return eval_binop(op, std::move(left), std::move(right));
    }
    case node_kind::unaryop: {
        const std::string& op = pool->strings[id];
        emma_value inner = eval_node(pool->children[id][0], scp);
        if (op == "-") {
            if (std::holds_alternative<int64_t>(inner))
                return -std::get<int64_t>(inner);
            if (std::holds_alternative<double>(inner))
                return -std::get<double>(inner);
            if (std::holds_alternative<bool>(inner))
                return static_cast<int64_t>(std::get<bool>(inner) ? -1 : 0);
            return static_cast<int64_t>(0);
        }
        if (op == "not") {
            return static_cast<bool>(!truthy(inner));
        }
        return nullptr;
    }
    case node_kind::attr: {
        emma_value obj = eval_node(pool->children[id][0], scp);
        const std::string& name = pool->strings[id];
        if (std::holds_alternative<std::shared_ptr<emma_dict>>(obj)) {
            auto& d = std::get<std::shared_ptr<emma_dict>>(obj);
            if (!d)
                return nullptr;
            auto it = d->items.find(name);
            if (it != d->items.end())
                return it->second;
            return nullptr;
        }
        return nullptr;
    }
    case node_kind::index: {
        emma_value obj = eval_node(pool->children[id][0], scp);
        emma_value key = eval_node(pool->children[id][1], scp);
        if (std::holds_alternative<std::shared_ptr<emma_list>>(obj)) {
            auto& arr = std::get<std::shared_ptr<emma_list>>(obj);
            if (!arr)
                return nullptr;
            int64_t idx = to_number_int(key);
            if (idx < 0)
                idx += static_cast<int64_t>(arr->items.size());
            if (idx < 0 || idx >= static_cast<int64_t>(arr->items.size()))
                return nullptr;
            return arr->items[static_cast<size_t>(idx)];
        }
        if (std::holds_alternative<std::shared_ptr<emma_dict>>(obj)) {
            auto& d = std::get<std::shared_ptr<emma_dict>>(obj);
            if (!d)
                return nullptr;
            std::string k = std::holds_alternative<std::string>(key) ? std::get<std::string>(key)
                                                                     : emma_value_to_string(key);
            auto it = d->items.find(k);
            if (it != d->items.end())
                return it->second;
            return nullptr;
        }
        if (std::holds_alternative<std::string>(obj)) {
            const std::string& s = std::get<std::string>(obj);
            int64_t idx = to_number_int(key);
            if (idx < 0)
                idx += static_cast<int64_t>(s.size());
            if (idx < 0 || idx >= static_cast<int64_t>(s.size()))
                return std::string();
            return std::string(1, s[static_cast<size_t>(idx)]);
        }
        return nullptr;
    }
    case node_kind::call: {
        const auto& kids = pool->children[id];
        emma_value callee = eval_node(kids[0], scp);
        std::vector<emma_value> args;
        args.reserve(kids.size() - 1);
        for (size_t i = 1; i < kids.size(); ++i) {
            args.push_back(eval_node(kids[i], scp));
        }
        if (std::holds_alternative<std::shared_ptr<callable>>(callee)) {
            auto& fn = std::get<std::shared_ptr<callable>>(callee);
            if (!fn)
                return nullptr;
            return call_fn(fn, std::move(args));
        }
        // 非可调用值
        std::ostringstream oss;
        oss << "emma: value is not callable at line " << pool->lines[id];
        throw std::runtime_error(oss.str());
    }
    case node_kind::array_lit: {
        auto arr = std::make_shared<emma_list>();
        arr->items.reserve(pool->children[id].size());
        for (node_id c : pool->children[id]) {
            arr->items.push_back(eval_node(c, scp));
        }
        return arr;
    }
    case node_kind::dict_lit: {
        auto d = std::make_shared<emma_dict>();
        const auto& kids = pool->children[id];
        for (size_t i = 0; i + 1 < kids.size(); i += 2) {
            emma_value k = eval_node(kids[i], scp);
            emma_value v = eval_node(kids[i + 1], scp);
            std::string key_s = std::holds_alternative<std::string>(k) ? std::get<std::string>(k)
                                                                       : emma_value_to_string(k);
            d->items[std::move(key_s)] = std::move(v);
        }
        return d;
    }
    case node_kind::fn_def: {
        // lambda / 局部 fn 表达式化
        int64_t nparam = pool->ints[id];
        std::vector<std::string> params;
        const auto& kids = pool->children[id];
        for (int64_t i = 0; i < nparam; ++i) {
            params.push_back(pool->strings[kids[static_cast<size_t>(i)]]);
        }
        node_id body_id = kids[static_cast<size_t>(nparam)];
        auto fn = std::make_shared<callable>();
        fn->name = pool->strings[id];
        fn->params = std::move(params);
        fn->body = {body_id};
        fn->closure = scp;
        return fn;
    }
    default:
        return nullptr;
    }
}

void interpreter::impl::do_assign(node_id target_id, emma_value value,
                                  const std::shared_ptr<scope>& scp) {
    node_kind k = pool->kinds[target_id];
    if (k == node_kind::ident) {
        scp->set(pool->strings[target_id], std::move(value));
        return;
    }
    if (k == node_kind::attr) {
        emma_value obj = eval_node(pool->children[target_id][0], scp);
        const std::string& name = pool->strings[target_id];
        if (std::holds_alternative<std::shared_ptr<emma_dict>>(obj)) {
            auto& d = std::get<std::shared_ptr<emma_dict>>(obj);
            if (d)
                d->items[name] = std::move(value);
        }
        return;
    }
    if (k == node_kind::index) {
        emma_value obj = eval_node(pool->children[target_id][0], scp);
        emma_value key = eval_node(pool->children[target_id][1], scp);
        if (std::holds_alternative<std::shared_ptr<emma_list>>(obj)) {
            auto& arr = std::get<std::shared_ptr<emma_list>>(obj);
            if (arr) {
                int64_t idx = to_number_int(key);
                if (idx < 0)
                    idx += static_cast<int64_t>(arr->items.size());
                if (idx >= 0 && idx < static_cast<int64_t>(arr->items.size())) {
                    arr->items[static_cast<size_t>(idx)] = std::move(value);
                }
            }
        } else if (std::holds_alternative<std::shared_ptr<emma_dict>>(obj)) {
            auto& d = std::get<std::shared_ptr<emma_dict>>(obj);
            if (d) {
                std::string k_s = std::holds_alternative<std::string>(key)
                                      ? std::get<std::string>(key)
                                      : emma_value_to_string(key);
                d->items[std::move(k_s)] = std::move(value);
            }
        }
    }
}

emma_value interpreter::impl::call_fn(const std::shared_ptr<callable>& fn,
                                      std::vector<emma_value> args) {
    call_stack.push_back(fn->name.empty() ? "<lambda>" : fn->name);
    try {
        emma_value result = nullptr;
        if (fn->host_impl) {
            result = fn->host_impl(std::move(args));
        } else {
            auto call_scope = std::make_shared<scope>(fn->closure);
            for (size_t i = 0; i < fn->params.size(); ++i) {
                emma_value argument = i < args.size() ? std::move(args[i]) : emma_value(nullptr);
                call_scope->define(fn->params[i], std::move(argument));
            }
            if (!fn->body.empty()) {
                try {
                    exec_block(fn->body[0], call_scope);
                } catch (runtime_signal& signal) {
                    if (signal.kind == signal_kind::ret) {
                        result = std::move(signal.value);
                    } else {
                        throw std::runtime_error("emma: break/continue escaped function body");
                    }
                }
            }
        }
        call_stack.pop_back();
        return result;
    } catch (const emma_exception& exception) {
        emma_error error = exception.error();
        if (error.kind == error_kind::runtime_error && error.call_stack.empty()) {
            for (auto iterator = call_stack.rbegin(); iterator != call_stack.rend(); ++iterator) {
                if (!error.call_stack.empty())
                    error.call_stack += " <- ";
                error.call_stack += *iterator;
            }
        }
        call_stack.pop_back();
        throw emma_exception(std::move(error));
    } catch (const std::exception& exception) {
        emma_error error;
        error.kind = error_kind::runtime_error;
        error.message = exception.what();
        set_error_location_from_message(error);
        for (auto iterator = call_stack.rbegin(); iterator != call_stack.rend(); ++iterator) {
            if (!error.call_stack.empty())
                error.call_stack += " <- ";
            error.call_stack += *iterator;
        }
        call_stack.pop_back();
        throw emma_exception(std::move(error));
    } catch (...) {
        call_stack.pop_back();
        throw;
    }
}

emma_value interpreter::impl::eval_binop(const std::string& op, emma_value left, emma_value right) {
    // 字符串拼接 (.. 或 + 只要一侧是字符串)
    if (op == "..") {
        return emma_value_to_string(left) + emma_value_to_string(right);
    }
    if (op == "+" &&
        (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right))) {
        return emma_value_to_string(left) + emma_value_to_string(right);
    }
    // 相等/不等: 直接 variant 比较 (但要考虑 int/double 混合)
    if (op == "==" || op == "!=") {
        bool eq;
        if (both_int(left, right)) {
            eq = std::get<int64_t>(left) == std::get<int64_t>(right);
        } else if (is_numeric(left) && is_numeric(right)) {
            eq = std::abs(to_number_double(left) - to_number_double(right)) < 1e-12;
        } else if (std::holds_alternative<std::nullptr_t>(left) &&
                   std::holds_alternative<std::nullptr_t>(right)) {
            eq = true;
        } else if (std::holds_alternative<std::string>(left) &&
                   std::holds_alternative<std::string>(right)) {
            eq = std::get<std::string>(left) == std::get<std::string>(right);
        } else if (std::holds_alternative<bool>(left) && std::holds_alternative<bool>(right)) {
            eq = std::get<bool>(left) == std::get<bool>(right);
        } else if (left.index() != right.index()) {
            eq = false;
        } else {
            eq = false;
        }
        return op == "==" ? eq : !eq;
    }
    // 数值二元
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (both_int(left, right) && op != "/") {
            int64_t l = std::get<int64_t>(left);
            int64_t r = std::get<int64_t>(right);
            if (op == "+")
                return l + r;
            if (op == "-")
                return l - r;
            if (op == "*")
                return l * r;
            if (op == "%")
                return r == 0 ? static_cast<int64_t>(0) : (l % r);
        }
        double l = to_number_double(left);
        double r = to_number_double(right);
        if (op == "+")
            return l + r;
        if (op == "-")
            return l - r;
        if (op == "*")
            return l * r;
        if (op == "/")
            return r == 0.0 ? 0.0 : (l / r);
        if (op == "%")
            return r == 0.0 ? 0.0 : std::fmod(l, r);
    }
    // 比较
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (std::holds_alternative<std::string>(left) &&
            std::holds_alternative<std::string>(right)) {
            const auto& ls = std::get<std::string>(left);
            const auto& rs = std::get<std::string>(right);
            if (op == "<")
                return static_cast<bool>(ls < rs);
            if (op == ">")
                return static_cast<bool>(ls > rs);
            if (op == "<=")
                return static_cast<bool>(ls <= rs);
            if (op == ">=")
                return static_cast<bool>(ls >= rs);
        }
        double l = to_number_double(left);
        double r = to_number_double(right);
        if (op == "<")
            return static_cast<bool>(l < r);
        if (op == ">")
            return static_cast<bool>(l > r);
        if (op == "<=")
            return static_cast<bool>(l <= r);
        if (op == ">=")
            return static_cast<bool>(l >= r);
    }
    return nullptr;
}

} // namespace sao::plugins::emma_host
