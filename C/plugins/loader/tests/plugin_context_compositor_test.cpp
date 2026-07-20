#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace sao::plugins::loader;

namespace {

struct provider_probe;

struct provider_session {
    provider_probe* owner = nullptr;
    std::string plugin_id;
};

struct provider_probe {
    int retain_calls = 0;
    int release_calls = 0;
    int create_session_calls = 0;
    int quiesce_session_calls = 0;
    int destroy_session_calls = 0;
    uint64_t next_token = 100;
    std::vector<std::string> calls;
    std::unordered_map<uint64_t, std::string> layers;
    plugin_context_t* reentry_context = nullptr;
    bool invoke_cursor_callback = false;
    bool reenter_create = false;
    bool create_reentered = false;
    bool reenter_destroy = false;
    bool destroy_reentered = false;
    int32_t nested_create_status = SAO_ERR_NOT_INITIALIZED;
    int32_t nested_destroy_status = SAO_OK;
    int32_t create_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t upload_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t position_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t visible_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t input_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    int32_t destroy_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
};

provider_session* session_cast(plugin_context_platform_session_t session) {
    return static_cast<provider_session*>(session);
}

void SAO_PLUGINS_CALL provider_retain(void* user_data) {
    ++static_cast<provider_probe*>(user_data)->retain_calls;
}

void SAO_PLUGINS_CALL provider_release(void* user_data) {
    ++static_cast<provider_probe*>(user_data)->release_calls;
}

int32_t SAO_PLUGINS_CALL provider_create_session(void* user_data,
                                                 const plugin_context_platform_session_spec* spec,
                                                 plugin_context_platform_session_t* out_session) {
    if (user_data == nullptr || spec == nullptr || out_session == nullptr ||
        spec->struct_size < sizeof(plugin_context_platform_session_spec) ||
        spec->plugin_id_utf8 == nullptr) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* probe = static_cast<provider_probe*>(user_data);
    auto session = std::make_unique<provider_session>();
    session->owner = probe;
    session->plugin_id = spec->plugin_id_utf8;
    *out_session = session.release();
    ++probe->create_session_calls;
    probe->calls.push_back("session.create:" + std::string(spec->plugin_id_utf8));
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL provider_quiesce_session(void*,
                                                  plugin_context_platform_session_t session) {
    if (session == nullptr) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* state = session_cast(session);
    ++state->owner->quiesce_session_calls;
    state->owner->calls.push_back("session.quiesce:" + state->plugin_id);
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL provider_destroy_session(void*,
                                                  plugin_context_platform_session_t session) {
    if (session == nullptr) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<provider_session> state(session_cast(session));
    ++state->owner->destroy_session_calls;
    state->owner->calls.push_back("session.destroy:" + state->plugin_id);
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL
provider_create_compositor_layer(void*, plugin_context_platform_session_t session,
                                 const plugin_context_compositor_layer_spec* spec,
                                 plugin_context_platform_token_t* out_provider_token) {
    if (session == nullptr || spec == nullptr || out_provider_token == nullptr ||
        spec->struct_size < sizeof(plugin_context_compositor_layer_spec) ||
        spec->name_utf8 == nullptr) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("layer.create:" + state->plugin_id + ":" + spec->name_utf8);
    if (probe->create_status != SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK) {
        return probe->create_status;
    }
    if (probe->reenter_create && !probe->create_reentered) {
        probe->create_reentered = true;
        probe->nested_create_status = sao_plugins_ctx_create_compositor_layer(
            probe->reentry_context, spec->name_utf8, spec->width, spec->height, spec->x, spec->y,
            spec->z, spec->click_through, spec->high_fps, spec->target_fps);
    }
    const uint64_t token = ++probe->next_token;
    probe->layers.emplace(token, state->plugin_id + ":" + spec->name_utf8);
    *out_provider_token = token;
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL provider_upload_compositor_frame(
    void*, plugin_context_platform_session_t session,
    plugin_context_platform_token_t provider_token, const uint8_t*, size_t, uint32_t, uint32_t) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("layer.upload:" + probe->layers.at(provider_token));
    return probe->upload_status;
}

int32_t SAO_PLUGINS_CALL provider_set_compositor_layer_position(
    void*, plugin_context_platform_session_t session,
    plugin_context_platform_token_t provider_token, int32_t, int32_t) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("layer.position:" + probe->layers.at(provider_token));
    return probe->position_status;
}

int32_t SAO_PLUGINS_CALL
provider_set_compositor_layer_visible(void*, plugin_context_platform_session_t session,
                                      plugin_context_platform_token_t provider_token, bool) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("layer.visible:" + probe->layers.at(provider_token));
    return probe->visible_status;
}

int32_t SAO_PLUGINS_CALL
provider_set_compositor_layer_input(void*, plugin_context_platform_session_t session,
                                    plugin_context_platform_token_t provider_token,
                                    const plugin_context_compositor_input_spec* spec) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    probe->calls.push_back("layer.input:" + probe->layers.at(provider_token));
    if (probe->input_status != SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK) {
        return probe->input_status;
    }
    if (probe->invoke_cursor_callback && spec->cursor_pos != nullptr) {
        spec->cursor_pos(12.0F, 34.0F, spec->user_data);
    }
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL
provider_destroy_compositor_layer(void*, plugin_context_platform_session_t session,
                                  plugin_context_platform_token_t provider_token) {
    auto* state = session_cast(session);
    auto* probe = state->owner;
    const auto found = probe->layers.find(provider_token);
    if (found == probe->layers.end()) {
        return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
    }
    const std::string owned_name = found->second;
    probe->calls.push_back("layer.destroy:" + owned_name);
    if (probe->destroy_status != SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK) {
        return probe->destroy_status;
    }
    if (probe->reenter_destroy && !probe->destroy_reentered) {
        probe->destroy_reentered = true;
        const auto separator = owned_name.find(':');
        probe->nested_destroy_status = sao_plugins_ctx_destroy_compositor_layer(
            probe->reentry_context, owned_name.substr(separator + 1).c_str());
    }
    probe->layers.erase(found);
    return SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

plugin_context_platform_provider make_provider(provider_probe& probe) {
    plugin_context_platform_provider provider{};
    provider.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &probe;
    provider.retain = provider_retain;
    provider.release = provider_release;
    provider.create_session = provider_create_session;
    provider.quiesce_session = provider_quiesce_session;
    provider.destroy_session = provider_destroy_session;
    provider.create_compositor_layer = provider_create_compositor_layer;
    provider.upload_compositor_frame = provider_upload_compositor_frame;
    provider.set_compositor_layer_position = provider_set_compositor_layer_position;
    provider.set_compositor_layer_visible = provider_set_compositor_layer_visible;
    provider.destroy_compositor_layer = provider_destroy_compositor_layer;
    provider.set_compositor_layer_input = provider_set_compositor_layer_input;
    return provider;
}

plugin_handle_t add_context_plugin(const char* plugin_id) {
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
    REQUIRE(handle != nullptr);
    return handle;
}

void remove_context_plugin(plugin_handle_t handle) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) == SAO_OK);
}

