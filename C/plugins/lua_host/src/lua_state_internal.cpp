#include "lua_state_internal.h"

#include "sao/plugins/loader/loader_status.h"

#include <atomic>
#include <unordered_map>
#include <utility>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}
#endif

namespace sao::plugins::lua_host::detail {

#if defined(SAO_HAS_LUA)

struct state_control final {
    explicit state_control(lua_State* value)
        : state(value), mutex(std::make_shared<std::recursive_mutex>()) {}

    lua_State* state = nullptr;
    std::shared_ptr<std::recursive_mutex> mutex;
    std::atomic_bool closing{false};
    size_t active_operations = 0;
    size_t live_plugins = 0;
    std::string last_error;
};

namespace {

std::mutex g_states_mutex;
std::unordered_map<lua_State*, std::shared_ptr<state_control>> g_states;

std::shared_ptr<state_control> find_state(lua_State* state) {
    std::lock_guard lock(g_states_mutex);
    const auto found = g_states.find(state);
    return found == g_states.end() ? nullptr : found->second;
}

int tostring_dispatch(lua_State* state) {
    luaL_tolstring(state, 1, nullptr);
    return 1;
}

} // namespace

state_operation::~state_operation() noexcept { release(); }

lua_State* state_operation::state() const noexcept {
    return control_ == nullptr ? nullptr : control_->state;
}

state_operation::operator bool() const noexcept {
    return counted_ && control_ != nullptr && control_->state != nullptr;
}

void state_operation::release() noexcept {
    if (counted_ && control_ != nullptr && control_->active_operations > 0) {
        --control_->active_operations;
    }
    counted_ = false;
    if (lock_.owns_lock()) lock_.unlock();
    control_.reset();
}

state_close_operation::~state_close_operation() noexcept {
    if (control_ != nullptr) cancel_state_close(*this);
}

lua_State* state_close_operation::state() const noexcept {
    return control_ == nullptr ? nullptr : control_->state;
}

state_close_operation::operator bool() const noexcept {
    return control_ != nullptr && lock_.owns_lock() && control_->state != nullptr;
}

int32_t register_state(lua_State* state) noexcept {
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        auto control = std::make_shared<state_control>(state);
        std::lock_guard lock(g_states_mutex);
        return g_states.emplace(state, std::move(control)).second
                   ? SAO_OK
                   : sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t acquire_state_operation(lua_State* state,
                                state_operation& operation) noexcept {
    if (state == nullptr || operation.control_ != nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        auto control = find_state(state);
        if (control == nullptr ||
            control->closing.load(std::memory_order_acquire)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        std::unique_lock lock(*control->mutex);
        if (control->closing.load(std::memory_order_acquire) ||
            control->state != state) {
            return SAO_ERR_HANDLE_INVALID;
        }
        ++control->active_operations;
        operation.control_ = std::move(control);
        operation.lock_ = std::move(lock);
        operation.counted_ = true;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t begin_state_close(lua_State* state,
                          state_close_operation& operation) noexcept {
    if (state == nullptr || operation.control_ != nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::shared_ptr<state_control> control;
        {
            std::lock_guard map_lock(g_states_mutex);
            const auto found = g_states.find(state);
            if (found == g_states.end()) return SAO_ERR_HANDLE_INVALID;
            control = found->second;
            bool expected = false;
            if (!control->closing.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
        }
        std::unique_lock lock(*control->mutex);
        if (control->active_operations != 0 || control->live_plugins != 0) {
            control->closing.store(false, std::memory_order_release);
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        operation.control_ = std::move(control);
        operation.lock_ = std::move(lock);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void cancel_state_close(state_close_operation& operation) noexcept {
    if (operation.control_ != nullptr) {
        operation.control_->closing.store(false, std::memory_order_release);
    }
    if (operation.lock_.owns_lock()) operation.lock_.unlock();
    operation.control_.reset();
}

void finish_state_close(state_close_operation& operation) noexcept {
    if (operation.control_ == nullptr) return;
    lua_State* state = operation.control_->state;
    operation.control_->state = nullptr;
    {
        std::lock_guard map_lock(g_states_mutex);
        const auto found = g_states.find(state);
        if (found != g_states.end() && found->second == operation.control_) {
            g_states.erase(found);
        }
    }
    if (operation.lock_.owns_lock()) operation.lock_.unlock();
    operation.control_.reset();
}

std::shared_ptr<std::recursive_mutex> state_mutex(lua_State* state) noexcept {
    try {
        auto control = find_state(state);
        return control == nullptr ? nullptr : control->mutex;
    } catch (...) {
        return nullptr;
    }
}

int32_t add_live_plugin_locked(lua_State* state) noexcept {
    try {
        auto control = find_state(state);
        if (control == nullptr || control->state != state ||
            control->closing.load(std::memory_order_acquire)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        ++control->live_plugins;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void remove_live_plugin_locked(lua_State* state) noexcept {
    try {
        auto control = find_state(state);
        if (control != nullptr && control->live_plugins > 0) {
            --control->live_plugins;
        }
    } catch (...) {
    }
}

void clear_state_error_locked(lua_State* state) noexcept {
    try {
        auto control = find_state(state);
        if (control != nullptr) control->last_error.clear();
    } catch (...) {
    }
}

void set_state_error_locked(lua_State* state, std::string message) noexcept {
    try {
        auto control = find_state(state);
        if (control != nullptr) control->last_error = std::move(message);
    } catch (...) {
    }
}

void capture_state_error_locked(lua_State* state, int index) noexcept {
    if (state == nullptr) return;
    try {
        if (lua_type(state, index) != LUA_TSTRING) {
            set_state_error_locked(state, "Lua operation failed");
            return;
        }
        size_t length = 0;
        const char* message = lua_tolstring(state, index, &length);
        set_state_error_locked(
            state, message == nullptr ? std::string("Lua operation failed")
                                      : std::string(message, length));
    } catch (...) {
        set_state_error_locked(state, "Lua operation failed");
    }
}

std::string take_state_error_locked(lua_State* state) noexcept {
    try {
        auto control = find_state(state);
        if (control == nullptr) return {};
        std::string result = std::move(control->last_error);
        control->last_error.clear();
        return result;
    } catch (...) {
        return {};
    }
}

int protected_trampoline(lua_State* state,
                         int (*function)(lua_State*),
                         void* context,
                         int result_count) noexcept {
    if (state == nullptr || function == nullptr ||
        (result_count < 0 && result_count != LUA_MULTRET)) {
        return LUA_ERRRUN;
    }
    lua_pushlightuserdata(state, context);
    return protected_function(state, function, 1, result_count);
}

int protected_function(lua_State* state,
                       int (*function)(lua_State*),
                       int argument_count,
                       int result_count) noexcept {
    if (state == nullptr || function == nullptr || argument_count < 0 ||
        argument_count > lua_gettop(state) ||
        (result_count < 0 && result_count != LUA_MULTRET)) {
        return LUA_ERRRUN;
    }
    lua_pushcfunction(state, function);
    lua_insert(state, lua_gettop(state) - argument_count);
    return lua_pcall(state, argument_count, result_count, 0);
}

int protected_tostring(lua_State* state, int index) noexcept {
    if (state == nullptr) return LUA_ERRRUN;
    index = lua_absindex(state, index);
    lua_pushvalue(state, index);
    return protected_function(state, tostring_dispatch, 1, 1);
}

#else

state_operation::~state_operation() noexcept = default;
lua_State* state_operation::state() const noexcept { return nullptr; }
state_operation::operator bool() const noexcept { return false; }
void state_operation::release() noexcept {}

state_close_operation::~state_close_operation() noexcept = default;
lua_State* state_close_operation::state() const noexcept { return nullptr; }
state_close_operation::operator bool() const noexcept { return false; }

int32_t register_state(lua_State*) noexcept { return SAO_ERR_NOT_IMPLEMENTED; }
int32_t acquire_state_operation(lua_State*, state_operation&) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
int32_t begin_state_close(lua_State*, state_close_operation&) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
void cancel_state_close(state_close_operation&) noexcept {}
void finish_state_close(state_close_operation&) noexcept {}
std::shared_ptr<std::recursive_mutex> state_mutex(lua_State*) noexcept {
    return nullptr;
}
int32_t add_live_plugin_locked(lua_State*) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
void remove_live_plugin_locked(lua_State*) noexcept {}
void clear_state_error_locked(lua_State*) noexcept {}
void set_state_error_locked(lua_State*, std::string) noexcept {}
void capture_state_error_locked(lua_State*, int) noexcept {}
std::string take_state_error_locked(lua_State*) noexcept { return {}; }
int protected_trampoline(lua_State*, int (*)(lua_State*), void*, int) noexcept {
    return 0;
}
int protected_function(lua_State*, int (*)(lua_State*), int, int) noexcept {
    return 0;
}
int protected_tostring(lua_State*, int) noexcept { return 0; }

#endif

} // namespace sao::plugins::lua_host::detail
