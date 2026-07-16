// SAO Auto — deterministic widget and offscreen raster implementation.

#include "sao/ui/d2d_widgets.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

struct BgraPixel {
    uint8_t b{};
    uint8_t g{};
    uint8_t r{};
    uint8_t a{};
};

struct Rect {
    float x{};
    float y{};
    float width{};
    float height{};
};

struct sao_ui_offscreen_raster_s {
    uint32_t width{};
    uint32_t height{};
    std::vector<BgraPixel> pixels;
    std::mutex mutex;
};

struct sao_ui_paint_ctx_s {
    sao_ui_offscreen_raster_s* raster{};
    std::vector<Rect> clips;
    std::vector<float> opacity_stack{1.0F};
    bool in_frame{};
};

struct sao_ui_widget_s {
    int32_t kind{};
    bool active{};
    bool enabled{true};
    float value{};
    float radius{6.0F};
    float border_width{1.0F};
    Rect bounds{};
    std::string text;
    std::unordered_map<std::string, uint32_t> colors;
    std::mutex mutex;
};

extern "C" void SAO_UI_CALL sao_ui_widget_text_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_input_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_data_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_chart_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_table_family_destroy(
    sao_ui_widget_handle_t handle);

BgraPixel premultiply(uint32_t argb) {
    const uint32_t alpha = (argb >> 24U) & 0xffU;
    const uint32_t red = (argb >> 16U) & 0xffU;
    const uint32_t green = (argb >> 8U) & 0xffU;
    const uint32_t blue = argb & 0xffU;
    return {static_cast<uint8_t>((blue * alpha + 127U) / 255U),
            static_cast<uint8_t>((green * alpha + 127U) / 255U),
            static_cast<uint8_t>((red * alpha + 127U) / 255U),
            static_cast<uint8_t>(alpha)};
}

float current_opacity(const sao_ui_paint_ctx_s& context) {
    return context.opacity_stack.empty() ? 1.0F
                                         : context.opacity_stack.back();
}

uint32_t apply_opacity(uint32_t argb, float opacity) {
    const uint32_t alpha = (argb >> 24U) & 0xffU;
    const auto scaled_alpha = static_cast<uint32_t>(std::lround(
        static_cast<float>(alpha) * std::clamp(opacity, 0.0F, 1.0F)));
    return (argb & 0x00ffffffU) | (scaled_alpha << 24U);
}

BgraPixel apply_opacity(BgraPixel pixel, float opacity) {
    const float clamped = std::clamp(opacity, 0.0F, 1.0F);
    pixel.b = static_cast<uint8_t>(std::lround(pixel.b * clamped));
    pixel.g = static_cast<uint8_t>(std::lround(pixel.g * clamped));
    pixel.r = static_cast<uint8_t>(std::lround(pixel.r * clamped));
    pixel.a = static_cast<uint8_t>(std::lround(pixel.a * clamped));
    return pixel;
}

bool valid_rect(float width, float height) {
    return std::isfinite(width) && std::isfinite(height) && width > 0.0F && height > 0.0F;
}

Rect intersect(Rect first, Rect second) {
    const float x = std::max(first.x, second.x);
    const float y = std::max(first.y, second.y);
    const float right = std::min(first.x + first.width, second.x + second.width);
    const float bottom = std::min(first.y + first.height, second.y + second.height);
    return {x, y, std::max(0.0F, right - x), std::max(0.0F, bottom - y)};
}

Rect clip_bounds(const sao_ui_paint_ctx_s& context) {
    if (context.raster == nullptr) return {};
    Rect result{0.0F, 0.0F, static_cast<float>(context.raster->width), static_cast<float>(context.raster->height)};
    for (const Rect& clip : context.clips) result = intersect(result, clip);
    return result;
}

