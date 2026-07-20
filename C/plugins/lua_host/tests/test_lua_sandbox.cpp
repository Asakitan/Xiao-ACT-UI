// test_lua_sandbox.cpp — Lua 沙箱 10 项行为测试
//
// hermetic mock + 合成 script — 不真加载不受信 Lua, 全走 lua_host + sandbox
// API + luaL_dostring 合成脚本.  未 gated (SAO_HAS_LUA 缺) 时四个 arm/disarm/
// state/is_armed API 都返 SAO_ERR_NOT_IMPLEMENTED / 0 / false, test 走
// SUCCEED skip 路径.
//
// **声明**: 不真跑不受信 Lua/AS 脚本, 所有 script 都是本文件里合成的 UTF-8
// 字面量, 走 restricted env + hook 拦截.

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/lua_host/lua_host.h"
#include "sao/plugins/lua_host/lua_sandbox.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace sao::plugins::lua_host;

namespace {

struct auto_free {
    char** p;
    ~auto_free() { if (p && *p) { sao_plugins_luahost_free_string(*p); *p = nullptr; } }
};

// 建 stdlib-on host — 沙盒装的关键在于 stdlib 装了才有 io/os/... 让沙盒剥离.
lua_host_handle_t make_host_stdlib() {
    lua_host_config cfg{};
    cfg.install_stdlib = true;
    lua_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_luahost_create(&cfg, &h);
    if (rc != SAO_OK) return nullptr;
    return h;
}

// 便利 execute — 返 (rc, error) 便于 REQUIRE.
struct exec_result {
    int32_t     rc;
    std::string result;
    std::string error;
};

exec_result exec_script(lua_host_handle_t h, const char* src) {
    char* r = nullptr;
    char* e = nullptr;
    auto_free f1{&r};
    auto_free f2{&e};
    int32_t rc = sao_plugins_luahost_execute(h, src, std::strlen(src), &r, &e);
    return {rc, r ? std::string(r) : "", e ? std::string(e) : ""};
}

} // namespace

