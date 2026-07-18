#include "sao/plugins/lua_host/lua_module_bridge.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "lua_bridge_internal.h"
#include "lua_json_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)

namespace {

using loader_context_t = sao::plugins::loader::plugin_context_t;

struct bridge_state;

struct event_callback_record {
    std::atomic<lua_State*> state{nullptr};
    std::shared_ptr<std::recursive_mutex> state_mutex;
    int function_ref = LUA_NOREF;
    uint32_t token = 0;
    uintptr_t dispatch_id = 0;
    std::atomic_bool closing{false};
};

struct bridge_state {
    lua_State* state = nullptr;
    loader_context_t* context = nullptr;
    std::shared_ptr<std::recursive_mutex> mutex =
        std::make_shared<std::recursive_mutex>();
    bool closing = false;
    std::unordered_map<uint32_t, std::shared_ptr<event_callback_record>>
        callbacks;
};

std::mutex g_bridge_map_mutex;
std::unordered_map<lua_State*, std::unique_ptr<bridge_state>> g_bridge_map;
std::unordered_map<lua_State*, std::vector<std::unique_ptr<bridge_state>>>
    g_retired_bridges;
std::mutex g_callback_map_mutex;
std::unordered_map<uintptr_t, std::shared_ptr<event_callback_record>>
    g_callback_map;
std::atomic_uintptr_t g_next_callback_id{1};

bridge_state* checked_bridge(lua_State* state) {
    auto** slot = static_cast<bridge_state**>(
        luaL_checkudata(state, 1, "SaoPluginContext"));
    if (slot == nullptr || *slot == nullptr || (*slot)->closing ||
        (*slot)->context == nullptr) {
        luaL_error(state, "plugin context is no longer available");
        return nullptr;
    }
    return *slot;
}

struct method_failure final {
    const char* operation = "ctx method";
    int32_t status = SAO_ERR_OS_CALL_FAILED;
};

[[noreturn]] int push_status_error(lua_State*, const char* operation,
                                   int32_t status) {
    throw method_failure{operation, status};
}

bool stack_json(lua_State* state, int index, std::string& serialized) {
    detail::json value;
    std::string error;
    if (!detail::stack_to_json(state, index, value, error)) {
        throw method_failure{"JSON conversion",
                             SAO_ERR_INVALID_ARGUMENT};
    }
    serialized = value.dump();
    return true;
}

int push_owned_json(lua_State* state, int32_t status, char* value,
                    const char* operation) {
    if (status != SAO_OK) {
        sao::plugins::loader::sao_plugins_ctx_free_string(value);
        return push_status_error(state, operation, status);
    }
    const auto parsed = detail::json::parse(
        value == nullptr ? "null" : value, nullptr, false);
    std::string conversion_error;
    const int32_t conversion = !parsed.is_discarded() &&
                                       detail::protected_push_json(
                                           state, parsed, conversion_error)
                                   ? SAO_OK
                                   : SAO_ERR_INVALID_ARGUMENT;
    sao::plugins::loader::sao_plugins_ctx_free_string(value);
    if (conversion != SAO_OK) {
        return push_status_error(state, operation, conversion);
    }
    return 1;
}

void event_callback(const char*, const char* event_json_utf8,
                    void* user_data) noexcept {
    try {
        const uintptr_t dispatch_id = reinterpret_cast<uintptr_t>(user_data);
        std::shared_ptr<event_callback_record> callback;
        {
            std::lock_guard map_lock(g_callback_map_mutex);
            const auto found = g_callback_map.find(dispatch_id);
            if (found == g_callback_map.end()) return;
            callback = found->second;
        }
        lua_State* state = callback->state.load(std::memory_order_acquire);
        if (state == nullptr) return;
        detail::state_operation operation;
        if (detail::acquire_state_operation(state, operation) != SAO_OK) {
            return;
        }
        if (callback->closing.load(std::memory_order_acquire) ||
            callback->state.load(std::memory_order_acquire) != state ||
            callback->function_ref == LUA_NOREF) {
            return;
        }
        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback->function_ref);
        const auto event = detail::json::parse(
            event_json_utf8 == nullptr ? "null" : event_json_utf8,
            nullptr, false);
        std::string conversion_error;
        if (event.is_discarded() ||
            !detail::protected_push_json(state, event, conversion_error)) {
            lua_settop(state, base);
            return;
        }
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
        }
    } catch (...) {
    }
}