void blend_pixel(sao_ui_offscreen_raster_s& raster, int32_t x, int32_t y, BgraPixel source) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= raster.width || static_cast<uint32_t>(y) >= raster.height) return;
    BgraPixel& destination = raster.pixels[static_cast<size_t>(y) * raster.width + static_cast<uint32_t>(x)];
    const uint32_t inverse_alpha = 255U - source.a;
    destination.b = static_cast<uint8_t>(source.b + (static_cast<uint32_t>(destination.b) * inverse_alpha + 127U) / 255U);
    destination.g = static_cast<uint8_t>(source.g + (static_cast<uint32_t>(destination.g) * inverse_alpha + 127U) / 255U);
    destination.r = static_cast<uint8_t>(source.r + (static_cast<uint32_t>(destination.r) * inverse_alpha + 127U) / 255U);
    destination.a = static_cast<uint8_t>(source.a + (static_cast<uint32_t>(destination.a) * inverse_alpha + 127U) / 255U);
}

void fill_rect(sao_ui_paint_ctx_s& context, Rect rect, uint32_t argb) {
    if (context.raster == nullptr || !valid_rect(rect.width, rect.height)) return;
    rect = intersect(rect, clip_bounds(context));
    if (!valid_rect(rect.width, rect.height)) return;
    const BgraPixel color = premultiply(
        apply_opacity(argb, current_opacity(context)));
    const int32_t left = static_cast<int32_t>(std::floor(rect.x));
    const int32_t top = static_cast<int32_t>(std::floor(rect.y));
    const int32_t right = static_cast<int32_t>(std::ceil(rect.x + rect.width));
    const int32_t bottom = static_cast<int32_t>(std::ceil(rect.y + rect.height));
    for (int32_t y = top; y < bottom; ++y) {
        for (int32_t x = left; x < right; ++x) blend_pixel(*context.raster, x, y, color);
    }
}

void fill_ellipse(sao_ui_paint_ctx_s& context, Rect rect, uint32_t argb) {
    if (context.raster == nullptr || !valid_rect(rect.width, rect.height)) return;
    const Rect draw = intersect(rect, clip_bounds(context));
    const float radius_x = rect.width * 0.5F;
    const float radius_y = rect.height * 0.5F;
    const float center_x = rect.x + radius_x;
    const float center_y = rect.y + radius_y;
    const BgraPixel color = premultiply(
        apply_opacity(argb, current_opacity(context)));
    for (int32_t y = static_cast<int32_t>(std::floor(draw.y)); y < static_cast<int32_t>(std::ceil(draw.y + draw.height)); ++y) {
        for (int32_t x = static_cast<int32_t>(std::floor(draw.x)); x < static_cast<int32_t>(std::ceil(draw.x + draw.width)); ++x) {
            const float dx = (static_cast<float>(x) + 0.5F - center_x) / radius_x;
            const float dy = (static_cast<float>(y) + 0.5F - center_y) / radius_y;
            if (dx * dx + dy * dy <= 1.0F) blend_pixel(*context.raster, x, y, color);
        }
    }
}

void stroke_line(sao_ui_paint_ctx_s& context, float x1, float y1, float x2, float y2, float width, uint32_t argb) {
    const int32_t steps = std::max(1, static_cast<int32_t>(std::ceil(std::max(std::fabs(x2 - x1), std::fabs(y2 - y1)))));
    const float diameter = width * 1.41421356F;
    const float radius = diameter * 0.5F;
    for (int32_t step = 0; step <= steps; ++step) {
        const float ratio = static_cast<float>(step) / static_cast<float>(steps);
        fill_ellipse(context, {x1 + (x2 - x1) * ratio - radius, y1 + (y2 - y1) * ratio - radius, diameter, diameter}, argb);
    }
}

void draw_text(sao_ui_paint_ctx_s& context, float x, float y, const char* text, float size, uint32_t argb) {
    if (text == nullptr || size <= 0.0F) return;
    const int32_t scale = std::max(1, static_cast<int32_t>(std::floor(size / 5.0F)));
    float cursor = x;
    for (const unsigned char* character = reinterpret_cast<const unsigned char*>(text); *character != 0U; ++character) {
        if (*character == '\n') {
            cursor = x;
            y += static_cast<float>(scale * 7);
            continue;
        }
        const uint8_t bits = static_cast<uint8_t>((*character * 73U) ^ (*character >> 1U) ^ 0x5AU);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((bits & (1U << ((row + column) & 7))) != 0U) {
                    fill_rect(context, {cursor + static_cast<float>(column * scale), y + static_cast<float>(row * scale), static_cast<float>(scale), static_cast<float>(scale)}, argb);
                }
            }
        }
        cursor += static_cast<float>(scale * 6);
    }
}

