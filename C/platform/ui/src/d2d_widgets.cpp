// SAO Auto — deterministic widget and offscreen raster implementation.

#include "sao/ui/d2d_widgets.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"
#include "sao/ui/widget_kit.h"

#include "panel_theme_internal.h"
#include "widget_paint_internal.h"
#include "widget_raster_internal.h"
#include "widget_typed_internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace sao::ui::raster;

namespace sao::ui::detail {
// True-font DirectWrite backend (widget_text_render_win.cpp).  Returns false
// when unavailable so draw_text below keeps the procedural fallback.
bool draw_text_dwrite(sao_ui_paint_ctx_s& context, float x, float y, const char* text_utf8,
                      float size_px, uint32_t argb) noexcept;
} // namespace sao::ui::detail

struct sao_ui_widget_s {
    int32_t kind{};
    bool active{};
    bool enabled{true};
    bool hovered{};
    bool pressed{};
    bool focused{};
    bool show_arrows{};
    bool keyboard_nudge{};
    float value{};
    float page_size{1.0F};
    float content_size{1.0F};
    float nudge_step{0.01F};
    float radius{};
    float border_width{1.0F};
    Rect bounds{};
    std::string text;
    std::string style;
    std::unordered_map<std::string, uint32_t> prop_colors;
    std::unordered_map<std::string, uint32_t> theme_overrides;
    std::mutex mutex;
};

struct GenericWidgetPropsState {
    bool active{};
    bool enabled{true};
    bool show_arrows{};
    bool keyboard_nudge{};
    float value{};
    float page_size{1.0F};
    float content_size{1.0F};
    float nudge_step{0.01F};
    float radius{};
    float border_width{1.0F};
    std::string text;
    std::string style;
    std::unordered_map<std::string, uint32_t> colors;
};

extern "C" sao_status_t SAO_UI_CALL
sao_ui_script_canvas_paint_widget(sao_ui_widget_handle_t widget, sao_ui_paint_ctx_handle_t context,
                                  float x, float y, float width, float height);

namespace {

constexpr int32_t kInteractionHovered = 0;
constexpr int32_t kInteractionPressed = 1;
constexpr int32_t kInteractionFocused = 2;
constexpr int32_t kMaxStrokeSteps = 1 << 20;
constexpr size_t kMaxPolygonPoints = 1U << 16U;
constexpr uint64_t kMaxRasterPrimitivePixels = uint64_t{1} << 28U;
constexpr uint64_t kMaxPolygonEdgeIterations = uint64_t{1} << 30U;
constexpr uint64_t kMaxScanlineIterations = uint64_t{1} << 20U;
constexpr long double kMaxExactIterationIndex = 9007199254740992.0L;

struct FiniteRasterClip {
    double left{};
    double top{};
    double right{};
    double bottom{};

