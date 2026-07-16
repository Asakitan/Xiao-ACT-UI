// test_hello_angel_wave8.cpp — Wave 8 / Agent d Phase 8 首切片
//
// 5 CASE:
//   1. engine_init_success
//   2. load_hello_angel_plugin_success
//   3. hello_angel_on_load_registers_ui_panel (SDK 记账)
//   4. hello_angel_on_tick_ticks_3_times
//   5. hello_angel_unload_cleanup_no_leak
//
// 无 AngelScript SDK 时 SUCCEED("skipped") 退出 0, 跟 wave3 test 一致.

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/angel_host/as_host.h"
#include "sao/plugins/angel_host/as_plugin_lifecycle.h"

#include <cstring>
#include <string>

using namespace sao::plugins::angel_host;

// 这两条辅助 API 在 as_host.cpp 里定义但 header 未声明 (wave3 内嵌 fwd).
extern "C" {
    SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void);
    SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ashost_free_string(char* s);
}

// hello_angel plugin.json 绝对路径 (CMake 定义 SAO_HELLO_ANGEL_JSON)
#ifndef SAO_HELLO_ANGEL_JSON
#define SAO_HELLO_ANGEL_JSON "plugins/examples/hello_angel/plugin.json"
#endif

namespace {

as_host_handle_t g_host = nullptr;
as_plugin_handle_t g_plugin = nullptr;

} // namespace

TEST_CASE("angel_host wave8 :: engine_init_success", "[plugins][angel][wave8]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel SDK not available in this build");
        return;
    }
    as_host_config cfg{};
    int32_t rc = sao_plugins_ashost_create(&cfg, &g_host);
    REQUIRE(rc == SAO_OK);
    REQUIRE(g_host != nullptr);
    // 版本非空
    const char* v = sao_plugins_ashost_version();
    REQUIRE(v != nullptr);
    REQUIRE(std::strlen(v) > 0);
}

TEST_CASE("angel_host wave8 :: load_hello_angel_plugin_success", "[plugins][angel][wave8]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel SDK not available");
        return;
    }
    REQUIRE(g_host != nullptr);
    char* err = nullptr;
    int32_t rc = sao_plugins_ashost_load_plugin(
        g_host, SAO_HELLO_ANGEL_JSON, &g_plugin, &err);
    INFO("load rc=" << rc << " err=" << (err ? err : "(null)"));
    REQUIRE(rc == SAO_OK);
    REQUIRE(g_plugin != nullptr);
    if (err) sao_plugins_ashost_free_string(err);
}

TEST_CASE("angel_host wave8 :: hello_angel_on_load_registers_ui_panel",
          "[plugins][angel][wave8]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel SDK not available");
        return;
    }
    REQUIRE(g_plugin != nullptr);
    as_sdk_counters c{};
    int32_t rc = sao_plugins_ashost_get_sdk_counters(g_plugin, &c);
    REQUIRE(rc == SAO_OK);
    // on_load 里调了 2 次 log_info, 1 次 register_ui_panel, 1 次 register_hotkey
    REQUIRE(c.log_info_calls >= 2);
    REQUIRE(c.register_ui_panel_calls == 1);
    REQUIRE(c.register_hotkey_calls == 1);
    // last panel id 是 "AS Hello"
    REQUIRE(std::string(c.last_panel_id_utf8) == "AS Hello");
    // last hotkey key 是 "F9"
    REQUIRE(std::string(c.last_hotkey_key_utf8) == "F9");
    REQUIRE(std::string(c.last_hotkey_id_utf8) == "greet_hotkey");
}

TEST_CASE("angel_host wave8 :: hello_angel_on_tick_ticks_3_times",
          "[plugins][angel][wave8]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel SDK not available");
        return;
    }
    REQUIRE(g_plugin != nullptr);
    for (int i = 0; i < 3; ++i) {
        char* err = nullptr;
        int32_t rc = sao_plugins_ashost_tick_plugin(g_plugin, &err);
        INFO("tick " << i << " rc=" << rc << " err=" << (err ? err : "(null)"));
        REQUIRE(rc == SAO_OK);
        if (err) sao_plugins_ashost_free_string(err);
    }
    // 读全局 tick_count = 3
    int32_t v = 0;
    int32_t rc = sao_plugins_ashost_read_global_int(g_plugin, "tick_count", &v);
    REQUIRE(rc == SAO_OK);
    REQUIRE(v == 3);
    rc = sao_plugins_ashost_read_global_int(g_plugin, "total_ticks_seen", &v);
    REQUIRE(rc == SAO_OK);
    REQUIRE(v == 3);
}

TEST_CASE("angel_host wave8 :: hello_angel_unload_cleanup_no_leak",
          "[plugins][angel][wave8]") {
    if (!sao_plugins_ashost_is_available()) {
        SUCCEED("angel SDK not available");
        return;
    }
    REQUIRE(g_plugin != nullptr);
    char* err = nullptr;
    int32_t rc = sao_plugins_ashost_unload_plugin(g_plugin, &err);
    INFO("unload rc=" << rc << " err=" << (err ? err : "(null)"));
    REQUIRE(rc == SAO_OK);
    if (err) sao_plugins_ashost_free_string(err);
    g_plugin = nullptr;
    // host destroy
    REQUIRE(g_host != nullptr);
    rc = sao_plugins_ashost_destroy(g_host);
    REQUIRE(rc == SAO_OK);
    g_host = nullptr;
}
