#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host::detail {

enum lua_permission : uint32_t {
    permission_none = 0,
    permission_fs = 1u << 0u,
    permission_net = 1u << 1u,
    permission_process = 1u << 2u,
    permission_hotkey = 1u << 3u,
    permission_memory_access = 1u << 4u,
    permission_input_control = 1u << 5u,
    permission_engine_access = 1u << 6u,
    permission_unsafe = 1u << 31u,
};

struct state_control;

class lua_stack_guard final {
  public:
    explicit lua_stack_guard(lua_State* state) noexcept;
    ~lua_stack_guard() noexcept;

    lua_stack_guard(const lua_stack_guard&) = delete;
    lua_stack_guard& operator=(const lua_stack_guard&) = delete;

    void dismiss() noexcept;
    [[nodiscard]] int base() const noexcept;

  private:
    lua_State* state_ = nullptr;
    int base_ = 0;
};

class lua_registry_ref final {
  public:
    lua_registry_ref() = default;
    lua_registry_ref(lua_State* state, int reference) noexcept;
    ~lua_registry_ref() noexcept;

    lua_registry_ref(const lua_registry_ref&) = delete;
    lua_registry_ref& operator=(const lua_registry_ref&) = delete;

    void reset(lua_State* state = nullptr, int reference = -2) noexcept;
    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] int release() noexcept;

  private:
    lua_State* state_ = nullptr;
    int reference_ = -2;
};

class state_operation final {
  public:
    state_operation() = default;
    ~state_operation() noexcept;

    state_operation(const state_operation&) = delete;
    state_operation& operator=(const state_operation&) = delete;
    state_operation(state_operation&&) = delete;
    state_operation& operator=(state_operation&&) = delete;

    [[nodiscard]] lua_State* state() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

  private:
    friend int32_t acquire_state_operation(lua_State*, state_operation&) noexcept;
    friend int32_t acquire_state_operation_for_close(lua_State*, state_operation&) noexcept;
    friend int32_t acquire_state_operation_impl(lua_State*, state_operation&, bool) noexcept;

    void release() noexcept;

    std::shared_ptr<state_control> control_;
    std::unique_lock<std::recursive_mutex> lock_;
    bool counted_ = false;
};

class state_close_operation final {
  public:
    state_close_operation() = default;
    ~state_close_operation() noexcept;

    state_close_operation(const state_close_operation&) = delete;
    state_close_operation& operator=(const state_close_operation&) = delete;
    state_close_operation(state_close_operation&&) = delete;
    state_close_operation& operator=(state_close_operation&&) = delete;

    [[nodiscard]] lua_State* state() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

  private:
    friend int32_t begin_state_close(lua_State*, state_close_operation&) noexcept;
    friend void cancel_state_close(state_close_operation&) noexcept;
    friend void finish_state_close(state_close_operation&) noexcept;

    std::shared_ptr<state_control> control_;
    std::unique_lock<std::recursive_mutex> lock_;
};

int32_t register_state(lua_State* state) noexcept;
lua_State* main_thread(lua_State* state) noexcept;
int32_t acquire_state_operation(lua_State* state, state_operation& operation) noexcept;
int32_t acquire_state_operation_for_close(lua_State* state, state_operation& operation) noexcept;
int32_t preflight_state_close(lua_State* state) noexcept;
int32_t request_state_close_cancel(lua_State* state) noexcept;
void clear_state_close_cancel(lua_State* state) noexcept;
int32_t begin_state_close(lua_State* state, state_close_operation& operation) noexcept;
void cancel_state_close(state_close_operation& operation) noexcept;
void finish_state_close(state_close_operation& operation) noexcept;

std::shared_ptr<std::recursive_mutex> state_mutex(lua_State* state) noexcept;

int32_t set_state_permissions(lua_State* state, uint32_t permissions) noexcept;
bool state_has_permission_locked(lua_State* state, lua_permission permission) noexcept;

int32_t add_live_plugin_locked(lua_State* state) noexcept;
void remove_live_plugin_locked(lua_State* state) noexcept;

bool interruptible_state_sleep(lua_State* state, std::chrono::duration<double> duration) noexcept;

void clear_state_error_locked(lua_State* state) noexcept;
void set_state_error_locked(lua_State* state, std::string message) noexcept;
void capture_state_error_locked(lua_State* state, int index) noexcept;
std::string take_state_error_locked(lua_State* state) noexcept;

int protected_trampoline(lua_State* state, int (*function)(lua_State*), void* context,
                         int result_count) noexcept;
int protected_function(lua_State* state, int (*function)(lua_State*), int argument_count,
                       int result_count) noexcept;
int protected_tostring(lua_State* state, int index) noexcept;

} // namespace sao::plugins::lua_host::detail
