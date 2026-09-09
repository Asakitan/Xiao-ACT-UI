#include "classic_text_roles.h"
#include "widget_paint_internal.h"
#include "widget_raster_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sao::ui::detail {
namespace {
using sao::ui::raster::GpuPaintDispatch;
using sao::ui::raster::Rect;

constexpr size_t kMaxRecordedCommands = 1U << 18U;
constexpr size_t kMaxRecordedClipDepth = 1024U;
constexpr size_t kMaxRecordedTextBytes = 1U << 20U;
constexpr size_t kMaxRecordedBitmapBytes = size_t{64U} * 1024U * 1024U;
constexpr size_t kMaxRecordedPayloadBytes = size_t{128U} * 1024U * 1024U;

enum class PaintCommandKind : uint8_t {
    PushClip,
    PopClip,
    FillRect,
    FillRoundedRect,
    StrokeRoundedRect,
    FillEllipse,
    StrokeLine,
    FillPolygon,
    DrawText,
    BlitBgra
};

struct PaintCommand {
    PaintCommandKind kind{};
    Rect rect{};
    float a{}, b{}, c{}, d{}, width{}, radius{}, opacity{1.0F};
    uint32_t argb{};
    uint32_t source_width{}, source_height{}, source_stride{};
    uint8_t text_role{}, text_weight{};
    std::vector<int32_t> points;
    std::vector<uint8_t> bytes;
    std::string text;
};

#ifndef NDEBUG
const char* paint_command_name(PaintCommandKind kind) noexcept {
    switch (kind) {
    case PaintCommandKind::PushClip:
        return "push_clip";
    case PaintCommandKind::PopClip:
        return "pop_clip";
    case PaintCommandKind::FillRect:
        return "fill_rect";
    case PaintCommandKind::FillRoundedRect:
        return "fill_rounded";
    case PaintCommandKind::StrokeRoundedRect:
        return "stroke_rounded";
    case PaintCommandKind::FillEllipse:
        return "fill_ellipse";
    case PaintCommandKind::StrokeLine:
        return "stroke_line";
    case PaintCommandKind::FillPolygon:
        return "fill_polygon";
    case PaintCommandKind::DrawText:
        return "draw_text";
    case PaintCommandKind::BlitBgra:
        return "blit_bgra";
    }
    return "unknown";
}

void log_replay_failure(size_t index, const PaintCommand& command, sao_status_t status,
                        const sao_ui_paint_ctx_s& target) noexcept {
    std::fprintf(stderr,
                 "UI paint replay command[%zu] %s status=%d backend=%d clipDepth=%zu "
                 "rect=(%.3f,%.3f %.3fx%.3f) line=(%.3f,%.3f %.3f,%.3f) "
                 "width=%.3f radius=%.3f opacity=%.6f points=%zu bitmap=%ux%u/%u\n",
                 index, paint_command_name(command.kind), status, target.backend_status,
                 target.clips.size(), command.rect.x, command.rect.y, command.rect.width,
                 command.rect.height, command.a, command.b, command.c, command.d, command.width,
                 command.radius, command.opacity, command.points.size() / 2U, command.source_width,
                 command.source_height, command.source_stride);
}
#endif
} // namespace

struct PaintDisplayList {
    uint32_t width{};
    uint32_t height{};
    std::vector<PaintCommand> commands;
};

void paint_display_list_size(const PaintDisplayList& list, uint32_t* width,
                             uint32_t* height) noexcept {
    if (width != nullptr)
        *width = list.width;
    if (height != nullptr)
        *height = list.height;
}

