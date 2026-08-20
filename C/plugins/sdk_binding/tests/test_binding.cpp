#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/sdk_binding/binding_angel.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/plugins/sdk_binding/binding_emma.h"
#include "sao/plugins/sdk_binding/binding_lua.h"

#include <cstdlib>
#include <chrono>
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

[[noreturn]] void sao_test_assertion_failure(const char* file, int line,
                                              const char* expression) {
    std::fprintf(stderr, "assertion failed at %s:%d: %s\n", file, line, expression);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

#define SAO_TEST_ASSERT(condition)                                             \
    do {                                                                       \
        if (!(condition))                                                      \
            sao_test_assertion_failure(__FILE__, __LINE__, #condition);        \
    } while (false)

struct fixture_state {
    int loads = 0;
    int unloads = 0;
    int invokes = 0;
    int dispatches = 0;
    int releases = 0;
    int release_attempts = 0;
    int next_callback_id = 0;
    bool available = true;
    bool fail_unload = false;
    bool fail_load_with_plugin = false;
    bool load_returns_null = false;
    bool fail_wrap = false;
    bool throw_wrap = false;
    bool throw_release = false;
    bool throw_release_once = false;
    bool track_callback_in_dispatch = false;
    bool release_during_wrap = false;
    bool track_callbacks_during_load = false;
    size_t callbacks_during_load = 0;
    bool release_during_unload = false;
    bool runtime_alive = false;
    bool release_after_runtime_teardown = false;
    bool reenter_release = false;
    bool reentrant_release_invoked = false;
    void* reentrant_target = nullptr;
    void* callback_override = nullptr;
    bool preserve_callback_storage = false;
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

class joining_thread {
  public:
    explicit joining_thread(std::thread thread) : thread_(std::move(thread)) {}
    ~joining_thread() {
        if (thread_.joinable())
            thread_.join();
    }

    joining_thread(const joining_thread&) = delete;
    joining_thread& operator=(const joining_thread&) = delete;

    void join() {
        if (thread_.joinable())
            thread_.join();
    }

  private:
    std::thread thread_;
};

bool SAO_PLUGINS_CALL available(void* user_data) {
    return static_cast<fixture_state*>(user_data)->available;
}

void SAO_PLUGINS_CALL release_legacy_callback(void* callback_user_data) {
    auto* callback = static_cast<fixture_callback*>(callback_user_data);
    auto* state = callback->state;
    ++state->release_attempts;
    if (state->throw_release || state->throw_release_once) {
        state->throw_release_once = false;
        throw std::runtime_error("release fixture");
    }
    ++state->releases;
    if (!state->preserve_callback_storage)
        delete callback;
}

void SAO_PLUGINS_CALL release_provider_callback(void* callback_user_data, void* user_data) {
    auto* callback = static_cast<fixture_callback*>(callback_user_data);
    auto* state = static_cast<fixture_state*>(user_data);
    std::unique_lock lock(state->mutex);
    ++state->release_attempts;
    if (!state->runtime_alive)
        state->release_after_runtime_teardown = true;
    if (state->reenter_release) {
        state->reentrant_release_invoked = true;
        state->reenter_release = false;
        void* target = state->reentrant_target != nullptr ? state->reentrant_target
                                                          : callback_user_data;
        lock.unlock();
        sao_plugins_binding_release_callback(state->language, target);
        lock.lock();
    }
    if (state->block_release) {
        state->release_entered = true;
        state->release_cv.notify_all();
        state->release_cv.wait(lock, [state] { return state->allow_release; });
    }
    if (state->throw_release || state->throw_release_once) {
        state->throw_release_once = false;
        throw std::runtime_error("provider release fixture");
    }
    ++state->releases;
    state->events.push_back("release:" + std::to_string(callback->id));
    if (!state->preserve_callback_storage)
        delete callback;
}

int32_t SAO_PLUGINS_CALL load(void* context, void* runtime, void** out_plugin, void* user_data) {
    if (context == nullptr || runtime == nullptr || out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* state = static_cast<fixture_state*>(user_data);
    ++state->loads;
    state->runtime_alive = true;
    if (state->track_callbacks_during_load) {
        for (size_t index = 0; index < state->callbacks_during_load; ++index) {
            auto* callback = new fixture_callback{state, ++state->next_callback_id};
            const int32_t status = sao_plugins_binding_track_callback(
                callback, &release_legacy_callback);
            if (status != SAO_OK)
                release_legacy_callback(callback);
        }
    }
    if (state->load_returns_null) {
        *out_plugin = nullptr;
        return SAO_OK;
    }
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
    if (instance->state->release_during_unload)
        sao_plugins_binding_release_all_callbacks();
    {
        std::lock_guard lock(instance->state->mutex);
        instance->state->events.emplace_back("unload:end");
        instance->state->runtime_alive = false;
    }
    ++instance->state->unloads;
    delete instance;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL invoke(void* plugin, const char* method,
                                const uint8_t* /*arguments*/, size_t /*arguments_size*/,
                                uint8_t* output, size_t output_capacity, size_t* required,
                                char* error, size_t error_capacity, void* /*user_data*/) {
    auto* instance = static_cast<fixture_plugin*>(plugin);
    ++instance->state->invokes;
    if (std::strcmp(method, "fail") == 0) {
        std::snprintf(error, error_capacity, "%s", "fixture error");
        return SAO_ERR_OS_CALL_FAILED;
    }
    constexpr char result[] = "{\"ok\":true}";
    *required = sizeof(result);
    if (output == nullptr || output_capacity < sizeof(result))
        return SAO_ERR_BUFFER_TOO_SMALL;
    std::memcpy(output, result, sizeof(result));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL dispatch(language_binding_operation operation,
                                  language_binding_request* request, void* user_data) {
    auto* state = static_cast<fixture_state*>(user_data);
    ++state->dispatches;
    if (operation != language_binding_operation::callback_wrap)
        return SAO_OK;
    auto* callback = state->callback_override != nullptr
                         ? static_cast<fixture_callback*>(state->callback_override)
                         : new fixture_callback{state, ++state->next_callback_id};
    *request->out_callback = callback;
    *request->out_user_data = callback;
    if (state->track_callback_in_dispatch) {
        state->track_status = sao_plugins_binding_track_callback(
            callback, &release_legacy_callback);
    }
    if (state->release_during_wrap)
        sao_plugins_binding_release_callback(state->language, callback);
    if (state->throw_wrap)
        throw std::runtime_error("wrap fixture");
    if (state->fail_wrap)
        return SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
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
    SAO_TEST_ASSERT(sao_plugins_binding_emma_wrap_callback(
                        runtime, reinterpret_cast<emma_value_ptr>(&callable), out_callback,
                        out_user_data) == expected);
}

void case_callback_capacity_busy_caller_releases() {
    static int context = 0;
    static int runtime = 0;
    {
        fixture_state state{};
        state.track_callbacks_during_load = true;
        state.callbacks_during_load = SAO_SDK_BINDING_MAX_CALLBACK_OWNERSHIP + 1;
        const auto adapter = fixture_adapter(language_host_kind::emma, &state);
        SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
        plugin_binding_handle_t binding = nullptr;
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                            language_host_kind::emma, &context, &runtime, &binding) == SAO_OK);
        SAO_TEST_ASSERT(binding != nullptr);
        SAO_TEST_ASSERT(state.releases == 1);
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(binding) == SAO_OK);
        SAO_TEST_ASSERT(state.releases ==
                        static_cast<int>(SAO_SDK_BINDING_MAX_CALLBACK_OWNERSHIP + 1));
        SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                        SAO_OK);
    }
    fixture_state unscoped{};
    auto* caller_owned = new fixture_callback{&unscoped, ++unscoped.next_callback_id};
    SAO_TEST_ASSERT(sao_plugins_binding_track_callback(caller_owned, &release_legacy_callback) ==
                    SAO_ERR_NOT_INITIALIZED);
    release_legacy_callback(caller_owned);
    sao_plugins_binding_release_all_callbacks();
    SAO_TEST_ASSERT(unscoped.releases == 1);
}

void case_language_release_apis() {
    static int context = 0;
    static int runtime = 0;
    static int callable = 0;
    for (const auto language : {language_host_kind::lua, language_host_kind::angel,
                                language_host_kind::emma, language_host_kind::csharp}) {
        fixture_state state{};
        const auto adapter = fixture_adapter(language, &state);
        SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
        plugin_binding_handle_t plugin = nullptr;
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(language, &context, &runtime, &plugin) ==
                        SAO_OK);
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
            SAO_TEST_ASSERT(false);
        }
        SAO_TEST_ASSERT(status == SAO_OK && callback != nullptr && user_data != nullptr);
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
            SAO_TEST_ASSERT(false);
        }
        SAO_TEST_ASSERT(state.releases == 1 && state.release_attempts == 1);
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
        SAO_TEST_ASSERT(state.releases == 1);
        SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language) == SAO_OK);
    }
}

