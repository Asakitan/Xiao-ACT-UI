#pragma once

#include <cstdint>

namespace sao::legacy_plugins {

inline constexpr int32_t kLogLevelInfo = 1;
inline constexpr int32_t kLogLevelWarning = 2;
inline constexpr int32_t kLogLevelError = 3;

[[nodiscard]] bool logging_enabled() noexcept;

void emit_log(int32_t level, const char* utf8_component, int32_t status,
              const char* utf8_message) noexcept;

} // namespace sao::legacy_plugins
