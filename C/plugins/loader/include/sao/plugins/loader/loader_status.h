#pragma once

#include <cstdint>

namespace sao::plugins::loader {

inline constexpr int32_t SAO_PLUGINS_ERR_UNSUPPORTED = -1000;
inline constexpr int32_t SAO_PLUGINS_ERR_ALREADY_EXISTS = -1001;
inline constexpr int32_t SAO_PLUGINS_ERR_DEPENDENCY_MISSING = -1002;
inline constexpr int32_t SAO_PLUGINS_ERR_DEPENDENCY_CYCLE = -1003;
inline constexpr int32_t SAO_PLUGINS_ERR_ABI_MISMATCH = -1004;
inline constexpr int32_t SAO_PLUGINS_ERR_CAPABILITY_MISMATCH = -1005;
inline constexpr int32_t SAO_PLUGINS_ERR_BUSY = -1006;
inline constexpr int32_t SAO_PLUGINS_ERR_VERSION_MISMATCH = -1007;
inline constexpr int32_t SAO_PLUGINS_ERR_NOT_OWNER = -1008;

} // namespace sao::plugins::loader