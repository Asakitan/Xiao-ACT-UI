#include "sao/plugins/lua_host/lua_host.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/lua_host/lua_stdlib.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}
#include "lua_bridge_internal.h"
#include "lua_sandbox_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)

struct lua_host_s {
    lua_State* state = nullptr;
    void (*message_callback)(const char*, int, void*) = nullptr;
    void* callback_user_data = nullptr;
};

namespace {

struct host_control final {
    explicit host_control(lua_host_s* value) : host(value) {}
    lua_host_s* host = nullptr;
    std::recursive_mutex mutex;
    bool closing = false;
    size_t active_operations = 0;
    std::condition_variable_any idle;
};

std::mutex g_hosts_mutex;
std::unordered_map<lua_host_s*, std::shared_ptr<host_control>> g_hosts;
constexpr size_t kMaximumHostOperationNesting = 64;
thread_local std::array<host_control*, kMaximumHostOperationNesting> g_active_host_operations{};
thread_local size_t g_active_host_operation_depth = 0;

bool host_operation_active_on_current_thread(const host_control* control) noexcept {
    return std::find(g_active_host_operations.begin(),
                     g_active_host_operations.begin() + g_active_host_operation_depth,
                     control) != g_active_host_operations.begin() + g_active_host_operation_depth;
}

class host_operation final {
  public:
    host_operation() = default;
    ~host_operation() {
        if (control_ != nullptr) {
            if (g_active_host_operation_depth > 0 &&
                g_active_host_operations[g_active_host_operation_depth - 1] == control_.get()) {
                g_active_host_operations[--g_active_host_operation_depth] = nullptr;
            }
            std::lock_guard lock(control_->mutex);
            if (control_->active_operations > 0)
                --control_->active_operations;
            control_->idle.notify_all();
        }
    }
    host_operation(const host_operation&) = delete;
    host_operation& operator=(const host_operation&) = delete;

    lua_host_s* host() const noexcept {
        return control_ == nullptr ? nullptr : control_->host;
    }

