// SAO Auto — panel search/filter helper row implementation.
//
// Generic filter-chips + search-box row.  Registered in the data widget
// family (tag 171) so it flows through the existing typed dispatch.
// Owns a search-box query and a set of toggleable filter chips; state
// is exposed via the JSON props pipeline and state_json().

#include "sao/ui/widget_filter_row.h"
#include "sao/ui/widget_kit.h"

#include "panel_theme_internal.h"
#include "widget_paint_internal.h"
#include "widget_typed_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr int32_t kFilterRowTag = 171;  // SAO_UI_WIDGET_FILTER_ROW
constexpr int32_t kDefaultSearchWidth = 200;
constexpr int32_t kDefaultChipHeight = 26;
constexpr int32_t kDefaultChipGap = 8;
constexpr int32_t kDefaultFontSize = 13;
constexpr int32_t kDefaultPadX = 10;
constexpr int32_t kDefaultPadY = 6;
constexpr int32_t kDefaultRadius = 8;

uint32_t resolve_or(uint32_t override_value, SaoUiColorToken token) {
    return override_value != 0 ? override_value
                               : sao::ui::detail::panel_theme_color(token);
}

struct OwnedChip {
    std::string label;
    int32_t chip_id{-1};
    bool selected{false};
};

struct FilterRowState {
    int32_t tag{kFilterRowTag};
    SaoUiFilterRowSpec spec{};
    std::string placeholder;
    std::string query;
    std::vector<OwnedChip> chips;
    mutable std::mutex mtx;
};

struct FilterRowPropsSnapshot {
    std::string query;
    std::vector<std::pair<int32_t, bool>> chip_selection;
};

}  // namespace

template <typename State>
std::shared_ptr<State> as_data(sao_ui_widget_handle_t handle, int32_t kind) {
    return std::static_pointer_cast<State>(sao::ui::detail::acquire_widget_handle(
        handle, sao::ui::detail::WidgetHandleFamily::data, kind));
}

std::shared_ptr<FilterRowState> as_filter_row(sao_ui_widget_handle_t handle) {
    return as_data<FilterRowState>(handle, kFilterRowTag);
}