struct callback_reentry {
    plugin_context_t* context = nullptr;
    int calls = 0;
    int32_t status = SAO_ERR_NOT_INITIALIZED;
};

void reentrant_cursor_callback(float, float, void* user_data) {
    auto* reentry = static_cast<callback_reentry*>(user_data);
    ++reentry->calls;
    reentry->status = sao_plugins_ctx_set_compositor_layer_visible(reentry->context, "hud", false);
}

} // namespace

TEST_CASE("plugin context compositor gates unsupported capabilities after validation",
          "[plugins][loader][context][compositor]") {
    auto handle = add_context_plugin("compositor_gate");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    wchar_t* selected_path = reinterpret_cast<wchar_t*>(1);
    CHECK(sao_plugins_ctx_open_file(nullptr, nullptr, nullptr, nullptr, 0, &selected_path) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(selected_path == nullptr);
    CHECK(sao_plugins_ctx_open_file(context, "{", nullptr, nullptr, 0, &selected_path) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_open_file(context, "[]", nullptr, nullptr, 0, &selected_path) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(selected_path == nullptr);
    CHECK(sao_plugins_ctx_open_window(context, nullptr, 640, 480) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_open_window(context, "panel", 0, 480) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_open_window(context, "panel", 640, 480) == SAO_PLUGINS_ERR_UNSUPPORTED);

    const std::vector<uint8_t> frame(4 * 4 * 4, 0x7f);
    CHECK(sao_plugins_ctx_create_compositor_layer(context, nullptr, 4, 4, 0, 0, 1, false, false,
                                                  60) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_create_compositor_layer(context, "hud", 0, 4, 0, 0, 1, false, false,
                                                  60) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_create_compositor_layer(context, "hud", 4, 4, 0, 0, 1, false, false,
                                                  60) == SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao_plugins_ctx_upload_compositor_frame(context, "hud", frame.data(), frame.size() - 1, 4,
                                                  4) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_upload_compositor_frame(context, "hud", frame.data(), frame.size(), 4,
                                                  4) == SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao_plugins_ctx_set_compositor_layer_position(context, "hud", 1, 2) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao_plugins_ctx_set_compositor_layer_visible(context, "hud", true) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao_plugins_ctx_destroy_compositor_layer(context, "hud") == SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao_plugins_ctx_set_compositor_layer_input(context, "hud", nullptr, nullptr, nullptr,
                                                     nullptr,
                                                     nullptr) == SAO_PLUGINS_ERR_UNSUPPORTED);

    sao_plugins_ctx_destroy(context);
    remove_context_plugin(handle);
}

TEST_CASE("plugin context compositor preserves ABI 1.0 provider compatibility",
          "[plugins][loader][context][compositor][abi]") {
    provider_probe probe;
    auto provider = make_provider(probe);
    provider.abi_version = 1u << 16u;
    provider.struct_size =
        static_cast<uint32_t>(offsetof(plugin_context_platform_provider, create_compositor_layer));
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_context_plugin("compositor_abi_1_0");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    CHECK(sao_plugins_ctx_create_compositor_layer(context, "hud", 2, 2, 0, 0, 0, true, false, 0) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);

    sao_plugins_ctx_destroy(context);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_context_plugin(handle);
}

TEST_CASE("plugin context compositor forwards owned tokens and tolerates callback reentry",
          "[plugins][loader][context][compositor][reentry]") {
    provider_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_context_plugin("compositor_forward");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    probe.reentry_context = context;

    REQUIRE(sao_plugins_ctx_create_compositor_layer(context, "hud", 4, 4, 10, 20, 140, false, true,
                                                    120) == SAO_OK);
    CHECK(sao_plugins_ctx_create_compositor_layer(context, "hud", 4, 4, 10, 20, 140, false, true,
                                                  120) == SAO_PLUGINS_ERR_ALREADY_EXISTS);

    const std::vector<uint8_t> frame(4 * 4 * 4, 0x7f);
    CHECK(sao_plugins_ctx_upload_compositor_frame(context, "hud", frame.data(), frame.size() - 1, 4,
                                                  4) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_ctx_upload_compositor_frame(context, "hud", frame.data(), frame.size(), 4,
                                                    4) == SAO_OK);
    REQUIRE(sao_plugins_ctx_set_compositor_layer_position(context, "hud", 30, 40) == SAO_OK);
    REQUIRE(sao_plugins_ctx_set_compositor_layer_visible(context, "hud", true) == SAO_OK);

    callback_reentry reentry{context};
    probe.invoke_cursor_callback = true;
    REQUIRE(sao_plugins_ctx_set_compositor_layer_input(context, "hud", reentrant_cursor_callback,
                                                       nullptr, nullptr, nullptr,
                                                       &reentry) == SAO_OK);
    CHECK(reentry.calls == 1);
    CHECK(reentry.status == SAO_OK);

    probe.reenter_destroy = true;
    REQUIRE(sao_plugins_ctx_destroy_compositor_layer(context, "hud") == SAO_OK);
    CHECK(probe.destroy_reentered);
    CHECK(probe.nested_destroy_status == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_ctx_destroy_compositor_layer(context, "hud") == SAO_ERR_HANDLE_INVALID);
    CHECK(probe.layers.empty());

    sao_plugins_ctx_destroy(context);
    CHECK(probe.quiesce_session_calls == 1);
    CHECK(probe.destroy_session_calls == 1);
    CHECK(probe.retain_calls == 1);
    CHECK(probe.release_calls == 1);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_context_plugin(handle);
}

TEST_CASE("compositor create preserves retryable ownership when local publication rollback fails",
          "[plugins][loader][context][compositor][create][rollback][ownership][hardening][focused]") {
    provider_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_context_plugin("compositor_create_rollback");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    probe.reentry_context = context;
    probe.reenter_create = true;
    probe.destroy_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_OS_CALL_FAILED;

    CHECK(sao_plugins_ctx_create_compositor_layer(context, "race", 2, 2, 0, 0, 0, true, false,
                                                  0) == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.create_reentered);
    CHECK(probe.nested_create_status == SAO_OK);
    CHECK(probe.layers.size() == 2);

    probe.destroy_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    sao_plugins_ctx_destroy(context);
    CHECK(probe.layers.empty());
    CHECK(static_cast<size_t>(std::count_if(probe.calls.begin(), probe.calls.end(),
                                            [](const std::string& call) {
                                                return call.starts_with("layer.destroy:");
                                            })) == 3);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_context_plugin(handle);
}

TEST_CASE("plugin context compositor maps provider statuses and tears down in reverse order",
          "[plugins][loader][context][compositor][teardown]") {
    provider_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto handle = add_context_plugin("compositor_teardown");
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    probe.create_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ALREADY_EXISTS;
    CHECK(sao_plugins_ctx_create_compositor_layer(context, "provider_duplicate", 2, 2, 0, 0, 0,
                                                  true, false,
                                                  0) == SAO_PLUGINS_ERR_ALREADY_EXISTS);
    probe.create_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;

    REQUIRE(sao_plugins_ctx_create_compositor_layer(context, "alpha", 2, 2, 0, 0, 0, true, false,
                                                    0) == SAO_OK);
    REQUIRE(sao_plugins_ctx_create_compositor_layer(context, "beta", 2, 2, 0, 0, 0, true, false,
                                                    0) == SAO_OK);
    REQUIRE(sao_plugins_ctx_create_compositor_layer(context, "gamma", 2, 2, 0, 0, 0, true, false,
                                                    0) == SAO_OK);

    const std::vector<uint8_t> frame(2 * 2 * 4, 0x20);
    probe.upload_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNSUPPORTED;
    CHECK(sao_plugins_ctx_upload_compositor_frame(context, "alpha", frame.data(), frame.size(), 2,
                                                  2) == SAO_PLUGINS_ERR_UNSUPPORTED);
    probe.upload_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    probe.position_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
    CHECK(sao_plugins_ctx_set_compositor_layer_position(context, "alpha", 1, 2) ==
          SAO_ERR_HANDLE_INVALID);
    probe.position_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    probe.visible_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ACCESS_DENIED;
    CHECK(sao_plugins_ctx_set_compositor_layer_visible(context, "alpha", true) ==
          SAO_ERR_OS_CALL_FAILED);
    probe.visible_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    probe.input_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    CHECK(sao_plugins_ctx_set_compositor_layer_input(context, "alpha", nullptr, nullptr, nullptr,
                                                     nullptr, nullptr) == SAO_ERR_INVALID_ARGUMENT);
    probe.input_status = SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;

    REQUIRE(sao_plugins_ctx_destroy_compositor_layer(context, "beta") == SAO_OK);
    sao_plugins_ctx_destroy(context);

    std::vector<std::string> destroyed;
    for (const auto& call : probe.calls) {
        if (call.starts_with("layer.destroy:"))
            destroyed.push_back(call);
    }
    REQUIRE(destroyed == std::vector<std::string>{"layer.destroy:compositor_teardown:beta",
                                                  "layer.destroy:compositor_teardown:gamma",
                                                  "layer.destroy:compositor_teardown:alpha"});
    REQUIRE(probe.layers.empty());
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_context_plugin(handle);
}

TEST_CASE("plugin context compositor tokens are isolated per plugin session",
          "[plugins][loader][context][compositor][ownership]") {
    provider_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    auto first_handle = add_context_plugin("compositor_owner_a");
    auto second_handle = add_context_plugin("compositor_owner_b");
    auto* first = sao_plugins_ctx_create(first_handle);
    auto* second = sao_plugins_ctx_create(second_handle);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    REQUIRE(sao_plugins_ctx_create_compositor_layer(first, "first_only", 2, 2, 0, 0, 0, true, false,
                                                    0) == SAO_OK);
    REQUIRE(sao_plugins_ctx_create_compositor_layer(second, "second_only", 2, 2, 0, 0, 0, true,
                                                    false, 0) == SAO_OK);
    CHECK(sao_plugins_ctx_destroy_compositor_layer(first, "second_only") == SAO_ERR_HANDLE_INVALID);
    CHECK(sao_plugins_ctx_destroy_compositor_layer(second, "first_only") == SAO_ERR_HANDLE_INVALID);

    sao_plugins_ctx_destroy(first);
    sao_plugins_ctx_destroy(second);
    CHECK(probe.layers.empty());
    CHECK(probe.retain_calls == 2);
    CHECK(probe.release_calls == 2);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    remove_context_plugin(first_handle);
    remove_context_plugin(second_handle);
}
