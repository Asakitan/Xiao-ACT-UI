// test_as_sandbox_wave18b.cpp — Wave 18 / Agent b AngelScript sandbox 8 case
//
// hermetic mock — 全走本地合成 script + sandbox_arm 白名单验证.
// **声明**: 不真跑不受信 Lua/AS 脚本, 全合成 UTF-8 字面量 + 走沙盒.

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/angel_host/as_host.h"
#include "sao/plugins/angel_host/as_sandbox.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace sao::plugins::angel_host;

extern "C" {
    SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void);
    SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
    sao_plugins_ashost_execute(as_host_handle_t host,
                               const char* source_utf8,
                               size_t source_len,
                               char** out_result_utf8,
                               char** out_error_utf8);
    SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ashost_free_string(char* s);
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

struct exec_res {
    int32_t rc;
    std::string result;
    std::string error;
};

exec_res exec_script(as_host_handle_t h, const char* src) {
    char* r = nullptr;
    char* e = nullptr;
    auto_free f1{&r};
    auto_free f2{&e};
    int32_t rc = sao_plugins_ashost_execute(h, src, std::strlen(src), &r, &e);
    return {rc, r ? std::string(r) : "", e ? std::string(e) : ""};
}

} // namespace

TEST_CASE("wave18b_as_arm_null_engine_invalid_argument", "[as][sandbox][wave18b]") {
    as_sandbox_config cfg{};
    int32_t rc = sao_plugins_ashost_sandbox_arm(nullptr, &cfg);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wave18b_as_arm_null_config_invalid", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    REQUIRE(h != nullptr);
    auto* engine = sao_plugins_ashost_engine(h);
    REQUIRE(engine != nullptr);
    int32_t rc = sao_plugins_ashost_sandbox_arm(engine, nullptr);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
    sao_plugins_ashost_destroy(h);
}

TEST_CASE("wave18b_as_arm_registers_whitelist_only", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    REQUIRE(h != nullptr);
    auto* engine = sao_plugins_ashost_engine(h);
    as_sandbox_config cfg{};
    cfg.allow_string_type = false;   // 也 gate File → 编译 fail
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine, &cfg) == SAO_OK);
    REQUIRE(sao_plugins_ashost_sandbox_is_armed(engine));

    // File 未注册 → 编译 fail.  用 __entry__ 让 host 探到 entry.
    const char* src =
        "int __entry__() {\n"
        "    File f;\n"
        "    return 0;\n"
        "}\n";
    auto r = exec_script(h, src);
    REQUIRE(r.rc != SAO_OK);
    REQUIRE_FALSE(r.error.empty());

    sao_plugins_ashost_sandbox_disarm(engine);
    sao_plugins_ashost_destroy(h);
}

TEST_CASE("wave18b_as_arm_math_functions_available", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    REQUIRE(h != nullptr);
    auto* engine = sao_plugins_ashost_engine(h);
    as_sandbox_config cfg{};
    cfg.allow_string_type = false;
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine, &cfg) == SAO_OK);

    // sqrt(16) → 4
    const char* src =
        "float __entry__() {\n"
        "    return sqrt(16.0f);\n"
        "}\n";
    auto r = exec_script(h, src);
    REQUIRE(r.rc == SAO_OK);
    REQUIRE(r.result == "4");

    sao_plugins_ashost_sandbox_disarm(engine);
    sao_plugins_ashost_destroy(h);
}

TEST_CASE("wave18b_as_arm_string_type_gated_by_config", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    REQUIRE(h != nullptr);
    auto* engine = sao_plugins_ashost_engine(h);

    // allow_string_type = false → 编译 `string x` 应 fail
    as_sandbox_config cfg{};
    cfg.allow_string_type = false;
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine, &cfg) == SAO_OK);

    const char* src_no_str =
        "int __entry__() {\n"
        "    string x;\n"
        "    return 0;\n"
        "}\n";
    auto r = exec_script(h, src_no_str);
    REQUIRE(r.rc != SAO_OK);
    REQUIRE_FALSE(r.error.empty());

    sao_plugins_ashost_sandbox_disarm(engine);
    sao_plugins_ashost_destroy(h);

    // 反面: allow_string_type=true → 同一段 script 编译成功 (return 0 finish OK)
    auto* h2 = make_host();
    auto* engine2 = sao_plugins_ashost_engine(h2);
    as_sandbox_config cfg2{};
    cfg2.allow_string_type = true;
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine2, &cfg2) == SAO_OK);
    auto r2 = exec_script(h2, src_no_str);
    REQUIRE(r2.rc == SAO_OK);
    sao_plugins_ashost_sandbox_disarm(engine2);
    sao_plugins_ashost_destroy(h2);
}

TEST_CASE("wave18b_as_arm_jit_forced_off", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    auto* engine = sao_plugins_ashost_engine(h);
    as_sandbox_config cfg{};
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine, &cfg) == SAO_OK);
    REQUIRE(sao_plugins_ashost_sandbox_jit_off(engine));
    // JIT off 由 sao_plugins_ashost_sandbox_jit_off snapshot 断言 — 我们在
    // sandbox_arm 里已 engine->SetJITCompiler(nullptr).  直接引擎侧断言需
    // 引入 <angelscript.h>, 保持 test target 干净不引入.
    sao_plugins_ashost_sandbox_disarm(engine);
    sao_plugins_ashost_destroy(h);
}

TEST_CASE("wave18b_as_arm_max_context_ms_records_config", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    auto* engine = sao_plugins_ashost_engine(h);
    as_sandbox_config cfg{};
    cfg.max_context_execution_ms = 250;
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine, &cfg) == SAO_OK);
    REQUIRE(sao_plugins_ashost_sandbox_max_context_ms(engine) == 250);
    sao_plugins_ashost_sandbox_disarm(engine);
    REQUIRE(sao_plugins_ashost_sandbox_max_context_ms(engine) == 0);
    sao_plugins_ashost_destroy(h);
}

TEST_CASE("wave18b_as_arm_denied_globals_enforced", "[as][sandbox][wave18b]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel_host: not available, skipped");
        return;
    }
    auto* h = make_host();
    auto* engine = sao_plugins_ashost_engine(h);
    // 拒 sqrt — 白名单里的.  arm 后 script 里 `sqrt(4.0f)` 应编译 fail.
    static const char* const denies[] = { "sqrt" };
    as_sandbox_config cfg{};
    cfg.deny_globals = denies;
    cfg.deny_globals_count = 1;
    cfg.allow_string_type = false;
    REQUIRE(sao_plugins_ashost_sandbox_arm(engine, &cfg) == SAO_OK);

    const char* src =
        "float __entry__() {\n"
        "    return sqrt(4.0f);\n"
        "}\n";
    auto r = exec_script(h, src);
    REQUIRE(r.rc != SAO_OK);
    REQUIRE_FALSE(r.error.empty());

    sao_plugins_ashost_sandbox_disarm(engine);
    sao_plugins_ashost_destroy(h);
}