int ctx_log(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* message = luaL_checkstring(state, 2);
    sao::plugins::loader::sao_plugins_ctx_log(bridge->context, message);
    return 0;
}

int ctx_set_defaults(lua_State* state) {
    auto* bridge = checked_bridge(state);
    std::string defaults;
    if (!stack_json(state, 2, defaults)) return 0;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_defaults(
        bridge->context, defaults.c_str());
    if (status != SAO_OK) return push_status_error(state, "set_defaults", status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_get_setting(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* key = luaL_checkstring(state, 2);
    char* json = nullptr;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_get_setting(
        bridge->context, key, &json);
    if (status == SAO_ERR_HANDLE_INVALID && lua_gettop(state) >= 3) {
        lua_pushvalue(state, 3);
        return 1;
    }
    return push_owned_json(state, status, json, "get_setting");
}

int ctx_set_setting(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* key = luaL_checkstring(state, 2);
    std::string value;
    if (!stack_json(state, 3, value)) return 0;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_setting(
        bridge->context, key, value.c_str());
    if (status != SAO_OK) return push_status_error(state, "set_setting", status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_register_ui_panel(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* panel_id = luaL_checkstring(state, 2);
    std::string metadata;
    if (!stack_json(state, 3, metadata)) return 0;
    if ((lua_gettop(state) >= 4 && !lua_isnoneornil(state, 4)) ||
        (lua_gettop(state) >= 5 && !lua_isnoneornil(state, 5))) {
        return push_status_error(
            state, "register_ui_panel callbacks",
            sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_register_ui_panel(
            bridge->context, panel_id, metadata.c_str(), nullptr, nullptr,
            nullptr);
    if (status != SAO_OK) {
        return push_status_error(state, "register_ui_panel", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int subscribe_impl(lua_State* state, bool once) {
    auto* bridge = checked_bridge(state);
    const char* topic = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TFUNCTION);
    lua_pushvalue(state, 3);
    const int function_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    std::shared_ptr<event_callback_record> callback;
    try {
        callback = std::make_shared<event_callback_record>();
    } catch (...) {
        luaL_unref(state, LUA_REGISTRYINDEX, function_ref);
        throw;
    }
    callback->state = state;
    callback->state_mutex = detail::state_mutex(state);
    callback->function_ref = function_ref;
    try {
        std::lock_guard map_lock(g_callback_map_mutex);
        do {
            callback->dispatch_id = g_next_callback_id.fetch_add(1);
        } while (callback->dispatch_id == 0 ||
                 g_callback_map.contains(callback->dispatch_id));
        g_callback_map.emplace(callback->dispatch_id, callback);
    } catch (...) {
        luaL_unref(state, LUA_REGISTRYINDEX, function_ref);
        throw;
    }
    const auto rollback = [&](bool unsubscribe) noexcept {
        callback->closing = true;
        callback->state = nullptr;
        if (unsubscribe && bridge->context != nullptr) {
            (void)sao::plugins::loader::sao_plugins_ctx_unsubscribe(
                bridge->context, callback->token);
        }
        {
            std::lock_guard map_lock(g_callback_map_mutex);
            g_callback_map.erase(callback->dispatch_id);
        }
        luaL_unref(state, LUA_REGISTRYINDEX, function_ref);
    };
    uint32_t token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status =
            once ? sao::plugins::loader::sao_plugins_ctx_subscribe_once(
                       bridge->context, topic, event_callback,
                       reinterpret_cast<void*>(callback->dispatch_id),
                       &token)
                 : sao::plugins::loader::sao_plugins_ctx_subscribe(
                       bridge->context, topic, event_callback,
                       reinterpret_cast<void*>(callback->dispatch_id),
                       &token);
    } catch (...) {
        rollback(false);
        throw;
    }
    if (status != SAO_OK) {
        rollback(false);
        return push_status_error(state, once ? "subscribe_once" : "subscribe",
                                 status);
    }
    callback->token = token;
    bool inserted = false;
    try {
        inserted = bridge->callbacks.emplace(token, callback).second;
    } catch (...) {
        rollback(true);
        throw;
    }
    if (!inserted) {
        rollback(true);
        return push_status_error(
            state, once ? "subscribe_once" : "subscribe",
            sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    }
    lua_pushinteger(state, static_cast<lua_Integer>(token));
    return 1;
}

int ctx_subscribe(lua_State* state) { return subscribe_impl(state, false); }
int ctx_subscribe_once(lua_State* state) { return subscribe_impl(state, true); }

int ctx_unsubscribe(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const auto token = static_cast<uint32_t>(luaL_checkinteger(state, 2));
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_unsubscribe(
        bridge->context, token);
    if (status != SAO_OK) return push_status_error(state, "unsubscribe", status);
    const auto found = bridge->callbacks.find(token);
    if (found != bridge->callbacks.end()) {
        found->second->closing = true;
        found->second->state = nullptr;
        {
            std::lock_guard map_lock(g_callback_map_mutex);
            g_callback_map.erase(found->second->dispatch_id);
        }
        luaL_unref(state, LUA_REGISTRYINDEX, found->second->function_ref);
        bridge->callbacks.erase(found);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_emit(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* topic = luaL_checkstring(state, 2);
    std::string payload = "null";
    if (lua_gettop(state) >= 3 && !lua_isnil(state, 3) &&
        !stack_json(state, 3, payload)) {
        return 0;
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_emit(
        bridge->context, topic, payload.c_str());
    if (status != SAO_OK) return push_status_error(state, "emit", status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_get_snapshot(lua_State* state) {
    auto* bridge = checked_bridge(state);
    char* value = nullptr;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_get_snapshot(bridge->context,
                                                            &value);
    return push_owned_json(state, status, value, "get_snapshot");
}

int ctx_recent_events(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const auto limit = static_cast<uint32_t>(luaL_optinteger(state, 2, 64));
    const char* topic = luaL_optstring(state, 3, nullptr);
    char* value = nullptr;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_recent_events(
        bridge->context, limit, topic, &value);
    return push_owned_json(state, status, value, "recent_events");
}

int unsupported(lua_State* state) {
    const char* operation = lua_tostring(state, lua_upvalueindex(1));
    return luaL_error(
        state, "%s failed with status %d",
        operation == nullptr ? "ctx operation" : operation,
        sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
}

template <int (*Function)(lua_State*)>
int safe_method(lua_State* state) noexcept {
    const char* operation = nullptr;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        return Function(state);
    } catch (const method_failure& failure) {
        operation = failure.operation;
        status = failure.status;
    } catch (...) {
        operation = "ctx method crossed C++ exception boundary";
    }
    return luaL_error(state, "%s failed with status %d", operation, status);
}

void set_method(lua_State* state, const char* name, lua_CFunction function) {
    lua_pushcfunction(state, function);
    lua_setfield(state, -2, name);
}

void set_unsupported(lua_State* state, const char* name) {
    lua_pushstring(state, name);
    lua_pushcclosure(state, unsupported, 1);
    lua_setfield(state, -2, name);
}

int register_ctx_body(lua_State* state) {
    auto* bridge = static_cast<bridge_state*>(lua_touserdata(state, 1));
    if (bridge == nullptr) return luaL_error(state, "invalid ctx bridge");
    auto** slot = static_cast<bridge_state**>(
        lua_newuserdatauv(state, sizeof(bridge_state*), 0));
    *slot = bridge;
    if (luaL_newmetatable(state, "SaoPluginContext") != 0) {
        lua_newtable(state);
        set_method(state, "log", safe_method<ctx_log>);
        set_method(state, "set_defaults", safe_method<ctx_set_defaults>);
        set_method(state, "get_setting", safe_method<ctx_get_setting>);
        set_method(state, "setting", safe_method<ctx_get_setting>);
        set_method(state, "set_setting", safe_method<ctx_set_setting>);
        set_method(state, "register_ui_panel",
                   safe_method<ctx_register_ui_panel>);
        set_method(state, "subscribe", safe_method<ctx_subscribe>);
        set_method(state, "subscribe_once", safe_method<ctx_subscribe_once>);
        set_method(state, "unsubscribe", safe_method<ctx_unsubscribe>);
        set_method(state, "emit", safe_method<ctx_emit>);
        set_method(state, "get_snapshot", safe_method<ctx_get_snapshot>);
        set_method(state, "recent_events", safe_method<ctx_recent_events>);
        for (const char* name : {
                 "register_render_hook", "set_overlay", "clear_overlay",
                 "request_redraw", "register_hotkey", "set_interval",
                 "set_timeout", "clear_timer", "notify", "toast",
                 "open_file", "open_window", "register_engine",
                 "get_engine", "load_local", "ensure_requirements"}) {
            set_unsupported(state, name);
        }
        lua_setfield(state, -2, "__index");
        lua_pushliteral(state, "locked");
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
    lua_setglobal(state, "ctx");
    return 0;
}

int clear_ctx_body(lua_State* state) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushliteral(state, "ctx");
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
    return 0;
}

} // namespace

namespace detail {

std::shared_ptr<std::recursive_mutex> bridge_mutex(lua_State* state) noexcept {
    return state_mutex(state);
}

int32_t teardown_ctx_bridge_locked(lua_State* state) noexcept {
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        bridge_state* bridge = nullptr;
        {
            std::lock_guard lock(g_bridge_map_mutex);
            const auto found = g_bridge_map.find(state);
            if (found == g_bridge_map.end()) return SAO_OK;
            auto& retired = g_retired_bridges[state];
            retired.reserve(retired.size() + 1);
            retired.push_back(std::move(found->second));
            bridge = retired.back().get();
            g_bridge_map.erase(found);
        }
        bridge->closing = true;
        for (auto& [token, callback] : bridge->callbacks) {
            callback->closing.store(true, std::memory_order_release);
            callback->state.store(nullptr, std::memory_order_release);
            {
                std::lock_guard map_lock(g_callback_map_mutex);
                g_callback_map.erase(callback->dispatch_id);
            }
            if (bridge->context != nullptr) {
                (void)sao::plugins::loader::sao_plugins_ctx_unsubscribe(
                    bridge->context, token);
            }
            luaL_unref(state, LUA_REGISTRYINDEX, callback->function_ref);
        }
        bridge->callbacks.clear();
        int32_t status = SAO_OK;
        if (protected_function(state, clear_ctx_body, 0, 0) != LUA_OK) {
            capture_state_error_locked(state, -1);
            lua_pop(state, 1);
            status = SAO_ERR_OS_CALL_FAILED;
        }
        bridge->context = nullptr;
        bridge->state = nullptr;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t teardown_ctx_bridge(lua_State* state) noexcept {
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    state_operation operation;
    const int32_t status = acquire_state_operation(state, operation);
    return status == SAO_OK ? teardown_ctx_bridge_locked(state) : status;
}

void release_ctx_bridges_locked(lua_State* state) noexcept {
    try {
        std::lock_guard lock(g_bridge_map_mutex);
        g_retired_bridges.erase(state);
    } catch (...) {
    }
}

} // namespace detail

#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ctx(lua_State* state, void* ctx_handle) {
    if (state == nullptr || ctx_handle == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    try {
        detail::state_operation operation;
        int32_t status =
            detail::acquire_state_operation(state, operation);
        if (status != SAO_OK) return status;
        auto bridge = std::make_unique<bridge_state>();
        bridge->state = state;
        bridge->context =
            static_cast<sao::plugins::loader::plugin_context_t*>(ctx_handle);
        bridge->mutex = detail::state_mutex(state);
        if (!bridge->mutex) return SAO_ERR_HANDLE_INVALID;
        bridge_state* bridge_ptr = bridge.get();
        {
            std::lock_guard lock(g_bridge_map_mutex);
            if (g_bridge_map.contains(state)) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            g_bridge_map.emplace(state, std::move(bridge));
        }
        const int base = lua_gettop(state);
        if (detail::protected_trampoline(state, register_ctx_body,
                                         bridge_ptr, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            (void)detail::teardown_ctx_bridge_locked(state);
            return SAO_ERR_OS_CALL_FAILED;
        }
        lua_settop(state, base);
        return SAO_OK;
    } catch (...) {
        (void)detail::teardown_ctx_bridge(state);
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ui(lua_State* state) {
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_mem(lua_State* state, void* ctx_handle) {
    if (state == nullptr || ctx_handle == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_engine(lua_State* state, void* ctx_handle) {
    if (state == nullptr || ctx_handle == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_python_compat(lua_State* state, bool enable) {
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return enable ? sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED : SAO_OK;
}

} // namespace sao::plugins::lua_host
