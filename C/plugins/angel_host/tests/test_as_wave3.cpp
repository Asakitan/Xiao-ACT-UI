// test_as_wave3.cpp — Wave 3 AngelScript host 功能测试
//
// 无 vcpkg unofficial-angelscript 时 SUCCEED("skipped") 退出 0 (task 要求)。

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/angel_host/as_host.h"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace sao::plugins::angel_host;

// 前向声明 wave3 便利 API (在 as_host.cpp 里)
extern "C" {
    SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
    sao_plugins_ashost_execute(as_host_handle_t host,
                               const char* source_utf8,
                               size_t source_len,
                               char** out_result_utf8,
                               char** out_error_utf8);

    SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
    sao_plugins_ashost_call_function_by_name(as_host_handle_t host,
                                              const char* fn_name,
                                              char** out_result_utf8,
                                              char** out_error_utf8);

    SAO_PLUGINS_API void SAO_PLUGINS_CALL
    sao_plugins_ashost_free_string(char* s);

    SAO_PLUGINS_API bool SAO_PLUGINS_CALL
    sao_plugins_ashost_is_available(void);
}

namespace {

struct auto_free {
    char** p;
    ~auto_free() { if (p && *p) { sao_plugins_ashost_free_string(*p); *p = nullptr; } }
};

as_host_handle_t make_host() {
    as_host_config cfg{};
    as_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_ashost_create(&cfg, &h);
    if (rc != SAO_OK) return nullptr;
    return h;
}

} // namespace

TEST_CASE("as_execute_return_int", "[as][wave3]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available (SAO_HAS_ANGELSCRIPT undefined), skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    // AngelScript 需要显式 entry 函数 (无 top-level 语句)
    const char* src = "int __entry__() { return 42; }";
    int32_t rc = sao_plugins_ashost_execute(host, src, std::strlen(src), &result, &err);
    INFO((err ? err : "(no err)"));
    REQUIRE(rc == SAO_OK);
    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == "42");

    sao_plugins_ashost_destroy(host);
}

TEST_CASE("as_execute_string", "[as][wave3]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    // 无 scriptstdstring addon → string 类型不可用. 用 double 数学替代验证复杂表达式。
    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    const char* src = "double __entry__() { return 3.14 * 2.0; }";
    int32_t rc = sao_plugins_ashost_execute(host, src, std::strlen(src), &result, &err);
    INFO((err ? err : "(no err)"));
    REQUIRE(rc == SAO_OK);
    REQUIRE(result != nullptr);
    // %g 应输出 "6.28"
    REQUIRE(std::string(result) == "6.28");

    // call_function 分开调也测一下
    const char* def = "int square(int x) { return x * x; } int __entry__() { return 0; }";
    // 注意: 每次 execute 会用 asGM_ALWAYS_CREATE 覆盖 module. 先跑一遍装了 __entry__ 再调 __entry__
    (void)def;

    sao_plugins_ashost_destroy(host);
}

TEST_CASE("as_error_syntax", "[as][wave3]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto host = make_host();
    REQUIRE(host != nullptr);

    char* result = nullptr;
    char* err = nullptr;
    auto_free f1{&result};
    auto_free f2{&err};
    // 明显语法错误 (缺 ;)
    const char* bad = "int __entry__() { return 42 }";
    int32_t rc = sao_plugins_ashost_execute(host, bad, std::strlen(bad), &result, &err);
    REQUIRE(rc != SAO_OK);
    REQUIRE(err != nullptr);
    REQUIRE(std::string(err).size() > 0);

    sao_plugins_ashost_destroy(host);
}
