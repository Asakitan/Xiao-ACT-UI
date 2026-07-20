// Lightweight Emma host smoke test without Catch2.

#include "sao/plugins/emma_host/emma_error.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_lexer.h"

#include <cassert>
#include <cstdio>
#include <memory>

int main() {
    using namespace sao::plugins::emma_host;

    {
        interpreter interp;
        interp.install_builtins();
        interp.register_global("x", int64_t{42});
        std::string error;
        std::vector<node_id> statements;
        const int32_t rc = interp.execute(statements, error);
        assert(rc == SAO_ERR_NOT_INITIALIZED);
    }

    {
        interpreter interp;
        ast_pool pool;
        interp.set_pool(&pool);
        std::string error;
        std::vector<node_id> statements;
        const int32_t rc = interp.execute(statements, error);
        assert(rc == SAO_OK);
    }

    {
        auto parent = std::make_shared<scope>();
        parent->define("a", int64_t{1});
        assert(parent->has("a"));
        assert(std::get<int64_t>(parent->get("a")) == 1);
        auto child = std::make_shared<scope>(parent);
        child->set("a", int64_t{2});
        assert(std::get<int64_t>(parent->get("a")) == 2);
    }

    emma_error error{};
    error.kind = error_kind::runtime_error;
    error.message = "boom";
    assert(sao_plugins_emma_error_status(&error) == SAO_ERR_OS_CALL_FAILED);

    const auto tokens = tokenize_source("let x = 1 + 2");
    assert(!tokens.empty());
    assert(tokens.front().kind == token_kind::keyword);
    assert(tokens.front().value == "let");

    std::printf("emma_host smoke test passed\n");
    return 0;
}
