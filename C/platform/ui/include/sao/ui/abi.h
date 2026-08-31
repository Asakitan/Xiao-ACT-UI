// SAO Auto — platform/ui ABI export macros.

#pragma once

#include <cstdint>

#include "sao/core/status.h"

#ifdef __cplusplus
#include <memory>
#endif

#if defined(_WIN32)
#if defined(SAO_UI_BUILDING_DLL)
#define SAO_UI_API __declspec(dllexport)
#elif defined(SAO_UI_USING_DLL)
#define SAO_UI_API __declspec(dllimport)
#else
#define SAO_UI_API
#endif
#else
#define SAO_UI_API
#endif

#define SAO_UI_CALL __cdecl

#define SAO_UI_ABI_VERSION_MAJOR 1u
// Minor 10 adds the append-only sao_ui_sound_* named sound API. Minor 9
// expands the append-only public theme color and metric tables to
// 92/26 entries. Minor 8 added a leading uint32_t struct_size field to SaoPanelDescriptor,
// SaoCompositorConfig, and SaoLayerConfig so future ABI additions can extend
// the tail while the platform reads only bytes the caller understands. Legacy
// callers (struct_size == 0) are treated as their canonical v1 sizes
// (SAO_UI_PANEL_DESCRIPTOR_V1_SIZE / SAO_UI_COMPOSITOR_CONFIG_V1_SIZE /
// SAO_UI_LAYER_CONFIG_V1_SIZE) so no source or binary caller has to change
// for the guard alone. Minor 7 added sao_ui_compositor_host_dpi().
#define SAO_UI_ABI_VERSION_MINOR 10u
#define SAO_UI_ABI_VERSION ((SAO_UI_ABI_VERSION_MAJOR << 16) | SAO_UI_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

SAO_UI_API uint32_t SAO_UI_CALL sao_ui_abi_version(void);

#ifdef __cplusplus
} // extern "C"

namespace sao::ui::detail {

enum class WidgetHandleFamily : uint8_t {
    text,
    data,
    table,
    input,
    chart,
    script_canvas,
    generic,
};

struct WidgetHandleMetadata {
    WidgetHandleFamily family{};
    int32_t kind{-1};
    uint64_t generation{};
};

void* register_widget_handle(WidgetHandleFamily family, int32_t kind,
                             std::shared_ptr<void> state) noexcept;

bool register_external_widget_handle(void* handle, WidgetHandleFamily family, int32_t kind,
                                     uint64_t* out_generation = nullptr) noexcept;

std::shared_ptr<void> acquire_widget_handle(void* handle, WidgetHandleFamily expected_family,
                                            int32_t expected_kind) noexcept;

std::shared_ptr<void> retire_widget_handle(void* handle, WidgetHandleFamily expected_family,
                                           WidgetHandleMetadata* out_metadata = nullptr) noexcept;

bool inspect_widget_handle(void* handle, WidgetHandleMetadata* out_metadata) noexcept;

bool register_widget_lifecycle(void* handle, WidgetHandleFamily family, int32_t kind,
                               uint64_t generation = 0) noexcept;

bool acquire_widget_lifecycle(void* handle) noexcept;
void release_widget_lifecycle(void* handle) noexcept;
bool retire_widget_lifecycle(void* handle) noexcept;

sao_status_t release_widget_event_handlers(void* handle, uint32_t* out_removed_count) noexcept;

} // namespace sao::ui::detail
#endif
