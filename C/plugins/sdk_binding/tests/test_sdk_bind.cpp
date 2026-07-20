// test_sdk_bind.cpp — sdk_binding runtime dispatcher 单测
//
// 6 CASE:
//   1. sdk_bind_log_info_dispatches
//   2. sdk_bind_unknown_method_returns_unsupported
//   3. sdk_bind_add_hotkey_requires_callback
//   4. sdk_bind_publish_event_dispatches
//   5. sdk_bind_plugin_id_returns_manifest_id
//   6. sdk_bind_method_name_roundtrip
//
// 依赖: 只测中央 dispatcher, 不依赖真 Python/Lua/AS/Emma 引擎；使用真实
// loader plugin context 和最小 Emma host adapter 建立 headless binding。

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_emma.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace sao::plugins::sdk_binding;

namespace {

bool SAO_PLUGINS_CALL test_host_available(void*) {
    return true;
}

int32_t SAO_PLUGINS_CALL test_host_load(void* context, void* runtime, void** out_plugin, void*) {
    if (context == nullptr || runtime == nullptr || out_plugin == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_plugin = runtime;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL test_host_unload(void* plugin, void*) {
    return plugin == nullptr ? SAO_ERR_INVALID_ARGUMENT : SAO_OK;
}

int32_t SAO_PLUGINS_CALL test_host_invoke(void*, const char*, const uint8_t*, size_t, uint8_t*,
                                          size_t, size_t*, char*, size_t, void*) {
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

language_host_adapter_vtable make_test_host_adapter() {
    language_host_adapter_vtable adapter{};
    adapter.language = language_host_kind::emma;
    adapter.available = &test_host_available;
    adapter.load_plugin = &test_host_load;
    adapter.unload_plugin = &test_host_unload;
    adapter.invoke = &test_host_invoke;
    return adapter;
}

// CASE 1: log 派发
void case_sdk_bind_log_info_dispatches(plugin_binding_handle_t plugin) {
    const char* msg = "hello sdk-binding";
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_log,
        msg, std::strlen(msg),
        nullptr, 0);
    assert(rc == SAO_OK);
    const char* logged = sao_plugins_binding_test_last_log(plugin);
    assert(logged != nullptr);
    assert(std::strcmp(logged, "hello sdk-binding") == 0);
    std::printf("  [OK] sdk_bind_log_info_dispatches\n");
}

// CASE 2: 中央 dispatcher 未直接支持的 method → UNSUPPORTED
void case_sdk_bind_unknown_method_returns_unsupported(plugin_binding_handle_t plugin) {
    // method_notify 不在当前中央 dispatcher 的直接实现集合中。
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_notify,
        nullptr, 0, nullptr, 0);
    assert(rc == sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);

    // register_ui_panel 也未实装
    rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_register_ui_panel,
        "id", 2, nullptr, 0);
    assert(rc == sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);

    std::printf("  [OK] sdk_bind_unknown_method_returns_unsupported\n");
}

// CASE 3: legacy add_hotkey 缺 callback 时 fail closed
void case_sdk_bind_add_hotkey_requires_callback(plugin_binding_handle_t plugin) {
    const char* hotkey_id = "F5";
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_register_hotkey,
        hotkey_id, std::strlen(hotkey_id),
        nullptr, 0);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);
    assert(sao_plugins_binding_test_has_hotkey(plugin, "F5") == false);

    // 空 hotkey_id → INVALID_ARGUMENT
    rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::method_register_hotkey,
        nullptr, 0, nullptr, 0);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);

    std::printf("  [OK] sdk_bind_add_hotkey_requires_callback\n");
}

// CASE 4: publish_event (method_emit) 派发 + 记账
void case_sdk_bind_publish_event_dispatches(plugin_binding_handle_t plugin) {
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

    std::printf("  [OK] sdk_bind_publish_event_dispatches\n");
}

// CASE 5: plugin_id 属性返回 loader manifest ID
void case_sdk_bind_plugin_id_returns_manifest_id(plugin_binding_handle_t plugin) {
    char buf[64] = {};
    int32_t rc = sao_plugins_sdk_bind_call(
        plugin, sdk_method_id::prop_plugin_id,
        nullptr, 0,
        buf, sizeof(buf));
    assert(rc == SAO_OK);
    assert(std::strcmp(buf, "sdk_binding_dispatch") == 0);
    std::printf("  [OK] sdk_bind_plugin_id_returns_manifest_id\n");
}

// CASE 6: method name 表反查双向
void case_sdk_bind_method_name_roundtrip() {
    const char* name = sao_plugins_binding_method_name(sdk_method_id::method_log);
    assert(name != nullptr);
    assert(std::strcmp(name, "log") == 0);

    sdk_method_id id = sao_plugins_binding_method_from_name("log");
    assert(id == sdk_method_id::method_log);

    id = sao_plugins_binding_method_from_name("__notreal__");
    assert(id == sdk_method_id::method_count_);
    std::printf("  [OK] sdk_bind_method_name_roundtrip\n");
}

} // namespace

int main() {
    const auto adapter = make_test_host_adapter();
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);

    sao::plugins::loader::plugin_manifest manifest{};
    manifest.plugin_id = "sdk_binding_dispatch";
    manifest.name = "SDK binding dispatcher";
    manifest.version = "1";
    manifest.entry = "plugin.emma";
    manifest.language = sao::plugins::loader::engine_kind::emma;
    manifest.source_path = ".";

    const auto registry = sao::plugins::loader::sao_plugins_registry_instance();
    sao::plugins::loader::plugin_handle_t loader_plugin = nullptr;
    assert(sao::plugins::loader::sao_plugins_registry_add_plugin(
               registry, &manifest, &loader_plugin) == SAO_OK);
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(loader_plugin);
    assert(context != nullptr);

    static int runtime_state = 0;
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_emma_activate(
               reinterpret_cast<plugin_context_ptr>(context),
               reinterpret_cast<emma_interpreter_ptr>(&runtime_state),
               &plugin) == SAO_OK);
    assert(plugin != nullptr);

    std::printf("test_sdk_bind:\n");
    case_sdk_bind_log_info_dispatches(plugin);
    case_sdk_bind_unknown_method_returns_unsupported(plugin);
    case_sdk_bind_add_hotkey_requires_callback(plugin);
    case_sdk_bind_publish_event_dispatches(plugin);
    case_sdk_bind_plugin_id_returns_manifest_id(plugin);
    case_sdk_bind_method_name_roundtrip();
    std::printf("test_sdk_bind: 6 cases passed\n");

    assert(sao_plugins_binding_emma_deactivate(plugin) == SAO_OK);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    assert(sao::plugins::loader::sao_plugins_registry_remove(registry, loader_plugin) == SAO_OK);
    return 0;
}
