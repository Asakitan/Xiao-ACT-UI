// Lua 5.4 host interpreter behavior tests.

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/lua_host/lua_host.h"

#include <cstring>
#include <string>

using namespace sao::plugins::lua_host;

namespace {

struct auto_free {
    char** value;

    ~auto_free() {
        if (value != nullptr && *value != nullptr) {
            sao_plugins_luahost_free_string(*value);
            *value = nullptr;
        }
    }
};

lua_host_handle_t make_host() {
    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    if (sao_plugins_luahost_create(&config, &host) != SAO_OK) {
        return nullptr;
    }
    return host;
}

} // namespace

TEST_CASE("lua_execute_return_number", "[lua][interpreter]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* error = nullptr;
    auto_free result_guard{&result};
    auto_free error_guard{&error};
    const char* source = "return 42";
    const int32_t rc =
        sao_plugins_luahost_execute(host, source, std::strlen(source), &result, &error);
    REQUIRE(rc == SAO_OK);
    REQUIRE(error == nullptr);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "42");

    sao_plugins_luahost_destroy(host);
}

TEST_CASE("lua_execute_string_concat", "[lua][interpreter]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* error = nullptr;
    auto_free result_guard{&result};
    auto_free error_guard{&error};
    const char* source = "return 'a' .. 'b'";
    const int32_t rc =
        sao_plugins_luahost_execute(host, source, std::strlen(source), &result, &error);
    REQUIRE(rc == SAO_OK);
    REQUIRE(error == nullptr);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "ab");

    sao_plugins_luahost_destroy(host);
}

TEST_CASE("lua_execute_table_access", "[lua][interpreter]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* error = nullptr;
    auto_free result_guard{&result};
    auto_free error_guard{&error};
    const char* source = "local t = {name='sword', damage=42}; return t.damage";
    const int32_t rc =
        sao_plugins_luahost_execute(host, source, std::strlen(source), &result, &error);
    REQUIRE(rc == SAO_OK);
    REQUIRE(error == nullptr);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "42");

    const char* definition = "function greet() return 'hello' end";
    REQUIRE(sao_plugins_luahost_execute(host, definition, std::strlen(definition), nullptr,
                                        nullptr) == SAO_OK);

    char* call_result = nullptr;
    char* call_error = nullptr;
    auto_free call_result_guard{&call_result};
    auto_free call_error_guard{&call_error};
    REQUIRE(sao_plugins_luahost_call_function(host, "greet", &call_result, &call_error) ==
            SAO_OK);
    REQUIRE(call_error == nullptr);
    REQUIRE(call_result != nullptr);
    REQUIRE(std::string(call_result) == "hello");

    sao_plugins_luahost_destroy(host);
}

TEST_CASE("lua_error_syntax_returns_error", "[lua][interpreter]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* error = nullptr;
    auto_free result_guard{&result};
    auto_free error_guard{&error};
    const char* source = "function bad(";
    REQUIRE(sao_plugins_luahost_execute(host, source, std::strlen(source), &result, &error) !=
            SAO_OK);
    REQUIRE(error != nullptr);
    REQUIRE_FALSE(std::string(error).empty());

    sao_plugins_luahost_destroy(host);
}
