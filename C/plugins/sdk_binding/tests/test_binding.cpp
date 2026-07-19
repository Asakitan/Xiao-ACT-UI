#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/sdk_binding/binding_angel.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/plugins/sdk_binding/binding_emma.h"
#include "sao/plugins/sdk_binding/binding_lua.h"

#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sao::plugins::sdk_binding;

namespace {

struct fixture_state {
    int loads = 0;
    int unloads = 0;
    int invokes = 0;
    int dispatches = 0;
    int releases = 0;
    int next_callback_id = 0;
    bool available = true;
    bool fail_unload = false;
    bool fail_load_with_plugin = false;
    bool fail_wrap = false;
    bool throw_wrap = false;
    bool throw_release = false;
    bool track_callback_in_dispatch = false;
    bool release_during_unload = false;
    bool runtime_alive = false;
    bool release_after_runtime_teardown = false;
    bool reenter_release = false;
    bool block_release = false;
    bool release_entered = false;
    bool allow_release = false;
    int32_t track_status = SAO_OK;
    language_host_kind language = language_host_kind::emma;
    std::mutex mutex;
    std::condition_variable release_cv;
    std::vector<std::string> events;
};

struct fixture_plugin {
    fixture_state* state = nullptr;
};

struct fixture_callback {
    fixture_state* state = nullptr;
    int id = 0;
};

bool SAO_PLUGINS_CALL available(void* user_data) {
    return static_cast<fixture_state*>(user_data)->available;
}

int32_t SAO_PLUGINS_CALL load(void* context, void* runtime, void** out_plugin, void* user_data) {
    if (context == nullptr || runtime == nullptr || out_plugin == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* state = static_cast<fixture_state*>(user_data);
    ++state->loads;
    state->runtime_alive = true;
    *out_plugin = new fixture_plugin{state};
    return state->fail_load_with_plugin ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

int32_t SAO_PLUGINS_CALL unload(void* plugin, void* /*user_data*/) {
    auto* instance = static_cast<fixture_plugin*>(plugin);
    {
        std::lock_guard lock(instance->state->mutex);
        instance->state->events.emplace_back("unload:begin");
        if (instance->state->fail_unload)
            return SAO_ERR_OS_CALL_FAILED;
    }
    if (instance->state->release_during_unload) {
        sao_plugins_binding_release_all_callbacks();
    }
    {
        std::lock_guard lock(instance->state->mutex);
        instance->state->events.emplace_back("unload:end");
        instance->state->runtime_alive = false;
    }
    ++instance->state->unloads;
    delete instance;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL invoke(void* plugin, const char* method, const uint8_t* /*arguments*/,
                                size_t /*arguments_size*/, uint8_t* output, size_t output_capacity,
                                size_t* required, char* error, size_t error_capacity,
                                void* /*user_data*/) {
    auto* instance = static_cast<fixture_plugin*>(plugin);
    ++instance->state->invokes;
    if (std::strcmp(method, "fail") == 0) {
        std::snprintf(error, error_capacity, "%s", "fixture error");
        return SAO_ERR_OS_CALL_FAILED;
    }
    constexpr char result[] = "{\"ok\":true}";
    *required = sizeof(result);
    if (output == nullptr || output_capacity < sizeof(result)) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, result, sizeof(result));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL dispatch(language_binding_operation operation,
                                  language_binding_request* request, void* user_data) {
    auto* state = static_cast<fixture_state*>(user_data);
    ++state->dispatches;
    if (operation != language_binding_operation::callback_wrap)
        return SAO_OK;
    auto callback = std::make_unique<fixture_callback>();
    callback->state = state;
    callback->id = ++state->next_callback_id;
    *request->out_callback = callback.get();
    *request->out_user_data = callback.release();
    if (state->track_callback_in_dispatch) {
        state->track_status = sao_plugins_binding_track_callback(
            *request->out_user_data, +[](void* callback_user_data) {
                auto tracked = std::unique_ptr<fixture_callback>(
                    static_cast<fixture_callback*>(callback_user_data));
                std::lock_guard lock(tracked->state->mutex);
                ++tracked->state->releases;
                tracked->state->events.push_back("release:" + std::to_string(tracked->id));
                if (!tracked->state->runtime_alive) {
                    tracked->state->release_after_runtime_teardown = true;
                }
            });
    }
    if (state->throw_wrap)
        throw std::runtime_error("wrap fixture");
    if (state->fail_wrap)
        return SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
}

void SAO_PLUGINS_CALL release_provider_callback(void* callback_user_data, void* user_data) {
    auto callback =
        std::unique_ptr<fixture_callback>(static_cast<fixture_callback*>(callback_user_data));
    auto* state = static_cast<fixture_state*>(user_data);
    std::unique_lock lock(state->mutex);
    ++state->releases;
    state->events.push_back("release:" + std::to_string(callback->id));
    if (!state->runtime_alive)
        state->release_after_runtime_teardown = true;
    if (state->reenter_release) {
        lock.unlock();
        sao_plugins_binding_release_callback(state->language, callback_user_data);
        lock.lock();
    }
    if (state->block_release) {
        state->release_entered = true;
        state->release_cv.notify_all();
        state->release_cv.wait(lock, [state] { return state->allow_release; });
    }
    if (state->throw_release)
        throw std::runtime_error("release fixture");
}

int32_t SAO_PLUGINS_CALL throwing_barrier(void*) {
    throw std::runtime_error("barrier fixture");
}

language_host_adapter_vtable fixture_adapter(language_host_kind language, fixture_state* state,
                                             uint32_t flags = 0) {
    language_host_adapter_vtable adapter{};
    adapter.flags = flags;
    adapter.language = language;
    adapter.available = &available;
    adapter.load_plugin = &load;
    adapter.unload_plugin = &unload;
    adapter.invoke = &invoke;
    adapter.dispatch = &dispatch;
    adapter.release_callback = &release_provider_callback;
    adapter.user_data = state;
    state->language = language;
    return adapter;
}

struct old_language_host_adapter_vtable {
    uint32_t struct_size = sizeof(old_language_host_adapter_vtable);
    uint32_t abi_version = SAO_LANGUAGE_HOST_PROVIDER_ABI_VERSION;
    uint32_t flags = 0;
    language_host_kind language = language_host_kind::python;
    bool(SAO_PLUGINS_CALL* available)(void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* load_plugin)(void* context, void* runtime, void** out_plugin,
                                           void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* unload_plugin)(void* plugin, void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* invoke)(void* plugin, const char* method_name_utf8,
                                      const uint8_t* args_json_utf8, size_t args_size,
                                      uint8_t* out_result_json_utf8, size_t out_capacity,
                                      size_t* out_required, char* out_error_utf8,
                                      size_t error_capacity, void* user_data) = nullptr;
    int32_t(SAO_PLUGINS_CALL* dispatch)(language_binding_operation operation,
                                        language_binding_request* request,
                                        void* user_data) = nullptr;
    void* user_data = nullptr;
};

static_assert(sizeof(old_language_host_adapter_vtable) == SAO_LANGUAGE_HOST_ADAPTER_V1_SIZE);

old_language_host_adapter_vtable old_fixture_adapter(language_host_kind language,
                                                     fixture_state* state) {
    old_language_host_adapter_vtable adapter{};
    adapter.language = language;
    adapter.available = &available;
    adapter.load_plugin = &load;
    adapter.unload_plugin = &unload;
    adapter.invoke = &invoke;
    adapter.dispatch = &dispatch;
    adapter.user_data = state;
    state->language = language;
    return adapter;
}

void wrap_emma_callback(emma_interpreter_ptr runtime, void** out_callback, void** out_user_data,
                        int32_t expected = SAO_OK) {
    static int callable = 0;
    assert(sao_plugins_binding_emma_wrap_callback(runtime,
                                                  reinterpret_cast<emma_value_ptr>(&callable),
                                                  out_callback, out_user_data) == expected);
}

void case_language_release_apis() {
    static int context = 0;
    static int runtime = 0;
    static int callable = 0;
    for (const auto language : {language_host_kind::lua, language_host_kind::angel,
                                language_host_kind::emma, language_host_kind::csharp}) {
        fixture_state state{};
        const auto adapter = fixture_adapter(language, &state);
        assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
        plugin_binding_handle_t plugin = nullptr;
        assert(sao_plugins_binding_plugin_load(language, &context, &runtime, &plugin) == SAO_OK);
        void* callback = nullptr;
        void* user_data = nullptr;
        int32_t status = SAO_ERR_NOT_IMPLEMENTED;
        switch (language) {
        case language_host_kind::lua:
            status = sao_plugins_binding_lua_wrap_callback(reinterpret_cast<lua_State*>(&runtime),
                                                           1, &callback, &user_data);
            break;
        case language_host_kind::angel:
            status = sao_plugins_binding_angel_wrap_callback(
                reinterpret_cast<asIScriptEngine*>(&runtime),
                reinterpret_cast<asIScriptFunction*>(&callable), &callback, &user_data);
            break;
        case language_host_kind::emma:
            status = sao_plugins_binding_emma_wrap_callback(
                reinterpret_cast<emma_interpreter_ptr>(&runtime),
                reinterpret_cast<emma_value_ptr>(&callable), &callback, &user_data);
            break;
        case language_host_kind::csharp:
            status = sao_plugins_binding_csharp_wrap_delegate(
                reinterpret_cast<csharp_domain_ptr>(&runtime), &callable, &callback, &user_data);
            break;
        default:
            assert(false);
        }
        assert(status == SAO_OK && callback != nullptr && user_data != nullptr);
        switch (language) {
        case language_host_kind::lua:
            sao_plugins_binding_lua_release_callback(user_data);
            sao_plugins_binding_lua_release_callback(user_data);
            break;
        case language_host_kind::angel:
            sao_plugins_binding_angel_release_callback(user_data);
            sao_plugins_binding_angel_release_callback(user_data);
            break;
        case language_host_kind::emma:
            sao_plugins_binding_emma_release_callback(user_data);
            sao_plugins_binding_emma_release_callback(user_data);
            break;
        case language_host_kind::csharp:
            sao_plugins_binding_csharp_release_delegate(user_data);
            sao_plugins_binding_csharp_release_delegate(user_data);
            break;
        default:
            assert(false);
        }
        assert(state.releases == 1);
        assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
        assert(state.releases == 1);
        assert(sao_plugins_binding_unregister_language_host(language) == SAO_OK);
    }
}

void case_unload_releases_callbacks() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback_a = nullptr;
    void* user_data_a = nullptr;
    void* callback_b = nullptr;
    void* user_data_b = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback_a, &user_data_a);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback_b, &user_data_b);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.releases == 2);
    assert(state.events.size() == 4);
    assert(state.events[0] == "unload:begin");
    assert(state.events[1] == "release:2");
    assert(state.events[2] == "release:1");
    assert(state.events[3] == "unload:end");
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_failed_unload_retains_callbacks() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_unload = true;
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    assert(state.releases == 0);
    state.fail_unload = false;
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.releases == 1);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_failed_wrap_rolls_back_provider_ownership() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    for (const bool throw_wrap : {false, true}) {
        state.fail_wrap = !throw_wrap;
        state.throw_wrap = throw_wrap;
        void* callback = reinterpret_cast<void*>(1);
        void* user_data = reinterpret_cast<void*>(1);
        wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data,
                           SAO_ERR_OS_CALL_FAILED);
        assert(callback == nullptr && user_data == nullptr);
    }
    assert(state.releases == 2);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.releases == 2);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_tracked_failed_wrap_releases_once() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_wrap = true;
    state.track_callback_in_dispatch = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback = reinterpret_cast<void*>(1);
    void* user_data = reinterpret_cast<void*>(1);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data,
                       SAO_ERR_OS_CALL_FAILED);
    assert(state.track_status == SAO_OK);
    assert(callback == nullptr && user_data == nullptr);
    assert(state.releases == 1);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.releases == 1);
    assert(!state.release_after_runtime_teardown);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_failed_load_with_plugin_rolls_back() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_load_with_plugin = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = reinterpret_cast<plugin_binding_handle_t>(1);
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_ERR_OS_CALL_FAILED);
    assert(plugin == nullptr);
    assert(state.loads == 1 && state.unloads == 1);
    assert(!state.runtime_alive);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_old_adapter_prefix_is_compatible() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.track_callback_in_dispatch = true;
    state.release_during_unload = true;
    const auto adapter = old_fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(
               reinterpret_cast<const language_host_adapter_vtable*>(&adapter)) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    assert(plugin != nullptr);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    assert(callback != nullptr && user_data != nullptr);
    assert(state.track_status == SAO_OK && state.releases == 0);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.loads == 1 && state.unloads == 1 && state.releases == 1);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_runtime_teardown_sentinel() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.releases == 1);
    assert(!state.runtime_alive);
    assert(!state.release_after_runtime_teardown);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_missing_teardown_release_fails_without_uaf() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    assert(state.releases == 0);
    assert(!state.runtime_alive);
    assert(!state.release_after_runtime_teardown);
    delete static_cast<fixture_callback*>(user_data);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