    bool empty() const noexcept {
        return right <= left || bottom <= top;
    }
};

bool finite_rect_inputs(float x, float y, float width, float height) noexcept {
    return std::isfinite(x) && std::isfinite(y) && valid_rect(width, height);
}

bool finite_float_rect(float x, float y, float width, float height) noexcept {
    if (!finite_rect_inputs(x, y, width, height))
        return false;
    return std::isfinite(x + width) && std::isfinite(y + height);
}

bool finite_raster_clip(const sao_ui_paint_ctx_s& context, FiniteRasterClip* out_clip) noexcept {
    if (context.raster == nullptr || out_clip == nullptr ||
        context.raster->width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        context.raster->height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    FiniteRasterClip clip{0.0, 0.0, static_cast<double>(context.raster->width),
                          static_cast<double>(context.raster->height)};
    for (const Rect& item : context.clips) {
        if (!finite_float_rect(item.x, item.y, item.width, item.height))
            return false;
        clip.left = std::max(clip.left, static_cast<double>(item.x));
        clip.top = std::max(clip.top, static_cast<double>(item.y));
        clip.right = std::min(clip.right, static_cast<double>(item.x + item.width));
        clip.bottom = std::min(clip.bottom, static_cast<double>(item.y + item.height));
        if (clip.empty()) {
            clip.right = clip.left;
            clip.bottom = clip.top;
            break;
        }
    }
    *out_clip = clip;
    return true;
}

bool clip_line_parameter(double p, double q, double* start, double* end) noexcept {
    if (p == 0.0)
        return q >= 0.0;
    const double ratio = q / p;
    if (p < 0.0) {
        if (ratio > *end)
            return false;
        *start = std::max(*start, ratio);
    } else {
        if (ratio < *start)
            return false;
        *end = std::min(*end, ratio);
    }
    return true;
}

bool clip_line_to_rect(double* x1, double* y1, double* x2, double* y2,
                       const FiniteRasterClip& clip, bool* out_visible) noexcept {
    *out_visible = false;
    const double delta_x = *x2 - *x1;
    const double delta_y = *y2 - *y1;
    if (delta_y == 0.0) {
        if (*y1 < clip.top || *y1 > clip.bottom)
            return true;
        const bool forward = *x1 <= *x2;
        const double low = std::max(std::min(*x1, *x2), clip.left);
        const double high = std::min(std::max(*x1, *x2), clip.right);
        if (high < low)
            return true;
        *x1 = forward ? low : high;
        *x2 = forward ? high : low;
        *out_visible = true;
        return true;
    }
    if (delta_x == 0.0) {
        if (*x1 < clip.left || *x1 > clip.right)
            return true;
        const bool forward = *y1 <= *y2;
        const double low = std::max(std::min(*y1, *y2), clip.top);
        const double high = std::min(std::max(*y1, *y2), clip.bottom);
        if (high < low)
            return true;
        *y1 = forward ? low : high;
        *y2 = forward ? high : low;
        *out_visible = true;
        return true;
    }
    double start = 0.0;
    double end = 1.0;
    if (!clip_line_parameter(-delta_x, *x1 - clip.left, &start, &end) ||
        !clip_line_parameter(delta_x, clip.right - *x1, &start, &end) ||
        !clip_line_parameter(-delta_y, *y1 - clip.top, &start, &end) ||
        !clip_line_parameter(delta_y, clip.bottom - *y1, &start, &end)) {
        return true;
    }
    const double original_x = *x1;
    const double original_y = *y1;
    *x1 = original_x + delta_x * start;
    *y1 = original_y + delta_y * start;
    *x2 = original_x + delta_x * end;
    *y2 = original_y + delta_y * end;
    *out_visible = true;
    return true;
}

struct GenericWidgetRegistry {
    std::mutex mutex;
    std::unordered_set<sao_ui_widget_handle_t> active;
    std::unordered_set<sao_ui_widget_handle_t> known;
    std::vector<std::unique_ptr<sao_ui_widget_s>> storage;
};

GenericWidgetRegistry& generic_widget_registry() {
    static GenericWidgetRegistry registry;
    return registry;
}

bool generic_widget_known(sao_ui_widget_handle_t handle) noexcept {
    try {
        auto& registry = generic_widget_registry();
        std::lock_guard lock(registry.mutex);
        return registry.known.contains(handle);
    } catch (...) {
        return false;
    }
}

class GenericLifecycleLease {
  public:
    explicit GenericLifecycleLease(void* handle) noexcept : handle_(handle) {
        acquired_ = sao::ui::detail::acquire_widget_lifecycle(handle_);
    }

    ~GenericLifecycleLease() {
        if (acquired_)
            sao::ui::detail::release_widget_lifecycle(handle_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    void* handle_{};
    bool acquired_{};
};

} // namespace

extern "C" void SAO_UI_CALL sao_ui_widget_text_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_chart_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_table_family_destroy(sao_ui_widget_handle_t handle);

void fill_rounded_rect(sao_ui_paint_ctx_s& context, Rect rect, float radius, uint32_t argb) {
    if (context.raster == nullptr || !valid_rect(rect.width, rect.height))
        return;
    radius = std::clamp(radius, 0.0F, std::min(rect.width, rect.height) * 0.5F);
    if (radius <= 0.0F) {
        fill_rect(context, rect, argb);
        return;
    }
    const Rect draw = intersect(rect, clip_bounds(context));
    if (!valid_rect(draw.width, draw.height))
        return;
    const BgraPixel color = premultiply(apply_opacity(argb, current_opacity(context)));
    const float inner_left = rect.x + radius;
    const float inner_top = rect.y + radius;
    const float inner_right = rect.x + rect.width - radius;
    const float inner_bottom = rect.y + rect.height - radius;
    const float radius_squared = radius * radius;
    for (int32_t y = static_cast<int32_t>(std::floor(draw.y));
         y < static_cast<int32_t>(std::ceil(draw.y + draw.height)); ++y) {
        for (int32_t x = static_cast<int32_t>(std::floor(draw.x));
             x < static_cast<int32_t>(std::ceil(draw.x + draw.width)); ++x) {
            const float pixel_x = static_cast<float>(x) + 0.5F;
            const float pixel_y = static_cast<float>(y) + 0.5F;
            const float nearest_x = std::clamp(pixel_x, inner_left, inner_right);
            const float nearest_y = std::clamp(pixel_y, inner_top, inner_bottom);
            const float delta_x = pixel_x - nearest_x;
            const float delta_y = pixel_y - nearest_y;
            if (delta_x * delta_x + delta_y * delta_y <= radius_squared)
                blend_pixel(*context.raster, x, y, color);
        }
    }
}

void fill_ellipse(sao_ui_paint_ctx_s& context, Rect rect, uint32_t argb) {
    if (context.raster == nullptr || !finite_rect_inputs(rect.x, rect.y, rect.width, rect.height))
        return;
    FiniteRasterClip clip{};
    if (!finite_raster_clip(context, &clip) || clip.empty())
        return;
    const float rect_right_value = rect.x + rect.width;
    const float rect_bottom_value = rect.y + rect.height;
    const double rect_right = std::isfinite(rect_right_value)
                                  ? rect_right_value
                                  : static_cast<double>(rect.x) + rect.width;
    const double rect_bottom = std::isfinite(rect_bottom_value)
                                   ? rect_bottom_value
                                   : static_cast<double>(rect.y) + rect.height;
    const double draw_left = std::max(static_cast<double>(rect.x), clip.left);
    const double draw_top = std::max(static_cast<double>(rect.y), clip.top);
    const double draw_right = std::min(rect_right, clip.right);
    const double draw_bottom = std::min(rect_bottom, clip.bottom);
    if (draw_right <= draw_left || draw_bottom <= draw_top)
        return;
    const int32_t left = static_cast<int32_t>(std::floor(draw_left));
    const int32_t top = static_cast<int32_t>(std::floor(draw_top));
    const int32_t right = static_cast<int32_t>(std::ceil(draw_right));
    const int32_t bottom = static_cast<int32_t>(std::ceil(draw_bottom));
    const float radius_x_value = rect.width * 0.5F;
    const float radius_y_value = rect.height * 0.5F;
    const float center_x_value = rect.x + radius_x_value;
    const float center_y_value = rect.y + radius_y_value;
    const BgraPixel color = premultiply(apply_opacity(argb, current_opacity(context)));
    for (int32_t y = top; y < bottom; ++y) {
        for (int32_t x = left; x < right; ++x) {
            const float dx =
                (static_cast<float>(x) + 0.5F - center_x_value) / radius_x_value;
            const float dy =
                (static_cast<float>(y) + 0.5F - center_y_value) / radius_y_value;
            if (dx * dx + dy * dy <= 1.0F)
                blend_pixel(*context.raster, x, y, color);
        }
    }
}

bool stroke_line(sao_ui_paint_ctx_s& context, float x1, float y1, float x2, float y2, float width,
                 uint32_t argb) {
    if (context.raster == nullptr || !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(width) || width <= 0.0F) {
        return false;
    }
    FiniteRasterClip clip{};
    if (!finite_raster_clip(context, &clip))
        return false;
    if (clip.empty())
        return true;
    const double checked_diameter = static_cast<double>(width) * 1.41421356;
    if (!std::isfinite(checked_diameter) ||
        checked_diameter > std::numeric_limits<float>::max()) {
        return false;
    }
    const float diameter = width * 1.41421356F;
    if (!std::isfinite(diameter))
        return false;
    const float radius = diameter * 0.5F;
    FiniteRasterClip expanded{clip.left - radius, clip.top - radius, clip.right + radius,
                              clip.bottom + radius};
    double clipped_x1 = x1;
    double clipped_y1 = y1;
    double clipped_x2 = x2;
    double clipped_y2 = y2;
    bool visible = false;
    if (!clip_line_to_rect(&clipped_x1, &clipped_y1, &clipped_x2, &clipped_y2, expanded,
                           &visible)) {
        return false;
    }
    if (!visible)
        return true;
    const double float_max = std::numeric_limits<float>::max();
    if (clipped_x1 < -float_max || clipped_x1 > float_max || clipped_y1 < -float_max ||
        clipped_y1 > float_max || clipped_x2 < -float_max || clipped_x2 > float_max ||
        clipped_y2 < -float_max || clipped_y2 > float_max) {
        return false;
    }
    const float safe_x1 = static_cast<float>(clipped_x1);
    const float safe_y1 = static_cast<float>(clipped_y1);
    const float safe_x2 = static_cast<float>(clipped_x2);
    const float safe_y2 = static_cast<float>(clipped_y2);
    const float delta_x = safe_x2 - safe_x1;
    const float delta_y = safe_y2 - safe_y1;
    if (!std::isfinite(delta_x) || !std::isfinite(delta_y))
        return false;
    const double step_count =
        std::ceil(std::max(std::fabs(static_cast<double>(delta_x)),
                           std::fabs(static_cast<double>(delta_y))));
    if (!std::isfinite(step_count) || step_count > kMaxStrokeSteps)
        return false;
    const int32_t steps = std::max(1, static_cast<int32_t>(step_count));
    for (int32_t step = 0; step <= steps; ++step) {
        const float ratio = static_cast<float>(step) / static_cast<float>(steps);
        fill_ellipse(context,
                     {safe_x1 + delta_x * ratio - radius,
                      safe_y1 + delta_y * ratio - radius, diameter, diameter},
                     argb);
    }
    return true;
}

uint32_t blend_argb(uint32_t from, uint32_t to, float amount) {
    const float t = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [t](uint32_t left, uint32_t right) {
        return static_cast<uint32_t>(std::lround(
            static_cast<float>(left) + (static_cast<float>(right) - static_cast<float>(left)) * t));
    };
    return (channel((from >> 24U) & 0xffU, (to >> 24U) & 0xffU) << 24U) |
           (channel((from >> 16U) & 0xffU, (to >> 16U) & 0xffU) << 16U) |
           (channel((from >> 8U) & 0xffU, (to >> 8U) & 0xffU) << 8U) |
           channel(from & 0xffU, to & 0xffU);
}

uint32_t lighten_argb(uint32_t color, float amount) {
    return (color & 0xff000000U) |
           (blend_argb(color | 0xff000000U,
                       sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_WHITE), amount) &
            0x00ffffffU);
}

uint32_t darken_argb(uint32_t color, float amount) {
    return (color & 0xff000000U) |
           (blend_argb(color | 0xff000000U,
                       sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_BLACK), amount) &
            0x00ffffffU);
}

bool contains(Rect rect, float x, float y) {
    return valid_rect(rect.width, rect.height) && x >= rect.x && y >= rect.y &&
           x < rect.x + rect.width && y < rect.y + rect.height;
}

struct ScrollbarGeometry {
    bool horizontal{};
    Rect decrement_arrow{};
    Rect increment_arrow{};
    Rect track_hit{};
    Rect track{};
    Rect thumb{};
    float travel{};
    float page_fraction{1.0F};
};

ScrollbarGeometry scrollbar_geometry(float bounds_x, float bounds_y, float bounds_width,
                                     float bounds_height, float page_size, float content_size,
                                     float value, bool show_arrows) {
    ScrollbarGeometry geometry{};
    const Rect bounds{bounds_x, bounds_y, bounds_width, bounds_height};
    geometry.horizontal = bounds.width >= bounds.height;
    const float major = geometry.horizontal ? bounds.width : bounds.height;
    const float cross = geometry.horizontal ? bounds.height : bounds.width;
    const float arrow_extent = show_arrows ? std::min(cross, major * 0.25F) : 0.0F;
    if (geometry.horizontal) {
        geometry.decrement_arrow = {bounds.x, bounds.y, arrow_extent, bounds.height};
        geometry.increment_arrow = {bounds.x + bounds.width - arrow_extent, bounds.y, arrow_extent,
                                    bounds.height};
        geometry.track_hit = {bounds.x + arrow_extent, bounds.y,
                              std::max(0.0F, bounds.width - arrow_extent * 2.0F), bounds.height};
    } else {
        geometry.decrement_arrow = {bounds.x, bounds.y, bounds.width, arrow_extent};
        geometry.increment_arrow = {bounds.x, bounds.y + bounds.height - arrow_extent, bounds.width,
                                    arrow_extent};
        geometry.track_hit = {bounds.x, bounds.y + arrow_extent, bounds.width,
                              std::max(0.0F, bounds.height - arrow_extent * 2.0F)};
    }
    const float track_thickness = std::clamp(cross * 0.32F, 2.0F, std::max(2.0F, cross));
    if (geometry.horizontal) {
        geometry.track = {geometry.track_hit.x, bounds.y + (bounds.height - track_thickness) * 0.5F,
                          geometry.track_hit.width, track_thickness};
    } else {
        geometry.track = {bounds.x + (bounds.width - track_thickness) * 0.5F, geometry.track_hit.y,
                          track_thickness, geometry.track_hit.height};
    }
    geometry.page_fraction = content_size <= 0.0F
                                 ? 1.0F
                                 : std::clamp(page_size / content_size, 0.0F, 1.0F);
    const float track_major =
        geometry.horizontal ? geometry.track_hit.width : geometry.track_hit.height;
    const float minimum_thumb = std::min(track_major, std::max(8.0F, cross * 0.75F));
    const float thumb_major =
        std::clamp(track_major * geometry.page_fraction, minimum_thumb, track_major);
    geometry.travel = std::max(0.0F, track_major - thumb_major);
    const float offset = geometry.travel * std::clamp(value, 0.0F, 1.0F);
    const float thumb_cross = std::max(2.0F, cross - 2.0F);
    if (geometry.horizontal) {
        geometry.thumb = {geometry.track_hit.x + offset,
                          bounds.y + (bounds.height - thumb_cross) * 0.5F, thumb_major,
                          thumb_cross};
    } else {
        geometry.thumb = {bounds.x + (bounds.width - thumb_cross) * 0.5F,
                          geometry.track_hit.y + offset, thumb_cross, thumb_major};
    }
    return geometry;
}

ScrollbarGeometry scrollbar_geometry(const sao_ui_widget_s& widget, Rect bounds) {
    return scrollbar_geometry(bounds.x, bounds.y, bounds.width, bounds.height, widget.page_size,
                              widget.content_size, widget.value, widget.show_arrows);
}

void copy_scrollbar_geometry(const ScrollbarGeometry& source, SaoUiScrollbarGeometry* target) {
    target->horizontal = source.horizontal ? 1 : 0;
    target->decrement_x = source.decrement_arrow.x;
    target->decrement_y = source.decrement_arrow.y;
    target->decrement_width = source.decrement_arrow.width;
    target->decrement_height = source.decrement_arrow.height;
    target->increment_x = source.increment_arrow.x;
    target->increment_y = source.increment_arrow.y;
    target->increment_width = source.increment_arrow.width;
    target->increment_height = source.increment_arrow.height;
    target->track_hit_x = source.track_hit.x;
    target->track_hit_y = source.track_hit.y;
    target->track_hit_width = source.track_hit.width;
    target->track_hit_height = source.track_hit.height;
    target->track_x = source.track.x;
    target->track_y = source.track.y;
    target->track_width = source.track.width;
    target->track_height = source.track.height;
    target->thumb_x = source.thumb.x;
    target->thumb_y = source.thumb.y;
    target->thumb_width = source.thumb.width;
    target->thumb_height = source.thumb.height;
    target->travel = source.travel;
    target->page_fraction = source.page_fraction;
}

struct WideScrollbarRect {
    long double x{};
    long double y{};
    long double width{};
    long double height{};
};

struct WideScrollbarGeometry {
    bool horizontal{};
    WideScrollbarRect decrement_arrow{};
    WideScrollbarRect increment_arrow{};
    WideScrollbarRect track_hit{};
    WideScrollbarRect track{};
    WideScrollbarRect thumb{};
    long double travel{};
    long double page_fraction{1.0L};
};

WideScrollbarGeometry scrollbar_geometry_wide(float bounds_x, float bounds_y, float bounds_width,
                                              float bounds_height, float page_size,
                                              float content_size, float value, bool show_arrows) {
    WideScrollbarGeometry geometry{};
    const long double x = static_cast<long double>(bounds_x);
    const long double y = static_cast<long double>(bounds_y);
    const long double width = static_cast<long double>(bounds_width);
    const long double height = static_cast<long double>(bounds_height);
    const long double page = static_cast<long double>(page_size);
    const long double content = static_cast<long double>(content_size);
    const long double position = static_cast<long double>(value);
    geometry.horizontal = width >= height;
    const long double major = geometry.horizontal ? width : height;
    const long double cross = geometry.horizontal ? height : width;
    const long double arrow_extent = show_arrows ? std::min(cross, major * 0.25L) : 0.0L;
    if (geometry.horizontal) {
        geometry.decrement_arrow = {x, y, arrow_extent, height};
        geometry.increment_arrow = {x + width - arrow_extent, y, arrow_extent, height};
        geometry.track_hit = {x + arrow_extent, y,
                              std::max(0.0L, width - arrow_extent * 2.0L), height};
    } else {
        geometry.decrement_arrow = {x, y, width, arrow_extent};
        geometry.increment_arrow = {x, y + height - arrow_extent, width, arrow_extent};
        geometry.track_hit = {x, y + arrow_extent, width,
                              std::max(0.0L, height - arrow_extent * 2.0L)};
    }
    const long double track_thickness =
        std::clamp(cross * 0.32L, 2.0L, std::max(2.0L, cross));
    if (geometry.horizontal) {
        geometry.track = {geometry.track_hit.x, y + (height - track_thickness) * 0.5L,
                          geometry.track_hit.width, track_thickness};
    } else {
        geometry.track = {x + (width - track_thickness) * 0.5L, geometry.track_hit.y,
                          track_thickness, geometry.track_hit.height};
    }
    geometry.page_fraction = std::clamp(page / content, 0.0L, 1.0L);
    const long double track_major =
        geometry.horizontal ? geometry.track_hit.width : geometry.track_hit.height;
    const long double minimum_thumb = std::min(track_major, std::max(8.0L, cross * 0.75L));
    const long double thumb_major =
        std::clamp(track_major * geometry.page_fraction, minimum_thumb, track_major);
    geometry.travel = std::max(0.0L, track_major - thumb_major);
    const long double offset = geometry.travel * std::clamp(position, 0.0L, 1.0L);
    const long double thumb_cross = std::max(2.0L, cross - 2.0L);
    if (geometry.horizontal) {
        geometry.thumb = {geometry.track_hit.x + offset,
                          y + (height - thumb_cross) * 0.5L, thumb_major, thumb_cross};
    } else {
        geometry.thumb = {x + (width - thumb_cross) * 0.5L,
                          geometry.track_hit.y + offset, thumb_cross, thumb_major};
    }
    return geometry;
}

bool copy_scrollbar_geometry_wide(const WideScrollbarGeometry& source,
                                  SaoUiScrollbarGeometry* target) {
    const long double float_max = static_cast<long double>(std::numeric_limits<float>::max());
    const auto valid_value = [float_max](long double value) {
        return std::isfinite(value) && value >= -float_max && value <= float_max;
    };
    const auto valid_rect = [&valid_value](const WideScrollbarRect& rect) {
        return valid_value(rect.x) && valid_value(rect.y) && valid_value(rect.width) &&
               valid_value(rect.height) && rect.width >= 0.0L && rect.height >= 0.0L;
    };
    if (target == nullptr || !valid_rect(source.decrement_arrow) ||
        !valid_rect(source.increment_arrow) || !valid_rect(source.track_hit) ||
        !valid_rect(source.track) || !valid_rect(source.thumb) ||
        !valid_value(source.travel) || source.travel < 0.0L ||
        !valid_value(source.page_fraction) || source.page_fraction < 0.0L ||
        source.page_fraction > 1.0L) {
        return false;
    }

    SaoUiScrollbarGeometry candidate{};
    candidate.horizontal = source.horizontal ? 1 : 0;
    candidate.decrement_x = static_cast<float>(source.decrement_arrow.x);
    candidate.decrement_y = static_cast<float>(source.decrement_arrow.y);
    candidate.decrement_width = static_cast<float>(source.decrement_arrow.width);
    candidate.decrement_height = static_cast<float>(source.decrement_arrow.height);
    candidate.increment_x = static_cast<float>(source.increment_arrow.x);
    candidate.increment_y = static_cast<float>(source.increment_arrow.y);
    candidate.increment_width = static_cast<float>(source.increment_arrow.width);
    candidate.increment_height = static_cast<float>(source.increment_arrow.height);
    candidate.track_hit_x = static_cast<float>(source.track_hit.x);
    candidate.track_hit_y = static_cast<float>(source.track_hit.y);
    candidate.track_hit_width = static_cast<float>(source.track_hit.width);
    candidate.track_hit_height = static_cast<float>(source.track_hit.height);
    candidate.track_x = static_cast<float>(source.track.x);
    candidate.track_y = static_cast<float>(source.track.y);
    candidate.track_width = static_cast<float>(source.track.width);
    candidate.track_height = static_cast<float>(source.track.height);
    candidate.thumb_x = static_cast<float>(source.thumb.x);
    candidate.thumb_y = static_cast<float>(source.thumb.y);
    candidate.thumb_width = static_cast<float>(source.thumb.width);
    candidate.thumb_height = static_cast<float>(source.thumb.height);
    candidate.travel = static_cast<float>(source.travel);
    candidate.page_fraction = static_cast<float>(source.page_fraction);
    const float values[] = {
        candidate.decrement_x,      candidate.decrement_y,      candidate.decrement_width,
        candidate.decrement_height, candidate.increment_x,      candidate.increment_y,
        candidate.increment_width,  candidate.increment_height, candidate.track_hit_x,
        candidate.track_hit_y,      candidate.track_hit_width,  candidate.track_hit_height,
        candidate.track_x,          candidate.track_y,          candidate.track_width,
        candidate.track_height,     candidate.thumb_x,         candidate.thumb_y,
        candidate.thumb_width,      candidate.thumb_height,    candidate.travel,
        candidate.page_fraction};
    for (float value : values) {
        if (!std::isfinite(value))
            return false;
    }
    const float dimensions[] = {candidate.decrement_width, candidate.decrement_height,
                                candidate.increment_width, candidate.increment_height,
                                candidate.track_hit_width, candidate.track_hit_height,
                                candidate.track_width, candidate.track_height, candidate.thumb_width,
                                candidate.thumb_height, candidate.travel, candidate.page_fraction};
    for (float dimension : dimensions) {
        if (dimension < 0.0F)
            return false;
    }
    *target = candidate;
    return true;
}
void paint_chevron(sao_ui_paint_ctx_s& context, Rect bounds, bool horizontal, bool increment,
                   uint32_t color) {
    if (!valid_rect(bounds.width, bounds.height))
        return;
    const float cx = bounds.x + bounds.width * 0.5F;
    const float cy = bounds.y + bounds.height * 0.5F;
    const float span = std::max(2.0F, std::min(bounds.width, bounds.height) * 0.22F);
    if (horizontal) {
        const float direction = increment ? 1.0F : -1.0F;
        stroke_line(context, cx - direction * span * 0.5F, cy - span, cx + direction * span * 0.5F,
                    cy, 1.0F, color);
        stroke_line(context, cx + direction * span * 0.5F, cy, cx - direction * span * 0.5F,
                    cy + span, 1.0F, color);
    } else {
        const float direction = increment ? 1.0F : -1.0F;
        stroke_line(context, cx - span, cy - direction * span * 0.5F, cx,
                    cy + direction * span * 0.5F, 1.0F, color);
        stroke_line(context, cx, cy + direction * span * 0.5F, cx + span,
                    cy - direction * span * 0.5F, 1.0F, color);
    }
}

void paint_focus_ring(sao_ui_paint_ctx_s& context, Rect bounds, float radius, bool rounded,
                      uint32_t color) {
    const Rect outer{bounds.x - 1.0F, bounds.y - 1.0F, bounds.width + 2.0F, bounds.height + 2.0F};
    if (rounded)
        fill_rounded_rect(context, outer, radius + 1.0F, color);
    else
        fill_rect(context, outer, color);
}

void draw_text(sao_ui_paint_ctx_s& context, float x, float y, const char* text, float size,
               uint32_t argb) {
    if (text == nullptr || size <= 0.0F)
        return;
    // Prefer the true-font DirectWrite path; fall back to procedural glyphs
    // when DWrite/D2D/WIC is unavailable so a paint pass never crashes.
    if (sao::ui::detail::draw_text_dwrite(context, x, y, text, size, argb))
        return;
    const int32_t scale = std::max(1, static_cast<int32_t>(std::floor(size / 5.0F)));
    float cursor = x;
    for (const unsigned char* character = reinterpret_cast<const unsigned char*>(text);
         *character != 0U; ++character) {
        if (*character == '\n') {
            cursor = x;
            y += static_cast<float>(scale * 7);
            continue;
        }
        const uint8_t bits = static_cast<uint8_t>((*character * 73U) ^ (*character >> 1U) ^ 0x5AU);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((bits & (1U << ((row + column) & 7))) != 0U) {
                    fill_rect(context,
                              {cursor + static_cast<float>(column * scale),
                               y + static_cast<float>(row * scale), static_cast<float>(scale),
                               static_cast<float>(scale)},
                              argb);
                }
            }
        }
        cursor += static_cast<float>(scale * 6);
    }
}

