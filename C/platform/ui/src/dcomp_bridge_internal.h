#pragma once

#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/dcomp_bridge.h"

namespace sao::ui::detail {

struct DcompExternalVisual;

struct DcompExternalVisualConfig {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    int32_t z_order{};
    int32_t band{};
    float opacity{1.0F};
    bool visible{true};
};

sao_status_t create_dcomp_external_visual(
    sao_ui_dcomp_bridge_handle_t bridge,
    const DcompExternalVisualConfig& config,
    DcompExternalVisual** out_visual) noexcept;

sao_status_t update_dcomp_external_visual(
    DcompExternalVisual* visual,
    const DcompExternalVisualConfig& config) noexcept;

sao_status_t commit_dcomp_external_visual(DcompExternalVisual* visual) noexcept;

sao_status_t destroy_dcomp_external_visual(DcompExternalVisual* visual) noexcept;
void abandon_dcomp_external_visual(DcompExternalVisual* visual) noexcept;

void* dcomp_external_visual_target(DcompExternalVisual* visual) noexcept;
void* dcomp_external_visual_device(DcompExternalVisual* visual) noexcept;

} // namespace sao::ui::detail
