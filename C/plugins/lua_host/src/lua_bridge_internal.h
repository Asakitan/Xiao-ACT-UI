#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

struct lua_State;

namespace sao::plugins::lua_host::detail {

std::shared_ptr<std::recursive_mutex> bridge_mutex(lua_State* state) noexcept;
int32_t teardown_ctx_bridge(lua_State* state) noexcept;
int32_t teardown_ctx_bridge_locked(lua_State* state) noexcept;
void release_ctx_bridges_locked(lua_State* state) noexcept;

} // namespace sao::plugins::lua_host::detail