uint32_t widget_color(const sao_ui_widget_s& widget, const char* name, uint32_t fallback) {
    const auto theme_override = widget.theme_overrides.find(name);
    if (theme_override != widget.theme_overrides.end())
        return theme_override->second;
    const auto prop_color = widget.prop_colors.find(name);
    return prop_color == widget.prop_colors.end() ? fallback : prop_color->second;
}

uint32_t style_color(std::string_view style, const char* role, uint32_t fallback) {
    SaoUiColorToken token = SAO_UI_TOKEN_APP_TEXT;
    bool resolved = false;
    if (style == "title" || style == "label") {
        token = SAO_UI_TOKEN_APP_TEXT;
        resolved = std::strcmp(role, "fg") == 0;
    } else if (style == "subtitle" || style == "mono" || style == "muted") {
        token = SAO_UI_TOKEN_APP_TEXT_DIM;
        resolved = std::strcmp(role, "fg") == 0;
    } else if (style == "value" || style == "gold") {
        token = SAO_UI_TOKEN_APP_GOLD;
        resolved = std::strcmp(role, "fg") == 0 || std::strcmp(role, "accent") == 0;
    } else if (style == "ok") {
        token = SAO_UI_TOKEN_APP_GREEN;
        resolved = std::strcmp(role, "fg") == 0 || std::strcmp(role, "accent") == 0 ||
                   std::strcmp(role, "fill") == 0;
    } else if (style == "warn") {
        token = SAO_UI_TOKEN_APP_ORANGE;
        resolved = std::strcmp(role, "fg") == 0 || std::strcmp(role, "accent") == 0 ||
                   std::strcmp(role, "fill") == 0;
    } else if (style == "bad" || style == "danger") {
        token = SAO_UI_TOKEN_APP_RED;
        resolved = std::strcmp(role, "fg") == 0 || std::strcmp(role, "accent") == 0 ||
                   std::strcmp(role, "fill") == 0;
    } else if (style == "accent" || style == "primary") {
        token = SAO_UI_TOKEN_APP_ACCENT;
        resolved = std::strcmp(role, "fg") == 0 || std::strcmp(role, "accent") == 0 ||
                   std::strcmp(role, "fill") == 0;
    } else if (style == "ghost") {
        if (std::strcmp(role, "fill") == 0)
            return 0x00000000U;
        token = SAO_UI_TOKEN_APP_ACCENT;
        resolved = std::strcmp(role, "border") == 0 || std::strcmp(role, "accent") == 0;
    }
    return resolved ? sao::ui::detail::panel_theme_color(token) : fallback;
}

