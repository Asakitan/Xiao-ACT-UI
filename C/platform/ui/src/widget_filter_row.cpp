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
#include <unordered_set>

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
constexpr size_t kMaxFilterChips = 4096;

uint32_t resolve_or(uint32_t override_value, SaoUiColorToken token) {
    return override_value != 0 && !sao::ui::detail::panel_theme_high_contrast()
               ? override_value
               : sao::ui::detail::panel_theme_color(token);
}

struct OwnedChip {
    std::string label;
    int32_t chip_id{-1};
    bool selected{false};

    bool operator==(const OwnedChip&) const = default;
};

struct FilterRowState {
    int32_t tag{kFilterRowTag};
    SaoUiFilterRowSpec spec{};
    std::string placeholder;
    std::string query;
    std::vector<OwnedChip> chips;
    mutable std::mutex mtx;
};


bool build_owned_chips(const SaoUiFilterChipSpec* chips, size_t count,
                       std::vector<OwnedChip>* out) {
    if (out == nullptr || (chips == nullptr && count != 0) || count > kMaxFilterChips)
        return false;
    std::unordered_set<int32_t> ids;
    out->clear();
    out->reserve(count);
    for (size_t index = 0; index < count; ++index) {
        if (chips[index].label_utf8 == nullptr ||
            !ids.insert(chips[index].chip_id).second)
            return false;
        out->push_back({chips[index].label_utf8, chips[index].chip_id, chips[index].selected});
    }
    return true;
}

bool same_spec(const SaoUiFilterRowSpec& left, const SaoUiFilterRowSpec& right) {
    return left.search_box_width_px == right.search_box_width_px &&
           left.chip_height_px == right.chip_height_px &&
           left.chip_gap_px == right.chip_gap_px && left.font_size_px == right.font_size_px &&
           left.pad_x_px == right.pad_x_px && left.pad_y_px == right.pad_y_px &&
           left.radius_px == right.radius_px && left.bg_argb == right.bg_argb &&
           left.fg_argb == right.fg_argb && left.border_argb == right.border_argb &&
           left.chip_bg_argb == right.chip_bg_argb &&
           left.chip_selected_argb == right.chip_selected_argb &&
           left.chip_fg_argb == right.chip_fg_argb &&
           left.chip_selected_fg_argb == right.chip_selected_fg_argb &&
           left.theme_override == right.theme_override;
}

