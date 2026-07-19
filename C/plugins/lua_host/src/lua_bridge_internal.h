#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

struct lua_State;

namespace sao::plugins::lua_host::detail {

std::shared_ptr<std::recursive_mutex> bridge_mutex(lua_State* state) noexcept;
std::size_t ctx_menu_checkpoint_locked(lua_State* state) noexcept;
int32_t commit_ctx_enable_menus_locked(lua_State* state, std::size_t checkpoint) noexcept;
int32_t rollback_ctx_menus_locked(lua_State* state, std::size_t checkpoint) noexcept;
int32_t remove_ctx_enable_menus_locked(lua_State* state) noexcept;
int32_t quiesce_ctx_menu_providers(lua_State* state) noexcept;
bool has_ctx_bridge_locked(lua_State* state) noexcept;
int32_t resume_ctx_menu_providers_locked(lua_State* state) noexcept;
int32_t push_canonical_ctx_locked(lua_State* state) noexcept;
int32_t teardown_ctx_bridge(lua_State* state) noexcept;
int32_t teardown_ctx_bridge_locked(lua_State* state) noexcept;
void release_ctx_bridges_locked(lua_State* state) noexcept;

} // namespace sao::plugins::lua_host::detail