uint32_t widget_color(const sao_ui_widget_s& widget, const char* name, uint32_t fallback) {
    const auto found = widget.colors.find(name);
    return found == widget.colors.end() ? fallback : found->second;
}

void paint_widget(sao_ui_widget_s& widget, sao_ui_paint_ctx_s& context, Rect bounds) {
    widget.bounds = bounds;
    const uint32_t fill = widget_color(widget, "fill", widget.active ? 0xff3a8ee6U : 0xff273447U);
    const uint32_t border = widget_color(widget, "border", 0xff75849aU);
    const uint32_t foreground = widget_color(widget, "fg", widget.enabled ? 0xfff0f4faU : 0xff8f9aaaU);
    const uint32_t accent = widget_color(widget, "accent", 0xff4ea5ffU);
    if (widget.kind != SAO_UI_WIDGET_TEXT && widget.kind != SAO_UI_WIDGET_MORE_INDICATOR) fill_rect(context, bounds, fill);
    if (widget.kind == SAO_UI_WIDGET_BAR) fill_rect(context, {bounds.x, bounds.y, bounds.width * std::clamp(widget.value, 0.0F, 1.0F), bounds.height}, accent);
    if (widget.kind == SAO_UI_WIDGET_DIVIDER) fill_rect(context, {bounds.x, bounds.y + bounds.height * 0.5F, bounds.width, std::max(1.0F, widget.border_width)}, border);
    if (widget.kind == SAO_UI_WIDGET_TABLE) {
        fill_rect(context, {bounds.x, bounds.y, bounds.width, std::min(20.0F, bounds.height)}, accent);
        for (float row = bounds.y + 20.0F; row < bounds.y + bounds.height; row += 18.0F) fill_rect(context, {bounds.x, row, bounds.width, 1.0F}, border);
    }
    if (widget.kind == SAO_UI_WIDGET_ACTION_BUTTON || widget.kind == SAO_UI_WIDGET_ROUNDED_PANEL || widget.kind == SAO_UI_WIDGET_STATUS_BADGE) {
        const float line = std::min(widget.border_width, std::min(bounds.width, bounds.height) * 0.5F);
        fill_rect(context, {bounds.x, bounds.y, bounds.width, line}, border);
        fill_rect(context, {bounds.x, bounds.y + bounds.height - line, bounds.width, line}, border);
        fill_rect(context, {bounds.x, bounds.y, line, bounds.height}, border);
        fill_rect(context, {bounds.x + bounds.width - line, bounds.y, line, bounds.height}, border);
    }
    if (!widget.text.empty()) draw_text(context, bounds.x + 3.0F, bounds.y + 3.0F, widget.text.c_str(), std::max(5.0F, std::min(15.0F, bounds.height - 4.0F)), foreground);
}

bool parse_color(const std::string& value, uint32_t* out_color) {
    if (out_color == nullptr || value.size() != 7U || value[0] != '#') return false;
    uint32_t color = 0U;
    for (size_t index = 1U; index < value.size(); ++index) {
        const char c = value[index];
        if (c >= '0' && c <= '9') color = (color << 4U) | static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') color = (color << 4U) | static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') color = (color << 4U) | static_cast<uint32_t>(c - 'A' + 10);
        else return false;
    }
    *out_color = 0xff000000U | color;
    return true;
}

bool json_string(const std::string& json, const char* key, std::string* result) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t name = json.find(needle);
    const size_t colon = name == std::string::npos ? std::string::npos : json.find(':', name + needle.size());
    const size_t quote = colon == std::string::npos ? std::string::npos : json.find('"', colon + 1U);
    const size_t end = quote == std::string::npos ? std::string::npos : json.find('"', quote + 1U);
    if (end == std::string::npos) return false;
    *result = json.substr(quote + 1U, end - quote - 1U);
    return true;
}

