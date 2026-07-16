// test_sdk_bind_wave4.cpp — G2.7 sdk_binding runtime 首切片单测
//
// 4 CASE:
//   1. sdk_bind_log_info_dispatches
//   2. sdk_bind_unknown_method_returns_error
//   3. sdk_bind_add_hotkey_records_registration
//   4. sdk_bind_publish_event_dispatches
//
// 依赖: 只测中央 dispatcher, 不依赖真 Python/Lua/AS/Emma 引擎; 用 emma
// activate 作 headless plugin binding (interp 可传空)。

#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_emma.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace sao::plugins::sdk_binding;

namespace {

plugin_binding_handle_t make_test_binding() {
    // 传一个"占位 ctx"指针 (unit test 不解引用). Wave 4 shim 只记账。
    static int dummy_ctx_state = 0;
    plugin_binding_handle_t plugin = nullptr;
    int32_t rc = sao_plugins_binding_emma_activate(
        reinterpret_cast<plugin_context_ptr>(&dummy_ctx_state),
        /*interp*/ nullptr,
        &plugin);
    assert(rc == SAO_OK);
    assert(plugin != nullptr);
    return plugin;
}

void free_test_binding(plugin_binding_handle_t plugin) {
    int32_t rc = sao_plugins_binding_emma_deactivate(plugin);
    assert(rc == SAO_OK);
}

// CASE 1: log 派发
void case_sdk_bind_log_info_dispatches() {
    plugin_binding_handle_t plugin = make_test_binding();
    const char* msg = "hello wave4";
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_log,
        msg, std::strlen(msg),
        nullptr, 0);
    assert(rc == SAO_OK);
    const char* logged = sao_plugins_binding_test_last_log(plugin);
    assert(logged != nullptr);
    assert(std::strcmp(logged, "hello wave4") == 0);
    free_test_binding(plugin);
    std::printf("  [OK] sdk_bind_log_info_dispatches\n");
}

// CASE 2: 未实装 method → METHOD_NOT_IMPLEMENTED
void case_sdk_bind_unknown_method_returns_error() {
    plugin_binding_handle_t plugin = make_test_binding();
    // method_notify (Wave 5 才实装) → METHOD_NOT_IMPLEMENTED
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_notify,
        nullptr, 0, nullptr, 0);
    assert(rc == SAO_ERR_METHOD_NOT_IMPLEMENTED);

    // register_ui_panel 也未实装
    rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_register_ui_panel,
        "id", 2, nullptr, 0);
    assert(rc == SAO_ERR_METHOD_NOT_IMPLEMENTED);

    free_test_binding(plugin);
    std::printf("  [OK] sdk_bind_unknown_method_returns_error\n");
}

// CASE 3: add_hotkey (method_register_hotkey) 命中
void case_sdk_bind_add_hotkey_records_registration() {
    plugin_binding_handle_t plugin = make_test_binding();
    // 记 hotkey_id = "F5" (Wave 4 shim: 整段 args 当 key)
    const char* hotkey_id = "F5";
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_register_hotkey,
        hotkey_id, std::strlen(hotkey_id),
        nullptr, 0);
    assert(rc == SAO_OK);
    assert(sao_plugins_binding_test_has_hotkey(plugin, "F5") == true);
    assert(sao_plugins_binding_test_has_hotkey(plugin, "F6") == false);

    // 空 hotkey_id → INVALID_ARGUMENT
    rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_register_hotkey,
        nullptr, 0, nullptr, 0);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);

    free_test_binding(plugin);
    std::printf("  [OK] sdk_bind_add_hotkey_records_registration\n");
}

// CASE 4: publish_event (method_emit) 派发 + 记账
void case_sdk_bind_publish_event_dispatches() {
    plugin_binding_handle_t plugin = make_test_binding();
    const char* topic = "damage";
    // 派发 3 次
    for (int i = 0; i < 3; ++i) {
        int32_t rc = sao_plugins_sdk_bind_call(
            plugin, sdk_method_id::method_emit,
            topic, std::strlen(topic),
            nullptr, 0);
        assert(rc == SAO_OK);
    }
    uint32_t cnt = sao_plugins_binding_test_event_count(plugin, "damage");
    assert(cnt == 3);
    // 其他 topic 应为 0
    assert(sao_plugins_binding_test_event_count(plugin, "heal") == 0);

    free_test_binding(plugin);
    std::printf("  [OK] sdk_bind_publish_event_dispatches\n");
}

// bonus: plugin_id 属性也走同一 dispatcher (Wave 4 shim 记账)
void case_sdk_bind_plugin_id_returns_shim() {
    plugin_binding_handle_t plugin = make_test_binding();
    char buf[64] = {};
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::prop_plugin_id,
        nullptr, 0,
        buf, sizeof(buf));
    assert(rc == SAO_OK);
    // Wave 4 shim: "ctx@0x..." 格式
    assert(std::strncmp(buf, "ctx@", 4) == 0);
    free_test_binding(plugin);
    std::printf("  [OK] sdk_bind_plugin_id_returns_shim (bonus)\n");
}

// bonus: method name 表反查双向
void case_sdk_bind_method_name_roundtrip() {
    const char* name = sao_plugins_binding_method_name(sdk_method_id::method_log);
    assert(name != nullptr);
    assert(std::strcmp(name, "log") == 0);

    sdk_method_id id = sao_plugins_binding_method_from_name("log");
    assert(id == sdk_method_id::method_log);

    id = sao_plugins_binding_method_from_name("__notreal__");
    assert(id == sdk_method_id::method_count_);
    std::printf("  [OK] sdk_bind_method_name_roundtrip (bonus)\n");
}

} // namespace

int main() {
    std::printf("test_sdk_bind_wave4:\n");
    case_sdk_bind_log_info_dispatches();
    case_sdk_bind_unknown_method_returns_error();
    case_sdk_bind_add_hotkey_records_registration();
    case_sdk_bind_publish_event_dispatches();
    case_sdk_bind_plugin_id_returns_shim();
    case_sdk_bind_method_name_roundtrip();
    std::printf("test_sdk_bind_wave4: 6 cases passed\n");
    return 0;
}