sao_status_t emit_value_changed(sao_ui_widget_handle_t handle, const nlohmann::json& payload) {
    const std::string bytes = payload.dump();
    return sao_ui_widget_dispatch_event(handle, SAO_UI_EVT_VALUE_CHANGED,
                                        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

struct FilterRowPropsSnapshot {
    SaoUiFilterRowSpec spec{};
    std::string placeholder;
    std::string query;
    std::vector<OwnedChip> chips;
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
        if (spec->theme_override < 0 || spec->theme_override > SAO_UI_THEME_COUNT)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::vector<OwnedChip> owned_chips;
        if (!build_owned_chips(spec->chips, spec->chip_count, &owned_chips))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        state->spec = *spec;
        state->spec.chips = nullptr;
        state->spec.chip_count = 0;
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
        state->chips = std::move(owned_chips);
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
    const std::string next = text_utf8 ? text_utf8 : "";
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        changed = state->query != next;
        state->query = next;
    }
    return changed ? emit_value_changed(handle, nlohmann::json{{"query", next}})
                   : SAO_STATUS_OK;
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
    bool selected = false;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        for (auto& chip : state->chips) {
            if (chip.chip_id == chip_id) {
                chip.selected = !chip.selected;
                selected = chip.selected;
                goto changed;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    }
changed:
    return emit_value_changed(handle, nlohmann::json{{"chip_id", chip_id}, {"selected", selected}});
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_filter_row_set_chip_selected(
    sao_ui_widget_handle_t handle, int32_t chip_id, bool selected) {
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        for (auto& chip : state->chips) {
            if (chip.chip_id == chip_id) {
                changed = chip.selected != selected;
                chip.selected = selected;
                goto changed;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    }
changed:
    return changed ? emit_value_changed(handle, nlohmann::json{{"chip_id", chip_id}, {"selected", selected}})
                   : SAO_STATUS_OK;
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
    if ((selected_ids == nullptr && selected_count != 0) || selected_count == 0) {
        if (selected_ids == nullptr && selected_count != 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
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
    if (handle == nullptr || (props_json_utf8 == nullptr && props_len != 0) || props_len == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        const nlohmann::json props = nlohmann::json::parse(
            props_json_utf8, props_json_utf8 + props_len, nullptr, false);
        if (!props.is_object() || !sao::ui::detail::widget_props_has_only(
                props, {"query", "chips", "placeholder", "search_width", "chip_height",
                        "chip_gap", "font_size", "pad_x", "pad_y", "radius", "bg", "fg",
                        "border", "chip_bg", "chip_selected", "chip_fg", "chip_selected_fg",
                        "theme"}))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto state = as_filter_row(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;

        SaoUiFilterRowSpec next_spec{};
        std::string next_placeholder;
        std::string next_query;
        std::vector<OwnedChip> next_chips;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            next_spec = state->spec;
            next_placeholder = state->placeholder;
            next_query = state->query;
            next_chips = state->chips;
        }
        next_spec.chips = nullptr;
        next_spec.chip_count = 0;

        if (const auto it = props.find("query"); it != props.end()) {
            if (!it->is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            next_query = it->get<std::string>();
        }
        if (const auto it = props.find("placeholder"); it != props.end()) {
            if (!it->is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            next_placeholder = it->get<std::string>();
        }
        if (const auto it = props.find("chips"); it != props.end()) {
            if (!it->is_array()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            std::unordered_set<int32_t> seen;
            for (const auto& item : *it) {
                if (!item.is_object() || !sao::ui::detail::widget_props_has_only(item, {"id", "selected"}))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const auto id_it = item.find("id");
                const auto selected_it = item.find("selected");
                int32_t id = 0;
                if (id_it == item.end() || !sao::ui::detail::widget_props_i32(*id_it, &id) ||
                    selected_it == item.end() || !selected_it->is_boolean() ||
                    !seen.insert(id).second)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                for (auto& chip : next_chips)
                    if (chip.chip_id == id)
                        chip.selected = selected_it->get<bool>();
            }
        }
        auto parse_nonnegative = [&](const char* key, int32_t* target, int32_t fallback) {
            const auto it = props.find(key);
            if (it == props.end()) return true;
            int32_t value = 0;
            if (!sao::ui::detail::widget_props_i32(*it, &value) || value < 0)
                return false;
            *target = value > 0 ? value : fallback;
            return true;
        };
        if (!parse_nonnegative("search_width", &next_spec.search_box_width_px, kDefaultSearchWidth) ||
            !parse_nonnegative("chip_height", &next_spec.chip_height_px, kDefaultChipHeight) ||
            !parse_nonnegative("chip_gap", &next_spec.chip_gap_px, kDefaultChipGap) ||
            !parse_nonnegative("font_size", &next_spec.font_size_px, kDefaultFontSize) ||
            !parse_nonnegative("pad_x", &next_spec.pad_x_px, kDefaultPadX) ||
            !parse_nonnegative("pad_y", &next_spec.pad_y_px, kDefaultPadY) ||
            !parse_nonnegative("radius", &next_spec.radius_px, kDefaultRadius))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto parse_color = [&](const char* key, uint32_t* target) {
            const auto it = props.find(key);
            if (it == props.end()) return true;
            uint32_t value = 0;
            if (!sao::ui::detail::widget_props_argb(*it, &value)) return false;
            *target = value;
            return true;
        };
        if (!parse_color("bg", &next_spec.bg_argb) || !parse_color("fg", &next_spec.fg_argb) ||
            !parse_color("border", &next_spec.border_argb) ||
            !parse_color("chip_bg", &next_spec.chip_bg_argb) ||
            !parse_color("chip_selected", &next_spec.chip_selected_argb) ||
            !parse_color("chip_fg", &next_spec.chip_fg_argb) ||
            !parse_color("chip_selected_fg", &next_spec.chip_selected_fg_argb))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (const auto it = props.find("theme"); it != props.end()) {
            int32_t value = 0;
            if (!sao::ui::detail::widget_props_i32(*it, &value) || value < 0 ||
                value > SAO_UI_THEME_COUNT)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            next_spec.theme_override = static_cast<SaoUiThemeId>(value);
        }

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            changed = !same_spec(state->spec, next_spec) || state->placeholder != next_placeholder ||
                      state->query != next_query || state->chips != next_chips;
            state->spec = next_spec;
            state->spec.chips = nullptr;
            state->spec.chip_count = 0;
            state->placeholder = std::move(next_placeholder);
            state->query = std::move(next_query);
            state->chips = std::move(next_chips);
        }
        return changed ? emit_value_changed(handle, props) : SAO_STATUS_OK;
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

sao_status_t widget_filter_row_apply_props(
    sao_ui_widget_handle_t handle, const WidgetPropsJson& props,
    WidgetPropsSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    try {
        auto state = as_filter_row(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        auto snapshot = std::make_shared<FilterRowPropsSnapshot>();
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            snapshot->spec = state->spec;
            snapshot->spec.chips = nullptr;
            snapshot->spec.chip_count = 0;
            snapshot->placeholder = state->placeholder;
            snapshot->query = state->query;
            snapshot->chips = state->chips;
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

sao_status_t widget_filter_row_restore_props(
    sao_ui_widget_handle_t handle, const WidgetPropsSnapshot& snapshot) noexcept {
    const auto previous = std::static_pointer_cast<FilterRowPropsSnapshot>(snapshot);
    if (previous == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_filter_row(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec = previous->spec;
    state->spec.chips = nullptr;
    state->spec.chip_count = 0;
    state->placeholder = previous->placeholder;
    state->query = previous->query;
    state->chips = previous->chips;
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
                                                resolve_or(0, SAO_UI_TOKEN_PLACEHOLDER));
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
        size_t visible = 0;
        size_t dropped = 0;
        const float row_right = xf + wf - gap;
        for (const auto& chip : chips) {
            const uint32_t fill = chip.selected ? chip_sel : chip_bg;
            const uint32_t c_fg = chip.selected ? chip_sel_fg : chip_fg;
            // Estimate chip width from label length (rough: 6px/char + padding).
            const float chip_w = static_cast<float>(chip.label.size()) * 6.0F +
                                  static_cast<float>(spec.pad_x_px) * 2.0F;
            if (chip_x + chip_w > row_right)
                break;
            status = paint_rounded_rect(context, chip_x, chip_y, chip_w, chip_h,
                                         std::min(radius, chip_h * 0.5F), fill);
            if (status != SAO_STATUS_OK)
                return status;
            // Clip label to the chip's interior so long UTF-8 labels
            // never overpaint the next chip.
            const float label_space = std::max(0.0F, chip_w - static_cast<float>(spec.pad_x_px) * 2.0F);
            const std::string clipped_label = label_space >=
                    static_cast<float>(chip.label.size()) * 6.0F
                ? chip.label
                : chip.label.substr(0, static_cast<size_t>(std::max(0.0F, label_space / 6.0F)));
            status = sao_ui_paint_ctx_draw_utf8(
                context, chip_x + static_cast<float>(spec.pad_x_px),
                chip_y + (chip_h - font_size) * 0.5F, clipped_label.c_str(), font_size, c_fg);
            if (status != SAO_STATUS_OK)
                return status;
            chip_x += chip_w + gap;
            ++visible;
        }
        dropped = chips.size() - visible;
        if (dropped > 0) {
            const std::string folded = "+" + std::to_string(dropped);
            const float fold_w = static_cast<float>(folded.size()) * 6.0F + 16.0F;
            if (chip_x + fold_w <= row_right) {
                status = paint_rounded_rect(context, chip_x, chip_y, fold_w, chip_h,
                                             std::min(radius, chip_h * 0.5F), chip_bg);
                if (status != SAO_STATUS_OK)
                    return status;
                status = sao_ui_paint_ctx_draw_utf8(
                    context, chip_x + 8.0F, chip_y + (chip_h - font_size) * 0.5F,
                    folded.c_str(), font_size, chip_fg);
                if (status != SAO_STATUS_OK)
                    return status;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

}  // namespace sao::ui::detail