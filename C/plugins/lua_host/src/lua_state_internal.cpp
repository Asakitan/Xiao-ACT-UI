#include "lua_state_internal.h"

#include "sao/plugins/loader/loader_status.h"

#include <atomic>
#include <condition_variable>
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
    std::atomic_bool close_cancel_requested{false};
    size_t active_operations = 0;
    std::atomic_size_t live_plugins{0};
    uint32_t permissions = permission_unsafe;
    std::string last_error;
    std::mutex sleep_mutex;
    std::condition_variable sleep_condition;
};

namespace {

std::mutex g_states_mutex;
std::unordered_map<lua_State*, std::shared_ptr<state_control>> g_states;

std::shared_ptr<state_control> find_state(lua_State* state) {
    state = main_thread(state);
    std::lock_guard lock(g_states_mutex);
    const auto found = g_states.find(state);
    return found == g_states.end() ? nullptr : found->second;
}

int tostring_dispatch(lua_State* state) {
    luaL_tolstring(state, 1, nullptr);
    return 1;
}

} // namespace

lua_stack_guard::lua_stack_guard(lua_State* state) noexcept
    : state_(state), base_(state == nullptr ? 0 : lua_gettop(state)) {}

lua_stack_guard::~lua_stack_guard() noexcept {
    if (state_ != nullptr)
        lua_settop(state_, base_);
}

void lua_stack_guard::dismiss() noexcept {
    state_ = nullptr;
}

int lua_stack_guard::base() const noexcept {
    return base_;
}

lua_registry_ref::lua_registry_ref(lua_State* state, int reference) noexcept
    : state_(main_thread(state)), reference_(reference) {}

lua_registry_ref::~lua_registry_ref() noexcept {
    reset();
}

void lua_registry_ref::reset(lua_State* state, int reference) noexcept {
    if (state_ != nullptr && reference_ != LUA_NOREF && reference_ != LUA_REFNIL)
        luaL_unref(state_, LUA_REGISTRYINDEX, reference_);
    state_ = main_thread(state);
    reference_ = reference;
}

int lua_registry_ref::get() const noexcept {
    return reference_;
}

int lua_registry_ref::release() noexcept {
    const int result = reference_;
    state_ = nullptr;
    reference_ = LUA_NOREF;
    return result;
}

state_operation::~state_operation() noexcept {
    release();
}

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
    if (lock_.owns_lock())
        lock_.unlock();
    control_.reset();
}

state_close_operation::~state_close_operation() noexcept {
    if (control_ != nullptr)
        cancel_state_close(*this);
}

lua_State* state_close_operation::state() const noexcept {
    return control_ == nullptr ? nullptr : control_->state;
}

state_close_operation::operator bool() const noexcept {
    return control_ != nullptr && lock_.owns_lock() && control_->state != nullptr;
}