void case_unload_releases_callbacks() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback_a = nullptr;
    void* user_data_a = nullptr;
    void* callback_b = nullptr;
    void* user_data_b = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback_a, &user_data_a);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback_b, &user_data_b);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 2);
    SAO_TEST_ASSERT(state.events.size() == 4);
    SAO_TEST_ASSERT(state.events[0] == "release:2");
    SAO_TEST_ASSERT(state.events[1] == "release:1");
    SAO_TEST_ASSERT(state.events[2] == "unload:begin");
    SAO_TEST_ASSERT(state.events[3] == "unload:end");
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_failed_unload_retains_callbacks() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_unload = true;
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(state.releases == 1);
    state.fail_unload = false;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_failed_wrap_rolls_back_provider_ownership() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    for (const bool throw_wrap : {false, true}) {
        state.fail_wrap = !throw_wrap;
        state.throw_wrap = throw_wrap;
        void* callback = reinterpret_cast<void*>(1);
        void* user_data = reinterpret_cast<void*>(1);
        wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data,
                           SAO_ERR_OS_CALL_FAILED);
        SAO_TEST_ASSERT(callback == nullptr && user_data == nullptr);
    }
    SAO_TEST_ASSERT(state.releases == 2 && state.release_attempts == 2);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 2);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_tracked_failed_wrap_releases_once() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_wrap = true;
    state.track_callback_in_dispatch = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = reinterpret_cast<void*>(1);
    void* user_data = reinterpret_cast<void*>(1);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data,
                       SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(state.track_status == SAO_OK);
    SAO_TEST_ASSERT(callback == nullptr && user_data == nullptr);
    SAO_TEST_ASSERT(state.releases == 1 && state.release_attempts == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1);
    SAO_TEST_ASSERT(!state.release_after_runtime_teardown);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_tracked_success_wrap_released_before_return_is_not_reowned() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.track_callback_in_dispatch = true;
    state.release_during_wrap = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = reinterpret_cast<void*>(1);
    void* user_data = reinterpret_cast<void*>(1);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data,
                       SAO_ERR_HANDLE_INVALID);
    SAO_TEST_ASSERT(callback == nullptr && user_data == nullptr);
    SAO_TEST_ASSERT(state.track_status == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1 && state.release_attempts == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1 && state.release_attempts == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_failed_wrap_release_failure_is_retained_for_unload() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_wrap = true;
    state.throw_release_once = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = reinterpret_cast<void*>(1);
    void* user_data = reinterpret_cast<void*>(1);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data,
                       SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(callback == nullptr && user_data == nullptr);
    SAO_TEST_ASSERT(state.release_attempts == 1 && state.releases == 0);
    SAO_TEST_ASSERT(state.runtime_alive);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.release_attempts == 2 && state.releases == 1);
    SAO_TEST_ASSERT(!state.release_after_runtime_teardown);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_duplicate_callback_handle_across_bindings_is_ambiguous() {
    static int context = 0;
    static int runtime_a = 0;
    static int runtime_b = 0;
    fixture_state state{};
    state.preserve_callback_storage = true;
    auto* shared_callback = new fixture_callback{&state, ++state.next_callback_id};
    state.callback_override = shared_callback;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t binding_a = nullptr;
    plugin_binding_handle_t binding_b = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime_a, &binding_a) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime_b, &binding_b) == SAO_OK);
    void* callback_a = nullptr;
    void* user_data_a = nullptr;
    void* callback_b = nullptr;
    void* user_data_b = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime_a), &callback_a,
                       &user_data_a);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime_b), &callback_b,
                       &user_data_b);
    SAO_TEST_ASSERT(user_data_a == shared_callback && user_data_b == shared_callback);
    sao_plugins_binding_release_callback(language_host_kind::emma, shared_callback);
    SAO_TEST_ASSERT(state.releases == 0);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(binding_a) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1);
    state.runtime_alive = true;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(binding_b) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 2);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
    delete shared_callback;
}