sao_status_t publish_filter_row_state(std::shared_ptr<FilterRowState> state,
                                      sao_ui_widget_handle_t* out_handle) {
    void* const handle = sao::ui::detail::register_widget_handle(
        sao::ui::detail::WidgetHandleFamily::data, kFilterRowTag, std::move(state));
    if (handle == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(handle);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_create(
    void* /*d3d_device_ptr*/, const SaoUiFilterRowSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr || spec == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto state = std::make_shared<FilterRowState>();
        state->spec = *spec;
        state->spec.search_box_width_px =
            spec->search_box_width_px > 0 ? spec->search_box_width_px : kDefaultSearchWidth;
        state->spec.chip_height_px =
            spec->chip_height_px > 0 ? spec->chip_height_px : kDefaultChipHeight;
        state->spec.chip_gap_px =
            spec->chip_gap_px > 0 ? spec->chip_gap_px : kDefaultChipGap;
        state->spec.font_size_px =
            spec->font_size_px > 0 ? spec->font_size_px : kDefaultFontSize;
        state->spec.pad_x_px = spec->pad_x_px > 0 ? spec->pad_x_px : kDefaultPadX;
        state->spec.pad_y_px = spec->pad_y_px > 0 ? spec->pad_y_px : kDefaultPadY;
        state->spec.radius_px = spec->radius_px > 0 ? spec->radius_px : kDefaultRadius;
        state->placeholder = spec->placeholder_utf8 ? spec->placeholder_utf8 : "";
        for (size_t i = 0; i < spec->chip_count; ++i) {
            const auto& chip = spec->chips[i];
            OwnedChip owned;
            owned.label = chip.label_utf8 ? chip.label_utf8 : "";
            owned.chip_id = chip.chip_id;
            owned.selected = chip.selected;
            state->chips.push_back(std::move(owned));
        }
        return publish_filter_row_state(std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_set_query(
    sao_ui_widget_handle_t handle, const char* text_utf8) {
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->query = text_utf8 ? text_utf8 : "";
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_get_query(
    sao_ui_widget_handle_t handle, char* out_utf8, size_t capacity,
    size_t* out_bytes_written) {
    if (out_bytes_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_bytes_written = state->query.size();
    if (out_utf8 == nullptr || capacity <= state->query.size())
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out_utf8, state->query.c_str(), state->query.size() + 1);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_toggle_chip(
    sao_ui_widget_handle_t handle, int32_t chip_id) {
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    for (auto& chip : state->chips) {
        if (chip.chip_id == chip_id) {
            chip.selected = !chip.selected;
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_set_chip_selected(
    sao_ui_widget_handle_t handle, int32_t chip_id, bool selected) {
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    for (auto& chip : state->chips) {
        if (chip.chip_id == chip_id) {
            chip.selected = selected;
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_is_chip_selected(
    sao_ui_widget_handle_t handle, int32_t chip_id, bool* out_selected) {
    if (out_selected == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    for (const auto& chip : state->chips) {
        if (chip.chip_id == chip_id) {
            *out_selected = chip.selected;
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_matches_text(
    sao_ui_widget_handle_t handle, const char* candidate_utf8, bool* out_match) {
    if (out_match == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    if (state->query.empty()) {
        *out_match = true;
        return SAO_STATUS_OK;
    }
    if (candidate_utf8 == nullptr) {
        *out_match = false;
        return SAO_STATUS_OK;
    }
    // Case-insensitive substring match.
    std::string q = state->query;
    std::string c = candidate_utf8;
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(c.begin(), c.end(), c.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    *out_match = c.find(q) != std::string::npos;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_matches_chips(
    sao_ui_widget_handle_t handle, const int32_t* selected_ids,
    size_t selected_count, bool* out_match) {
    if (out_match == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    if (selected_ids == nullptr || selected_count == 0) {
        *out_match = true;  // no chip filter active
        return SAO_STATUS_OK;
    }
    for (size_t i = 0; i < selected_count; ++i) {
        for (const auto& chip : state->chips) {
            if (chip.chip_id == selected_ids[i] && chip.selected) {
                *out_match = true;
                return SAO_STATUS_OK;
            }
        }
    }
    *out_match = false;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_apply_props(
    sao_ui_widget_handle_t handle, const uint8_t* props_json_utf8, size_t props_len) {
    if (handle == nullptr || (props_json_utf8 == nullptr && props_len != 0))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        nlohmann::json props = nlohmann::json::parse(
            props_json_utf8, props_json_utf8 + props_len, nullptr, false);
        if (!props.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (!sao::ui::detail::widget_props_has_only(props,
                {"query", "chips", "placeholder", "search_width", "chip_height",
                 "chip_gap", "font_size", "pad_x", "pad_y", "radius", "bg", "fg",
                 "border", "chip_bg", "chip_selected", "chip_fg", "chip_selected_fg",
                 "theme"}))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto state = as_filter_row(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(state->mtx);
        const auto query_it = props.find("query");
        if (query_it != props.end()) {
            if (!query_it->is_string())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->query = query_it->get<std::string>();
        }
        const auto placeholder_it = props.find("placeholder");
        if (placeholder_it != props.end()) {
            if (!placeholder_it->is_string())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->placeholder = placeholder_it->get<std::string>();
        }
        const auto chips_it = props.find("chips");
        if (chips_it != props.end()) {
            if (!chips_it->is_array())
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            for (const auto& item : *chips_it) {
                if (!item.is_object())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const auto id_it = item.find("id");
                const auto sel_it = item.find("selected");
                if (id_it == item.end() || !id_it->is_number_integer())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                if (sel_it == item.end() || !sel_it->is_boolean())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const int32_t chip_id = id_it->get<int32_t>();
                const bool selected = sel_it->get<bool>();
                for (auto& chip : state->chips) {
                    if (chip.chip_id == chip_id) {
                        chip.selected = selected;
                        break;
                    }
                }
            }
        }
        const auto sw_it = props.find("search_width");
        if (sw_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*sw_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.search_box_width_px = v > 0 ? v : kDefaultSearchWidth;
        }
        const auto ch_it = props.find("chip_height");
        if (ch_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*ch_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.chip_height_px = v > 0 ? v : kDefaultChipHeight;
        }
        const auto cg_it = props.find("chip_gap");
        if (cg_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*cg_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.chip_gap_px = v > 0 ? v : kDefaultChipGap;
        }
        const auto fs_it = props.find("font_size");
        if (fs_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*fs_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.font_size_px = v > 0 ? v : kDefaultFontSize;
        }
        const auto px_it = props.find("pad_x");
        if (px_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*px_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.pad_x_px = v > 0 ? v : kDefaultPadX;
        }
        const auto py_it = props.find("pad_y");
        if (py_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*py_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.pad_y_px = v > 0 ? v : kDefaultPadY;
        }
        const auto r_it = props.find("radius");
        if (r_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*r_it, &v) || v < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.radius_px = v > 0 ? v : kDefaultRadius;
        }
        const auto resolve_color = [&](const char* key, uint32_t* target) -> bool {
            const auto it = props.find(key);
            if (it == props.end()) return true;
            uint32_t v = 0;
            if (!sao::ui::detail::widget_props_argb(*it, &v))
                return false;
            *target = v;
            return true;
        };
        if (!resolve_color("bg", &state->spec.bg_argb) ||
            !resolve_color("fg", &state->spec.fg_argb) ||
            !resolve_color("border", &state->spec.border_argb) ||
            !resolve_color("chip_bg", &state->spec.chip_bg_argb) ||
            !resolve_color("chip_selected", &state->spec.chip_selected_argb) ||
            !resolve_color("chip_fg", &state->spec.chip_fg_argb) ||
            !resolve_color("chip_selected_fg", &state->spec.chip_selected_fg_argb))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto theme_it = props.find("theme");
        if (theme_it != props.end()) {
            int32_t v = 0;
            if (!sao::ui::detail::widget_props_i32(*theme_it, &v) ||
                v < 0 || v >= SAO_UI_THEME_COUNT)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.theme_override = static_cast<SaoUiThemeId>(v);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_state_json(
    sao_ui_widget_handle_t handle, char* out_json_utf8, size_t capacity,
    size_t* out_bytes_written) {
    if (out_bytes_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    nlohmann::json doc;
    doc["query"] = state->query;
    nlohmann::json chips_arr = nlohmann::json::array();
    for (const auto& chip : state->chips) {
        nlohmann::json c;
        c["id"] = chip.chip_id;
        c["selected"] = chip.selected;
        chips_arr.push_back(std::move(c));
    }
    doc["chips"] = std::move(chips_arr);
    const std::string bytes = doc.dump();
    *out_bytes_written = bytes.size();
    if (out_json_utf8 == nullptr || capacity <= bytes.size())
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out_json_utf8, bytes.c_str(), bytes.size() + 1);
    return SAO_STATUS_OK;
}

// ── Typed-family hooks (called from widget_data.cpp dispatch) ─────
namespace sao::ui::detail {

sao_status_t widget_filter_row_apply_props(sao_ui_widget_handle_t handle,
                                           const WidgetPropsJson& props,
                                           WidgetPropsSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    try {
        auto snapshot = std::make_shared<FilterRowPropsSnapshot>();
        auto state = as_filter_row(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            snapshot->query = state->query;
            for (const auto& chip : state->chips)
                snapshot->chip_selection.emplace_back(chip.chip_id, chip.selected);
        }
        const std::string bytes = props.dump();
        const sao_status_t status = sao_ui_filter_row_apply_props(
            handle, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (status != SAO_STATUS_OK)
            return status;
        *out_snapshot = std::move(snapshot);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t widget_filter_row_restore_props(sao_ui_widget_handle_t handle,
                                             const WidgetPropsSnapshot& snapshot) noexcept {
    const auto previous = std::static_pointer_cast<FilterRowPropsSnapshot>(snapshot);
    if (previous == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->query = previous->query;
    for (const auto& [id, selected] : previous->chip_selection) {
        for (auto& chip : state->chips) {
            if (chip.chip_id == id) {
                chip.selected = selected;
                break;
            }
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t widget_filter_row_paint(sao_ui_widget_handle_t handle,
                                      sao_ui_paint_ctx_handle_t context,
                                      int32_t x, int32_t y,
                                      int32_t width, int32_t height) noexcept {
    try {
        auto state = as_filter_row(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        SaoUiFilterRowSpec spec{};
        std::string placeholder;
        std::string query;
        std::vector<OwnedChip> chips;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            spec = state->spec;
            placeholder = state->placeholder;
            query = state->query;
            chips = state->chips;
        }
        const uint32_t bg = resolve_or(spec.bg_argb, SAO_UI_TOKEN_APP_CARD);
        const uint32_t fg = resolve_or(spec.fg_argb, SAO_UI_TOKEN_APP_TEXT);
        const uint32_t border = resolve_or(spec.border_argb, SAO_UI_TOKEN_APP_BORDER);
        const float radius = static_cast<float>(spec.radius_px);
        const float xf = static_cast<float>(x);
        const float yf = static_cast<float>(y);
        const float wf = static_cast<float>(width);
        const float hf = static_cast<float>(height);

        // Row background pill.
        sao_status_t status = paint_rounded_rect(context, xf, yf, wf, hf, radius, border);
        if (status != SAO_STATUS_OK)
            return status;
        status = paint_rounded_rect(context, xf + 1.0F, yf + 1.0F, wf - 2.0F, hf - 2.0F,
                                    std::max(0.0F, radius - 1.0F), bg);
        if (status != SAO_STATUS_OK)
            return status;

        // Search-box (left portion) — draw query or placeholder text.
        const float pad_x = static_cast<float>(spec.pad_x_px);
        const float pad_y = static_cast<float>(spec.pad_y_px);
        const float search_w = static_cast<float>(spec.search_box_width_px);
        const float font_size = static_cast<float>(spec.font_size_px);
        const float text_y = yf + pad_y + 2.0F;
        if (!query.empty()) {
            status = sao_ui_paint_ctx_draw_utf8(context, xf + pad_x, text_y, query.c_str(),
                                                font_size, fg);
        } else if (!placeholder.empty()) {
            status = sao_ui_paint_ctx_draw_utf8(context, xf + pad_x, text_y, placeholder.c_str(),
                                                font_size,
                                                resolve_or(0, SAO_UI_TOKEN_APP_TEXT_DIM));
        } else {
            status = SAO_STATUS_OK;
        }
        if (status != SAO_STATUS_OK)
            return status;

        // Chips (right of search box).
        const uint32_t chip_bg = resolve_or(spec.chip_bg_argb, SAO_UI_TOKEN_APP_BG);
        const uint32_t chip_sel = resolve_or(spec.chip_selected_argb, SAO_UI_TOKEN_APP_ACCENT);
        const uint32_t chip_fg = resolve_or(spec.chip_fg_argb, SAO_UI_TOKEN_APP_TEXT);
        const uint32_t chip_sel_fg = resolve_or(spec.chip_selected_fg_argb, SAO_UI_TOKEN_WHITE);
        const float chip_h = static_cast<float>(spec.chip_height_px);
        const float gap = static_cast<float>(spec.chip_gap_px);
        float chip_x = xf + search_w + gap;
        const float chip_y = yf + (hf - chip_h) * 0.5F;
        for (const auto& chip : chips) {
            const uint32_t fill = chip.selected ? chip_sel : chip_bg;
            const uint32_t c_fg = chip.selected ? chip_sel_fg : chip_fg;
            // Estimate chip width from label length (rough: 6px/char + padding).
            const float chip_w = static_cast<float>(chip.label.size()) * 6.0F +
                                  static_cast<float>(spec.pad_x_px) * 2.0F;
            status = paint_rounded_rect(context, chip_x, chip_y, chip_w, chip_h,
                                         std::min(radius, chip_h * 0.5F), fill);
            if (status != SAO_STATUS_OK)
                return status;
            status = sao_ui_paint_ctx_draw_utf8(
                context, chip_x + static_cast<float>(spec.pad_x_px),
                chip_y + (chip_h - font_size) * 0.5F, chip.label.c_str(), font_size, c_fg);
            if (status != SAO_STATUS_OK)
                return status;
            chip_x += chip_w + gap;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

}  // namespace sao::ui::detail