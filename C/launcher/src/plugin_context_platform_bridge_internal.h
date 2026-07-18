#pragma once

#include <cstdint>

namespace sao::launcher {

int32_t registerPluginContextPlatformBridge() noexcept;
int32_t unregisterPluginContextPlatformBridge() noexcept;

} // namespace sao::launcher
