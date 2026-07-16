// test_lua_wave3.cpp — Wave 3 Lua 5.4 host 功能测试
//
// 通过 sao_plugins_luahost_is_available() 检查 Lua 是否 gated 编译进来了。
// 若否, 所有 CASE 走 SUCCEED("...") 直接过 (对齐 task: 无 lua 时打 skipped 退 0)。

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/lua_host/lua_host.h"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace sao::plugins::lua_host;

// 前向声明扩展 API (定义在 lua_host.cpp 的 wave3 段)
extern "C" {
    SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
    sao_plugins_luahost_execute(lua_host_handle_t host,
                                const char* source_utf8,
                                size_t source_len,
                                char** out_result_utf8,
                                char** out_error_utf8);

    SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
    sao_plugins_luahost_call_function(lua_host_handle_t host,
                                       const char* fn_name,
                                       char** out_result_utf8,
                                       char** out_error_utf8);

    SAO_PLUGINS_API void SAO_PLUGINS_CALL
    sao_plugins_luahost_free_string(char* s);

    SAO_PLUGINS_API bool SAO_PLUGINS_CALL
    sao_plugins_luahost_is_available(void);
}

namespace {

struct auto_free {
    char** p;
    ~auto_free() { if (p && *p) { sao_plugins_luahost_free_string(*p); *p = nullptr; } }
};

// 起一个 stdlib-enabled host
lua_host_handle_t make_host() {
    lua_host_config cfg{};
    cfg.install_stdlib = true;
    lua_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_luahost_create(&cfg, &h);
    if (rc != SAO_OK) return nullptr;
    return h;
}

} // namespace

TEST_CASE("lua_execute_return_number", "[lua][wave3]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    const char* src = "return 42";
    int32_t rc = sao_plugins_luahost_execute(host, src, std::strlen(src), &result, &err);
    REQUIRE(rc == SAO_OK);
    REQUIRE(err == nullptr);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "42");

    sao_plugins_luahost_destroy(host);
}

TEST_CASE("lua_execute_string_concat", "[lua][wave3]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    const char* src = "return 'a' .. 'b'";
    int32_t rc = sao_plugins_luahost_execute(host, src, std::strlen(src), &result, &err);
    REQUIRE(rc == SAO_OK);
    REQUIRE(err == nullptr);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "ab");

    sao_plugins_luahost_destroy(host);
}

TEST_CASE("lua_execute_table_access", "[lua][wave3]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    const char* src = "local t = {name='sword', damage=42}; return t.damage";
    int32_t rc = sao_plugins_luahost_execute(host, src, std::strlen(src), &result, &err);
    REQUIRE(rc == SAO_OK);
    REQUIRE(err == nullptr);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "42");

    // 再来一个: 定义全局函数, 然后 call_function 调
    const char* def = "function greet() return 'hello' end";
    int32_t rc2 = sao_plugins_luahost_execute(host, def, std::strlen(def), nullptr, nullptr);
    REQUIRE(rc2 == SAO_OK);

    char* result2 = nullptr;
    char* err2 = nullptr;
    auto_free f3{&result2};
    auto_free f4{&err2};
    int32_t rc3 = sao_plugins_luahost_call_function(host, "greet", &result2, &err2);
    REQUIRE(rc3 == SAO_OK);
    REQUIRE(err2 == nullptr);
    REQUIRE(result2 != nullptr);
    REQUIRE(std::string(result2) == "hello");

    sao_plugins_luahost_destroy(host);
}

TEST_CASE("lua_error_syntax_returns_error", "[lua][wave3]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    // 明显语法错误
    const char* bad = "function bad(";
    int32_t rc = sao_plugins_luahost_execute(host, bad, std::strlen(bad), &result, &err);
    REQUIRE(rc != SAO_OK);
    REQUIRE(err != nullptr);
    // Lua 会说 "unexpected symbol" / "syntax error" / "'<name>' expected" etc.
    REQUIRE(std::string(err).size() > 0);

    sao_plugins_luahost_destroy(host);
}