struct emit_reentry_probe {
    plugin_binding_handle_t plugin = nullptr;
    int calls = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
};

void emit_reentry_callback(const char*, const char*, void* user_data) {
    auto* probe = static_cast<emit_reentry_probe*>(user_data);
    ++probe->calls;
    char plugin_id[64]{};
    probe->status = sao_plugins_sdk_bind_call(probe->plugin, sdk_method_id::prop_plugin_id, nullptr,
                                              0, plugin_id, sizeof(plugin_id));
}

void case_emit_callback_can_reenter_binding() {
    using namespace sao::plugins::loader;

    plugin_manifest manifest{};
    manifest.plugin_id = "sdk_binding_emit_reentry";
    manifest.name = "sdk binding emit reentry";
    manifest.version = "1";
    manifest.entry = "plugin.emma";
    manifest.language = engine_kind::emma;
    manifest.source_path = ".";

    const auto registry = sao_plugins_registry_instance();
    sao::plugins::loader::plugin_handle_t loader_plugin = nullptr;
    assert(sao_plugins_registry_add_plugin(registry, &manifest, &loader_plugin) == SAO_OK);
    auto* context = sao_plugins_ctx_create(loader_plugin);
    assert(context != nullptr);

    static int runtime = 0;
    fixture_state state{};
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t binding = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, context, &runtime, &binding) ==
           SAO_OK);

    emit_reentry_probe probe{binding};
    uint32_t token = 0;
    assert(sao_plugins_ctx_subscribe(context, "reentry", &emit_reentry_callback, &probe, &token) ==
           SAO_OK);
    constexpr char topic[] = "reentry";
    assert(sao_plugins_sdk_bind_call(binding, sdk_method_id::method_emit, topic, sizeof(topic) - 1,
                                     nullptr, 0) == SAO_OK);
    assert(probe.calls == 1 && probe.status == SAO_OK);
    assert(sao_plugins_ctx_unsubscribe(context, token) == SAO_OK);
    assert(sao_plugins_binding_plugin_unload(binding) == SAO_OK);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    assert(sao_plugins_registry_remove(registry, loader_plugin) == SAO_OK);
}