namespace {
struct RecordingState {
    std::thread::id owner{std::this_thread::get_id()};
    std::shared_ptr<PaintDisplayList> list;
    size_t clip_depth{};
    bool recording{};
    bool sealed{};
    bool failed{};
    bool completed{};
    size_t retained_payload_bytes{};
};

bool on_owner(const RecordingState* state) noexcept {
    return state && state->owner == std::this_thread::get_id();
}

bool append(RecordingState* state, PaintCommand command) noexcept {
    if (!on_owner(state) || !state->recording || state->sealed || !state->list) {
        if (on_owner(state))
            state->failed = true;
        return false;
    }
    if (state->list->commands.size() >= kMaxRecordedCommands) {
        state->failed = true;
        return false;
    }
    try {
        state->list->commands.push_back(std::move(command));
        return true;
    } catch (...) {
        state->failed = true;
        return false;
    }
}

bool can_retain_payload(RecordingState* state, size_t bytes) noexcept {
    if (!on_owner(state) || !state->recording || state->sealed || !state->list) {
        if (on_owner(state))
            state->failed = true;
        return false;
    }
    if (state->list->commands.size() >= kMaxRecordedCommands || bytes > kMaxRecordedPayloadBytes ||
        state->retained_payload_bytes > kMaxRecordedPayloadBytes - bytes) {
        state->failed = true;
        return false;
    }
    return true;
}

void destroy(void* opaque) noexcept {
    delete static_cast<RecordingState*>(opaque);
}
sao_status_t begin(void* opaque) noexcept {
    auto* state = static_cast<RecordingState*>(opaque);
    if (!on_owner(state) || state->recording || state->sealed || !state->list)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    state->list->commands.clear();
    state->clip_depth = 0;
    state->retained_payload_bytes = 0;
    state->failed = false;
    state->completed = false;
    state->recording = true;
    return SAO_STATUS_OK;
}
sao_status_t end(void* opaque) noexcept {
    auto* state = static_cast<RecordingState*>(opaque);
    if (!on_owner(state) || !state->recording)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    const bool unbalanced = state->clip_depth != 0;
    const bool failed = state->failed;
    state->recording = false;
    state->clip_depth = 0;
    state->completed = !failed && !unbalanced;
    if (!state->completed && state->list)
        state->list->commands.clear();
    if (failed)
        return SAO_STATUS_ERR_UNKNOWN;
    if (unbalanced)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return SAO_STATUS_OK;
}
sao_status_t push_clip(void* opaque, Rect rect) noexcept {
    auto* state = static_cast<RecordingState*>(opaque);
    if (!on_owner(state) || !state->recording || state->clip_depth >= kMaxRecordedClipDepth) {
        if (on_owner(state) && state->recording)
            state->failed = true;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PaintCommand command{};
    command.kind = PaintCommandKind::PushClip;
    command.rect = rect;
    if (!append(state, std::move(command)))
        return SAO_STATUS_ERR_UNKNOWN;
    ++state->clip_depth;
    return SAO_STATUS_OK;
}
sao_status_t pop_clip(void* opaque) noexcept {
    auto* state = static_cast<RecordingState*>(opaque);
    if (!on_owner(state) || !state->recording || state->clip_depth == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PaintCommand command{};
    command.kind = PaintCommandKind::PopClip;
    if (!append(state, std::move(command)))
        return SAO_STATUS_ERR_UNKNOWN;
    --state->clip_depth;
    return SAO_STATUS_OK;
}
bool fill_rect(void* opaque, Rect rect, uint32_t color, float opacity) noexcept {
    PaintCommand c{};
    c.kind = PaintCommandKind::FillRect;
    c.rect = rect;
    c.argb = color;
    c.opacity = opacity;
    return append(static_cast<RecordingState*>(opaque), std::move(c));
}
bool fill_rounded(void* opaque, Rect rect, float radius, uint32_t color, float opacity) noexcept {
    PaintCommand c{};
    c.kind = PaintCommandKind::FillRoundedRect;
    c.rect = rect;
    c.radius = radius;
    c.argb = color;
    c.opacity = opacity;
    return append(static_cast<RecordingState*>(opaque), std::move(c));
}
bool stroke_rounded(void* opaque, Rect rect, float radius, float width, uint32_t color,
                    float opacity) noexcept {
    PaintCommand c{};
    c.kind = PaintCommandKind::StrokeRoundedRect;
    c.rect = rect;
    c.radius = radius;
    c.width = width;
    c.argb = color;
    c.opacity = opacity;
    return append(static_cast<RecordingState*>(opaque), std::move(c));
}
bool fill_ellipse(void* opaque, Rect rect, uint32_t color, float opacity) noexcept {
    PaintCommand c{};
    c.kind = PaintCommandKind::FillEllipse;
    c.rect = rect;
    c.argb = color;
    c.opacity = opacity;
    return append(static_cast<RecordingState*>(opaque), std::move(c));
}
bool stroke_line(void* opaque, float x1, float y1, float x2, float y2, float width, uint32_t color,
                 float opacity) noexcept {
    PaintCommand c{};
    c.kind = PaintCommandKind::StrokeLine;
    c.a = x1;
    c.b = y1;
    c.c = x2;
    c.d = y2;
    c.width = width;
    c.argb = color;
    c.opacity = opacity;
    return append(static_cast<RecordingState*>(opaque), std::move(c));
}
bool fill_polygon(void* opaque, const int32_t* points, size_t count, uint32_t color,
                  float opacity) noexcept {
    if (!points || count < 3 || count > std::numeric_limits<size_t>::max() / 2)
        return false;
    auto* state = static_cast<RecordingState*>(opaque);
    const size_t value_count = count * 2U;
    if (value_count > std::numeric_limits<size_t>::max() / sizeof(int32_t) ||
        !can_retain_payload(state, value_count * sizeof(int32_t)))
        return false;
    try {
        PaintCommand c{};
        c.kind = PaintCommandKind::FillPolygon;
        c.argb = color;
        c.opacity = opacity;
        c.points.assign(points, points + value_count);
        if (!append(state, std::move(c)))
            return false;
        state->retained_payload_bytes += value_count * sizeof(int32_t);
        return true;
    } catch (...) {
        state->failed = true;
        return false;
    }
}
bool draw_text(void* opaque, float x, float y, const char* text, float size, uint32_t color,
               float opacity, uint8_t role, uint8_t weight) noexcept {
    if (!text)
        return false;
    auto* state = static_cast<RecordingState*>(opaque);
    size_t text_size = 0;
    while (text_size <= kMaxRecordedTextBytes && text[text_size] != '\0')
        ++text_size;
    if (text_size > kMaxRecordedTextBytes || !can_retain_payload(state, text_size))
        return false;
    try {
        PaintCommand c{};
        c.kind = PaintCommandKind::DrawText;
        c.a = x;
        c.b = y;
        c.width = size;
        c.argb = color;
        c.opacity = opacity;
        c.text.assign(text, text_size);
        c.text_role = role;
        c.text_weight = weight;
        if (!append(state, std::move(c)))
            return false;
        state->retained_payload_bytes += text_size;
        return true;
    } catch (...) {
        state->failed = true;
        return false;
    }
}
bool blit(void* opaque, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride,
          Rect destination, float opacity) noexcept {
    if (!pixels || !width || !height || width > std::numeric_limits<uint32_t>::max() / 4U ||
        stride < width * 4U ||
        static_cast<size_t>(stride) > std::numeric_limits<size_t>::max() / height)
        return false;
    auto* state = static_cast<RecordingState*>(opaque);
    const size_t byte_count = static_cast<size_t>(stride) * height;
    if (byte_count > kMaxRecordedBitmapBytes || !can_retain_payload(state, byte_count))
        return false;
    try {
        PaintCommand c{};
        c.kind = PaintCommandKind::BlitBgra;
        c.rect = destination;
        c.opacity = opacity;
        c.source_width = width;
        c.source_height = height;
        c.source_stride = stride;
        c.bytes.resize(byte_count);
        std::memcpy(c.bytes.data(), pixels, c.bytes.size());
        if (!append(state, std::move(c)))
            return false;
        state->retained_payload_bytes += byte_count;
        return true;
    } catch (...) {
        state->failed = true;
        return false;
    }
}

const GpuPaintDispatch kRecordingDispatch{
    destroy,        begin,        end,         push_clip,    pop_clip,  fill_rect, fill_rounded,
    stroke_rounded, fill_ellipse, stroke_line, fill_polygon, draw_text, blit};

sao_status_t replay_command(const PaintCommand& command, sao_ui_paint_ctx_handle_t target) {
    const auto with_opacity = [&](auto&& callback) {
        sao_status_t status = sao_ui_paint_ctx_push_opacity(target, command.opacity);
        if (status != SAO_STATUS_OK)
            return status;
        status = callback();
        const sao_status_t pop = sao_ui_paint_ctx_pop_opacity(target);
        return status == SAO_STATUS_OK ? pop : status;
    };
    switch (command.kind) {
    case PaintCommandKind::PushClip:
        return sao_ui_paint_ctx_push_clip(target, command.rect.x, command.rect.y,
                                          command.rect.width, command.rect.height);
    case PaintCommandKind::PopClip:
        return sao_ui_paint_ctx_pop_clip(target);
    case PaintCommandKind::FillRect:
        return with_opacity([&] {
            return sao_ui_paint_ctx_fill_rect(target, command.rect.x, command.rect.y,
                                              command.rect.width, command.rect.height,
                                              command.argb);
        });
    case PaintCommandKind::FillRoundedRect:
        return with_opacity([&] {
            return sao_ui_paint_ctx_fill_rounded_rect(target, command.rect.x, command.rect.y,
                                                      command.rect.width, command.rect.height,
                                                      command.radius, command.argb);
        });
    case PaintCommandKind::StrokeRoundedRect:
        return with_opacity([&] {
            return paint_rounded_rect_stroke(target, command.rect.x, command.rect.y,
                                             command.rect.width, command.rect.height,
                                             command.radius, command.width, command.argb);
        });
    case PaintCommandKind::FillEllipse:
        return with_opacity([&] {
            return sao_ui_paint_ctx_fill_ellipse(target, command.rect.x, command.rect.y,
                                                 command.rect.width, command.rect.height,
                                                 command.argb);
        });
    case PaintCommandKind::StrokeLine:
        return with_opacity([&] {
            return sao_ui_paint_ctx_stroke_line(target, command.a, command.b, command.c, command.d,
                                                command.width, command.argb);
        });
    case PaintCommandKind::FillPolygon:
        return with_opacity([&] {
            return sao_ui_paint_ctx_fill_polygon(target, command.points.data(),
                                                 command.points.size() / 2, command.argb);
        });
    case PaintCommandKind::DrawText:
        return with_opacity([&] {
            const ScopedTextRole role(static_cast<ClassicTextRole>(command.text_role),
                                      static_cast<ClassicTextWeight>(command.text_weight));
            return sao_ui_paint_ctx_draw_utf8(target, command.a, command.b, command.text.c_str(),
                                              command.width, command.argb);
        });
    case PaintCommandKind::BlitBgra:
        return with_opacity([&] {
            return sao_ui_paint_ctx_blit_premultiplied_bgra(
                target, command.bytes.data(), command.source_width, command.source_height,
                command.source_stride, command.rect.x, command.rect.y, command.rect.width,
                command.rect.height);
        });
    }
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}
} // namespace

sao_status_t create_recording_paint_context(uint32_t width, uint32_t height,
                                            sao_ui_paint_ctx_handle_t* out) noexcept {
    if (!out || !width || !height)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    try {
        auto state = std::make_unique<RecordingState>();
        state->list = std::make_shared<PaintDisplayList>();
        state->list->width = width;
        state->list->height = height;
        auto context = std::make_unique<sao_ui_paint_ctx_s>();
        context->gpu_state = state.get();
        context->gpu_dispatch = &kRecordingDispatch;
        context->target_width = width;
        context->target_height = height;
        state.release();
        *out = context.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t seal_recording_paint_context(sao_ui_paint_ctx_handle_t context,
                                          std::shared_ptr<const PaintDisplayList>* out) noexcept {
    if (!context || !out || context->gpu_dispatch != &kRecordingDispatch)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = {};
    auto* state = static_cast<RecordingState*>(context->gpu_state);
    if (!on_owner(state) || state->recording || state->sealed || !state->list ||
        !state->completed || !context->clips.empty() || context->opacity_stack.size() != 1U)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    state->sealed = true;
    *out = state->list;
    return SAO_STATUS_OK;
}

sao_status_t replay_paint_display_list(const PaintDisplayList& list,
                                       sao_ui_paint_ctx_handle_t target) noexcept {
    if (!target || !target->gpu_state || target->gpu_dispatch == &kRecordingDispatch ||
        target->target_width != list.width || target->target_height != list.height)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_status_t status = sao_ui_paint_ctx_begin_frame(target);
    if (status != SAO_STATUS_OK)
        return status;
    for (size_t index = 0; index < list.commands.size(); ++index) {
        const auto& command = list.commands[index];
        status = replay_command(command, target);
        if (status != SAO_STATUS_OK) {
#ifndef NDEBUG
            log_replay_failure(index, command, status, *target);
#endif
            break;
        }
    }
    if (status != SAO_STATUS_OK) {
        while (!target->clips.empty())
            (void)sao_ui_paint_ctx_pop_clip(target);
        while (target->opacity_stack.size() > 1U)
            (void)sao_ui_paint_ctx_pop_opacity(target);
    }
    const sao_status_t end = sao_ui_paint_ctx_end_frame(target);
#ifndef NDEBUG
    if (end != SAO_STATUS_OK)
        std::fprintf(stderr, "UI paint replay end_frame status=%d commands=%zu\n", end,
                     list.commands.size());
#endif
    // A device-lost/end-draw failure is authoritative even when an earlier
    // command also failed; the compositor needs that status to rebuild D3D.
    return end != SAO_STATUS_OK ? end : status;
}
} // namespace sao::ui::detail