void case_failed_load_with_plugin_rolls_back() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.fail_load_with_plugin = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = reinterpret_cast<plugin_binding_handle_t>(1);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) ==
                    SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(plugin == nullptr);
    SAO_TEST_ASSERT(state.loads == 1 && state.unloads == 1);
    SAO_TEST_ASSERT(!state.runtime_alive);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_load_rollback_variants() {
    static int context = 0;
    static int runtime = 0;
    {
        fixture_state state{};
        state.fail_load_with_plugin = true;
        state.track_callbacks_during_load = true;
        state.callbacks_during_load = 1;
        state.throw_release_once = true;
        const auto adapter = fixture_adapter(language_host_kind::emma, &state);
        SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
        plugin_binding_handle_t binding = nullptr;
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                            language_host_kind::emma, &context, &runtime, &binding) ==
                        SAO_ERR_OS_CALL_FAILED);
        SAO_TEST_ASSERT(binding != nullptr);
        SAO_TEST_ASSERT(state.unloads == 0 && state.releases == 0);
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(binding) == SAO_OK);
        SAO_TEST_ASSERT(state.unloads == 1 && state.releases == 1);
        SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                        SAO_OK);
    }
    {
        fixture_state state{};
        state.load_returns_null = true;
        state.track_callbacks_during_load = true;
        state.callbacks_during_load = 1;
        const auto adapter = fixture_adapter(language_host_kind::emma, &state);
        SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
        plugin_binding_handle_t binding = reinterpret_cast<plugin_binding_handle_t>(1);
        SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                            language_host_kind::emma, &context, &runtime, &binding) ==
                        SAO_ERR_NOT_INITIALIZED);
        SAO_TEST_ASSERT(binding == nullptr);
        SAO_TEST_ASSERT(state.unloads == 0 && state.releases == 1);
        sao_plugins_binding_release_all_callbacks();
        SAO_TEST_ASSERT(state.releases == 1);
        SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                        SAO_OK);
    }
}