TEST_CASE("lua_sandbox_arm_null_L_returns_invalid_argument", "[lua][sandbox][sandbox]") {
    lua_sandbox_config cfg{};
    int32_t rc = sao_plugins_luahost_sandbox_arm(nullptr, &cfg);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("lua_sandbox_arm_null_config_invalid", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    REQUIRE(L != nullptr);
    int32_t rc = sao_plugins_luahost_sandbox_arm(L, nullptr);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_creates_restricted_env", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    REQUIRE(L != nullptr);

    lua_sandbox_config cfg{};
    int32_t rc = sao_plugins_luahost_sandbox_arm(L, &cfg);
    REQUIRE(rc == SAO_OK);
    REQUIRE(sao_plugins_luahost_sandbox_is_armed(L));

    // 白名单符号 assert / pairs / type 可用
    auto r = exec_script(h, "return type(42)");
    REQUIRE(r.rc == SAO_OK);
    REQUIRE(r.result == "number");

    int32_t rc2 = sao_plugins_luahost_sandbox_disarm(L);
    REQUIRE(rc2 == SAO_OK);
    REQUIRE_FALSE(sao_plugins_luahost_sandbox_is_armed(L));
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_io_module_absent_from_env", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    lua_sandbox_config cfg{};
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    // io 应该完全不可见 → `io` 是 nil → `io.open` 抛 attempt to index a nil value
    auto r = exec_script(h, "return io");
    REQUIRE(r.rc == SAO_OK);
    // return type 应为 "nil" (result 字符串是 nil)
    REQUIRE(r.result == "nil");

    // 强化: io.open 抛错
    auto r2 = exec_script(h, "return io.open('x','r')");
    REQUIRE(r2.rc != SAO_OK);
    REQUIRE_FALSE(r2.error.empty());

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_os_execute_absent", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    lua_sandbox_config cfg{};
    // 默认: allow_process = false, os 不放入 env
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    auto r = exec_script(h, "return type(os)");
    REQUIRE(r.rc == SAO_OK);
    REQUIRE(r.result == "nil");

    // 即使 allow_process=true, os.execute 也必须被剥离
    sao_plugins_luahost_sandbox_disarm(L);
    lua_sandbox_config cfg2{};
    cfg2.allow_process = true;
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg2) == SAO_OK);
    auto r2 = exec_script(h, "return type(os.execute)");
    REQUIRE(r2.rc == SAO_OK);
    REQUIRE(r2.result == "nil");

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_math_lib_present", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    lua_sandbox_config cfg{};
    cfg.allow_math_lib = true;
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    auto r = exec_script(h, "return math.sqrt(16)");
    REQUIRE(r.rc == SAO_OK);
    REQUIRE(r.result == "4.0");

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_string_lib_present_but_string_dump_absent", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    lua_sandbox_config cfg{};
    cfg.allow_string_lib = true;
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    // string.upper 应该在
    auto r = exec_script(h, "return string.upper('ab')");
    REQUIRE(r.rc == SAO_OK);
    REQUIRE(r.result == "AB");

    // string.dump 应该不在
    auto r2 = exec_script(h, "return type(string.dump)");
    REQUIRE(r2.rc == SAO_OK);
    REQUIRE(r2.result == "nil");

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_hook_count_terminates_infinite_loop", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    lua_sandbox_config cfg{};
    cfg.max_instructions_per_run = 10000u;  // 小上限让 hook 快速触发
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    // 死循环 — hook 应立刻抬 error
    auto r = exec_script(h, "while true do end");
    REQUIRE(r.rc != SAO_OK);
    REQUIRE_FALSE(r.error.empty());
    // 错误信息应包含我们的 hook 提示
    REQUIRE(r.error.find("instruction limit") != std::string::npos);

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_max_memory_bytes_enforced", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);
    lua_sandbox_config cfg{};
    cfg.max_memory_bytes = 262144;   // 256 KiB — 分大 table 会超
    cfg.max_instructions_per_run = 100000000u; // 别被 hook 提前打断
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    // 分配大 table — 每格约 40+ 字节 (Lua 5.4 TValue), 5 万格保底 2 MiB.
    // sandbox 会拒 alloc → Lua 抛 "not enough memory".
    const char* src =
        "local t = {}\n"
        "for i = 1, 50000 do t[i] = string.rep('x', 128) end\n"
        "return #t\n";
    auto r = exec_script(h, src);
    REQUIRE(r.rc != SAO_OK);
    REQUIRE_FALSE(r.error.empty());

    // 累计 bytes 应大于 0
    uint64_t used = sao_plugins_luahost_sandbox_bytes_used(L);
    (void)used;  // 只是查 API 存在 — 不同 gc 时机下值不稳

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}

TEST_CASE("lua_sandbox_arm_denied_globals_config_enforced", "[lua][sandbox][sandbox]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("lua_host: not available, skipped");
        return;
    }
    auto* h = make_host_stdlib();
    REQUIRE(h != nullptr);
    auto* L = sao_plugins_luahost_state(h);

    // 追加拒: assert (故意拿一个白名单符号验证配置真起作用)
    static const char* const denies[] = { "assert" };
    lua_sandbox_config cfg{};
    cfg.deny_specific_globals = denies;
    cfg.deny_specific_globals_count = 1;
    REQUIRE(sao_plugins_luahost_sandbox_arm(L, &cfg) == SAO_OK);

    // assert 应从 env 里被抹掉
    auto r = exec_script(h, "return type(assert)");
    REQUIRE(r.rc == SAO_OK);
    REQUIRE(r.result == "nil");

    // 白名单里的 type 应仍在
    auto r2 = exec_script(h, "return type(type)");
    REQUIRE(r2.rc == SAO_OK);
    REQUIRE(r2.result == "function");

    sao_plugins_luahost_sandbox_disarm(L);
    sao_plugins_luahost_destroy(h);
}
