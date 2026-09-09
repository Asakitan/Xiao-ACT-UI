#pragma once

#include "sao/ui/d2d_widgets.h"

#include <cstdint>
#include <memory>

namespace sao::ui::detail {

struct PaintDisplayList;

void paint_display_list_size(const PaintDisplayList& list, uint32_t* width,
                             uint32_t* height) noexcept;

// Creates an owner-thread Direct2D 1.1 paint context targeting an existing
// BGRA8 premultiplied D3D11 texture. The device and texture are borrowed;
// the context retains COM references until destroy. begin_frame clears the
// target transparent and end_frame flushes/propagates device-loss status.
sao_status_t create_gpu_paint_context(void* d3d11_device, void* d3d11_texture, uint32_t width,
                                      uint32_t height,
                                      sao_ui_paint_ctx_handle_t* out_context) noexcept;

sao_status_t create_recording_paint_context(uint32_t width, uint32_t height,
                                            sao_ui_paint_ctx_handle_t* out_context) noexcept;

sao_status_t
seal_recording_paint_context(sao_ui_paint_ctx_handle_t context,
                             std::shared_ptr<const PaintDisplayList>* out_display_list) noexcept;

// Replays one immutable list into a GPU context. This owns the target's
// begin/end frame pair and therefore also clears it transparent.
sao_status_t replay_paint_display_list(const PaintDisplayList& display_list,
                                       sao_ui_paint_ctx_handle_t gpu_target) noexcept;

sao_status_t paint_rounded_rect(sao_ui_paint_ctx_handle_t context, float x, float y, float width,
                                float height, float radius, uint32_t argb) noexcept;

sao_status_t paint_rounded_rect_stroke(sao_ui_paint_ctx_handle_t context, float x, float y,
                                       float width, float height, float radius, float stroke_width,
                                       uint32_t argb) noexcept;

sao_status_t paint_focus_ring(sao_ui_paint_ctx_handle_t context, float x, float y, float width,
                              float height, float radius, bool rounded, uint32_t argb) noexcept;

sao_status_t paint_elevation_shadow(sao_ui_paint_ctx_handle_t context, float x, float y,
                                    float width, float height, float radius, int32_t elevation,
                                    uint32_t argb) noexcept;

// True-font measurement (widget_text_render_win.cpp).  Returns false when
// DirectWrite is unavailable so callers keep their codepoint fallback.
bool measure_text_dwrite(const char* text_utf8, float size_px, float* out_width,
                         float* out_height) noexcept;

} // namespace sao::ui::detail