void case_old_adapter_prefix_is_compatible() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.track_callback_in_dispatch = true;
    state.release_during_unload = true;
    const auto adapter = old_fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(
                        reinterpret_cast<const language_host_adapter_vtable*>(&adapter)) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    SAO_TEST_ASSERT(plugin != nullptr);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    SAO_TEST_ASSERT(callback != nullptr && user_data != nullptr);
    SAO_TEST_ASSERT(state.track_status == SAO_OK && state.releases == 0);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.loads == 1 && state.unloads == 1 && state.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_runtime_teardown_sentinel() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.release_during_unload = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1);
    SAO_TEST_ASSERT(!state.runtime_alive);
    SAO_TEST_ASSERT(!state.release_after_runtime_teardown);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_missing_teardown_release_fails_without_uaf() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    state.throw_release = true;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(state.releases == 0);
    SAO_TEST_ASSERT(state.runtime_alive);
    SAO_TEST_ASSERT(!state.release_after_runtime_teardown);
    state.throw_release = false;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
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

void case_host_generation_rebind_after_teardown() {
    static int context = 0;
    static int runtime = 0;
    fixture_state first{};
    const auto first_adapter = fixture_adapter(language_host_kind::emma, &first);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&first_adapter) == SAO_OK);
    plugin_binding_handle_t first_plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &first_plugin) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(first_plugin) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);

    fixture_state second{};
    const auto second_adapter = fixture_adapter(language_host_kind::emma, &second);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&second_adapter) == SAO_OK);
    plugin_binding_handle_t second_plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &second_plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(second_plugin) == SAO_OK);
    SAO_TEST_ASSERT(second.loads == 1 && second.unloads == 1 && second.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
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
    plugin_handle_t loader_plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_registry_add_plugin(registry, &manifest, &loader_plugin) == SAO_OK);
    auto* context = sao_plugins_ctx_create(loader_plugin);
    SAO_TEST_ASSERT(context != nullptr);

    static int runtime = 0;
    fixture_state state{};
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t binding = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, context, &runtime, &binding) == SAO_OK);

    emit_reentry_probe probe{binding};
    uint32_t token = 0;
    SAO_TEST_ASSERT(sao_plugins_ctx_subscribe(context, "reentry", &emit_reentry_callback, &probe,
                                              &token) == SAO_OK);
    constexpr char topic[] = "reentry";
    SAO_TEST_ASSERT(sao_plugins_sdk_bind_call(binding, sdk_method_id::method_emit, topic,
                                              sizeof(topic) - 1, nullptr, 0) == SAO_OK);
    SAO_TEST_ASSERT(probe.calls == 1 && probe.status == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_ctx_unsubscribe(context, token) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(binding) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
    sao_plugins_ctx_destroy(context);
    SAO_TEST_ASSERT(sao_plugins_registry_remove(registry, loader_plugin) == SAO_OK);
}