void case_release_reentry_and_unload_race() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.reenter_release = true;
    state.block_release = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    std::thread releaser([user_data] { sao_plugins_binding_emma_release_callback(user_data); });
    {
        std::unique_lock lock(state.mutex);
        state.release_cv.wait(lock, [&state] { return state.release_entered; });
    }
    assert(sao_plugins_binding_plugin_unload(plugin) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    {
        std::lock_guard lock(state.mutex);
        state.allow_release = true;
    }
    state.release_cv.notify_all();
    releaser.join();
    assert(state.releases == 1);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    assert(state.releases == 1);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

void case_release_failure_is_reported_once() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.throw_release = true;
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_plugin_load(language_host_kind::emma, &context, &runtime, &plugin) ==
           SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    assert(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    assert(state.releases == 1);
    assert(!state.release_after_runtime_teardown);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);
}

} // namespace

int main() {
    assert(json_node::from_string("{\"value\":7}").to_string() == "{\"value\":7}");

    char* error = nullptr;
    assert(sao_plugins_binding_barrier(&throwing_barrier, nullptr, &error) ==
           SAO_ERR_OS_CALL_FAILED);
    assert(error != nullptr && std::strcmp(error, "barrier fixture") == 0);
    sao_plugins_binding_free_error(error);

    static int context = 0;
    static int runtime = 0;
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_emma_activate(reinterpret_cast<plugin_context_ptr>(&context),
                                             reinterpret_cast<emma_interpreter_ptr>(&runtime),
                                             &plugin) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    assert(plugin == nullptr);

    fixture_state python_state{};
    auto python = fixture_adapter(language_host_kind::python, &python_state);
    assert(sao_plugins_binding_register_language_host(&python) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    python.flags = SAO_LANGUAGE_HOST_PROVIDER_ISOLATED_PYTHON_ABI;
    assert(sao_plugins_binding_register_language_host(&python) == SAO_OK);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::python) == SAO_OK);

    fixture_state state{};
    const auto emma = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&emma) == SAO_OK);
    assert(sao_plugins_binding_language_host_available(language_host_kind::emma));
    assert(sao_plugins_binding_emma_activate(reinterpret_cast<plugin_context_ptr>(&context),
                                             reinterpret_cast<emma_interpreter_ptr>(&runtime),
                                             &plugin) == SAO_OK);
    assert(plugin != nullptr && state.loads == 1);

    size_t required = 0;
    assert(sao_plugins_binding_plugin_invoke(plugin, "run", nullptr, 0, nullptr, 0, &required) ==
           SAO_ERR_BUFFER_TOO_SMALL);
    assert(required == sizeof("{\"ok\":true}"));
    char output[32]{};
    assert(sao_plugins_binding_plugin_invoke(plugin, "run", nullptr, 0,
                                             reinterpret_cast<uint8_t*>(output), sizeof(output),
                                             &required) == SAO_OK);
    assert(std::strcmp(output, "{\"ok\":true}") == 0);

    assert(sao_plugins_binding_plugin_invoke(plugin, "fail", nullptr, 0, nullptr, 0, &required) ==
           SAO_ERR_OS_CALL_FAILED);
    assert(sao_plugins_binding_plugin_last_error(plugin, nullptr, 0, &required) ==
           SAO_ERR_BUFFER_TOO_SMALL);
    char last_error[32]{};
    assert(sao_plugins_binding_plugin_last_error(plugin, last_error, sizeof(last_error),
                                                 &required) == SAO_OK);
    assert(std::strcmp(last_error, "fixture error") == 0);

    language_binding_request request{};
    assert(sao_plugins_binding_dispatch_provider(language_host_kind::emma,
                                                 language_binding_operation::method_table,
                                                 &request) == SAO_OK);
    assert(state.dispatches == 1);

    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    assert(sao_plugins_binding_emma_deactivate(plugin) == SAO_OK);
    assert(state.unloads == 1 && state.invokes == 3);
    assert(sao_plugins_binding_unregister_language_host(language_host_kind::emma) == SAO_OK);

    case_language_release_apis();
    case_unload_releases_callbacks();
    case_failed_unload_retains_callbacks();
    case_failed_wrap_rolls_back_provider_ownership();
    case_tracked_failed_wrap_releases_once();
    case_failed_load_with_plugin_rolls_back();
    case_old_adapter_prefix_is_compatible();
    case_runtime_teardown_sentinel();
    case_missing_teardown_release_fails_without_uaf();
    case_emit_callback_can_reenter_binding();
    case_release_reentry_and_unload_race();
    case_release_failure_is_reported_once();

    std::printf("sdk_binding provider fixture passed\n");
    return 0;
}
