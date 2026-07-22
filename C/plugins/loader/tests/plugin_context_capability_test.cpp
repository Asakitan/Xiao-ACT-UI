#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace sao::plugins::loader;

namespace {

struct capability_probe;

struct capability_session {
    capability_probe* owner = nullptr;
    std::string plugin_id;
};

struct capability_probe {
    int retain_calls = 0;
    int release_calls = 0;
    int create_session_calls = 0;
    uint32_t session_spec_size = 0;
    bool callback_gate_present = false;
    int quiesce_session_calls = 0;
    int destroy_session_calls = 0;
    uint64_t next_token = 400;
    std::wstring selected_path = L"C:\\fixture\\selected.json";
    std::unordered_map<uint64_t, std::string> resources;
    std::vector<std::string> calls;
    plugin_context_t* reentry_context = nullptr;
    bool reenter_open_file = false;
    bool throw_open_window = false;
    int32_t nested_window_status = SAO_ERR_NOT_INITIALIZED;
    plugin_handle_t reentry_plugin = nullptr;
    int32_t nested_unload_status = SAO_ERR_NOT_INITIALIZED;
    lifecycle_state nested_unload_state = lifecycle_state::unknown;
    bool nested_unload_should_stop = true;
    bool reenter_unload = false;
    bool throw_release = false;
    int32_t create_session_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t destroy_session_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t open_file_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t release_file_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t open_window_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t close_window_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
};

capability_session* session_cast(plugin_context_platform_session_t session) {
    return static_cast<capability_session*>(session);
}

void SAO_PLUGINS_CALL provider_retain(void* user_data) {
    ++static_cast<capability_probe*>(user_data)->retain_calls;
}

void SAO_PLUGINS_CALL provider_release(void* user_data) {
    auto* probe = static_cast<capability_probe*>(user_data);
    ++probe->release_calls;
    if (probe->throw_release)
        throw std::runtime_error("fixture release failure");
}

int32_t SAO_PLUGINS_CALL provider_create_session(void* user_data,
                                                 const plugin_context_platform_session_spec* spec,
                                                 plugin_context_platform_session_t* out_session) {
    if (user_data == nullptr || spec == nullptr || out_session == nullptr ||
        spec->struct_size < SAO_PLUGIN_CONTEXT_PLATFORM_SESSION_SPEC_V1_2_SIZE ||
        spec->plugin_id_utf8 == nullptr) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* probe = static_cast<capability_probe*>(user_data);
    probe->session_spec_size = spec->struct_size;
    probe->callback_gate_present =
        spec->struct_size >= sizeof(plugin_context_platform_session_spec) &&
        spec->callback_gate_user_data != nullptr && spec->enter_callback != nullptr &&
        spec->leave_callback != nullptr;
    auto session = std::make_unique<capability_session>();
    session->owner = probe;
    session->plugin_id = spec->plugin_id_utf8;
    *out_session = session.release();
    ++probe->create_session_calls;
    probe->calls.push_back("session.create:" + std::string(spec->plugin_id_utf8));
    return probe->create_session_status;
}

int32_t SAO_PLUGINS_CALL provider_quiesce_session(void*,
                                                  plugin_context_platform_session_t session) {
    if (session == nullptr)
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    auto* state = session_cast(session);
    ++state->owner->quiesce_session_calls;
    state->owner->calls.push_back("session.quiesce:" + state->plugin_id);
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL provider_destroy_session(void*,
                                                  plugin_context_platform_session_t session) {
    if (session == nullptr)
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    auto* state = session_cast(session);
    ++state->owner->destroy_session_calls;
    state->owner->calls.push_back("session.destroy:" + state->plugin_id);
    if (state->owner->destroy_session_status != SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK)
        return state->owner->destroy_session_status;
    delete state;
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL provider_open_file(void*, plugin_context_platform_session_t session,
                                            const plugin_context_open_file_spec* spec,
                                            const wchar_t** out_selected_path,
                                            plugin_context_platform_token_t* out_provider_token) {
    if (session == nullptr || spec == nullptr || out_selected_path == nullptr ||
        out_provider_token == nullptr ||
        spec->struct_size < sizeof(plugin_context_open_file_spec)) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("file.open:" + state->plugin_id);
    const uint64_t token = ++probe->next_token;
    probe->resources.emplace(token, "file:" + state->plugin_id);
    *out_selected_path = probe->selected_path.c_str();
    *out_provider_token = token;
    if (probe->reenter_open_file) {
        probe->reenter_open_file = false;
        probe->nested_window_status =
            sao_plugins_ctx_open_window(probe->reentry_context, "nested", 320, 240);
    }
    return probe->open_file_status;
}

int32_t SAO_PLUGINS_CALL
provider_release_file_result(void*, plugin_context_platform_session_t session,
                             plugin_context_platform_token_t provider_token) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    const auto found = probe->resources.find(provider_token);
    if (found == probe->resources.end())
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
    probe->calls.push_back("file.release:" + state->plugin_id);
    if (probe->release_file_status != SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK)
        return probe->release_file_status;
    probe->resources.erase(found);
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL provider_open_window(void*, plugin_context_platform_session_t session,
                                              const plugin_context_open_window_spec* spec,
                                              plugin_context_platform_token_t* out_provider_token) {
    if (session == nullptr || spec == nullptr || out_provider_token == nullptr ||
        spec->struct_size < sizeof(plugin_context_open_window_spec) ||
        spec->panel_id_utf8 == nullptr) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("window.open:" + state->plugin_id + ":" + spec->panel_id_utf8);
    if (probe->throw_open_window)
        throw std::runtime_error("fixture open_window failure");
    if (probe->reenter_unload) {
        probe->reenter_unload = false;
        probe->nested_unload_status = sao_plugins_lifecycle_unload(probe->reentry_plugin);
        probe->nested_unload_state = sao_plugins_lifecycle_state(probe->reentry_plugin);
        probe->nested_unload_should_stop = sao_plugins_ctx_should_stop(probe->reentry_context);
    }
    const uint64_t token = ++probe->next_token;
    probe->resources.emplace(token, "window:" + state->plugin_id + ":" + spec->panel_id_utf8);
    *out_provider_token = token;
    return probe->open_window_status;
}

int32_t SAO_PLUGINS_CALL provider_close_window(void*, plugin_context_platform_session_t session,
                                               plugin_context_platform_token_t provider_token) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    const auto found = probe->resources.find(provider_token);
    if (found == probe->resources.end())
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
    probe->calls.push_back("window.close:" + found->second.substr(7));
    if (probe->close_window_status != SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK)
        return probe->close_window_status;
    probe->resources.erase(found);
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

plugin_context_platform_provider make_provider(capability_probe& probe) {
    plugin_context_platform_provider provider{};
    provider.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &probe;
    provider.retain = provider_retain;
    provider.release = provider_release;
    provider.create_session = provider_create_session;
    provider.quiesce_session = provider_quiesce_session;
    provider.destroy_session = provider_destroy_session;
    provider.open_file = provider_open_file;
    provider.release_file_result = provider_release_file_result;
    provider.open_window = provider_open_window;
    provider.close_window = provider_close_window;
    return provider;
}

plugin_handle_t add_plugin(const char* plugin_id) {
    plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.emma";
    manifest.language = engine_kind::emma;
    manifest.source_path = std::filesystem::temp_directory_path().string();
    manifest.abi_version = 2;
    plugin_handle_t handle = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    return handle;
}

int32_t SAO_PLUGINS_CALL adapter_load(plugin_handle_t, const plugin_manifest*, void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL adapter_simple(plugin_handle_t, void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t, bool* allow, void*) {
    *allow = true;
    return SAO_OK;
}

host_adapter_vtable make_adapter() {
    host_adapter_vtable adapter{};
    adapter.load_plugin = adapter_load;
    adapter.call_on_load = adapter_simple;
    adapter.call_on_enable = adapter_simple;
    adapter.call_on_disable = adapter_simple;
    adapter.call_on_unload = adapter_on_unload;
    adapter.unload_plugin = adapter_simple;
    return adapter;
}

void remove_plugin(plugin_handle_t handle) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) == SAO_OK);
}

std::vector<std::string> calls_with_prefix(const capability_probe& probe,
                                           const std::string& prefix) {
    std::vector<std::string> result;
    for (const auto& call : probe.calls) {
        if (call.starts_with(prefix))
            result.push_back(call);
    }
    return result;
}

} // namespace

