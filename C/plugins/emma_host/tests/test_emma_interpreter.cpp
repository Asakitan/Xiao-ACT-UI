// Emma tree-walk interpreter behavior tests.

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_lexer.h"
#include "sao/plugins/emma_host/emma_parser.h"
#include "sao/plugins/emma_host/emma_stdlib.h"

#include <memory>
#include <string>
#include <vector>

using namespace sao::plugins::emma_host;

namespace {

struct log_capture {
    std::string text;

    static void sink(const char* utf8, void* user_data) {
        auto* self = static_cast<log_capture*>(user_data);
        self->text += utf8;
        self->text.push_back('\n');
    }
};

emma_value execute_and_get_result(const std::string& source, log_capture* capture = nullptr) {
    auto interp = std::make_unique<interpreter>();
    ast_pool pool;
    interp->set_pool(&pool);
    sao_plugins_emma_install_stdlib(interp.get());

    if (capture != nullptr) {
        sao_plugins_emma_install_log_bridge(interp.get(), &log_capture::sink, capture);
    }

    const std::vector<token> tokens = tokenize_source(source);
    parser source_parser(tokens, &pool);
    std::vector<node_id> statements;
    std::string error;
    int32_t rc = source_parser.parse_program(statements, error);
    if (rc != SAO_OK) {
        FAIL("parse failed: " << error);
    }
    rc = interp->execute(statements, error);
    if (rc != SAO_OK) {
        FAIL("execute failed: " << error);
    }

    const auto main_function = interp->get_function("__main__");
    if (main_function != nullptr) {
        std::string call_error;
        return interp->call_function(main_function, {}, call_error);
    }
    return emma_value(nullptr);
}

int64_t as_int(const emma_value& value) {
    if (std::holds_alternative<int64_t>(value)) {
        return std::get<int64_t>(value);
    }
    if (std::holds_alternative<double>(value)) {
        return static_cast<int64_t>(std::get<double>(value));
    }
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? 1 : 0;
    }
    return 0;
}

std::string as_string(const emma_value& value) {
    if (std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(value);
    }
    if (std::holds_alternative<int64_t>(value)) {
        return std::to_string(std::get<int64_t>(value));
    }
    return {};
}

} // namespace

TEST_CASE("emma_execute_hello_world", "[emma][interpreter]") {
    log_capture capture;
    execute_and_get_result(R"emma(
        print("hello")
    )emma", &capture);
    REQUIRE(capture.text.find("hello") != std::string::npos);
}

TEST_CASE("emma_execute_arithmetic", "[emma][interpreter]") {
    const emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let x = 1 + 2
            return x
        end
    )emma");
    REQUIRE(as_int(result) == 3);
}

TEST_CASE("emma_execute_if_else", "[emma][interpreter]") {
    emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let x = 10
            if x > 5
                return "big"
            elif x > 0
                return "small"
            else
                return "zero"
            end
        end
    )emma");
    REQUIRE(as_string(result) == "big");

    result = execute_and_get_result(R"emma(
        fn __main__()
            let x = 3
            if x > 5
                return "big"
            elif x > 0
                return "small"
            else
                return "zero"
            end
        end
    )emma");
    REQUIRE(as_string(result) == "small");

    result = execute_and_get_result(R"emma(
        fn __main__()
            let x = -1
            if x > 5
                return "big"
            elif x > 0
                return "small"
            else
                return "zero"
            end
        end
    )emma");
    REQUIRE(as_string(result) == "zero");
}

TEST_CASE("emma_execute_while_loop", "[emma][interpreter]") {
    const emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let sum = 0
            let i = 1
            while i <= 10
                sum = sum + i
                i = i + 1
            end
            return sum
        end
    )emma");
    REQUIRE(as_int(result) == 55);
}

TEST_CASE("emma_execute_fn_call", "[emma][interpreter]") {
    const emma_value result = execute_and_get_result(R"emma(
        fn square(x)
            return x * x
        end
        fn __main__()
            let a = square(4)
            let b = square(5)
            return a + b
        end
    )emma");
    REQUIRE(as_int(result) == 41);
}

TEST_CASE("emma_execute_list_indexing", "[emma][interpreter]") {
    emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let xs = [10, 20, 30, 40]
            return xs[2]
        end
    )emma");
    REQUIRE(as_int(result) == 30);

    result = execute_and_get_result(R"emma(
        fn __main__()
            let xs = [10, 20, 30, 40]
            return len(xs)
        end
    )emma");
    REQUIRE(as_int(result) == 4);

    result = execute_and_get_result(R"emma(
        fn __main__()
            let xs = [10, 20, 30, 40]
            return xs[-1]
        end
    )emma");
    REQUIRE(as_int(result) == 40);
}

TEST_CASE("emma_execute_dict_access", "[emma][interpreter]") {
    emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let d = {name: "sword", damage: 42}
            return d.damage
        end
    )emma");
    REQUIRE(as_int(result) == 42);

    result = execute_and_get_result(R"emma(
        fn __main__()
            let d = {name: "sword", damage: 42}
            return d["name"]
        end
    )emma");
    REQUIRE(as_string(result) == "sword");

    result = execute_and_get_result(R"emma(
        fn __main__()
            let d = {}
            d["a"] = 1
            d["b"] = 2
            d["c"] = 3
            return len(d)
        end
    )emma");
    REQUIRE(as_int(result) == 3);
}

TEST_CASE("emma_for_range_sum", "[emma][interpreter]") {
    const emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let sum = 0
            for i in range(1, 6)
                sum = sum + i
            end
            return sum
        end
    )emma");
    REQUIRE(as_int(result) == 15);
}

TEST_CASE("emma_string_concat", "[emma][interpreter]") {
    const emma_value result = execute_and_get_result(R"emma(
        fn __main__()
            let a = "hello"
            let b = "world"
            return a .. " " .. b
        end
    )emma");
    REQUIRE(as_string(result) == "hello world");
}