std::string_view semantic_color_key(std::string_view key) noexcept {
    if (key == "APP_BG")
        return "canvas_bg";
    if (key == "APP_CARD")
        return "fill";
    if (key == "APP_BORDER")
        return "border";
    if (key == "APP_TEXT" || key == "APP_TEXT_2" || key == "APP_TEXT_DIM")
        return "fg";
    if (key == "APP_ACCENT")
        return "accent";
    return key;
}

void paint_widget(sao_ui_widget_s& widget, sao_ui_paint_ctx_s& context, Rect bounds) {
    widget.bounds = bounds;
    const std::string_view style = widget.style;
    uint32_t accent = widget_color(
        widget, "accent", style_color(style, "accent",
                                      sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)));
    uint32_t fill = widget_color(
        widget, "fill", style_color(style, "fill", widget.active
                                                    ? accent
                                                    : sao::ui::detail::panel_theme_color(
                                                          SAO_UI_TOKEN_APP_CARD)));
    uint32_t border =
        widget_color(widget, "border", sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER));
    const SaoUiColorToken foreground_token =
        widget.kind == SAO_UI_WIDGET_TEXT || widget.kind == SAO_UI_WIDGET_MORE_INDICATOR
            ? SAO_UI_TOKEN_APP_TEXT_2
            : SAO_UI_TOKEN_APP_TEXT;
    uint32_t foreground = widget_color(
        widget, "fg", style_color(style, "fg", sao::ui::detail::panel_theme_color(
                                                   widget.enabled ? foreground_token
                                                                  : SAO_UI_TOKEN_APP_TEXT_2)));
    const bool hovered = widget.enabled && widget.hovered;
    const bool pressed = widget.enabled && widget.pressed;
    const bool focused = widget.enabled && widget.focused;
    if (pressed)
        fill = darken_argb(fill, 0.12F);
    else if (hovered)
        fill = lighten_argb(fill, 0.08F);
    if (hovered && (widget.kind == SAO_UI_WIDGET_ACTION_BUTTON ||
                    widget.kind == SAO_UI_WIDGET_DROPDOWN_BUTTON)) {
        border = blend_argb(border, accent, 0.45F);
    }
    const bool rounded = widget.kind == SAO_UI_WIDGET_ACTION_BUTTON ||
                         widget.kind == SAO_UI_WIDGET_ROUNDED_PANEL ||
                         widget.kind == SAO_UI_WIDGET_STATUS_BADGE;
    if (focused) {
        paint_focus_ring(context, bounds, widget.radius, rounded,
                         widget_color(widget, "focus", accent));
    }

    if (widget.kind == SAO_UI_WIDGET_SCROLLBAR) {
        const ScrollbarGeometry geometry = scrollbar_geometry(widget, bounds);
        const uint32_t track = widget_color(widget, "track", fill);
        uint32_t thumb = widget_color(widget, "thumb", border);
        if (pressed)
            thumb = widget_color(widget, "thumb_active", darken_argb(accent, 0.12F));
        else if (hovered)
            thumb = widget_color(widget, "thumb_hover", lighten_argb(thumb, 0.08F));
        const uint32_t arrow = widget_color(widget, "arrow", foreground);
        fill_rounded_rect(context, geometry.track,
                          std::min(geometry.track.width, geometry.track.height) * 0.5F, track);
        fill_rounded_rect(context, geometry.thumb,
                          std::min(geometry.thumb.width, geometry.thumb.height) * 0.5F, thumb);
        if (widget.show_arrows) {
            paint_chevron(context, geometry.decrement_arrow, geometry.horizontal, false, arrow);
            paint_chevron(context, geometry.increment_arrow, geometry.horizontal, true, arrow);
        }
        return;
    }

    if (widget.kind == SAO_UI_WIDGET_CHECKBOX) {
        const float box = std::clamp(bounds.height - 4.0F, 6.0F, std::min(18.0F, bounds.width));
        const Rect box_bounds{bounds.x, bounds.y + (bounds.height - box) * 0.5F, box, box};
        fill_rounded_rect(context, box_bounds, std::min(widget.radius, box * 0.25F), border);
        const Rect inner{box_bounds.x + 1.0F, box_bounds.y + 1.0F, box_bounds.width - 2.0F,
                         box_bounds.height - 2.0F};
        fill_rounded_rect(context, inner, std::max(0.0F, widget.radius - 1.0F), fill);
        if (widget.active) {
            const float x1 = box_bounds.x + box * 0.22F;
            const float y1 = box_bounds.y + box * 0.52F;
            const float x2 = box_bounds.x + box * 0.43F;
            const float y2 = box_bounds.y + box * 0.72F;
            const float x3 = box_bounds.x + box * 0.80F;
            const float y3 = box_bounds.y + box * 0.28F;
            stroke_line(context, x1, y1, x2, y2, std::max(1.0F, box * 0.12F), accent);
            stroke_line(context, x2, y2, x3, y3, std::max(1.0F, box * 0.12F), accent);
        }
        if (!widget.text.empty()) {
            draw_text(context, box_bounds.x + box + 4.0F,
                      bounds.y + std::max(1.0F, (bounds.height - 12.0F) * 0.5F),
                      widget.text.c_str(), std::max(5.0F, std::min(15.0F, bounds.height - 4.0F)),
                      foreground);
        }
        return;
    }

    if (widget.kind == SAO_UI_WIDGET_RADIO) {
        const float ring = std::clamp(bounds.height - 4.0F, 6.0F, std::min(18.0F, bounds.width));
        const Rect ring_bounds{bounds.x, bounds.y + (bounds.height - ring) * 0.5F, ring, ring};
        fill_ellipse(context, ring_bounds, border);
        const Rect inner{ring_bounds.x + 1.0F, ring_bounds.y + 1.0F, ring_bounds.width - 2.0F,
                         ring_bounds.height - 2.0F};
        fill_ellipse(context, inner, fill);
        if (widget.active) {
            const float inset = ring * 0.28F;
            fill_ellipse(context,
                         {ring_bounds.x + inset, ring_bounds.y + inset, ring - inset * 2.0F,
                          ring - inset * 2.0F},
                         accent);
        }
        if (!widget.text.empty()) {
            draw_text(context, ring_bounds.x + ring + 4.0F,
                      bounds.y + std::max(1.0F, (bounds.height - 12.0F) * 0.5F),
                      widget.text.c_str(), std::max(5.0F, std::min(15.0F, bounds.height - 4.0F)),
                      foreground);
        }
        return;
    }

    if (widget.kind == SAO_UI_WIDGET_SLIDER) {
        const bool horizontal = bounds.width >= bounds.height;
        const float cross = horizontal ? bounds.height : bounds.width;
        const float thickness = std::clamp(cross * 0.22F, 2.0F, std::max(2.0F, cross));
        const float ratio = std::clamp(widget.value, 0.0F, 1.0F);
        const float base_thumb = std::clamp(cross - 4.0F, 6.0F, cross);
        const float thumb_size = std::min(cross, base_thumb + (hovered ? 2.0F : 0.0F));
        if (horizontal) {
            const float track_y = bounds.y + (bounds.height - thickness) * 0.5F;
            fill_rounded_rect(context, {bounds.x, track_y, bounds.width, thickness},
                              thickness * 0.5F, border);
            fill_rounded_rect(context,
                              {bounds.x, track_y, std::max(1.0F, bounds.width * ratio), thickness},
                              thickness * 0.5F, accent);
            const float center = bounds.x + ratio * bounds.width;
            fill_ellipse(context,
                         {center - thumb_size * 0.5F,
                          bounds.y + (bounds.height - thumb_size) * 0.5F, thumb_size, thumb_size},
                         pressed ? darken_argb(accent, 0.12F) : foreground);
        } else {
            const float track_x = bounds.x + (bounds.width - thickness) * 0.5F;
            fill_rounded_rect(context, {track_x, bounds.y, thickness, bounds.height},
                              thickness * 0.5F, border);
            const float filled = bounds.height * ratio;
            fill_rounded_rect(
                context,
                {track_x, bounds.y + bounds.height - filled, thickness, std::max(1.0F, filled)},
                thickness * 0.5F, accent);
            const float center = bounds.y + bounds.height - filled;
            fill_ellipse(context,
                         {bounds.x + (bounds.width - thumb_size) * 0.5F, center - thumb_size * 0.5F,
                          thumb_size, thumb_size},
                         pressed ? darken_argb(accent, 0.12F) : foreground);
        }
        return;
    }

    if (widget.kind != SAO_UI_WIDGET_TEXT && widget.kind != SAO_UI_WIDGET_MORE_INDICATOR) {
        if (rounded)
            fill_rounded_rect(context, bounds, widget.radius, border);
        else
            fill_rect(context, bounds, fill);
    }
    if (rounded) {
        const float line =
            std::clamp(widget.border_width, 0.0F, std::min(bounds.width, bounds.height) * 0.5F);
        const Rect inner{bounds.x + line, bounds.y + line, bounds.width - line * 2.0F,
                         bounds.height - line * 2.0F};
        if (valid_rect(inner.width, inner.height))
            fill_rounded_rect(context, inner, std::max(0.0F, widget.radius - line), fill);
    }
    if (pressed && rounded && bounds.width > 4.0F && bounds.height > 4.0F) {
        const Rect inner_border{bounds.x + 2.0F, bounds.y + 2.0F, bounds.width - 4.0F,
                                bounds.height - 4.0F};
        fill_rounded_rect(context, inner_border, std::max(0.0F, widget.radius - 2.0F), border);
        const Rect inner_fill{bounds.x + 3.0F, bounds.y + 3.0F, bounds.width - 6.0F,
                              bounds.height - 6.0F};
        if (valid_rect(inner_fill.width, inner_fill.height))
            fill_rounded_rect(context, inner_fill, std::max(0.0F, widget.radius - 3.0F), fill);
    }
    if (widget.kind == SAO_UI_WIDGET_BAR)
        fill_rect(context,
                  {bounds.x, bounds.y, bounds.width * std::clamp(widget.value, 0.0F, 1.0F),
                   bounds.height},
                  accent);
    if (widget.kind == SAO_UI_WIDGET_DIVIDER)
        fill_rect(context,
                  {bounds.x, bounds.y + bounds.height * 0.5F, bounds.width,
                   std::max(1.0F, widget.border_width)},
                  border);
    if (widget.kind == SAO_UI_WIDGET_TABLE) {
        fill_rect(context, {bounds.x, bounds.y, bounds.width, std::min(20.0F, bounds.height)},
                  accent);
        for (float row = bounds.y + 20.0F; row < bounds.y + bounds.height; row += 18.0F)
            fill_rect(context, {bounds.x, row, bounds.width, 1.0F}, border);
    }
    if (!widget.text.empty()) {
        const float padding =
            static_cast<float>(sao::ui::detail::panel_theme_metric(SAO_UI_METRIC_PADDING_S));
        const float pressed_offset = pressed && (widget.kind == SAO_UI_WIDGET_ACTION_BUTTON ||
                                                 widget.kind == SAO_UI_WIDGET_DROPDOWN_BUTTON)
                                         ? 1.0F
                                         : 0.0F;
        draw_text(context, bounds.x + padding, bounds.y + padding + pressed_offset,
                  widget.text.c_str(), std::max(5.0F, std::min(15.0F, bounds.height - padding)),
                  foreground);
    }
}