void case_reentrant_release_under_current_scope() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.reenter_release = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback_a = nullptr;
    void* user_data_a = nullptr;
    void* callback_b = nullptr;
    void* user_data_b = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback_a, &user_data_a);
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback_b, &user_data_b);
    state.reentrant_target = user_data_a;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.reentrant_release_invoked);
    SAO_TEST_ASSERT(state.releases == 2);
    SAO_TEST_ASSERT(state.release_attempts == 2);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}
void case_release_reentry_and_unload_race() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.block_release = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    joining_thread releaser(std::thread([user_data] {
        sao_plugins_binding_emma_release_callback(user_data);
    }));
    bool release_entered = false;
    {
        std::unique_lock lock(state.mutex);
        release_entered = state.release_cv.wait_for(
            lock, std::chrono::seconds(5), [&state] { return state.release_entered; });
    }
    int32_t busy_status = SAO_OK;
    if (release_entered)
        busy_status = sao_plugins_binding_plugin_unload(plugin);
    {
        std::lock_guard lock(state.mutex);
        state.allow_release = true;
    }
    state.release_cv.notify_all();
    releaser.join();
    SAO_TEST_ASSERT(release_entered);
    SAO_TEST_ASSERT(busy_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    SAO_TEST_ASSERT(state.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_cleanup_pending_retry_after_release_failure() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.throw_release_once = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(state.unloads == 0 && state.releases == 0 && state.release_attempts == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.unloads == 1 && state.releases == 1 && state.release_attempts == 2);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

void case_release_failure_is_reported_once() {
    static int context = 0;
    static int runtime = 0;
    fixture_state state{};
    state.throw_release = true;
    const auto adapter = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&adapter) == SAO_OK);
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_load(
                        language_host_kind::emma, &context, &runtime, &plugin) == SAO_OK);
    void* callback = nullptr;
    void* user_data = nullptr;
    wrap_emma_callback(reinterpret_cast<emma_interpreter_ptr>(&runtime), &callback, &user_data);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(state.release_attempts == 1 && state.releases == 0);
    SAO_TEST_ASSERT(state.runtime_alive);
    SAO_TEST_ASSERT(!state.release_after_runtime_teardown);
    state.throw_release = false;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_unload(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.release_attempts == 2 && state.releases == 1);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);
}

} // namespace

