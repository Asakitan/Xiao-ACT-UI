#pragma once

#include <cstddef>
#include <cstdint>

struct lua_State;

namespace sao::plugins::lua_host::detail {

struct package_path_snapshot {
    std::uint64_t layer_id = 0;
};

bool sandbox_is_armed_locked(lua_State* state) noexcept;
bool sandbox_stdlib_allowed_locked(lua_State* state) noexcept;
int32_t sandbox_disarm_locked(lua_State* state) noexcept;
int32_t sandbox_add_controlled_package_path_locked(lua_State* state, const char* path,
                                                   size_t path_length, const char* root,
                                                   size_t root_length,
                                                   package_path_snapshot& snapshot) noexcept;
int32_t sandbox_restore_package_path_locked(lua_State* state,
                                            package_path_snapshot& snapshot) noexcept;

} // namespace sao::plugins::lua_host::detail