sao_status_t paint_widget_group_opacity(sao_ui_widget_s& widget, sao_ui_paint_ctx_s& context,
                                        Rect bounds, float opacity) {
    const Rect visible = intersect(bounds, clip_bounds(context));
    if (!valid_rect(visible.width, visible.height)) {
        widget.bounds = bounds;
        return SAO_STATUS_OK;
    }
    const int32_t left = static_cast<int32_t>(std::floor(visible.x));
    const int32_t top = static_cast<int32_t>(std::floor(visible.y));
    const int32_t right = static_cast<int32_t>(std::ceil(visible.x + visible.width));
    const int32_t bottom = static_cast<int32_t>(std::ceil(visible.y + visible.height));
    if (right <= left || bottom <= top) {
        widget.bounds = bounds;
        return SAO_STATUS_OK;
    }
    try {
        sao_ui_offscreen_raster_s group;
        group.width = static_cast<uint32_t>(right - left);
        group.height = static_cast<uint32_t>(bottom - top);
        group.pixels.resize(static_cast<size_t>(group.width) * group.height);
        sao_ui_paint_ctx_s group_context;
        group_context.raster = &group;
        group_context.clips.push_back({visible.x - static_cast<float>(left),
                                       visible.y - static_cast<float>(top), visible.width,
                                       visible.height});
        paint_widget(widget, group_context,
                     {bounds.x - static_cast<float>(left), bounds.y - static_cast<float>(top),
                      bounds.width, bounds.height});
        widget.bounds = bounds;
        const float combined_opacity = std::clamp(opacity * current_opacity(context), 0.0F, 1.0F);
        for (uint32_t row = 0; row < group.height; ++row) {
            for (uint32_t column = 0; column < group.width; ++column) {
                BgraPixel source = group.pixels[static_cast<size_t>(row) * group.width + column];
                if (source.a == 0U)
                    continue;
                source = apply_opacity(source, combined_opacity);
                blend_pixel(*context.raster, left + static_cast<int32_t>(column),
                            top + static_cast<int32_t>(row), source);
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        widget.bounds = bounds;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

bool parse_color(const std::string& value, uint32_t* out_color) {
    if (out_color == nullptr || (value.size() != 7U && value.size() != 9U) || value[0] != '#') {
        return false;
    }
    uint32_t color = 0U;
    for (size_t index = 1U; index < value.size(); ++index) {
        const char c = value[index];
        if (c >= '0' && c <= '9')
            color = (color << 4U) | static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            color = (color << 4U) | static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            color = (color << 4U) | static_cast<uint32_t>(c - 'A' + 10);
        else
            return false;
    }
    *out_color =
        value.size() == 7U ? 0xff000000U | color : ((color & 0xffU) << 24U) | (color >> 8U);
    return true;
}

sao_status_t parse_widget_props(const uint8_t* props_json_utf8, size_t props_len,
                                int32_t widget_kind, GenericWidgetPropsState* out_state) {
    if (props_json_utf8 == nullptr || props_len == 0U || out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        const auto* begin = reinterpret_cast<const char*>(props_json_utf8);
        const auto document =
            nlohmann::json::parse(begin, begin + props_len, nullptr, false, false);
        if (document.is_discarded() || !document.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        GenericWidgetPropsState candidate;
        candidate.radius = static_cast<float>(
            sao::ui::detail::panel_theme_metric(SAO_UI_METRIC_BORDER_RADIUS_MEDIUM));
        for (const char* key : {"fill", "border", "fg", "accent", "canvas_bg", "track", "thumb",
                                "thumb_hover", "thumb_active", "arrow", "focus"}) {
            const auto property = document.find(key);
            if (property == document.end())
                continue;
            uint32_t color = 0U;
            if (!property->is_string() ||
                !parse_color(property->get_ref<const std::string&>(), &color)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            candidate.colors.emplace(key, color);
        }

        const auto parse_number = [&document](const char* key, float* out_value,
                                              bool* out_present) {
            const auto property = document.find(key);
            const bool present = property != document.end();
            if (out_present != nullptr)
                *out_present = present;
            if (!present)
                return true;
            if (!property->is_number())
                return false;
            const double value = property->get<double>();
            if (!std::isfinite(value) ||
                value < static_cast<double>(std::numeric_limits<float>::lowest()) ||
                value > static_cast<double>(std::numeric_limits<float>::max())) {
                return false;
            }
            *out_value = static_cast<float>(value);
            return true;
        };

        if (!parse_number("radius", &candidate.radius, nullptr) ||
            !parse_number("border_width", &candidate.border_width, nullptr)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const bool uses_numeric_value = widget_kind == SAO_UI_WIDGET_SCROLLBAR ||
                                        widget_kind == SAO_UI_WIDGET_BAR ||
                                        widget_kind == SAO_UI_WIDGET_SLIDER;
        if (uses_numeric_value) {
            float value = 0.0F;
            float ratio = 0.0F;
            bool has_value = false;
            bool has_ratio = false;
            if (!parse_number("value", &value, &has_value) ||
                !parse_number("ratio", &ratio, &has_ratio)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if (has_value || has_ratio) {
                candidate.value = std::clamp(has_value ? value : ratio, 0.0F, 1.0F);
            }
        }

        if (!parse_number("page_size", &candidate.page_size, nullptr) ||
            !parse_number("content_size", &candidate.content_size, nullptr) ||
            !parse_number("nudge_step", &candidate.nudge_step, nullptr) ||
            candidate.page_size < 0.0F || candidate.content_size <= 0.0F ||
            candidate.nudge_step <= 0.0F) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        const auto padding = document.find("padding");
        if (padding != document.end() && !padding->is_number_integer()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const auto tooltip = document.find("tooltip");
        if (tooltip != document.end() && !tooltip->is_string()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        bool selected_text = false;
        for (const char* key : {"text", "label", "title"}) {
            const auto property = document.find(key);
            if (property == document.end())
                continue;
            if (!property->is_string())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            if (!selected_text) {
                candidate.text = property->get<std::string>();
                selected_text = true;
            }
        }

        const auto style = document.find("style");
        if (style != document.end()) {
            if (!style->is_string())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            candidate.style = style->get<std::string>();
        }

        for (const auto [key, target] :
             {std::pair{"active", &candidate.active}, std::pair{"enabled", &candidate.enabled},
              std::pair{"show_arrows", &candidate.show_arrows},
              std::pair{"keyboard_nudge", &candidate.keyboard_nudge}}) {
            const auto property = document.find(key);
            if (property == document.end())
                continue;
            if (!property->is_boolean())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            *target = property->get<bool>();
        }
        const auto checked = document.find("checked");
        if (checked != document.end()) {
            if (!checked->is_boolean() ||
                (widget_kind != SAO_UI_WIDGET_CHECKBOX && widget_kind != SAO_UI_WIDGET_RADIO)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const bool checked_value = checked->get<bool>();
            const auto active = document.find("active");
            if (active != document.end() && active->get<bool>() != checked_value)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            candidate.active = checked_value;
        }

        *out_state = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_create(int32_t widget_kind, void*,
                                                         sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr || widget_kind < SAO_UI_WIDGET_ROUNDED_PANEL ||
        widget_kind > SAO_UI_WIDGET_ICON)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        auto widget = std::make_unique<sao_ui_widget_s>();
        widget->kind = widget_kind;
        widget->radius = static_cast<float>(
            sao::ui::detail::panel_theme_metric(SAO_UI_METRIC_BORDER_RADIUS_MEDIUM));
        sao_ui_widget_handle_t handle = widget.get();
        auto& registry = generic_widget_registry();
        std::lock_guard lock(registry.mutex);
        registry.active.insert(handle);
        try {
            registry.known.insert(handle);
            registry.storage.push_back(std::move(widget));
            if (!sao::ui::detail::register_external_widget_handle(
                    handle, sao::ui::detail::WidgetHandleFamily::generic, widget_kind, nullptr)) {
                registry.storage.pop_back();
                throw std::bad_alloc();
            }
        } catch (...) {
            registry.active.erase(handle);
            registry.known.erase(handle);
            throw;
        }
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_generic_backing_get_kind(sao_ui_widget_handle_t handle, int32_t* out_kind) {
    if (out_kind != nullptr)
        *out_kind = -1;
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        auto& registry = generic_widget_registry();
        std::lock_guard registry_lock(registry.mutex);
        if (!registry.active.contains(handle)) {
            if (registry.known.contains(handle))
                return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        std::lock_guard state_lock(handle->mutex);
        if (out_kind != nullptr)
            *out_kind = handle->kind;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_widget_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        auto& registry = generic_widget_registry();
        bool generic = false;
        {
            std::lock_guard registry_lock(registry.mutex);
            if (registry.known.contains(handle)) {
                if (!registry.active.erase(handle))
                    return;
                generic = true;
            }
        }
        if (generic) {
            (void)sao::ui::detail::retire_widget_lifecycle(handle);
            uint32_t removed = 0;
            (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);
            std::lock_guard state_lock(handle->mutex);
            handle->text.clear();
            handle->style.clear();
            handle->prop_colors.clear();
            handle->theme_overrides.clear();
            return;
        }
    } catch (...) {
        return;
    }
    int32_t kind = -1;
    if (sao_ui_widget_get_kind(handle, &kind) != SAO_STATUS_OK)
        return;
    if (kind >= SAO_UI_WIDGET_ROUNDED_PANEL && kind <= SAO_UI_WIDGET_ICON) {
        return;
    } else if (kind >= SAO_UI_WIDGET_LABEL && kind <= SAO_UI_WIDGET_DURATION_LABEL) {
        sao_ui_widget_text_family_destroy(handle);
    } else if (kind >= SAO_UI_WIDGET_BUTTON && kind <= SAO_UI_WIDGET_SLIDER_EXT) {
        sao_ui_widget_input_family_destroy(handle);
    } else if (kind >= SAO_UI_WIDGET_PROGRESS_BAR && kind <= SAO_UI_WIDGET_EMPTY_STATE) {
        if (kind == SAO_UI_WIDGET_TABLE_EXT || kind == SAO_UI_WIDGET_TREE_VIEW) {
            sao_ui_widget_table_family_destroy(handle);
        } else {
            sao_ui_widget_data_family_destroy(handle);
        }
    } else if (kind >= SAO_UI_WIDGET_TIME_SERIES_CHART && kind <= SAO_UI_WIDGET_SPARKLINE) {
        sao_ui_widget_chart_family_destroy(handle);
    } else if (kind == SAO_UI_WIDGET_SCRIPTABLE_CANVAS) {
        sao_ui_script_canvas_destroy(reinterpret_cast<sao_ui_script_canvas_handle_t>(handle));
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_apply_props(sao_ui_widget_handle_t handle,
                                                              const uint8_t* props_json_utf8,
                                                              size_t props_len) {
    if (handle == nullptr || (props_json_utf8 == nullptr && props_len != 0U))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    int32_t widget_kind = -1;
    const sao_status_t backing_status =
        sao_ui_widget_generic_backing_get_kind(handle, &widget_kind);
    if (backing_status == SAO_STATUS_ERR_SUBSCRIPTION_GONE)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (backing_status != SAO_STATUS_OK)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;

    GenericWidgetPropsState candidate;
    const sao_status_t parse_status =
        parse_widget_props(props_json_utf8, props_len, widget_kind, &candidate);
    if (parse_status != SAO_STATUS_OK)
        return parse_status;

    std::scoped_lock lock(handle->mutex);
    handle->active = candidate.active;
    handle->enabled = candidate.enabled;
    handle->hovered = false;
    handle->pressed = false;
    handle->focused = false;
    handle->show_arrows = candidate.show_arrows;
    handle->keyboard_nudge = candidate.keyboard_nudge;
    handle->value = candidate.value;
    handle->page_size = candidate.page_size;
    handle->content_size = candidate.content_size;
    handle->nudge_step = candidate.nudge_step;
    handle->radius = candidate.radius;
    handle->border_width = candidate.border_width;
    handle->text.swap(candidate.text);
    handle->style.swap(candidate.style);
    handle->prop_colors.swap(candidate.colors);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_theme_token(sao_ui_widget_handle_t handle,
                                                                  const char* token_key_utf8,
                                                                  uint32_t argb_value) {
    if (handle == nullptr || token_key_utf8 == nullptr || token_key_utf8[0] == '\0')
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    handle->theme_overrides[std::string(semantic_color_key(token_key_utf8))] = argb_value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_clear_theme_token(sao_ui_widget_handle_t handle,
                                                                    const char* token_key_utf8) {
    if (handle == nullptr || token_key_utf8 == nullptr || token_key_utf8[0] == '\0')
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    handle->theme_overrides.erase(std::string(semantic_color_key(token_key_utf8)));
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_paint(sao_ui_widget_handle_t handle,
                                                        sao_ui_paint_ctx_handle_t context, float x,
                                                        float y, float width, float height) {
    if (handle == nullptr || context == nullptr || context->raster == nullptr ||
        !valid_rect(width, height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        sao::ui::detail::WidgetHandleMetadata metadata{};
        if (sao::ui::detail::inspect_widget_handle(handle, &metadata) &&
            metadata.family == sao::ui::detail::WidgetHandleFamily::script_canvas &&
            metadata.kind == SAO_UI_WIDGET_SCRIPTABLE_CANVAS) {
            return sao_ui_script_canvas_paint_widget(handle, context, x, y, width, height);
        }
        GenericLifecycleLease lifecycle(handle);
        if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::scoped_lock lock(handle->mutex, context->raster->mutex);
        if (!handle->enabled)
            return paint_widget_group_opacity(*handle, *context, {x, y, width, height}, 0.4F);
        paint_widget(*handle, *context, {x, y, width, height});
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_hit_test(sao_ui_widget_handle_t handle,
                                                           float local_x, float local_y,
                                                           bool* out_hit) {
    if (handle == nullptr || out_hit == nullptr || !std::isfinite(local_x) ||
        !std::isfinite(local_y))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    *out_hit = handle->enabled && local_x >= 0.0F && local_y >= 0.0F &&
               local_x < handle->bounds.width && local_y < handle->bounds.height;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_active(sao_ui_widget_handle_t handle,
                                                             bool active) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    handle->active = active;
    return SAO_STATUS_OK;
}

namespace {

sao_status_t set_generic_interaction_state(sao_ui_widget_handle_t handle, int32_t state,
                                           bool value) {
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    switch (state) {
    case kInteractionHovered:
        handle->hovered = handle->enabled && value;
        if (!handle->hovered)
            handle->pressed = false;
        break;
    case kInteractionPressed:
        handle->pressed = handle->enabled && value;
        break;
    case kInteractionFocused:
        handle->focused = handle->enabled && value;
        break;
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

sao_status_t set_widget_interaction_state(sao_ui_widget_handle_t handle, int32_t state,
                                          bool value) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(handle, &metadata))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (metadata.family == sao::ui::detail::WidgetHandleFamily::generic)
        return set_generic_interaction_state(handle, state, value);
    if (metadata.family == sao::ui::detail::WidgetHandleFamily::input)
        return sao::ui::detail::widget_input_set_interaction_state(handle, state, value);
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

bool generic_kind_focusable(int32_t kind) {
    return kind == SAO_UI_WIDGET_ACTION_BUTTON || kind == SAO_UI_WIDGET_SCROLLBAR ||
           kind == SAO_UI_WIDGET_INPUT || kind == SAO_UI_WIDGET_SLIDER ||
           kind == SAO_UI_WIDGET_TABLE || kind == SAO_UI_WIDGET_DROPDOWN_BUTTON ||
           kind == SAO_UI_WIDGET_CHECKBOX || kind == SAO_UI_WIDGET_RADIO ||
           kind == SAO_UI_WIDGET_ICON;
}

bool generic_kind_has_value(int32_t kind) {
    return kind == SAO_UI_WIDGET_SCROLLBAR || kind == SAO_UI_WIDGET_BAR ||
           kind == SAO_UI_WIDGET_SLIDER;
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_hovered(sao_ui_widget_handle_t handle,
                                                              bool hovered) {
    return set_widget_interaction_state(handle, kInteractionHovered, hovered);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_pressed(sao_ui_widget_handle_t handle,
                                                              bool pressed) {
    return set_widget_interaction_state(handle, kInteractionPressed, pressed);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_focused(sao_ui_widget_handle_t handle,
                                                              bool focused) {
    return set_widget_interaction_state(handle, kInteractionFocused, focused);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_enabled(sao_ui_widget_handle_t handle,
                                                              bool enabled) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    handle->enabled = enabled;
    if (!enabled) {
        handle->hovered = false;
        handle->pressed = false;
        handle->focused = false;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_is_focusable(sao_ui_widget_handle_t handle,
                                                               bool* out_focusable) {
    if (handle == nullptr || out_focusable == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_focusable = false;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(handle, &metadata))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (metadata.family == sao::ui::detail::WidgetHandleFamily::input)
        return sao_ui_widget_input_is_focusable(handle, out_focusable);
    if (metadata.family != sao::ui::detail::WidgetHandleFamily::generic)
        return SAO_STATUS_OK;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    *out_focusable = handle->enabled && generic_kind_focusable(handle->kind);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_get_value(sao_ui_widget_handle_t handle,
                                                            float* out_value) {
    if (handle == nullptr || out_value == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(handle, &metadata))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (metadata.family == sao::ui::detail::WidgetHandleFamily::input &&
        metadata.kind == SAO_UI_WIDGET_SLIDER_EXT) {
        return sao_ui_slider_get_value(handle, out_value);
    }
    if (metadata.family != sao::ui::detail::WidgetHandleFamily::generic)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    if (!generic_kind_has_value(handle->kind))
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    *out_value = handle->value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_value(sao_ui_widget_handle_t handle,
                                                            float value) {
    if (handle == nullptr || !std::isfinite(value))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(handle, &metadata))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (metadata.family == sao::ui::detail::WidgetHandleFamily::input &&
        metadata.kind == SAO_UI_WIDGET_SLIDER_EXT) {
        return sao_ui_slider_set_value(handle, value);
    }
    if (metadata.family != sao::ui::detail::WidgetHandleFamily::generic)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const float normalized = std::clamp(value, 0.0F, 1.0F);
    {
        std::scoped_lock lock(handle->mutex);
        if (!generic_kind_has_value(handle->kind))
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        if (std::fabs(handle->value - normalized) <= std::numeric_limits<float>::epsilon())
            return SAO_STATUS_OK;
        handle->value = normalized;
    }
    char payload[64]{};
    std::snprintf(payload, sizeof(payload), "{\"value\":%.9g}", static_cast<double>(normalized));
    return sao_ui_widget_dispatch_event(handle, SAO_UI_EVT_VALUE_CHANGED,
                                        reinterpret_cast<const uint8_t*>(payload),
                                        std::strlen(payload));
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_nudge_value(sao_ui_widget_handle_t handle,
                                                              int32_t direction) {
    if (handle == nullptr || (direction != -1 && direction != 1))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(handle, &metadata))
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (metadata.family == sao::ui::detail::WidgetHandleFamily::input)
        return sao::ui::detail::widget_input_nudge_value(handle, direction);
    if (metadata.family != sao::ui::detail::WidgetHandleFamily::generic)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    float next = 0.0F;
    {
        GenericLifecycleLease lifecycle(handle);
        if (!lifecycle)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::scoped_lock lock(handle->mutex);
        if ((handle->kind != SAO_UI_WIDGET_SLIDER && handle->kind != SAO_UI_WIDGET_SCROLLBAR) ||
            !handle->keyboard_nudge) {
            return SAO_STATUS_ERR_ACCESS_DENIED;
        }
        next = std::clamp(handle->value + handle->nudge_step * static_cast<float>(direction), 0.0F,
                          1.0F);
    }
    return sao_ui_widget_set_value(handle, next);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scrollbar_geometry_compute(
    float bounds_x, float bounds_y, float bounds_width, float bounds_height, float page_size,
    float content_size, float value, int32_t show_arrows, SaoUiScrollbarGeometry* out_geometry) {
    if (out_geometry == nullptr || !std::isfinite(bounds_x) || !std::isfinite(bounds_y) ||
        !std::isfinite(bounds_width) || !std::isfinite(bounds_height) ||
        !std::isfinite(page_size) || !std::isfinite(content_size) || !std::isfinite(value) ||
        bounds_width <= 0.0F || bounds_height <= 0.0F || page_size < 0.0F || content_size <= 0.0F ||
        (show_arrows != 0 && show_arrows != 1)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const WideScrollbarGeometry geometry = scrollbar_geometry_wide(
        bounds_x, bounds_y, bounds_width, bounds_height, page_size, content_size, value,
        show_arrows != 0);
    return copy_scrollbar_geometry_wide(geometry, out_geometry) ? SAO_STATUS_OK
                                                                : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_scrollbar_hit_test(sao_ui_widget_handle_t handle,
                                                                     float local_x, float local_y,
                                                                     int32_t* out_part,
                                                                     float* out_page_value) {
    if (handle == nullptr || out_part == nullptr || out_page_value == nullptr ||
        !std::isfinite(local_x) || !std::isfinite(local_y)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_part = SAO_UI_SCROLLBAR_HIT_NONE;
    *out_page_value = 0.0F;
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::scoped_lock lock(handle->mutex);
    if (handle->kind != SAO_UI_WIDGET_SCROLLBAR)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    *out_page_value = handle->value;
    if (!handle->enabled || local_x < 0.0F || local_y < 0.0F || local_x >= handle->bounds.width ||
        local_y >= handle->bounds.height) {
        return SAO_STATUS_OK;
    }
    const ScrollbarGeometry geometry =
        scrollbar_geometry(*handle, {0.0F, 0.0F, handle->bounds.width, handle->bounds.height});
    if (contains(geometry.thumb, local_x, local_y)) {
        *out_part = SAO_UI_SCROLLBAR_HIT_THUMB;
        return SAO_STATUS_OK;
    }
    if (handle->show_arrows && contains(geometry.decrement_arrow, local_x, local_y)) {
        *out_part = SAO_UI_SCROLLBAR_HIT_DECREMENT_ARROW;
        *out_page_value = std::clamp(handle->value - handle->nudge_step, 0.0F, 1.0F);
        return SAO_STATUS_OK;
    }
    if (handle->show_arrows && contains(geometry.increment_arrow, local_x, local_y)) {
        *out_part = SAO_UI_SCROLLBAR_HIT_INCREMENT_ARROW;
        *out_page_value = std::clamp(handle->value + handle->nudge_step, 0.0F, 1.0F);
        return SAO_STATUS_OK;
    }
    if (!contains(geometry.track_hit, local_x, local_y))
        return SAO_STATUS_OK;
    const float coordinate = geometry.horizontal ? local_x : local_y;
    const float thumb_start = geometry.horizontal ? geometry.thumb.x : geometry.thumb.y;
    const float thumb_end =
        thumb_start + (geometry.horizontal ? geometry.thumb.width : geometry.thumb.height);
    const float page = std::clamp(geometry.page_fraction, handle->nudge_step, 1.0F);
    if (coordinate < thumb_start) {
        *out_part = SAO_UI_SCROLLBAR_HIT_TRACK_BEFORE;
        *out_page_value = std::clamp(handle->value - page, 0.0F, 1.0F);
    } else if (coordinate >= thumb_end) {
        *out_part = SAO_UI_SCROLLBAR_HIT_TRACK_AFTER;
        *out_page_value = std::clamp(handle->value + page, 0.0F, 1.0F);
    }
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_widget_test_props_state(sao_ui_widget_handle_t handle,
                                                                      const char* color_key,
                                                                      uint32_t* out_color,
                                                                      char* out_text,
                                                                      size_t out_text_capacity) {
    if (handle == nullptr || color_key == nullptr || out_color == nullptr || out_text == nullptr ||
        out_text_capacity == 0) {
        return false;
    }
    GenericLifecycleLease lifecycle(handle);
    if (!lifecycle || sao_ui_widget_generic_backing_get_kind(handle, nullptr) != SAO_STATUS_OK)
        return false;
    std::lock_guard lock(handle->mutex);
    const auto color = handle->prop_colors.find(color_key);
    if (color == handle->prop_colors.end())
        return false;
    *out_color = color->second;
    const size_t count = std::min(handle->text.size(), out_text_capacity - 1);
    std::memcpy(out_text, handle->text.data(), count);
    out_text[count] = '\0';
    return true;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_create(void*, void*,
                                                            sao_ui_paint_ctx_handle_t* out_ctx) {
    if (out_ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_ctx = nullptr;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" void SAO_UI_CALL sao_ui_paint_ctx_destroy(sao_ui_paint_ctx_handle_t context) {
    delete context;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_paint_ctx_begin_frame(sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->in_frame = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_end_frame(sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->in_frame = false;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_push_clip(sao_ui_paint_ctx_handle_t context,
                                                               float x, float y, float width,
                                                               float height) {
    if (context == nullptr || context->raster == nullptr ||
        !finite_float_rect(x, y, width, height)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        context->clips.push_back({x, y, width, height});
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_pop_clip(sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr || context->clips.empty())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->clips.pop_back();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_push_opacity(sao_ui_paint_ctx_handle_t context,
                                                                  float opacity_0_to_1) {
    if (context == nullptr || !std::isfinite(opacity_0_to_1) || opacity_0_to_1 < 0.0F ||
        opacity_0_to_1 > 1.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        context->opacity_stack.push_back(current_opacity(*context) * opacity_0_to_1);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_paint_ctx_pop_opacity(sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr || context->opacity_stack.size() <= 1U) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    context->opacity_stack.pop_back();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_offscreen_raster_create(
    const SaoUiOffscreenRasterDesc* desc, sao_ui_offscreen_raster_handle_t* out_raster) {
    if (out_raster == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_raster = nullptr;
    if (desc == nullptr || desc->width_px == 0U || desc->height_px == 0U ||
        desc->width_px > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        desc->height_px > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        desc->width_px > std::numeric_limits<uint32_t>::max() / sizeof(BgraPixel) ||
        static_cast<size_t>(desc->width_px) >
            std::numeric_limits<size_t>::max() / sizeof(BgraPixel) / desc->height_px) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto* raster = new (std::nothrow) sao_ui_offscreen_raster_s();
        if (raster == nullptr)
            return SAO_STATUS_ERR_UNKNOWN;
        try {
            raster->width = desc->width_px;
            raster->height = desc->height_px;
            raster->pixels.assign(static_cast<size_t>(raster->width) * raster->height,
                                  premultiply(desc->clear_argb));
        } catch (...) {
            delete raster;
            throw;
        }
        *out_raster = raster;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL
sao_ui_offscreen_raster_destroy(sao_ui_offscreen_raster_handle_t raster) {
    delete raster;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_offscreen_raster_snapshot(
    sao_ui_offscreen_raster_handle_t raster, uint8_t* out_bgra_premultiplied, size_t capacity,
    size_t* out_bytes_written, uint32_t* out_width_px, uint32_t* out_height_px,
    uint32_t* out_stride_bytes) {
    if (raster == nullptr || out_bytes_written == nullptr || out_width_px == nullptr ||
        out_height_px == nullptr || out_stride_bytes == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const size_t bytes = raster->pixels.size() * sizeof(BgraPixel);
    *out_bytes_written = bytes;
    *out_width_px = raster->width;
    *out_height_px = raster->height;
    *out_stride_bytes = raster->width * sizeof(BgraPixel);
    if (out_bgra_premultiplied == nullptr || capacity < bytes)
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::scoped_lock lock(raster->mutex);
    std::memcpy(out_bgra_premultiplied, raster->pixels.data(), bytes);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_create_offscreen(
    sao_ui_offscreen_raster_handle_t raster, sao_ui_paint_ctx_handle_t* out_context) {
    if (raster == nullptr || out_context == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_context = nullptr;
    try {
        auto* context = new (std::nothrow) sao_ui_paint_ctx_s();
        if (context == nullptr)
            return SAO_STATUS_ERR_UNKNOWN;
        context->raster = raster;
        *out_context = context;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_rect(sao_ui_paint_ctx_handle_t context,
                                                               float x, float y, float width,
                                                               float height, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !valid_rect(width, height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    fill_rect(*context, {x, y, width, height}, argb);
    return SAO_STATUS_OK;
}

sao_status_t sao::ui::detail::paint_rounded_rect(sao_ui_paint_ctx_handle_t context, float x,
                                                 float y, float width, float height, float radius,
                                                 uint32_t argb) noexcept {
    if (context == nullptr || context->raster == nullptr || !valid_rect(width, height) ||
        !std::isfinite(radius) || radius < 0.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->raster->mutex);
        fill_rounded_rect(*context, {x, y, width, height}, radius, argb);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_rounded_rect(
    sao_ui_paint_ctx_handle_t context, float x, float y, float width, float height, float radius,
    uint32_t argb) {
    return sao::ui::detail::paint_rounded_rect(context, x, y, width, height, radius, argb);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_stroke_line(sao_ui_paint_ctx_handle_t context,
                                                                 float x1, float y1, float x2,
                                                                 float y2, float width,
                                                                 uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !std::isfinite(x1) ||
        !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(width) ||
        width <= 0.0F)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    return stroke_line(*context, x1, y1, x2, y2, width, argb)
               ? SAO_STATUS_OK
               : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_ellipse(sao_ui_paint_ctx_handle_t context,
                                                                  float x, float y, float width,
                                                                  float height, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !valid_rect(width, height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    fill_ellipse(*context, {x, y, width, height}, argb);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_polygon(sao_ui_paint_ctx_handle_t context,
                                                                  const int32_t* points_xy,
                                                                  size_t point_count,
                                                                  uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || points_xy == nullptr ||
        point_count < 3U || point_count > kMaxPolygonPoints ||
        point_count > std::numeric_limits<size_t>::max() / 2U) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        int32_t min_x = points_xy[0];
        int32_t max_x = points_xy[0];
        int32_t min_y = points_xy[1];
        int32_t max_y = points_xy[1];
        for (size_t index = 1U; index < point_count; ++index) {
            min_x = std::min(min_x, points_xy[index * 2U]);
            max_x = std::max(max_x, points_xy[index * 2U]);
            min_y = std::min(min_y, points_xy[index * 2U + 1U]);
            max_y = std::max(max_y, points_xy[index * 2U + 1U]);
        }
        std::scoped_lock lock(context->raster->mutex);
        FiniteRasterClip clip{};
        if (!finite_raster_clip(*context, &clip))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (clip.empty())
            return SAO_STATUS_OK;
        const int64_t left = std::max<int64_t>(min_x, static_cast<int64_t>(std::ceil(clip.left)));
        const int64_t top = std::max<int64_t>(min_y, static_cast<int64_t>(std::ceil(clip.top)));
        const int64_t right = std::min<int64_t>(
            max_x, static_cast<int64_t>(std::ceil(clip.right)) - 1);
        const int64_t bottom = std::min<int64_t>(
            max_y, static_cast<int64_t>(std::ceil(clip.bottom)) - 1);
        if (right < left || bottom < top)
            return SAO_STATUS_OK;
        const uint64_t clipped_width = static_cast<uint64_t>(right - left) + 1U;
        const uint64_t clipped_height = static_cast<uint64_t>(bottom - top) + 1U;
        if (clipped_height > kMaxRasterPrimitivePixels / clipped_width ||
            clipped_height > kMaxPolygonEdgeIterations / point_count) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        std::vector<double> crossings;
        crossings.reserve(point_count);
        const BgraPixel color = premultiply(apply_opacity(argb, current_opacity(*context)));
        for (int64_t y = top; y <= bottom; ++y) {
            crossings.clear();
            for (size_t current = 0U, previous = point_count - 1U; current < point_count;
                 previous = current++) {
                const int64_t current_y_integer = points_xy[current * 2U + 1U];
                const int64_t previous_y_integer = points_xy[previous * 2U + 1U];
                if ((current_y_integer > y) != (previous_y_integer > y)) {
                    const float current_x = static_cast<float>(points_xy[current * 2U]);
                    const float current_y = static_cast<float>(current_y_integer);
                    const float previous_x = static_cast<float>(points_xy[previous * 2U]);
                    const float previous_y = static_cast<float>(previous_y_integer);
                    const float crossing =
                        (previous_x - current_x) * (static_cast<float>(y) - current_y) /
                            (previous_y - current_y) +
                        current_x;
                    if (!std::isfinite(crossing))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    crossings.push_back(crossing);
                }
            }
            std::sort(crossings.begin(), crossings.end());
            const auto fill_span = [&](double start, double end) {
                const int64_t span_left = std::max<int64_t>(
                    left, static_cast<int64_t>(std::ceil(start)));
                const int64_t span_right = std::min<int64_t>(
                    right, static_cast<int64_t>(std::ceil(end)) - 1);
                for (int64_t x = span_left; x <= span_right; ++x) {
                    blend_pixel(*context->raster, static_cast<int32_t>(x),
                                static_cast<int32_t>(y), color);
                }
            };
            size_t crossing_index = 0U;
            if ((crossings.size() & 1U) != 0U) {
                fill_span(static_cast<double>(left), crossings.front());
                crossing_index = 1U;
            }
            for (; crossing_index + 1U < crossings.size(); crossing_index += 2U) {
                fill_span(crossings[crossing_index], crossings[crossing_index + 1U]);
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_draw_utf8(sao_ui_paint_ctx_handle_t context,
                                                               float x, float y,
                                                               const char* text_utf8, float size_px,
                                                               uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || text_utf8 == nullptr ||
        !std::isfinite(size_px) || size_px <= 0.0F)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::scoped_lock lock(context->raster->mutex);
        draw_text(*context, x, y, text_utf8, size_px, argb);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_blit_premultiplied_bgra(
    sao_ui_paint_ctx_handle_t context, const uint8_t* bgra_pixels, uint32_t source_width_px,
    uint32_t source_height_px, uint32_t source_stride_bytes, float x, float y, float width,
    float height) {
    if (context == nullptr || context->raster == nullptr || bgra_pixels == nullptr ||
        source_width_px == 0U || source_height_px == 0U ||
        source_stride_bytes < source_width_px * sizeof(BgraPixel) || !valid_rect(width, height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    const Rect destination = intersect({x, y, width, height}, clip_bounds(*context));
    for (int32_t py = static_cast<int32_t>(std::floor(destination.y));
         py < static_cast<int32_t>(std::ceil(destination.y + destination.height)); ++py) {
        for (int32_t px = static_cast<int32_t>(std::floor(destination.x));
             px < static_cast<int32_t>(std::ceil(destination.x + destination.width)); ++px) {
            const uint32_t source_x =
                std::min(source_width_px - 1U, static_cast<uint32_t>((static_cast<float>(px) - x) *
                                                                     source_width_px / width));
            const uint32_t source_y =
                std::min(source_height_px - 1U, static_cast<uint32_t>((static_cast<float>(py) - y) *
                                                                      source_height_px / height));
            BgraPixel source{};
            std::memcpy(&source,
                        bgra_pixels + static_cast<size_t>(source_y) * source_stride_bytes +
                            static_cast<size_t>(source_x) * sizeof(BgraPixel),
                        sizeof(source));
            source = apply_opacity(source, current_opacity(*context));
            blend_pixel(*context->raster, px, py, source);
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_draw_corner_brackets(
    sao_ui_paint_ctx_handle_t context, float x, float y, float width, float height,
    float arm_length, float stroke_width, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !valid_rect(width, height) ||
        !std::isfinite(arm_length) || !std::isfinite(stroke_width) || arm_length <= 0.0F ||
        stroke_width <= 0.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const float arm = std::min(arm_length, std::min(width, height) * 0.5F);
    std::scoped_lock lock(context->raster->mutex);
    stroke_line(*context, x, y, x + arm, y, stroke_width, argb);
    stroke_line(*context, x, y, x, y + arm, stroke_width, argb);
    stroke_line(*context, x + width - arm, y, x + width, y, stroke_width, argb);
    stroke_line(*context, x + width, y, x + width, y + arm, stroke_width, argb);
    stroke_line(*context, x, y + height - arm, x, y + height, stroke_width, argb);
    stroke_line(*context, x, y + height, x + arm, y + height, stroke_width, argb);
    stroke_line(*context, x + width, y + height - arm, x + width, y + height, stroke_width,
                argb);
    stroke_line(*context, x + width - arm, y + height, x + width, y + height, stroke_width,
                argb);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_draw_scanlines(
    sao_ui_paint_ctx_handle_t context, float x, float y, float width, float height,
    float spacing, float line_height, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr ||
        !finite_float_rect(x, y, width, height) || !std::isfinite(spacing) ||
        !std::isfinite(line_height) || spacing <= 0.0F || line_height <= 0.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::scoped_lock lock(context->raster->mutex);
    FiniteRasterClip clip{};
    if (!finite_raster_clip(*context, &clip))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const long double request_right = static_cast<long double>(x) + width;
    const long double request_bottom = static_cast<long double>(y) + height;
    const long double draw_left = std::max<long double>(x, clip.left);
    const long double draw_right = std::min<long double>(request_right, clip.right);
    const long double draw_bottom = std::min<long double>(request_bottom, clip.bottom);
    if (clip.empty() || draw_right <= draw_left || draw_bottom <= clip.top)
        return SAO_STATUS_OK;
    long double first_index =
        std::floor((static_cast<long double>(clip.top) - line_height - y) / spacing) + 1.0L;
    first_index = std::max(0.0L, first_index);
    const long double end_index =
        std::ceil((draw_bottom - static_cast<long double>(y)) / spacing);
    if (!std::isfinite(first_index) || !std::isfinite(end_index) ||
        first_index > kMaxExactIterationIndex || end_index > kMaxExactIterationIndex) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (end_index <= first_index)
        return SAO_STATUS_OK;
    const long double count = end_index - first_index;
    if (count > kMaxScanlineIterations)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const uint64_t first = static_cast<uint64_t>(first_index);
    const uint64_t iterations = static_cast<uint64_t>(count);
    for (uint64_t offset = 0; offset < iterations; ++offset) {
        const long double line_y =
            static_cast<long double>(y) + static_cast<long double>(first + offset) * spacing;
        const long double line_top = std::max<long double>(line_y, clip.top);
        const long double line_bottom = std::min<long double>(
            std::min<long double>(line_y + line_height, request_bottom), clip.bottom);
        if (line_bottom <= line_top)
            continue;
        fill_rect(*context,
                  {static_cast<float>(draw_left), static_cast<float>(line_top),
                   static_cast<float>(draw_right - draw_left),
                   static_cast<float>(line_bottom - line_top)},
                  argb);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_draw_clock_pulse(
    sao_ui_paint_ctx_handle_t context, float center_x, float center_y, float radius,
    float phase_0_to_1, float stroke_width, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !std::isfinite(center_x) ||
        !std::isfinite(center_y) || !std::isfinite(radius) ||
        !std::isfinite(phase_0_to_1) || !std::isfinite(stroke_width) || radius <= 0.0F ||
        phase_0_to_1 < 0.0F || phase_0_to_1 > 1.0F || stroke_width <= 0.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const float pulse_radius = std::max(stroke_width, radius * phase_0_to_1);
    const uint32_t source_alpha = (argb >> 24U) & 0xffU;
    const uint32_t pulse_alpha = static_cast<uint32_t>(std::lround(
        static_cast<float>(source_alpha) * (1.0F - phase_0_to_1)));
    const uint32_t pulse_color = (argb & 0x00ffffffU) | (pulse_alpha << 24U);
    constexpr int32_t segments = 64;
    constexpr float pi = 3.14159265358979323846F;
    std::scoped_lock lock(context->raster->mutex);
    float previous_x = center_x + pulse_radius;
    float previous_y = center_y;
    for (int32_t segment = 1; segment <= segments; ++segment) {
        const float angle = 2.0F * pi * static_cast<float>(segment) /
                            static_cast<float>(segments);
        const float next_x = center_x + std::cos(angle) * pulse_radius;
        const float next_y = center_y + std::sin(angle) * pulse_radius;
        stroke_line(*context, previous_x, previous_y, next_x, next_y, stroke_width,
                    pulse_color);
        previous_x = next_x;
        previous_y = next_y;
    }
    const float tick = std::max(2.0F, radius * 0.12F);
    stroke_line(*context, center_x, center_y - pulse_radius - tick, center_x,
                center_y - pulse_radius + tick, stroke_width, pulse_color);
    stroke_line(*context, center_x + pulse_radius - tick, center_y,
                center_x + pulse_radius + tick, center_y, stroke_width, pulse_color);
    stroke_line(*context, center_x, center_y + pulse_radius - tick, center_x,
                center_y + pulse_radius + tick, stroke_width, pulse_color);
    stroke_line(*context, center_x - pulse_radius - tick, center_y,
                center_x - pulse_radius + tick, center_y, stroke_width, pulse_color);
    return SAO_STATUS_OK;
}
