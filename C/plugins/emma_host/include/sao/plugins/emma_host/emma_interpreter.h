// emma_interpreter.h — Emma 树遍历解释器
//
// 对齐 Python 源: emma_runtime.py 的 _EmmaInterpreter + _EmmaScope +
// _EmmaCallable + _ReturnSignal / _BreakSignal / _ContinueSignal。
//
// C++ 侧的 emma_value 是 std::variant, 作用域是 unordered_map 链, 控制流
// 用异常 (或非局部返回) 模拟 return/break/continue。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/emma_host/emma_parser.h"

namespace sao::plugins::emma_host {

class interpreter;
class scope;
struct callable;

// Emma 值 (对齐 Python EmmaInterpreter 里流通的 Python 对象)
//
// MSVC C++17/20 不允许 std::vector<T> / std::unordered_map<K,T> 里 T 是不完整
// 类型 (GCC/Clang 宽松, 标准前 C++23 是未定义). 用前置声明的 wrapper 结构体
// 打破循环: emma_list / emma_dict 内部持有 vector / map, 但 wrapper 本身可以
// 在这里前置声明, 完整定义留给 .cpp。这样 ABI (extern "C" 层) 与语义都不变。
struct emma_list;
struct emma_dict;

using emma_value = std::variant<
    std::nullptr_t,                                   // nil
    bool,                                             // 布尔
    int64_t,                                          // 整数
    double,                                           // 浮点数
    std::string,                                      // 字符串
    std::shared_ptr<emma_list>,                       // 数组 wrapper
    std::shared_ptr<emma_dict>,                       // 字典 wrapper
    std::shared_ptr<callable>                         // 函数 / lambda
>;

// wrapper 完整定义 (依赖 emma_value 已 alias 完成, 所以放在 alias 之后)
struct emma_list {
    std::vector<emma_value> items;
    emma_list() = default;
    emma_list(std::vector<emma_value> v) : items(std::move(v)) {}
};

struct emma_dict {
    std::unordered_map<std::string, emma_value> items;
    emma_dict() = default;
    emma_dict(std::unordered_map<std::string, emma_value> v) : items(std::move(v)) {}
};

// 作用域链 (对齐 Python _EmmaScope)
class scope {
public:
    explicit scope(std::shared_ptr<scope> parent = nullptr);
    emma_value get(const std::string& name) const;
    void set(const std::string& name, emma_value value);
    void define(const std::string& name, emma_value value);
    bool has(const std::string& name) const;
private:
    std::unordered_map<std::string, emma_value> vars_;
    std::shared_ptr<scope> parent_;
};

// 可调用体 (Emma fn 或注入的 host 回调)
struct callable {
    std::string name;                                 // "" for lambda
    std::vector<std::string> params;
    std::vector<node_id> body;                        // AST id 序列
    std::shared_ptr<scope> closure;                   // 定义时作用域
    std::function<emma_value(std::vector<emma_value>)> host_impl; // 非 null → 走 native
    interpreter* interp = nullptr;
};

// 解释器 (对齐 Python _EmmaInterpreter)
class interpreter {
public:
    interpreter();
    ~interpreter();

    // 注入内置 (print / str / int / float / len / type / ipairs / pairs / range /
    // abs / min / max), 对齐 python _builtins。
    void install_builtins();

    // 注册一个 host 全局 (比如 ctx)。
    void register_global(std::string name, emma_value value);

    // 解释一个 AST 程序。
    int32_t execute(const std::vector<node_id>& stmts, std::string& out_error);

    // 按名字取一个已定义函数, 用于宿主查 on_load / on_enable / on_unload。
    std::shared_ptr<callable> get_function(const std::string& name);

    // 由 host 侧调 Emma 函数 (Emma 的 on_load(ctx) 等)。
    emma_value call_function(const std::shared_ptr<callable>& fn,
                             std::vector<emma_value> args,
                             std::string& out_error);

    // AST 池
    const ast_pool* pool() const;
    void set_pool(const ast_pool* pool);

private:
    struct impl;
    std::unique_ptr<impl> pimpl_;
};

} // namespace sao::plugins::emma_host
