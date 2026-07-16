// test_emma.cpp — smoke test (轻量, 无 Catch2)
//
// 只跑 scope / error 桩; 真功能测走 test_emma_wave3.cpp (Catch2)。

#include "sao/plugins/emma_host/emma_lexer.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_error.h"

#include <cassert>
#include <cstdio>
#include <memory>

int main() {
    using namespace sao::plugins::emma_host;

    // interpreter 构造析构不能崩
    {
        interpreter interp;
        interp.install_builtins();
        interp.register_global("x", int64_t{42});
        // 无 stmts + 无 pool → SAO_ERR_NOT_INITIALIZED
        std::string err;
        std::vector<node_id> stmts;
        int32_t rc = interp.execute(stmts, err);
        assert(rc == SAO_ERR_NOT_INITIALIZED);
    }

    // 设置 pool 后 execute 空 stmts 应 OK
    {
        interpreter interp;
        ast_pool pool;
        interp.set_pool(&pool);
        std::string err;
        std::vector<node_id> stmts;
        int32_t rc = interp.execute(stmts, err);
        assert(rc == SAO_OK);
    }

    // scope 独立测: define / get / set / has 走一遍
    {
        auto s = std::make_shared<scope>();
        s->define("a", int64_t{1});
        assert(s->has("a"));
        auto v = s->get("a");
        assert(std::get<int64_t>(v) == 1);
        auto child = std::make_shared<scope>(s);
        child->set("a", int64_t{2});   // 应写回父作用域
        assert(std::get<int64_t>(s->get("a")) == 2);
    }

    // error status
    emma_error err{};
    err.kind = error_kind::runtime_error;
    err.message = "boom";
    assert(sao_plugins_emma_error_status(&err) == SAO_ERR_OS_CALL_FAILED);

    // lexer smoke
    auto tokens = tokenize_source("let x = 1 + 2");
    assert(!tokens.empty());
    assert(tokens.front().kind == token_kind::keyword);
    assert(tokens.front().value == "let");

    std::printf("emma_host smoke test passed\n");
    return 0;
}
