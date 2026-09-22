#pragma once

#include "sao/core/status.h"

namespace sao::launcher::platform_timer_owner {
sao_status_t bind_owner() noexcept;
sao_status_t pump() noexcept;
sao_status_t unbind_owner() noexcept;
}