int32_t register_state(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        *static_cast<lua_State**>(lua_getextraspace(state)) = state;
        auto control = std::make_shared<state_control>(state);
        std::lock_guard lock(g_states_mutex);
        return g_states.emplace(state, std::move(control)).second
                   ? SAO_OK
                   : sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

lua_State* main_thread(lua_State* state) noexcept {
    if (state == nullptr)
        return nullptr;
    auto* slot = static_cast<lua_State**>(lua_getextraspace(state));
    return slot != nullptr && *slot != nullptr ? *slot : state;
}

int32_t acquire_state_operation_impl(lua_State* state, state_operation& operation,
                                     bool allow_close_cancel) noexcept {
    if (state == nullptr || operation.control_ != nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control == nullptr || control->closing.load(std::memory_order_acquire) ||
            (!allow_close_cancel &&
             control->close_cancel_requested.load(std::memory_order_acquire))) {
            return SAO_ERR_HANDLE_INVALID;
        }
        std::unique_lock lock(*control->mutex);
        if (control->closing.load(std::memory_order_acquire) ||
            (!allow_close_cancel &&
             control->close_cancel_requested.load(std::memory_order_acquire)) ||
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

int32_t acquire_state_operation(lua_State* state, state_operation& operation) noexcept {
    return acquire_state_operation_impl(state, operation, false);
}

int32_t acquire_state_operation_for_close(lua_State* state, state_operation& operation) noexcept {
    return acquire_state_operation_impl(state, operation, true);
}

int32_t request_state_close_cancel(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        auto control = find_state(state);
        if (control == nullptr || control->closing.load(std::memory_order_acquire))
            return SAO_ERR_HANDLE_INVALID;
        control->close_cancel_requested.store(true, std::memory_order_release);
        control->sleep_condition.notify_all();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void clear_state_close_cancel(lua_State* state) noexcept {
    try {
        auto control = find_state(state);
        if (control != nullptr) {
            control->close_cancel_requested.store(false, std::memory_order_release);
            control->sleep_condition.notify_all();
        }
    } catch (...) {
    }
}

int32_t preflight_state_close(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        auto control = find_state(state);
        if (control == nullptr || control->state != main_thread(state) ||
            control->closing.load(std::memory_order_acquire)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        return control->live_plugins.load(std::memory_order_acquire) == 0
                   ? SAO_OK
                   : sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t begin_state_close(lua_State* state, state_close_operation& operation) noexcept {
    if (state == nullptr || operation.control_ != nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::shared_ptr<state_control> control;
    try {
        state = main_thread(state);
        {
            std::lock_guard map_lock(g_states_mutex);
            const auto found = g_states.find(state);
            if (found == g_states.end())
                return SAO_ERR_HANDLE_INVALID;
            control = found->second;
            bool expected = false;
            if (!control->closing.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            control->sleep_condition.notify_all();
        }
        std::unique_lock lock(*control->mutex);
        if (control->active_operations != 0 ||
            control->live_plugins.load(std::memory_order_acquire) != 0) {
            control->closing.store(false, std::memory_order_release);
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        operation.control_ = std::move(control);
        operation.lock_ = std::move(lock);
        return SAO_OK;
    } catch (...) {
        if (control != nullptr) {
            control->closing.store(false, std::memory_order_release);
            control->sleep_condition.notify_all();
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void cancel_state_close(state_close_operation& operation) noexcept {
    if (operation.control_ != nullptr) {
        operation.control_->closing.store(false, std::memory_order_release);
        operation.control_->close_cancel_requested.store(false, std::memory_order_release);
        operation.control_->sleep_condition.notify_all();
    }
    if (operation.lock_.owns_lock())
        operation.lock_.unlock();
    operation.control_.reset();
}

void finish_state_close(state_close_operation& operation) noexcept {
    if (operation.control_ == nullptr)
        return;
    lua_State* state = operation.control_->state;
    operation.control_->state = nullptr;
    {
        std::lock_guard map_lock(g_states_mutex);
        const auto found = g_states.find(state);
        if (found != g_states.end() && found->second == operation.control_) {
            g_states.erase(found);
        }
    }
    if (operation.lock_.owns_lock())
        operation.lock_.unlock();
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

int32_t set_state_permissions(lua_State* state, uint32_t permissions) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control == nullptr || control->state != state ||
            control->closing.load(std::memory_order_acquire)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        std::lock_guard lock(*control->mutex);
        if (control->state != state || control->closing.load(std::memory_order_acquire)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        control->permissions = permissions;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool state_has_permission_locked(lua_State* state, lua_permission permission) noexcept {
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control == nullptr || control->state != state)
            return false;
        return (control->permissions & permission_unsafe) != 0 ||
               (control->permissions & static_cast<uint32_t>(permission)) != 0;
    } catch (...) {
        return false;
    }
}

int32_t add_live_plugin_locked(lua_State* state) noexcept {
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control == nullptr || control->state != state ||
            control->closing.load(std::memory_order_acquire)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        control->live_plugins.fetch_add(1, std::memory_order_acq_rel);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void remove_live_plugin_locked(lua_State* state) noexcept {
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control != nullptr && control->live_plugins.load(std::memory_order_acquire) > 0) {
            control->live_plugins.fetch_sub(1, std::memory_order_acq_rel);
        }
    } catch (...) {
    }
}

bool interruptible_state_sleep(lua_State* state, std::chrono::duration<double> duration) noexcept {
    try {
        auto control = find_state(state);
        if (control == nullptr || duration.count() < 0.0)
            return false;
        std::unique_lock lock(control->sleep_mutex);
        return !control->sleep_condition.wait_for(lock, duration, [&control] {
            return control->closing.load(std::memory_order_acquire) ||
                   control->close_cancel_requested.load(std::memory_order_acquire);
        });
    } catch (...) {
        return false;
    }
}

void clear_state_error_locked(lua_State* state) noexcept {
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control != nullptr)
            control->last_error.clear();
    } catch (...) {
    }
}

void set_state_error_locked(lua_State* state, std::string message) noexcept {
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control != nullptr)
            control->last_error = std::move(message);
    } catch (...) {
    }
}

void capture_state_error_locked(lua_State* state, int index) noexcept {
    if (state == nullptr)
        return;
    try {
        if (lua_type(state, index) != LUA_TSTRING) {
            set_state_error_locked(state, "Lua operation failed");
            return;
        }
        size_t length = 0;
        const char* message = lua_tolstring(state, index, &length);
        set_state_error_locked(state, message == nullptr ? std::string("Lua operation failed")
                                                         : std::string(message, length));
    } catch (...) {
        set_state_error_locked(state, "Lua operation failed");
    }
}

std::string take_state_error_locked(lua_State* state) noexcept {
    try {
        state = main_thread(state);
        auto control = find_state(state);
        if (control == nullptr)
            return {};
        std::string result = std::move(control->last_error);
        control->last_error.clear();
        return result;
    } catch (...) {
        return {};
    }
}

int protected_trampoline(lua_State* state, int (*function)(lua_State*), void* context,
                         int result_count) noexcept {
    if (state == nullptr || function == nullptr ||
        (result_count < 0 && result_count != LUA_MULTRET)) {
        return LUA_ERRRUN;
    }
    lua_pushlightuserdata(state, context);
    return protected_function(state, function, 1, result_count);
}

int protected_function(lua_State* state, int (*function)(lua_State*), int argument_count,
                       int result_count) noexcept {
    if (state == nullptr || function == nullptr || argument_count < 0 ||
        argument_count > lua_gettop(state) || (result_count < 0 && result_count != LUA_MULTRET)) {
        return LUA_ERRRUN;
    }
    lua_pushcfunction(state, function);
    lua_insert(state, lua_gettop(state) - argument_count);
    return lua_pcall(state, argument_count, result_count, 0);
}

int protected_tostring(lua_State* state, int index) noexcept {
    if (state == nullptr)
        return LUA_ERRRUN;
    index = lua_absindex(state, index);
    lua_pushvalue(state, index);
    return protected_function(state, tostring_dispatch, 1, 1);
}

#else

lua_stack_guard::lua_stack_guard(lua_State*) noexcept {}
lua_stack_guard::~lua_stack_guard() noexcept = default;
void lua_stack_guard::dismiss() noexcept {}
int lua_stack_guard::base() const noexcept {
    return 0;
}

lua_registry_ref::lua_registry_ref(lua_State*, int) noexcept {}
lua_registry_ref::~lua_registry_ref() noexcept = default;
void lua_registry_ref::reset(lua_State*, int) noexcept {}
int lua_registry_ref::get() const noexcept {
    return -2;
}
int lua_registry_ref::release() noexcept {
    return -2;
}

state_operation::~state_operation() noexcept = default;
lua_State* state_operation::state() const noexcept {
    return nullptr;
}
state_operation::operator bool() const noexcept {
    return false;
}
void state_operation::release() noexcept {}

state_close_operation::~state_close_operation() noexcept = default;
lua_State* state_close_operation::state() const noexcept {
    return nullptr;
}
state_close_operation::operator bool() const noexcept {
    return false;
}

int32_t register_state(lua_State*) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
lua_State* main_thread(lua_State* state) noexcept {
    return state;
}
int32_t acquire_state_operation(lua_State*, state_operation&) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
int32_t acquire_state_operation_for_close(lua_State*, state_operation&) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
int32_t preflight_state_close(lua_State*) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
int32_t request_state_close_cancel(lua_State*) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
void clear_state_close_cancel(lua_State*) noexcept {}
int32_t begin_state_close(lua_State*, state_close_operation&) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
void cancel_state_close(state_close_operation&) noexcept {}
void finish_state_close(state_close_operation&) noexcept {}
std::shared_ptr<std::recursive_mutex> state_mutex(lua_State*) noexcept {
    return nullptr;
}
int32_t set_state_permissions(lua_State*, uint32_t) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
bool state_has_permission_locked(lua_State*, lua_permission) noexcept {
    return false;
}
int32_t add_live_plugin_locked(lua_State*) noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}
void remove_live_plugin_locked(lua_State*) noexcept {}
bool interruptible_state_sleep(lua_State*, std::chrono::duration<double>) noexcept {
    return false;
}
void clear_state_error_locked(lua_State*) noexcept {}
void set_state_error_locked(lua_State*, std::string) noexcept {}
void capture_state_error_locked(lua_State*, int) noexcept {}
std::string take_state_error_locked(lua_State*) noexcept {
    return {};
}
int protected_trampoline(lua_State*, int (*)(lua_State*), void*, int) noexcept {
    return 0;
}
int protected_function(lua_State*, int (*)(lua_State*), int, int) noexcept {
    return 0;
}
int protected_tostring(lua_State*, int) noexcept {
    return 0;
}

#endif

} // namespace sao::plugins::lua_host::detail