TEST_CASE("plugin context 1.2 file and window capabilities preserve the 1.1 prefix",
          "[plugins][loader][context][platform][abi]") {
    capability_probe probe;
    auto provider = make_provider(probe);
    provider.abi_version = (1u << 16u) | 1u;
    provider.struct_size =
        static_cast<uint32_t>(offsetof(plugin_context_platform_provider, open_file));
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_plugin("platform_abi_1_1");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    CHECK(probe.session_spec_size == SAO_PLUGIN_CONTEXT_PLATFORM_SESSION_SPEC_V1_2_SIZE);
    CHECK_FALSE(probe.callback_gate_present);
    wchar_t* selected = reinterpret_cast<wchar_t*>(1);
    CHECK(sao_plugins_ctx_open_file(context, "[]", "Pick", L"", 0, &selected) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(selected == nullptr);
    CHECK(sao_plugins_ctx_open_window(context, "panel", 320, 240) == SAO_PLUGINS_ERR_UNSUPPORTED);

    sao_plugins_ctx_destroy(context);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("plugin context owns file results and windows with reverse teardown and reentry",
          "[plugins][loader][context][platform][ownership][reentry]") {
    capability_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_plugin("platform_owner");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    probe.reentry_context = context;
    probe.reenter_open_file = true;

    REQUIRE(sao_plugins_ctx_open_window(context, "first", 640, 480) == SAO_OK);
    wchar_t* selected = nullptr;
    REQUIRE(sao_plugins_ctx_open_file(context, "[]", "Pick", L"C:\\fixture", 0, &selected) ==
            SAO_OK);
    REQUIRE(selected != nullptr);
    CHECK(std::wstring(selected) == probe.selected_path);
    sao_plugins_ctx_free_wstring(selected);
    CHECK(probe.nested_window_status == SAO_OK);
    REQUIRE(sao_plugins_ctx_open_window(context, "last", 800, 600) == SAO_OK);
    CHECK(sao_plugins_ctx_open_window(context, "last", 800, 600) == SAO_PLUGINS_ERR_ALREADY_EXISTS);

    sao_plugins_ctx_destroy(context);
    CHECK(calls_with_prefix(probe, "window.close:") ==
          std::vector<std::string>{"window.close:platform_owner:last",
                                   "window.close:platform_owner:nested",
                                   "window.close:platform_owner:first"});
    CHECK(calls_with_prefix(probe, "file.release:") ==
          std::vector<std::string>{"file.release:platform_owner"});
    CHECK(probe.resources.empty());
    CHECK(probe.retain_calls == 1);
    CHECK(probe.release_calls == 1);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("plugin context retries failed file release and partial window rollback",
          "[plugins][loader][context][platform][failure][rollback]") {
    capability_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_plugin("platform_retry");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    probe.release_file_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_OS_CALL_FAILED;
    wchar_t* selected = reinterpret_cast<wchar_t*>(1);
    CHECK(sao_plugins_ctx_open_file(context, "[]", "Pick", L"", 0, &selected) ==
          SAO_ERR_OS_CALL_FAILED);
    CHECK(selected == nullptr);
    CHECK(probe.resources.size() == 1);
    probe.release_file_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;

    probe.open_window_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ACCESS_DENIED;
    probe.close_window_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_OS_CALL_FAILED;
    CHECK(sao_plugins_ctx_open_window(context, "partial", 320, 240) == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.resources.size() == 2);
    probe.close_window_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;

    sao_plugins_ctx_destroy(context);
    CHECK(probe.resources.empty());
    CHECK(calls_with_prefix(probe, "window.close:").size() == 2);
    CHECK(calls_with_prefix(probe, "file.release:").size() == 2);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("plugin context converts a throwing platform callback into a status",
          "[plugins][loader][context][platform][throw-barrier]") {
    capability_probe probe;
    probe.throw_open_window = true;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_plugin("platform_throw");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    CHECK(sao_plugins_ctx_open_window(context, "throw", 320, 240) == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.resources.empty());

    sao_plugins_ctx_destroy(context);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("platform session destroy and provider release retry independently",
          "[plugins][loader][context][platform][teardown][retry]") {
    capability_probe probe;
    probe.throw_release = true;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_plugin("platform_release_retry");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    sao_plugins_ctx_destroy(context);
    CHECK(probe.destroy_session_calls == 1);
    CHECK(probe.release_calls == 1);
    CHECK(sao_plugins_ctx_unregister_platform_provider() == SAO_PLUGINS_ERR_BUSY);

    probe.throw_release = false;
    sao_plugins_ctx_destroy(context);
    CHECK(probe.destroy_session_calls == 1);
    CHECK(probe.release_calls == 2);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("failed platform create quarantines a returned session until cleanup can retry",
          "[plugins][loader][context][platform][create][quarantine][retry]") {
    capability_probe probe;
    probe.create_session_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_OS_CALL_FAILED;
    probe.destroy_session_status = SAO_ERR_OS_CALL_FAILED;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_plugin("platform_create_quarantine");
    CHECK(sao_plugins_ctx_create(handle) == nullptr);
    CHECK(probe.create_session_calls == 1);
    CHECK(probe.destroy_session_calls == 1);
    CHECK(probe.release_calls == 0);
    CHECK(sao_plugins_ctx_unregister_platform_provider() == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.destroy_session_calls == 2);

    probe.destroy_session_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    probe.throw_release = true;
    CHECK(sao_plugins_ctx_unregister_platform_provider() == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.destroy_session_calls == 3);
    CHECK(probe.release_calls == 1);

    probe.throw_release = false;
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    CHECK(probe.destroy_session_calls == 3);
    CHECK(probe.release_calls == 2);
    remove_plugin(handle);
}

TEST_CASE("platform callback unload reentry is busy before lifecycle state or stop changes",
          "[plugins][loader][context][platform][lifecycle][reentry]") {
    capability_probe probe;
    auto provider = make_provider(probe);
    const auto adapter = make_adapter();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::csharp, &adapter) == SAO_OK);

    plugin_manifest manifest;
    manifest.plugin_id = "platform_unload_reentry";
    manifest.name = manifest.plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.cs";
    manifest.language = engine_kind::csharp;
    manifest.source_path = std::filesystem::temp_directory_path().string();
    manifest.abi_version = 2;
    plugin_handle_t handle = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    probe.reentry_plugin = handle;
    probe.reentry_context = context;
    probe.reenter_unload = true;
    REQUIRE(sao_plugins_ctx_open_window(context, "reentry", 320, 240) == SAO_OK);
    CHECK(probe.nested_unload_status == SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.nested_unload_state == lifecycle_state::loaded_disabled);
    CHECK_FALSE(probe.nested_unload_should_stop);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    CHECK_FALSE(sao_plugins_ctx_should_stop(context));

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::csharp) == SAO_OK);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_plugin(handle);
}