    std::shared_ptr<host_control> control_;
    std::unique_lock<std::recursive_mutex> lock_;
};

int32_t acquire_host(lua_host_handle_t handle, host_operation& operation) {
    if (handle == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    std::shared_ptr<host_control> control;
    {
        std::lock_guard map_lock(g_hosts_mutex);
        const auto found = g_hosts.find(handle);
        if (found == g_hosts.end())
            return SAO_ERR_HANDLE_INVALID;
        control = found->second;
    }
    std::unique_lock lock(control->mutex);
    if (control->closing || control->host != handle) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (g_active_host_operation_depth == kMaximumHostOperationNesting)
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    ++control->active_operations;
    operation.control_ = std::move(control);
    g_active_host_operations[g_active_host_operation_depth++] = operation.control_.get();
    operation.lock_ = std::move(lock);
    operation.lock_.unlock();
    return SAO_OK;
}

void copy_output(std::string_view value, char** output) {
    if (output == nullptr)
        return;
    auto* copy = static_cast<char*>(std::malloc(value.size() + 1));
    if (copy == nullptr)
        return;
    if (!value.empty())
        std::memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    *output = copy;
}

void copy_lua_error(lua_State* state, char** output) {
    if (state == nullptr || lua_gettop(state) == 0)
        return;
    if (lua_type(state, -1) != LUA_TSTRING) {
        copy_output("Lua operation failed", output);
        return;
    }
    size_t length = 0;
    const char* message = lua_tolstring(state, -1, &length);
    if (message != nullptr)
        copy_output(std::string_view(message, length), output);
}

int print_hook(lua_State* state) {
    auto* host = static_cast<lua_host_s*>(lua_touserdata(state, lua_upvalueindex(1)));
    const int argument_count = lua_gettop(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int index = 1; index <= argument_count; ++index) {
        if (index > 1)
            luaL_addchar(&buffer, '\t');
        size_t length = 0;
        const char* value = luaL_tolstring(state, index, &length);
        if (value != nullptr)
            luaL_addlstring(&buffer, value, length);
        lua_pop(state, 1);
    }
    luaL_pushresult(&buffer);
    size_t length = 0;
    const char* message = lua_tolstring(state, -1, &length);
    if (host != nullptr && host->message_callback != nullptr) {
        std::string owned(message == nullptr ? "" : message, length);
        try {
            host->message_callback(owned.c_str(), 0, host->callback_user_data);
        } catch (...) {
            return luaL_error(state, "Lua print callback failed");
        }
    } else if (message != nullptr) {
        std::fwrite(message, 1, length, stdout);
        std::fputc('\n', stdout);
    }
    return 0;
}

int install_print_hook(lua_State* state) {
    void* user_data = lua_touserdata(state, 1);
    lua_pushlightuserdata(state, user_data);
    lua_pushcclosure(state, print_hook, 1);
    lua_setglobal(state, "print");
    return 0;
}

int32_t install_all_stdlib(lua_State* state) {
    lua_stdlib_config config{};
    config.base = true;
    config.coroutine = true;
    config.string_ = true;
    config.table_ = true;
    config.math = true;
    config.utf8 = true;
    config.io = true;
    config.os = true;
    config.package_ = true;
    config.debug_ = true;
    int32_t status = sao_plugins_luahost_install_stdlib(state, &config);
    if (status == SAO_OK) {
        status = sao_plugins_luahost_install_sao_stdlib(state);
    }
    return status;
}

int32_t format_first_result(lua_State* state, int base, char** output) {
    if (lua_gettop(state) <= base || output == nullptr)
        return SAO_OK;
    if (detail::protected_tostring(state, base + 1) != LUA_OK) {
        detail::capture_state_error_locked(state, -1);
        return SAO_ERR_OS_CALL_FAILED;
    }
    size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    if (value == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    copy_output(std::string_view(value, length), output);
    return *output == nullptr ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

struct named_call_data final {
    const char* name = nullptr;
    bool found = false;
};

int call_named_function(lua_State* state) {
    auto* data = static_cast<named_call_data*>(lua_touserdata(state, 1));
    lua_getglobal(state, data->name);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    data->found = true;
    lua_call(state, 0, LUA_MULTRET);
    return lua_gettop(state) - 1;
}

} // namespace

#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_create(const lua_host_config* config, lua_host_handle_t* out_host) {
    if (out_host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;
#if defined(SAO_HAS_LUA)
    try {
        auto host = std::make_unique<lua_host_s>();
        host->state = luaL_newstate();
        if (host->state == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        if (config != nullptr) {
            host->message_callback = config->message_callback;
            host->callback_user_data = config->callback_user_data;
        }
        int32_t status = detail::register_state(host->state);
        if (status != SAO_OK) {
            lua_close(host->state);
            return status;
        }
        auto control = std::make_shared<host_control>(host.get());
        {
            std::lock_guard map_lock(g_hosts_mutex);
            if (!g_hosts.emplace(host.get(), control).second) {
                detail::state_close_operation close;
                if (detail::begin_state_close(host->state, close) == SAO_OK) {
                    lua_close(host->state);
                    detail::finish_state_close(close);
                }
                return SAO_ERR_OS_CALL_FAILED;
            }
        }
        if (config != nullptr && config->install_stdlib) {
            status = install_all_stdlib(host->state);
            if (status == SAO_OK) {
                detail::state_operation operation;
                status = detail::acquire_state_operation(host->state, operation);
                if (status == SAO_OK &&
                    detail::protected_trampoline(host->state, install_print_hook, host.get(), 0) !=
                        LUA_OK) {
                    status = SAO_ERR_OS_CALL_FAILED;
                }
            }
            if (status != SAO_OK) {
                lua_host_s* failed = host.release();
                (void)sao_plugins_luahost_destroy(failed);
                return status;
            }
        }
        *out_host = host.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)config;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_destroy(lua_host_handle_t host) {
    if (host == nullptr)
        return SAO_ERR_HANDLE_INVALID;
#if defined(SAO_HAS_LUA)
    try {
        std::shared_ptr<host_control> control;
        {
            std::lock_guard map_lock(g_hosts_mutex);
            const auto found = g_hosts.find(host);
            if (found == g_hosts.end())
                return SAO_ERR_HANDLE_INVALID;
            control = found->second;
        }
        std::unique_lock host_lock(control->mutex);
        if (host_operation_active_on_current_thread(control.get())) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        if (control->closing) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        control->closing = true;
        const int32_t preflight_status = detail::preflight_state_close(host->state);
        if (preflight_status != SAO_OK) {
            control->closing = false;
            return preflight_status;
        }
        const int32_t cancel_status = detail::request_state_close_cancel(host->state);
        if (cancel_status != SAO_OK) {
            control->closing = false;
            return cancel_status;
        }
        control->idle.wait(host_lock, [&control] { return control->active_operations == 0; });
        if (detail::sandbox_is_armed_locked(host->state) &&
            detail::has_ctx_bridge_locked(host->state)) {
            detail::clear_state_close_cancel(host->state);
            control->closing = false;
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        const int32_t provider_status = detail::quiesce_ctx_menu_providers(host->state);
        if (provider_status != SAO_OK) {
            detail::clear_state_close_cancel(host->state);
            control->closing = false;
            return provider_status;
        }
        detail::state_close_operation close;
        const int32_t close_status = detail::begin_state_close(host->state, close);
        if (close_status != SAO_OK) {
            const int32_t resume_status = detail::resume_ctx_menu_providers_locked(host->state);
            detail::clear_state_close_cancel(host->state);
            control->closing = false;
            return resume_status == SAO_OK ? close_status : resume_status;
        }
        const int32_t bridge_status = detail::teardown_ctx_bridge_locked(host->state);
        if (bridge_status != SAO_OK) {
            const int32_t resume_status = detail::resume_ctx_menu_providers_locked(host->state);
            detail::cancel_state_close(close);
            control->closing = false;
            return resume_status == SAO_OK ? bridge_status : resume_status;
        }
        if (detail::sandbox_is_armed_locked(host->state)) {
            const int32_t sandbox_status = detail::sandbox_disarm_locked(host->state);
            if (sandbox_status != SAO_OK) {
                detail::cancel_state_close(close);
                control->closing = false;
                return sandbox_status;
            }
        }
        detail::release_ctx_bridges_locked(host->state);
        lua_close(host->state);
        detail::finish_state_close(close);
        {
            std::lock_guard map_lock(g_hosts_mutex);
            const auto found = g_hosts.find(host);
            if (found != g_hosts.end() && found->second == control) {
                g_hosts.erase(found);
            }
        }
        control->host = nullptr;
        host_lock.unlock();
        delete host;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API lua_State* SAO_PLUGINS_CALL
sao_plugins_luahost_state(lua_host_handle_t host) {
#if defined(SAO_HAS_LUA)
    if (host == nullptr)
        return nullptr;
    try {
        std::shared_ptr<host_control> control;
        {
            std::lock_guard map_lock(g_hosts_mutex);
            const auto found = g_hosts.find(host);
            if (found == g_hosts.end())
                return nullptr;
            control = found->second;
        }
        std::lock_guard host_lock(control->mutex);
        return control->closing || control->host != host ? nullptr : host->state;
    } catch (...) {
        return nullptr;
    }
#else
    (void)host;
    return nullptr;
#endif
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL sao_plugins_luahost_version(void) {
#if defined(SAO_HAS_LUA)
    return LUA_RELEASE;
#else
    return "lua_host: not available (SAO_HAS_LUA not defined)";
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_execute(lua_host_handle_t host, const char* source_utf8, size_t source_len,
                            char** out_result_utf8, char** out_error_utf8) {
    if (out_result_utf8 != nullptr)
        *out_result_utf8 = nullptr;
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (host == nullptr || source_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#if defined(SAO_HAS_LUA)
    try {
        host_operation host_lease;
        int32_t status = acquire_host(host, host_lease);
        if (status != SAO_OK)
            return status;
        detail::state_operation operation;
        status = detail::acquire_state_operation(host_lease.host()->state, operation);
        if (status != SAO_OK)
            return status;
        lua_State* state = operation.state();
        detail::clear_state_error_locked(state);
        const int base = lua_gettop(state);
        int lua_status = luaL_loadbufferx(state, source_utf8, source_len, "=<execute>", "t");
        if (lua_status != LUA_OK) {
            copy_lua_error(state, out_error_utf8);
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        lua_status = lua_pcall(state, 0, LUA_MULTRET, 0);
        if (lua_status != LUA_OK) {
            copy_lua_error(state, out_error_utf8);
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }
        status = format_first_result(state, base, out_result_utf8);
        if (status != SAO_OK && out_error_utf8 != nullptr) {
            copy_lua_error(state, out_error_utf8);
        }
        lua_settop(state, base);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)source_len;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function(lua_host_handle_t host, const char* function_name,
                                  char** out_result_utf8, char** out_error_utf8) {
    if (out_result_utf8 != nullptr)
        *out_result_utf8 = nullptr;
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (host == nullptr || function_name == nullptr || function_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#if defined(SAO_HAS_LUA)
    try {
        host_operation host_lease;
        int32_t status = acquire_host(host, host_lease);
        if (status != SAO_OK)
            return status;
        detail::state_operation operation;
        status = detail::acquire_state_operation(host_lease.host()->state, operation);
        if (status != SAO_OK)
            return status;
        lua_State* state = operation.state();
        detail::clear_state_error_locked(state);
        const int base = lua_gettop(state);
        named_call_data call{function_name, false};
        if (detail::protected_trampoline(state, call_named_function, &call, LUA_MULTRET) !=
            LUA_OK) {
            copy_lua_error(state, out_error_utf8);
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (!call.found) {
            copy_output("function not found", out_error_utf8);
            return SAO_ERR_HANDLE_INVALID;
        }
        status = format_first_result(state, base, out_result_utf8);
        if (status != SAO_OK && out_error_utf8 != nullptr) {
            copy_lua_error(state, out_error_utf8);
        }
        lua_settop(state, base);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_luahost_free_string(char* value) {
    std::free(value);
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_luahost_is_available(void) {
#if defined(SAO_HAS_LUA)
    return true;
#else
    return false;
#endif
}

} // namespace sao::plugins::lua_host