int main() {
    SAO_TEST_ASSERT(json_node::from_string("{\"value\":7}").to_string() == "{\"value\":7}");

    char* error = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_barrier(
                        reinterpret_cast<barrier_fn>(&throwing_barrier), nullptr, &error) ==
                    SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(error != nullptr && std::strcmp(error, "barrier fixture") == 0);
    sao_plugins_binding_free_error(error);

    static int context = 0;
    static int runtime = 0;
    plugin_binding_handle_t plugin = nullptr;
    SAO_TEST_ASSERT(sao_plugins_binding_emma_activate(
                        reinterpret_cast<plugin_context_ptr>(&context),
                        reinterpret_cast<emma_interpreter_ptr>(&runtime), &plugin) ==
                    sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    SAO_TEST_ASSERT(plugin == nullptr);

    fixture_state python_state{};
    auto python = fixture_adapter(language_host_kind::python, &python_state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&python) ==
                    sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    python.flags = SAO_LANGUAGE_HOST_PROVIDER_ISOLATED_PYTHON_ABI;
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&python) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::python) ==
                    SAO_OK);

    fixture_state state{};
    const auto emma = fixture_adapter(language_host_kind::emma, &state);
    SAO_TEST_ASSERT(sao_plugins_binding_register_language_host(&emma) == SAO_OK);
    SAO_TEST_ASSERT(sao_plugins_binding_language_host_available(language_host_kind::emma));
    SAO_TEST_ASSERT(sao_plugins_binding_emma_activate(
                        reinterpret_cast<plugin_context_ptr>(&context),
                        reinterpret_cast<emma_interpreter_ptr>(&runtime), &plugin) == SAO_OK);
    SAO_TEST_ASSERT(plugin != nullptr && state.loads == 1);

    size_t required = 0;
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_invoke(
                        plugin, "run", nullptr, 0, nullptr, 0, &required) == SAO_ERR_BUFFER_TOO_SMALL);
    SAO_TEST_ASSERT(required == sizeof("{\"ok\":true}"));
    char output[32]{};
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_invoke(
                        plugin, "run", nullptr, 0, reinterpret_cast<uint8_t*>(output),
                        sizeof(output), &required) == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(output, "{\"ok\":true}") == 0);

    SAO_TEST_ASSERT(sao_plugins_binding_plugin_invoke(
                        plugin, "fail", nullptr, 0, nullptr, 0, &required) ==
                    SAO_ERR_OS_CALL_FAILED);
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_last_error(plugin, nullptr, 0, &required) ==
                    SAO_ERR_BUFFER_TOO_SMALL);
    char last_error[32]{};
    SAO_TEST_ASSERT(sao_plugins_binding_plugin_last_error(
                        plugin, last_error, sizeof(last_error), &required) == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(last_error, "fixture error") == 0);

    language_binding_request request{};
    SAO_TEST_ASSERT(sao_plugins_binding_dispatch_provider(
                        language_host_kind::emma, language_binding_operation::method_table,
                        &request) == SAO_OK);
    SAO_TEST_ASSERT(state.dispatches == 1);

    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    SAO_TEST_ASSERT(sao_plugins_binding_emma_deactivate(plugin) == SAO_OK);
    SAO_TEST_ASSERT(state.unloads == 1 && state.invokes == 3);
    SAO_TEST_ASSERT(sao_plugins_binding_unregister_language_host(language_host_kind::emma) ==
                    SAO_OK);

    case_callback_capacity_busy_caller_releases();
    case_language_release_apis();
    case_unload_releases_callbacks();
    case_failed_unload_retains_callbacks();
    case_failed_wrap_rolls_back_provider_ownership();
    case_tracked_failed_wrap_releases_once();
    case_tracked_success_wrap_released_before_return_is_not_reowned();
    case_failed_wrap_release_failure_is_retained_for_unload();
    case_duplicate_callback_handle_across_bindings_is_ambiguous();
    case_failed_load_with_plugin_rolls_back();
    case_load_rollback_variants();
    case_old_adapter_prefix_is_compatible();
    case_runtime_teardown_sentinel();
    case_host_generation_rebind_after_teardown();
    case_missing_teardown_release_fails_without_uaf();
    case_emit_callback_can_reenter_binding();
    case_reentrant_release_under_current_scope();
    case_release_reentry_and_unload_race();
    case_cleanup_pending_retry_after_release_failure();
    case_release_failure_is_reported_once();

    std::printf("sdk_binding provider fixture passed\n");
    return 0;
}
