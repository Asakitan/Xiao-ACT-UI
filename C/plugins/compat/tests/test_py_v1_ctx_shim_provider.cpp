#include "sao/plugins/compat/py_v1_ctx_shim.h"
#include "sao/sdk/sao_sdk.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sao::plugins::compat;
using namespace sao::plugins::sdk_binding;

namespace {
[[noreturn]] void sao_test_assert_fail(const char* file, int line,
                                        const char* expression) {
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

#define SAO_TEST_ASSERT(...)                                                   \
    do {                                                                       \
        if (!(__VA_ARGS__)) {                                                  \
            sao_test_assert_fail(__FILE__, __LINE__, #__VA_ARGS__);            \
        }                                                                      \
    } while (false)
static_assert(static_cast<uint16_t>(sdk_method_id::method_register_menu_category) == 36);
static_assert(static_cast<uint16_t>(sdk_method_id::method_register_menu_surface) == 37);
static_assert(static_cast<uint16_t>(sdk_method_id::method_register_action_handler) == 38);

struct provider_fixture {
    uint64_t next_token = 100;
    std::vector<std::string> events;
    std::string last_plugin_id;
    std::string last_hotkey;
    uint32_t last_virtual_key = 0;
    uint32_t last_modifiers = 0;
    uint32_t last_timer_ms = 0;
    std::string last_notify;
    std::string last_overlay_surface;
};

provider_fixture* fixture(void* user_data) {
    return static_cast<provider_fixture*>(user_data);
}

void SAO_SDK_CALL retain(void* user_data) {
    fixture(user_data)->events.emplace_back("retain");
}

void SAO_SDK_CALL release(void* user_data) {
    fixture(user_data)->events.emplace_back("release");
}

sao_sdk_status_t SAO_SDK_CALL register_hotkey(
    void* user_data, const char* plugin_id, const char* binding_id,
    uint32_t virtual_key, uint32_t modifiers,
    sao_sdk_hotkey_callback_t, void*, uint64_t* out_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id;
    state->last_hotkey = binding_id;
    state->last_virtual_key = virtual_key;
    state->last_modifiers = modifiers;
    *out_token = state->next_token++;
    state->events.push_back("register:hotkey:" + std::to_string(*out_token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL unregister_hotkey(void* user_data,
                                                 uint64_t token) {
    fixture(user_data)->events.push_back(
        "unregister:hotkey:" + std::to_string(token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL register_timer(
    void* user_data, uint32_t interval_ms, sao_sdk_timer_callback_t,
    void*, uint64_t* out_token) {
    auto* state = fixture(user_data);
    state->last_timer_ms = interval_ms;
    *out_token = state->next_token++;
    state->events.push_back("register:timer:" + std::to_string(*out_token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL unregister_timer(void* user_data,
                                                uint64_t token) {
    fixture(user_data)->events.push_back(
        "unregister:timer:" + std::to_string(token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL show_notify(
    void* user_data, const char* plugin_id, const SaoSdkNotifySpec* spec,
    uint64_t* out_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id;
    state->last_notify = spec->text_utf8;
    *out_token = state->next_token++;
    state->events.push_back("register:notify:" + std::to_string(*out_token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL dismiss_notify(void* user_data,
                                              uint64_t token) {
    fixture(user_data)->events.push_back(
        "unregister:notify:" + std::to_string(token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL set_overlay(
    void* user_data, const char* plugin_id, const SaoSdkOverlaySpec* spec,
    uint64_t* out_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id;
    state->last_overlay_surface = spec->surface_id_utf8;
    *out_token = state->next_token++;
    state->events.push_back("register:overlay:" + std::to_string(*out_token));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL clear_overlay(void* user_data,
                                             uint64_t token) {
    fixture(user_data)->events.push_back(
        "unregister:overlay:" + std::to_string(token));
    return SAO_SDK_OK;
}

SaoSdkProviderVTable make_provider(provider_fixture* state) {
    SaoSdkProviderVTable provider{};
    provider.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = state;
    provider.retain = retain;
    provider.release = release;
    provider.register_timer = register_timer;
    provider.unregister_timer = unregister_timer;
    provider.register_hotkey = register_hotkey;
    provider.unregister_hotkey = unregister_hotkey;
    provider.show_notify = show_notify;
    provider.dismiss_notify = dismiss_notify;
    provider.set_overlay = set_overlay;
    provider.clear_overlay = clear_overlay;
    return provider;
}

void SAO_PLUGINS_CALL hotkey_callback(uint64_t, void*) {}
void SAO_PLUGINS_CALL timer_callback(uint64_t, void*) {}

struct callback_probe {
    uint32_t calls = 0;
};

void SAO_PLUGINS_CALL event_callback(const char*, const uint8_t*, size_t, void* user_data) {
    ++static_cast<callback_probe*>(user_data)->calls;
}

void SAO_PLUGINS_CALL counted_hotkey_callback(uint64_t, void* user_data) {
    ++static_cast<callback_probe*>(user_data)->calls;
}

void SAO_PLUGINS_CALL counted_timer_callback(uint64_t, void* user_data) {
    ++static_cast<callback_probe*>(user_data)->calls;
}

void SAO_PLUGINS_CALL panel_action_callback(const char*, const uint8_t*, size_t, void* user_data) {
    ++static_cast<callback_probe*>(user_data)->calls;
}

int32_t call(plugin_context_ptr ctx,
             const char* method,
             const std::string& arguments,
             std::string* out_result = nullptr,
             sdk_context_hotkey_callback_fn hotkey = nullptr,
             sdk_context_timer_callback_fn timer = nullptr) {
    char result[2048]{};
    size_t required = 0;
    sdk_context_call_request request{};
    request.args_json_utf8 = arguments.data();
    request.args_size = arguments.size();
    request.hotkey_callback = hotkey;
    request.timer_callback = timer;
    request.out_result_json_utf8 = result;
    request.out_capacity = sizeof(result);
    request.out_required = &required;
    const int32_t status = sao_plugins_compat_v1_call(ctx, method, &request);
    if (status == SAO_OK && out_result != nullptr) *out_result = result;
    return status;
}

nlohmann::json report_for(plugin_context_ptr ctx) {
    char* report = nullptr;
    SAO_TEST_ASSERT(sao_plugins_compat_v1_report(ctx, &report) == SAO_OK);
    SAO_TEST_ASSERT(report != nullptr);
    const auto parsed = nlohmann::json::parse(report);
    sao_plugins_compat_free_string(report);
    return parsed;
}

std::string method_status(const nlohmann::json& report,
                          const char* legacy_name) {
    const auto& methods = report.at("methods");
    const auto found = std::find_if(
        methods.begin(), methods.end(), [legacy_name](const auto& method) {
            return method.at("legacy") == legacy_name;
        });
    SAO_TEST_ASSERT(found != methods.end());
    return found->at("status").get<std::string>();
}

int32_t method_status_code(const nlohmann::json& report, const char* legacy_name) {
    const auto& methods = report.at("methods");
    const auto found =
        std::find_if(methods.begin(), methods.end(), [legacy_name](const auto& method) {
            return method.at("legacy") == legacy_name;
        });
    SAO_TEST_ASSERT(found != methods.end());
    return found->at("status_code").get<int32_t>();
}

} // namespace

int main() {
    SaoSdkContext sdk{};
    SAO_TEST_ASSERT(sao_sdk_bind_context("compat.provider", "1.2.3", &sdk) ==
           SAO_SDK_OK);
    auto ctx = reinterpret_cast<plugin_context_ptr>(&sdk);
    SAO_TEST_ASSERT(sao_plugins_compat_arm_v1_ctx_shim(ctx) == SAO_OK);

    SAO_TEST_ASSERT(sao_plugins_compat_is_v1_method("metadata"));
    SAO_TEST_ASSERT(sao_plugins_compat_is_v1_method("add_hotkey"));
    SAO_TEST_ASSERT(!sao_plugins_compat_is_v1_method("eval_unbounded"));
    SAO_TEST_ASSERT(sao_plugins_compat_ctx_v1_lookup_alias("add_hotkey") ==
           static_cast<uint16_t>(sdk_method_id::method_register_hotkey));
    SAO_TEST_ASSERT(sao_plugins_compat_ctx_v1_register_alias(
               "register_menu_surface",
               static_cast<uint16_t>(sdk_method_id::method_register_menu_category)) ==
           SAO_ERR_INVALID_ARGUMENT);
    SAO_TEST_ASSERT(sao_plugins_compat_ctx_v1_register_alias(
               "register_menu_category",
               static_cast<uint16_t>(sdk_method_id::method_register_menu_surface)) ==
           SAO_ERR_INVALID_ARGUMENT);
    SAO_TEST_ASSERT(sao_plugins_compat_ctx_v1_lookup_alias("register_menu_surface") ==
           static_cast<uint16_t>(sdk_method_id::method_register_menu_surface));
    SAO_TEST_ASSERT(sao_plugins_compat_ctx_v1_lookup_alias("register_menu_category") ==
           static_cast<uint16_t>(sdk_method_id::method_register_menu_category));

    std::string result;
    SAO_TEST_ASSERT(call(ctx, "metadata", "{}", &result) == SAO_OK);
    const auto metadata = nlohmann::json::parse(result);
    SAO_TEST_ASSERT(metadata.at("plugin_id") == "compat.provider");
    SAO_TEST_ASSERT(metadata.at("plugin_version") == "1.2.3");

    SAO_TEST_ASSERT(call(ctx, "set_setting", R"({"key":"rate","value":7})") ==
           SAO_OK);
    SAO_TEST_ASSERT(call(ctx, "get_setting", R"({"key":"rate","default":3})",
                &result) == SAO_OK);
    SAO_TEST_ASSERT(result == "7");
    SAO_TEST_ASSERT(call(ctx, "set_defaults",
                R"({"defaults":{"rate":9,"name":"legacy"}})") == SAO_OK);
    SAO_TEST_ASSERT(call(ctx, "setting", R"({"key":"rate"})", &result) == SAO_OK);
    SAO_TEST_ASSERT(result == "7");
    SAO_TEST_ASSERT(call(ctx, "setting", R"({"key":"name"})", &result) == SAO_OK);
    SAO_TEST_ASSERT(result == R"("legacy")");
    SAO_TEST_ASSERT(call(ctx, "set_defaults",
                R"({"defaults":{"safe":1,"complex":{"nested":true}}})") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(call(ctx, "setting", R"({"key":"safe","default":"absent"})",
                &result) == SAO_OK);
    SAO_TEST_ASSERT(result == R"("absent")");

    SAO_TEST_ASSERT(call(ctx, "emit", R"({"topic":"compat.event","payload":{"n":1}})") ==
           SAO_OK);
    SAO_TEST_ASSERT(call(ctx, "register_ui_panel",
                R"({"id":"legacy.panel","metadata":{"title":"Legacy Panel"},"spec":{"kind":"panel","children":[{"kind":"canvas"}]}})",
                &result) == SAO_OK);
    SAO_TEST_ASSERT(nlohmann::json::parse(result).get<uint64_t>() != 0);
    SAO_TEST_ASSERT(call(ctx, "register_menu_category",
                R"({"name":"Legacy","icon":"x"})") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    sdk_context_call_request callback_request{};
    callback_request.args_json_utf8 = "{}";
    callback_request.args_size = 2;
    callback_probe rejected_callbacks;
    callback_request.event_callback = event_callback;
    callback_request.hotkey_callback = counted_hotkey_callback;
    callback_request.timer_callback = counted_timer_callback;
    callback_request.panel_action_callback = panel_action_callback;
    callback_request.callback_user_data = &rejected_callbacks;
    SAO_TEST_ASSERT(sao_plugins_compat_v1_call(ctx, "register_action_handler", &callback_request) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(sao_plugins_compat_v1_call(ctx, "register_menu_surface", &callback_request) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(rejected_callbacks.calls == 0);
    SAO_TEST_ASSERT(call(ctx, "register_report_view", R"({"id":"report"})") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(call(ctx, "eval_unbounded", "{}") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);

    const auto without_provider = report_for(ctx);
    SAO_TEST_ASSERT(without_provider.at("provider_status") == SAO_SDK_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(method_status(without_provider, "metadata") == "supported");
    SAO_TEST_ASSERT(method_status(without_provider, "get_setting") == "supported");
    SAO_TEST_ASSERT(method_status(without_provider, "emit") == "supported");
    SAO_TEST_ASSERT(method_status(without_provider, "register_ui_panel") ==
           "supported");
    SAO_TEST_ASSERT(method_status(without_provider, "add_hotkey") == "unsupported");
    SAO_TEST_ASSERT(method_status(without_provider, "notify") == "unsupported");
    SAO_TEST_ASSERT(method_status(without_provider, "register_menu_surface") ==
           "unsupported");
    SAO_TEST_ASSERT(method_status(without_provider, "register_action_handler") == "unsupported");
    SAO_TEST_ASSERT(method_status_code(without_provider, "register_menu_category") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(method_status_code(without_provider, "register_menu_surface") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(method_status_code(without_provider, "register_action_handler") ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);

    SAO_TEST_ASSERT(call(ctx, "add_hotkey",
                R"({"id":"toggle","default_key":"CTRL+F7","label":"Toggle"})",
                nullptr, hotkey_callback) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(call(ctx, "set_interval", R"({"seconds":0.25})", nullptr,
                nullptr, timer_callback) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(call(ctx, "notify",
                R"({"title":"Legacy","message":"Ready","duration_s":1.5})") ==
                     sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);

    provider_fixture state;
    auto provider = make_provider(&state);
    SAO_TEST_ASSERT(sao_sdk_context_bind_provider(&sdk, &provider) == SAO_SDK_OK);

    SAO_TEST_ASSERT(call(ctx, "add_hotkey",
                R"({"id":"toggle","default_key":"CTRL+F7","label":"Toggle"})",
                &result, hotkey_callback) == SAO_OK);
    SAO_TEST_ASSERT(nlohmann::json::parse(result).get<uint64_t>() != 0);
    SAO_TEST_ASSERT(state.last_hotkey == "toggle");
    SAO_TEST_ASSERT(state.last_virtual_key == 0x76u);
    SAO_TEST_ASSERT(state.last_modifiers == 1u);

    SAO_TEST_ASSERT(call(ctx, "set_interval", R"({"seconds":0.25})", &result,
                nullptr, timer_callback) == SAO_OK);
    SAO_TEST_ASSERT(nlohmann::json::parse(result).get<uint64_t>() != 0);
    SAO_TEST_ASSERT(state.last_timer_ms == 250u);

    SAO_TEST_ASSERT(call(ctx, "notify",
                R"({"title":"Legacy","message":"Ready","duration_s":1.5})",
                &result) == SAO_OK);
    SAO_TEST_ASSERT(nlohmann::json::parse(result).get<uint64_t>() != 0);
    SAO_TEST_ASSERT(state.last_notify == "Legacy: Ready");

    SAO_TEST_ASSERT(call(ctx, "set_overlay",
                R"({"surface":"hud","spec":{"kind":"panel","children":[]}})",
                &result) == SAO_OK);
    SAO_TEST_ASSERT(state.last_overlay_surface == "hud");

    const auto with_provider = report_for(ctx);
    SAO_TEST_ASSERT(with_provider.at("provider_status") == SAO_SDK_OK);
    SAO_TEST_ASSERT(method_status(with_provider, "add_hotkey") == "supported");
    SAO_TEST_ASSERT(method_status(with_provider, "set_interval") == "supported");
    SAO_TEST_ASSERT(method_status(with_provider, "notify") == "supported");
    SAO_TEST_ASSERT(method_status(with_provider, "set_timeout") == "unsupported");
    SAO_TEST_ASSERT(method_status(with_provider, "register_menu_category") == "unsupported");
    SAO_TEST_ASSERT(method_status(with_provider, "register_menu_surface") == "unsupported");
    SAO_TEST_ASSERT(method_status(with_provider, "register_action_handler") == "unsupported");

    sao_sdk_context_destroy(&sdk);
    const std::vector<std::string> expected_tail = {
        "unregister:overlay:103", "unregister:notify:102",
        "unregister:timer:101", "unregister:hotkey:100", "release"};
    SAO_TEST_ASSERT(state.events.size() >= expected_tail.size() + 1);
    SAO_TEST_ASSERT(state.events.front() == "retain");
    SAO_TEST_ASSERT(std::equal(expected_tail.begin(), expected_tail.end(),
                      state.events.end() - expected_tail.size()));

    char* filters = nullptr;
    SAO_TEST_ASSERT(sao_plugins_compat_convert_open_file_filters(
               R"([{"name":"Text","spec":"*.txt"},["All","*.*"]])",
               &filters) == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(filters, "Text|*.txt|All|*.*") == 0);
    sao_plugins_compat_free_string(filters);

    std::printf("legacy v1 context provider mapping passed\n");
    return 0;
}