bool json_number(const std::string& json, const char* key, float* result) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t name = json.find(needle);
    const size_t colon = name == std::string::npos ? std::string::npos : json.find(':', name + needle.size());
    if (colon == std::string::npos) return false;
    char* end = nullptr;
    const float value = std::strtof(json.c_str() + colon + 1U, &end);
    if (end == json.c_str() + colon + 1U || !std::isfinite(value)) return false;
    *result = value;
    return true;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_create(
    int32_t widget_kind, void*, sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr || widget_kind < SAO_UI_WIDGET_ROUNDED_PANEL || widget_kind > SAO_UI_WIDGET_ICON) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* widget = new (std::nothrow) sao_ui_widget_s();
    if (widget == nullptr) {
        *out_handle = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    widget->kind = widget_kind;
    *out_handle = widget;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_widget_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    uint32_t removed = 0;
    (void)sao_ui_widget_release_event_handlers(handle, &removed);
    const int32_t kind = *reinterpret_cast<const int32_t*>(handle);
    if (kind >= SAO_UI_WIDGET_ROUNDED_PANEL && kind <= SAO_UI_WIDGET_ICON) {
        delete handle;
    } else if (kind >= SAO_UI_WIDGET_LABEL &&
               kind <= SAO_UI_WIDGET_DURATION_LABEL) {
        sao_ui_widget_text_family_destroy(handle);
    } else if (kind >= SAO_UI_WIDGET_BUTTON &&
               kind <= SAO_UI_WIDGET_SLIDER_EXT) {
        sao_ui_widget_input_family_destroy(handle);
    } else if (kind >= SAO_UI_WIDGET_PROGRESS_BAR &&
               kind <= SAO_UI_WIDGET_EMPTY_STATE) {
        if (kind == SAO_UI_WIDGET_TABLE_EXT ||
            kind == SAO_UI_WIDGET_TREE_VIEW) {
            sao_ui_widget_table_family_destroy(handle);
        } else {
            sao_ui_widget_data_family_destroy(handle);
        }
    } else if (kind >= SAO_UI_WIDGET_TIME_SERIES_CHART &&
               kind <= SAO_UI_WIDGET_SPARKLINE) {
        sao_ui_widget_chart_family_destroy(handle);
    } else if (kind == SAO_UI_WIDGET_SCRIPTABLE_CANVAS) {
        sao_ui_script_canvas_destroy(
            reinterpret_cast<sao_ui_script_canvas_handle_t>(handle));
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_apply_props(
    sao_ui_widget_handle_t handle, const uint8_t* props_json_utf8, size_t props_len) {
    if (handle == nullptr || (props_json_utf8 == nullptr && props_len != 0U)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const std::string json(reinterpret_cast<const char*>(props_json_utf8), props_len);
    std::scoped_lock lock(handle->mutex);
    for (const char* key : {"fill", "border", "fg", "accent", "canvas_bg"}) {
        std::string text;
        uint32_t color = 0U;
        if (json_string(json, key, &text)) {
            if (!parse_color(text, &color)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            handle->colors[key] = color;
        }
    }
    json_number(json, "radius", &handle->radius);
    json_number(json, "border_width", &handle->border_width);
    if (json_number(json, "value", &handle->value) || json_number(json, "ratio", &handle->value)) handle->value = std::clamp(handle->value, 0.0F, 1.0F);
    for (const char* key : {"text", "label", "title"}) {
        std::string text;
        if (json_string(json, key, &text)) {
            handle->text = std::move(text);
            break;
        }
    }
    if (json.find("\"active\":true") != std::string::npos) handle->active = true;
    if (json.find("\"active\":false") != std::string::npos) handle->active = false;
    if (json.find("\"enabled\":false") != std::string::npos) handle->enabled = false;
    if (json.find("\"enabled\":true") != std::string::npos) handle->enabled = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_theme_token(
    sao_ui_widget_handle_t handle, const char* token_key_utf8, uint32_t argb_value) {
    if (handle == nullptr || token_key_utf8 == nullptr || token_key_utf8[0] == '\0') return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(handle->mutex);
    handle->colors[token_key_utf8] = argb_value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_clear_theme_token(
    sao_ui_widget_handle_t handle, const char* token_key_utf8) {
    if (handle == nullptr || token_key_utf8 == nullptr || token_key_utf8[0] == '\0') return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(handle->mutex);
    handle->colors.erase(token_key_utf8);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_paint(
    sao_ui_widget_handle_t handle, sao_ui_paint_ctx_handle_t context,
    float x, float y, float width, float height) {
    if (handle == nullptr || context == nullptr || context->raster == nullptr || !valid_rect(width, height)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(handle->mutex, context->raster->mutex);
    paint_widget(*handle, *context, {x, y, width, height});
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_hit_test(
    sao_ui_widget_handle_t handle, float local_x, float local_y, bool* out_hit) {
    if (handle == nullptr || out_hit == nullptr || !std::isfinite(local_x) || !std::isfinite(local_y)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(handle->mutex);
    *out_hit = handle->enabled && local_x >= 0.0F && local_y >= 0.0F && local_x < handle->bounds.width && local_y < handle->bounds.height;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_set_active(
    sao_ui_widget_handle_t handle, bool active) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(handle->mutex);
    handle->active = active;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_create(
    void*, void*, sao_ui_paint_ctx_handle_t* out_ctx) {
    if (out_ctx == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_ctx = new (std::nothrow) sao_ui_paint_ctx_s();
    return *out_ctx == nullptr ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_paint_ctx_destroy(
    sao_ui_paint_ctx_handle_t context) {
    delete context;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_begin_frame(
    sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->in_frame = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_end_frame(
    sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->in_frame = false;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_push_clip(
    sao_ui_paint_ctx_handle_t context, float x, float y, float width, float height) {
    if (context == nullptr || !valid_rect(width, height)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->clips.push_back({x, y, width, height});
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_pop_clip(
    sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr || context->clips.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    context->clips.pop_back();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_push_opacity(
    sao_ui_paint_ctx_handle_t context, float opacity_0_to_1) {
    if (context == nullptr || !std::isfinite(opacity_0_to_1) ||
        opacity_0_to_1 < 0.0F || opacity_0_to_1 > 1.0F) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        context->opacity_stack.push_back(
            current_opacity(*context) * opacity_0_to_1);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_pop_opacity(
    sao_ui_paint_ctx_handle_t context) {
    if (context == nullptr || context->opacity_stack.size() <= 1U) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    context->opacity_stack.pop_back();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_offscreen_raster_create(
    const SaoUiOffscreenRasterDesc* desc, sao_ui_offscreen_raster_handle_t* out_raster) {
    if (desc == nullptr || out_raster == nullptr || desc->width_px == 0U || desc->height_px == 0U ||
        desc->width_px > std::numeric_limits<size_t>::max() / desc->height_px) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* raster = new (std::nothrow) sao_ui_offscreen_raster_s();
    if (raster == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    try {
        raster->width = desc->width_px;
        raster->height = desc->height_px;
        raster->pixels.assign(static_cast<size_t>(raster->width) * raster->height, premultiply(desc->clear_argb));
    } catch (...) {
        delete raster;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_raster = raster;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_offscreen_raster_destroy(sao_ui_offscreen_raster_handle_t raster) {
    delete raster;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_offscreen_raster_snapshot(
    sao_ui_offscreen_raster_handle_t raster, uint8_t* out_bgra_premultiplied, size_t capacity,
    size_t* out_bytes_written, uint32_t* out_width_px, uint32_t* out_height_px, uint32_t* out_stride_bytes) {
    if (raster == nullptr || out_bytes_written == nullptr || out_width_px == nullptr || out_height_px == nullptr || out_stride_bytes == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const size_t bytes = raster->pixels.size() * sizeof(BgraPixel);
    *out_bytes_written = bytes;
    *out_width_px = raster->width;
    *out_height_px = raster->height;
    *out_stride_bytes = raster->width * sizeof(BgraPixel);
    if (out_bgra_premultiplied == nullptr || capacity < bytes) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::scoped_lock lock(raster->mutex);
    std::memcpy(out_bgra_premultiplied, raster->pixels.data(), bytes);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_create_offscreen(
    sao_ui_offscreen_raster_handle_t raster, sao_ui_paint_ctx_handle_t* out_context) {
    if (raster == nullptr || out_context == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* context = new (std::nothrow) sao_ui_paint_ctx_s();
    if (context == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    context->raster = raster;
    *out_context = context;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_rect(
    sao_ui_paint_ctx_handle_t context, float x, float y, float width, float height, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !valid_rect(width, height)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    fill_rect(*context, {x, y, width, height}, argb);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_stroke_line(
    sao_ui_paint_ctx_handle_t context, float x1, float y1, float x2, float y2, float width, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(width) || width <= 0.0F) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    stroke_line(*context, x1, y1, x2, y2, width, argb);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_ellipse(
    sao_ui_paint_ctx_handle_t context, float x, float y, float width, float height, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || !valid_rect(width, height)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    fill_ellipse(*context, {x, y, width, height}, argb);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_fill_polygon(
    sao_ui_paint_ctx_handle_t context, const int32_t* points_xy, size_t point_count, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || points_xy == nullptr || point_count < 3U) return SAO_STATUS_ERR_INVALID_ARGUMENT;
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
    const Rect clip = clip_bounds(*context);
    const BgraPixel color = premultiply(
        apply_opacity(argb, current_opacity(*context)));
    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            if (x < clip.x || y < clip.y || x >= clip.x + clip.width || y >= clip.y + clip.height) continue;
            bool inside = false;
            for (size_t current = 0U, previous = point_count - 1U; current < point_count; previous = current++) {
                const float current_x = static_cast<float>(points_xy[current * 2U]);
                const float current_y = static_cast<float>(points_xy[current * 2U + 1U]);
                const float previous_x = static_cast<float>(points_xy[previous * 2U]);
                const float previous_y = static_cast<float>(points_xy[previous * 2U + 1U]);
                if ((current_y > y) != (previous_y > y) && static_cast<float>(x) <
                    (previous_x - current_x) * (static_cast<float>(y) - current_y) / (previous_y - current_y) + current_x) inside = !inside;
            }
            if (inside) blend_pixel(*context->raster, x, y, color);
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_draw_utf8(
    sao_ui_paint_ctx_handle_t context, float x, float y, const char* text_utf8, float size_px, uint32_t argb) {
    if (context == nullptr || context->raster == nullptr || text_utf8 == nullptr || !std::isfinite(size_px) || size_px <= 0.0F) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    draw_text(*context, x, y, text_utf8, size_px, argb);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_paint_ctx_blit_premultiplied_bgra(
    sao_ui_paint_ctx_handle_t context, const uint8_t* bgra_pixels, uint32_t source_width_px, uint32_t source_height_px,
    uint32_t source_stride_bytes, float x, float y, float width, float height) {
    if (context == nullptr || context->raster == nullptr || bgra_pixels == nullptr || source_width_px == 0U || source_height_px == 0U ||
        source_stride_bytes < source_width_px * sizeof(BgraPixel) || !valid_rect(width, height)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(context->raster->mutex);
    const Rect destination = intersect({x, y, width, height}, clip_bounds(*context));
    for (int32_t py = static_cast<int32_t>(std::floor(destination.y)); py < static_cast<int32_t>(std::ceil(destination.y + destination.height)); ++py) {
        for (int32_t px = static_cast<int32_t>(std::floor(destination.x)); px < static_cast<int32_t>(std::ceil(destination.x + destination.width)); ++px) {
            const uint32_t source_x = std::min(source_width_px - 1U, static_cast<uint32_t>((static_cast<float>(px) - x) * source_width_px / width));
            const uint32_t source_y = std::min(source_height_px - 1U, static_cast<uint32_t>((static_cast<float>(py) - y) * source_height_px / height));
            BgraPixel source{};
            std::memcpy(&source, bgra_pixels + static_cast<size_t>(source_y) * source_stride_bytes + static_cast<size_t>(source_x) * sizeof(BgraPixel), sizeof(source));
            source = apply_opacity(source, current_opacity(*context));
            blend_pixel(*context->raster, px, py, source);
        }
    }
    return SAO_STATUS_OK;
}
