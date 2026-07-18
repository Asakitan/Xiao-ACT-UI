#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host::detail {

struct state_control;

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
int32_t acquire_state_operation(lua_State* state,
                                state_operation& operation) noexcept;
int32_t begin_state_close(lua_State* state,
                          state_close_operation& operation) noexcept;
void cancel_state_close(state_close_operation& operation) noexcept;
void finish_state_close(state_close_operation& operation) noexcept;

std::shared_ptr<std::recursive_mutex> state_mutex(lua_State* state) noexcept;

int32_t add_live_plugin_locked(lua_State* state) noexcept;
void remove_live_plugin_locked(lua_State* state) noexcept;

void clear_state_error_locked(lua_State* state) noexcept;
void set_state_error_locked(lua_State* state, std::string message) noexcept;
void capture_state_error_locked(lua_State* state, int index) noexcept;
std::string take_state_error_locked(lua_State* state) noexcept;

int protected_trampoline(lua_State* state,
                         int (*function)(lua_State*),
                         void* context,
                         int result_count) noexcept;
int protected_function(lua_State* state,
                       int (*function)(lua_State*),
                       int argument_count,
                       int result_count) noexcept;
int protected_tostring(lua_State* state, int index) noexcept;

} // namespace sao::plugins::lua_host::detail
