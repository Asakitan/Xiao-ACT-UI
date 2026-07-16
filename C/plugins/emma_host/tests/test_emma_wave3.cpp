// test_emma_wave3.cpp — Wave 3 tree-walk Emma 解释器功能测试
//
// 覆盖 hello world / 算术 / if-else / while / fn call / list index / dict access。

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/emma_host/emma_lexer.h"
#include "sao/plugins/emma_host/emma_parser.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_stdlib.h"

#include <memory>
#include <string>
#include <vector>

using namespace sao::plugins::emma_host;

namespace {

// 收集 print 输出到 log_capture (通过 log_bridge)
struct log_capture {
    std::string text;
    static void sink(const char* utf8, void* ud) {
        auto* self = static_cast<log_capture*>(ud);
        self->text += utf8;
        self->text.push_back('\n');
    }
};

// 帮 test 简化: 编译并执行 source, 拿最后一个 stmt 的值 (若是 expr_stmt 直接返)
// 若脚本里有 return 语句在顶层, 得抓不到 (return 在 block 里才有意义);
// 所以 test 里用: fn __main__() ... return X end __main__() 模式取值。
emma_value execute_and_get_result(const std::string& source, log_capture* cap = nullptr) {
    auto interp = std::make_unique<interpreter>();
    ast_pool pool;
    interp->set_pool(&pool);
    sao_plugins_emma_install_stdlib(interp.get());

    if (cap) {
        sao_plugins_emma_install_log_bridge(interp.get(), &log_capture::sink, cap);
    }

    std::vector<token> tokens = tokenize_source(source);
    parser p(tokens, &pool);
    std::vector<node_id> stmts;
    std::string err;
    int32_t rc = p.parse_program(stmts, err);
    if (rc != SAO_OK) {
        FAIL("parse failed: " << err);
    }
    rc = interp->execute(stmts, err);
    if (rc != SAO_OK) {
        FAIL("execute failed: " << err);
    }
    // 若脚本定义了 fn __main__, 调之取结果
    auto main_fn = interp->get_function("__main__");
    if (main_fn) {
        std::string call_err;
        return interp->call_function(main_fn, {}, call_err);
    }
    // 否则找一个 result global
    return emma_value(nullptr);
}

int64_t as_int(const emma_value& v) {
    if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
    if (std::holds_alternative<double>(v))  return static_cast<int64_t>(std::get<double>(v));
    if (std::holds_alternative<bool>(v))    return std::get<bool>(v) ? 1 : 0;
    return 0;
}

std::string as_str(const emma_value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<int64_t>(v))    return std::to_string(std::get<int64_t>(v));
    return "";
}

} // namespace

TEST_CASE("emma_execute_hello_world", "[emma][wave3]") {
    log_capture cap;
    execute_and_get_result(R"emma(
        print("hello")
    )emma", &cap);
    REQUIRE(cap.text.find("hello") != std::string::npos);
}

TEST_CASE("emma_execute_arithmetic", "[emma][wave3]") {
    // let x = 1 + 2  →  __main__ 返 x
    emma_value r = execute_and_get_result(R"emma(
        fn __main__()
            let x = 1 + 2
            return x
        end
    )emma");
    REQUIRE(as_int(r) == 3);
}

TEST_CASE("emma_execute_if_else", "[emma][wave3]") {
    emma_value r = execute_and_get_result(R"emma(
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
    REQUIRE(as_str(r) == "big");

    r = execute_and_get_result(R"emma(
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
    REQUIRE(as_str(r) == "small");

    r = execute_and_get_result(R"emma(
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
    REQUIRE(as_str(r) == "zero");
}

TEST_CASE("emma_execute_while_loop", "[emma][wave3]") {
    // 1..=10 累加 (55)
    emma_value r = execute_and_get_result(R"emma(
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
    REQUIRE(as_int(r) == 55);
}

TEST_CASE("emma_execute_fn_call", "[emma][wave3]") {
    emma_value r = execute_and_get_result(R"emma(
        fn square(x)
            return x * x
        end
        fn __main__()
            let a = square(4)
            let b = square(5)
            return a + b
        end
    )emma");
    REQUIRE(as_int(r) == 41);
}

TEST_CASE("emma_execute_list_indexing", "[emma][wave3]") {
    emma_value r = execute_and_get_result(R"emma(
        fn __main__()
            let xs = [10, 20, 30, 40]
            return xs[2]
        end
    )emma");
    REQUIRE(as_int(r) == 30);

    // len(xs) == 4
    r = execute_and_get_result(R"emma(
        fn __main__()
            let xs = [10, 20, 30, 40]
            return len(xs)
        end
    )emma");
    REQUIRE(as_int(r) == 4);

    // negative index
    r = execute_and_get_result(R"emma(
        fn __main__()
            let xs = [10, 20, 30, 40]
            return xs[-1]
        end
    )emma");
    REQUIRE(as_int(r) == 40);
}

TEST_CASE("emma_execute_dict_access", "[emma][wave3]") {
    // attr 访问 + index 访问 两路都测
    emma_value r = execute_and_get_result(R"emma(
        fn __main__()
            let d = {name: "sword", damage: 42}
            return d.damage
        end
    )emma");
    REQUIRE(as_int(r) == 42);

    r = execute_and_get_result(R"emma(
        fn __main__()
            let d = {name: "sword", damage: 42}
            return d["name"]
        end
    )emma");
    REQUIRE(as_str(r) == "sword");

    // 字典赋值 + 长度
    r = execute_and_get_result(R"emma(
        fn __main__()
            let d = {}
            d["a"] = 1
            d["b"] = 2
            d["c"] = 3
            return len(d)
        end
    )emma");
    REQUIRE(as_int(r) == 3);
}

// 额外: for + range
TEST_CASE("emma_for_range_sum", "[emma][wave3]") {
    emma_value r = execute_and_get_result(R"emma(
        fn __main__()
            let sum = 0
            for i in range(1, 6)
                sum = sum + i
            end
            return sum
        end
    )emma");
    // 1+2+3+4+5 = 15
    REQUIRE(as_int(r) == 15);
}

// 额外: string 拼接 + .. 运算
TEST_CASE("emma_string_concat", "[emma][wave3]") {
    emma_value r = execute_and_get_result(R"emma(
        fn __main__()
            let a = "hello"
            let b = "world"
            return a .. " " .. b
        end
    )emma");
    REQUIRE(as_str(r) == "hello world");
}
