// SAO Auto — generic compositor-backed popup menu.

#include "sao/ui/popup.h"
#include "sao/ui/d2d_effects.h"
#include "sao/ui/d2d_widgets.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct EntryNode {
    std::string label;
    std::string icon;
    std::string accelerator;
    int32_t     entry_id = -1;
    bool        enabled = true;
    bool        checked = false;
    bool        is_separator = false;
    std::vector<EntryNode> children;
};

struct LevelRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<int32_t> row_top;
    std::vector<int32_t> row_bottom;
};

struct OpenLevel {
    int32_t parent_entry_index = -1;
    int32_t selected_index = -1;
    LevelRect rect;
};

struct ScreenBounds {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct HitTarget {
    int32_t depth = -1;
    int32_t index = -1;
    int32_t entry_id = -1;
    bool inside_level = false;
};

struct VisualRow {
    std::string label;
    std::string icon;
    std::string accelerator;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    bool has_submenu = false;
};

struct VisualLevel {
    LevelRect rect;
    int32_t selected_index = -1;
    std::vector<VisualRow> rows;
};

struct PopupPalette {
    uint32_t shadow = 0;
    uint32_t surface = 0;
    uint32_t surface_tint = 0;
    uint32_t cyan = 0;
    uint32_t cyan_soft = 0;
    uint32_t gold = 0;
    uint32_t gold_soft = 0;
    uint32_t text = 0;
    uint32_t text_secondary = 0;
    uint32_t text_disabled = 0;
    uint32_t selected = 0;
    uint32_t selected_disabled = 0;
    uint32_t row_divider = 0;
};

struct VisualSnapshot {
    PopupPalette palette;
    std::vector<VisualLevel> levels;
};

struct PopupFrame {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    ScreenBounds screen_bounds;
    std::vector<SaoUiLayerInputRect> input_rects;
};

struct PopupInputBinding {
    std::mutex mutex;
    sao_ui_popup_s* popup = nullptr;
};

struct PendingResult {
    sao_ui_popup_result_callback_t callback = nullptr;
    void* user_data = nullptr;
    int32_t entry_id = -1;
    bool dismissed = false;
    int32_t screen_x = -1;
    int32_t screen_y = -1;
};

constexpr int32_t kRowHeight = 28;
constexpr int32_t kSeparatorHeight = 8;
constexpr int32_t kEmptyPanelHeight = 24;
constexpr int32_t kVisualPadding = 6;
constexpr int32_t kShadowOffset = 3;
constexpr int32_t kPopupZOrder = 1'000'000;

std::atomic<uint64_t> g_popup_layer_sequence{0};

constexpr bool valid_theme_override(SaoUiThemeId theme_id) noexcept {
    const int32_t value = static_cast<int32_t>(theme_id);
    return value >= 0 && value <= SAO_UI_THEME_COUNT;
}

constexpr uint32_t with_alpha(uint32_t argb, uint8_t alpha) noexcept {
    return (static_cast<uint32_t>(alpha) << 24U) | (argb & 0x00FFFFFFU);
}

constexpr uint32_t capped_alpha(uint32_t argb, uint8_t maximum) noexcept {
    const uint8_t alpha = static_cast<uint8_t>(argb >> 24U);
    return with_alpha(argb, std::min(alpha, maximum));
}

PopupPalette make_popup_palette(SaoUiThemeId theme_id) noexcept {
    const auto color = [theme_id](SaoUiColorToken token) {
        return sao_ui_theme_resolve_color(theme_id, token);
    };
    return {
        capped_alpha(color(SAO_UI_TOKEN_BLACK), 0x66U),
        capped_alpha(color(SAO_UI_TOKEN_APP_BG), 0xD2U),
        capped_alpha(color(SAO_UI_TOKEN_APP_BORDER), 0x35U),
        color(SAO_UI_TOKEN_CORNER_CYAN),
        capped_alpha(color(SAO_UI_TOKEN_ACCENT_CYAN_SOFT), 0xB0U),
        color(SAO_UI_TOKEN_CIRCLE_ACTIVE_BORDER),
        capped_alpha(color(SAO_UI_TOKEN_ACCENT_GOLD_WARM), 0xA0U),
        color(SAO_UI_TOKEN_APP_TEXT),
        capped_alpha(color(SAO_UI_TOKEN_APP_TEXT_2), 0xCCU),
        capped_alpha(color(SAO_UI_TOKEN_APP_TEXT_DIM), 0x88U),
        capped_alpha(color(SAO_UI_TOKEN_APP_ACCENT), 0x5AU),
        capped_alpha(color(SAO_UI_TOKEN_APP_BORDER), 0x30U),
        capped_alpha(color(SAO_UI_TOKEN_APP_BORDER), 0x30U),
    };
}

std::mutex& binding_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<PopupInputBinding*, std::shared_ptr<PopupInputBinding>>&
binding_registry() {
    static std::unordered_map<PopupInputBinding*, std::shared_ptr<PopupInputBinding>> registry;
    return registry;
}

bool register_binding(const std::shared_ptr<PopupInputBinding>& binding) noexcept {
    try {
        std::lock_guard lock(binding_registry_mutex());
        binding_registry().emplace(binding.get(), binding);
        return true;
    } catch (...) {
        return false;
    }
}

void unregister_binding(PopupInputBinding* binding) noexcept {
    try {
        std::lock_guard lock(binding_registry_mutex());
        binding_registry().erase(binding);
    } catch (...) {
    }
}

std::shared_ptr<PopupInputBinding> acquire_binding(void* user_data) noexcept {
    if (user_data == nullptr) return {};
    try {
        auto* binding = static_cast<PopupInputBinding*>(user_data);
        std::lock_guard lock(binding_registry_mutex());
        const auto found = binding_registry().find(binding);
        return found == binding_registry().end() ? std::shared_ptr<PopupInputBinding>{}
                                                  : found->second;
    } catch (...) {
        return {};
    }
}

struct RasterDeleter {
    void operator()(sao_ui_offscreen_raster_s* raster) const noexcept {
        sao_ui_offscreen_raster_destroy(raster);
    }
};

struct PaintContextDeleter {
    void operator()(sao_ui_paint_ctx_s* context) const noexcept {
        sao_ui_paint_ctx_destroy(context);
    }
};

class Painter {
  public:
    explicit Painter(sao_ui_paint_ctx_handle_t context) : context_(context) {}

    void fill(float x, float y, float width, float height, uint32_t color) {
        merge(sao_ui_paint_ctx_fill_rect(context_, x, y, width, height, color));
    }

    void line(float x1, float y1, float x2, float y2, float width, uint32_t color) {
        merge(sao_ui_paint_ctx_stroke_line(context_, x1, y1, x2, y2, width, color));
    }

    void polygon(const int32_t* points, size_t count, uint32_t color) {
        merge(sao_ui_paint_ctx_fill_polygon(context_, points, count, color));
    }

    void text(float x, float y, const std::string& value, float size, uint32_t color) {
        if (!value.empty())
            merge(sao_ui_paint_ctx_draw_utf8(context_, x, y, value.c_str(), size, color));
    }

    void clipped_text(float x, float y, float width, float height,
                      const std::string& value, float size, uint32_t color) {
        if (value.empty() || width <= 0.0F || height <= 0.0F || status_ != SAO_STATUS_OK) return;
        const sao_status_t push = sao_ui_paint_ctx_push_clip(context_, x, y, width, height);
        if (push != SAO_STATUS_OK) {
            merge(push);
            return;
        }
        const sao_status_t draw = sao_ui_paint_ctx_draw_utf8(
            context_, x, y, value.c_str(), size, color);
        const sao_status_t pop = sao_ui_paint_ctx_pop_clip(context_);
        merge(draw);
        merge(pop);
    }

    sao_status_t status() const noexcept { return status_; }

  private:
    void merge(sao_status_t status) noexcept {
        if (status_ == SAO_STATUS_OK && status != SAO_STATUS_OK) status_ = status;
    }

    sao_ui_paint_ctx_handle_t context_ = nullptr;
    sao_status_t status_ = SAO_STATUS_OK;
};

}  // namespace

struct sao_ui_popup_s {
    std::mutex mu;
    std::mutex layer_mu;

    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_theme_handle_t theme = nullptr;

    bool visible = false;
    SaoUiPopupSpec spec_snapshot {};
    EntryNode root;
    sao_ui_popup_result_callback_t callback = nullptr;
    void* user_data = nullptr;
    std::vector<OpenLevel> levels;

    sao_ui_layer_handle_t layer = nullptr;
    ScreenBounds layer_screen_bounds {};
    bool layer_bounds_valid = false;
    std::shared_ptr<PopupInputBinding> input_binding;
};

namespace {

static void deep_copy_entries(
    const SaoUiPopupEntry* entries, size_t count,
    std::vector<EntryNode>* out) {
    out->clear();
    if (entries == nullptr || count == 0) return;
    out->reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const SaoUiPopupEntry& src = entries[i];
        EntryNode node;
        node.label = (src.label_utf8 != nullptr) ? src.label_utf8 : "";
        node.icon = (src.icon_utf8 != nullptr) ? src.icon_utf8 : "";
        node.accelerator = (src.accelerator_utf8 != nullptr) ? src.accelerator_utf8 : "";
        node.entry_id = src.entry_id;
        node.enabled = src.enabled;
        node.checked = src.checked;
        node.is_separator = src.is_separator;
        if (src.submenu_entries != nullptr && src.submenu_count > 0) {
            deep_copy_entries(src.submenu_entries, src.submenu_count, &node.children);
        }
        out->push_back(std::move(node));
    }
}

static bool compute_level_rect(
    const std::vector<EntryNode>& entries,
    int32_t anchor_x, int32_t anchor_y,
    int32_t width,
    LevelRect* out) {
    if (out == nullptr || width <= 0) return false;
    out->x = anchor_x;
    out->y = anchor_y;
    out->width = width;
    out->row_top.clear();
    out->row_bottom.clear();
    out->row_top.reserve(entries.size());
    out->row_bottom.reserve(entries.size());
    int64_t cursor_y = anchor_y;
    for (const EntryNode& e : entries) {
        const int32_t height = e.is_separator ? kSeparatorHeight : kRowHeight;
        const int64_t bottom = cursor_y + height;
        if (cursor_y < std::numeric_limits<int32_t>::min() ||
            bottom > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        out->row_top.push_back(static_cast<int32_t>(cursor_y));
        out->row_bottom.push_back(static_cast<int32_t>(bottom));
        cursor_y = bottom;
    }
    const int64_t total_height = cursor_y - anchor_y;
    if (total_height < 0 || total_height > std::numeric_limits<int32_t>::max()) return false;
    out->height = static_cast<int32_t>(total_height);
    return true;
}

static const std::vector<EntryNode>& level_entries(
    const sao_ui_popup_s* popup, int32_t level_idx) {
    const EntryNode* node = &popup->root;
    for (int32_t i = 1; i <= level_idx; ++i) {
        int32_t parent_idx = popup->levels[static_cast<size_t>(i)].parent_entry_index;
        node = &node->children[static_cast<size_t>(parent_idx)];
    }
    return node->children;
}

static bool is_selectable(const EntryNode& entry) noexcept {
    return entry.enabled && !entry.is_separator;
}

static int32_t advance_selection(
    const std::vector<EntryNode>& entries, int32_t current, int32_t direction) {
    if (entries.empty()) return -1;
    const int32_t n = static_cast<int32_t>(entries.size());
    int32_t start = current;
    if (start < 0) start = (direction > 0) ? -1 : n;
    for (int32_t step = 0; step < n; ++step) {
        int32_t candidate = start + direction * (step + 1);
        candidate = ((candidate % n) + n) % n;
        const EntryNode& e = entries[static_cast<size_t>(candidate)];
        if (is_selectable(e)) {
            return candidate;
        }
    }
    return -1;
}

static sao_status_t make_root_level(
    const std::vector<EntryNode>& entries,
    const SaoUiPopupSpec& spec,
    OpenLevel* out_level) {
    if (out_level == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto* constants = sao_ui_popup_layout_constants();
    const int32_t width = spec.anchor_w > 0 ? spec.anchor_w : constants->child_width;
    OpenLevel level;
    if (!compute_level_rect(entries, spec.anchor_x, spec.anchor_y, width, &level.rect))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    level.parent_entry_index = -1;
    level.selected_index = spec.allow_keyboard_nav
        ? advance_selection(entries, -1, +1) : -1;
    *out_level = std::move(level);
    return SAO_STATUS_OK;
}

static sao_status_t open_submenu_locked(
    sao_ui_popup_s* popup, int32_t depth, int32_t entry_index) {
    const auto& entries = level_entries(popup, depth);
    if (entry_index < 0 || entry_index >= static_cast<int32_t>(entries.size()))
        return SAO_STATUS_ERR_NOT_FOUND;
    const EntryNode& entry = entries[static_cast<size_t>(entry_index)];
    if (!entry.enabled || entry.is_separator || entry.children.empty())
        return SAO_STATUS_ERR_NOT_FOUND;

    const size_t child_level_index = static_cast<size_t>(depth + 1);
    if (popup->levels.size() > child_level_index &&
        popup->levels[child_level_index].parent_entry_index == entry_index) {
        popup->levels.resize(child_level_index + 1);
        return SAO_STATUS_OK;
    }
    popup->levels.resize(child_level_index);

    const OpenLevel& parent = popup->levels[static_cast<size_t>(depth)];
    const auto* constants = sao_ui_popup_layout_constants();
    const int64_t child_x = static_cast<int64_t>(parent.rect.x) +
        parent.rect.width + constants->gap_menu_child;
    if (child_x < std::numeric_limits<int32_t>::min() ||
        child_x > std::numeric_limits<int32_t>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    OpenLevel child;
    child.parent_entry_index = entry_index;
    const int32_t child_y = parent.rect.row_top[static_cast<size_t>(entry_index)];
    if (!compute_level_rect(entry.children, static_cast<int32_t>(child_x), child_y,
                            constants->child_width, &child.rect)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    child.selected_index = popup->spec_snapshot.allow_keyboard_nav
        ? advance_selection(entry.children, -1, +1) : -1;
    popup->levels.push_back(std::move(child));
    return SAO_STATUS_OK;
}

static HitTarget hit_target_locked(const sao_ui_popup_s* popup, int32_t x, int32_t y) {
    HitTarget hit;
    for (int32_t depth = static_cast<int32_t>(popup->levels.size()) - 1;
         depth >= 0; --depth) {
        const OpenLevel& level = popup->levels[static_cast<size_t>(depth)];
        const LevelRect& rect = level.rect;
        if (x < rect.x || x >= rect.x + rect.width ||
            y < rect.y || y >= rect.y + rect.height) {
            continue;
        }
        hit.depth = depth;
        hit.inside_level = true;
        const auto& entries = level_entries(popup, depth);
        for (size_t index = 0; index < entries.size(); ++index) {
            if (y >= rect.row_top[index] && y < rect.row_bottom[index]) {
                hit.index = static_cast<int32_t>(index);
                if (!entries[index].is_separator) hit.entry_id = entries[index].entry_id;
                return hit;
            }
        }
        return hit;
    }
    return hit;
}

static sao_status_t resolve_effective_theme_locked(
    const sao_ui_popup_s* popup, SaoUiThemeId* out_theme_id) {
    if (out_theme_id == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SaoUiThemeId theme_id = popup->spec_snapshot.theme_override;
    if (theme_id == SAO_UI_THEME_COUNT) {
        const sao_status_t status = popup->theme != nullptr
            ? sao_ui_theme_get_active(popup->theme, &theme_id)
            : sao_ui_theme_get_active_id(&theme_id);
        if (status != SAO_STATUS_OK) return status;
    }
    if (static_cast<int32_t>(theme_id) < 0 || theme_id >= SAO_UI_THEME_COUNT)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_theme_id = theme_id;
    return SAO_STATUS_OK;
}

static sao_status_t build_visual_snapshot_locked(
    const sao_ui_popup_s* popup, VisualSnapshot* out_snapshot) {
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    const sao_status_t theme_status = resolve_effective_theme_locked(popup, &theme_id);
    if (theme_status != SAO_STATUS_OK) return theme_status;
    out_snapshot->palette = make_popup_palette(theme_id);
    out_snapshot->levels.clear();
    out_snapshot->levels.reserve(popup->levels.size());
    for (int32_t depth = 0; depth < static_cast<int32_t>(popup->levels.size()); ++depth) {
        const OpenLevel& open_level = popup->levels[static_cast<size_t>(depth)];
        const auto& entries = level_entries(popup, depth);
        VisualLevel level;
        level.rect = open_level.rect;
        level.selected_index = open_level.selected_index;
        level.rows.reserve(entries.size());
        for (const EntryNode& entry : entries) {
            level.rows.push_back({entry.label, entry.icon, entry.accelerator,
                                  entry.enabled, entry.checked, entry.is_separator,
                                  !entry.children.empty()});
        }
        out_snapshot->levels.push_back(std::move(level));
    }
    return SAO_STATUS_OK;
}

static bool compute_screen_bounds(
    const VisualSnapshot& snapshot, ScreenBounds* out_bounds) {
    if (out_bounds == nullptr || snapshot.levels.empty()) return false;
    int64_t left = std::numeric_limits<int64_t>::max();
    int64_t top = std::numeric_limits<int64_t>::max();
    int64_t right = std::numeric_limits<int64_t>::min();
    int64_t bottom = std::numeric_limits<int64_t>::min();
    for (const VisualLevel& level : snapshot.levels) {
        const int32_t panel_height = std::max(level.rect.height, kEmptyPanelHeight);
        left = std::min(left, static_cast<int64_t>(level.rect.x));
        top = std::min(top, static_cast<int64_t>(level.rect.y));
        right = std::max(right, static_cast<int64_t>(level.rect.x) + level.rect.width);
        bottom = std::max(bottom, static_cast<int64_t>(level.rect.y) + panel_height);
    }
    left -= kVisualPadding;
    top -= kVisualPadding;
    right += kVisualPadding + kShadowOffset;
    bottom += kVisualPadding + kShadowOffset;
    const int64_t width = right - left;
    const int64_t height = bottom - top;
    if (left < std::numeric_limits<int32_t>::min() ||
        top < std::numeric_limits<int32_t>::min() ||
        left > std::numeric_limits<int32_t>::max() ||
        top > std::numeric_limits<int32_t>::max() ||
        width <= 0 || height <= 0 ||
        width > std::numeric_limits<int32_t>::max() ||
        height > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out_bounds = {static_cast<int32_t>(left), static_cast<int32_t>(top),
                   static_cast<int32_t>(width), static_cast<int32_t>(height)};
    return true;
}

static float text_width(const std::string& text, float size) {
    const int32_t scale = std::max(1, static_cast<int32_t>(std::floor(size / 5.0F)));
    return static_cast<float>(text.size() * static_cast<size_t>(scale * 6));
}

static void draw_level_border(
    Painter* painter, const PopupPalette& palette,
    float x, float y, float width, float height) {
    painter->fill(x + kShadowOffset, y + kShadowOffset, width, height, palette.shadow);
    painter->fill(x, y, width, height, palette.surface);
    painter->fill(x + 1.0F, y + 1.0F, width - 2.0F, 3.0F, palette.surface_tint);
    painter->fill(x, y, width, 1.0F, palette.cyan);
    painter->fill(x, y, 1.0F, height, palette.cyan_soft);
    painter->fill(x, y + height - 1.0F, width, 1.0F, palette.gold_soft);
    painter->fill(x + width - 1.0F, y, 1.0F, height, palette.gold);
    painter->fill(x + 5.0F, y + 4.0F, 20.0F, 1.0F, palette.cyan);
    painter->fill(x + width - 25.0F, y + height - 5.0F, 20.0F, 1.0F, palette.gold);
}

static void draw_separator(
    Painter* painter, const PopupPalette& palette,
    float x, float y, float width, float height) {
    const float center = y + height * 0.5F;
    const float half = std::max(1.0F, (width - 20.0F) * 0.5F);
    painter->fill(x + 10.0F, center, half, 1.0F, palette.cyan_soft);
    painter->fill(x + 10.0F + half, center, half, 1.0F, palette.gold_soft);
}

static void draw_check(Painter* painter, float x, float y, uint32_t color) {
    painter->line(x, y + 5.0F, x + 4.0F, y + 9.0F, 1.5F, color);
    painter->line(x + 4.0F, y + 9.0F, x + 11.0F, y + 1.0F, 1.5F, color);
}

static void draw_row(
    Painter* painter, const PopupPalette& palette,
    const VisualRow& row, bool selected,
    float x, float y, float width, float height) {
    if (row.separator) {
        draw_separator(painter, palette, x, y, width, height);
        return;
    }
    if (selected) {
        painter->fill(x + 2.0F, y, width - 4.0F, height,
                      row.enabled ? palette.selected : palette.selected_disabled);
        painter->fill(x + 2.0F, y, 3.0F, height,
                      row.enabled ? palette.cyan : palette.cyan_soft);
        painter->fill(x + width - 3.0F, y, 1.0F, height, palette.gold_soft);
    }
    painter->fill(x + 8.0F, y + height - 1.0F, width - 16.0F, 1.0F,
                  palette.row_divider);

    const uint32_t primary = row.enabled
        ? (selected ? palette.text : palette.text_secondary) : palette.text_disabled;
    const uint32_t accent = row.enabled
        ? (selected ? palette.gold : palette.cyan_soft) : palette.text_disabled;
    float icon_x = x + 9.0F;
    if (row.checked) {
        draw_check(painter, icon_x, y + (height - 11.0F) * 0.5F, accent);
        icon_x += 15.0F;
    }
    if (!row.icon.empty()) painter->text(icon_x, y + 8.0F, row.icon, 10.0F, accent);

    const float label_x = x + 31.0F;
    const float arrow_space = row.has_submenu ? 20.0F : 0.0F;
    const float accelerator_width = row.accelerator.empty()
        ? 0.0F : text_width(row.accelerator, 10.0F) + 9.0F;
    const float label_width = std::max(
        1.0F, width - (label_x - x) - 10.0F - arrow_space - accelerator_width);
    painter->clipped_text(label_x, y + 8.0F, label_width, height - 8.0F,
                          row.label, 10.0F, primary);
    if (!row.accelerator.empty()) {
        painter->text(x + width - 9.0F - arrow_space -
                          text_width(row.accelerator, 10.0F),
                      y + 8.0F, row.accelerator, 10.0F,
                      row.enabled ? palette.text_secondary : palette.text_disabled);
    }
    if (row.has_submenu) {
        const int32_t arrow_x = static_cast<int32_t>(std::lround(x + width - 12.0F));
        const int32_t arrow_y = static_cast<int32_t>(std::lround(y + height * 0.5F));
        const int32_t points[] = {
            arrow_x - 3, arrow_y - 5,
            arrow_x + 3, arrow_y,
            arrow_x - 3, arrow_y + 5,
        };
        painter->polygon(points, 3, accent);
    }
}

static sao_status_t render_popup_frame(
    const VisualSnapshot& snapshot, PopupFrame* out_frame) {
    if (out_frame == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        ScreenBounds bounds;
        if (!compute_screen_bounds(snapshot, &bounds)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        SaoUiOffscreenRasterDesc descriptor{};
        descriptor.width_px = static_cast<uint32_t>(bounds.width);
        descriptor.height_px = static_cast<uint32_t>(bounds.height);
        descriptor.clear_argb = 0;
        sao_ui_offscreen_raster_handle_t raw_raster = nullptr;
        sao_status_t status = sao_ui_offscreen_raster_create(&descriptor, &raw_raster);
        if (status != SAO_STATUS_OK) return status;
        std::unique_ptr<sao_ui_offscreen_raster_s, RasterDeleter> raster(raw_raster);

        sao_ui_paint_ctx_handle_t raw_context = nullptr;
        status = sao_ui_paint_ctx_create_offscreen(raster.get(), &raw_context);
        if (status != SAO_STATUS_OK) return status;
        std::unique_ptr<sao_ui_paint_ctx_s, PaintContextDeleter> context(raw_context);
        status = sao_ui_paint_ctx_begin_frame(context.get());
        if (status != SAO_STATUS_OK) return status;

        Painter painter(context.get());
        for (const VisualLevel& level : snapshot.levels) {
            const float x = static_cast<float>(level.rect.x - bounds.x);
            const float y = static_cast<float>(level.rect.y - bounds.y);
            const float width = static_cast<float>(level.rect.width);
            const float height = static_cast<float>(std::max(level.rect.height, kEmptyPanelHeight));
            draw_level_border(&painter, snapshot.palette, x, y, width, height);
            for (size_t index = 0; index < level.rows.size(); ++index) {
                const float row_y = static_cast<float>(level.rect.row_top[index] - bounds.y);
                const float row_height = static_cast<float>(
                    level.rect.row_bottom[index] - level.rect.row_top[index]);
                draw_row(&painter, snapshot.palette, level.rows[index],
                         level.selected_index == static_cast<int32_t>(index),
                         x, row_y, width, row_height);
            }
        }
        if (painter.status() != SAO_STATUS_OK) return painter.status();
        status = sao_ui_paint_ctx_end_frame(context.get());
        if (status != SAO_STATUS_OK) return status;

        PopupFrame frame;
        frame.width = descriptor.width_px;
        frame.height = descriptor.height_px;
        frame.stride = descriptor.width_px * 4U;
        frame.screen_bounds = bounds;
        frame.pixels.resize(static_cast<size_t>(frame.stride) * frame.height);
        size_t bytes_written = 0;
        uint32_t snapshot_width = 0;
        uint32_t snapshot_height = 0;
        uint32_t snapshot_stride = 0;
        status = sao_ui_offscreen_raster_snapshot(
            raster.get(), frame.pixels.data(), frame.pixels.size(), &bytes_written,
            &snapshot_width, &snapshot_height, &snapshot_stride);
        if (status != SAO_STATUS_OK) return status;
        if (bytes_written != frame.pixels.size() || snapshot_width != frame.width ||
            snapshot_height != frame.height || snapshot_stride != frame.stride) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        frame.input_rects.reserve(snapshot.levels.size());
        for (const VisualLevel& level : snapshot.levels) {
            if (level.rect.height <= 0) continue;
            frame.input_rects.push_back({
                level.rect.x - bounds.x,
                level.rect.y - bounds.y,
                level.rect.width,
                level.rect.height,
            });
        }
        *out_frame = std::move(frame);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

static sao_status_t resolve_layer_origin(
    sao_ui_compositor_handle_t compositor, const ScreenBounds& bounds,
    int32_t* out_x, int32_t* out_y) {
    if (out_x == nullptr || out_y == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    int64_t x = bounds.x;
    int64_t y = bounds.y;
    const sao_ui_overlay_host_handle_t host = sao_ui_compositor_host(compositor);
    if (host != nullptr) {
        SaoOverlayHostState state{};
        const sao_status_t status = sao_ui_overlay_host_get_state(host, &state);
        if (status != SAO_STATUS_OK) return status;
        // DPI-aware: subtract the host origin in desktop physical pixels
        // first (both bounds and geometry come from the same desktop space),
        // then scale the resulting host-local delta down to host logical
        // pixels. When the host reports 96 DPI this reduces to a plain
        // subtraction and cannot regress any caller on non-scaled monitors.
        const uint32_t dpi = state.dpi == 0u ? 96u : state.dpi;
        x -= state.geometry.x;
        y -= state.geometry.y;
        if (dpi != 96u) {
            x = x * static_cast<int64_t>(96) / static_cast<int64_t>(dpi);
            y = y * static_cast<int64_t>(96) / static_cast<int64_t>(dpi);
        }
    }
    if (x < std::numeric_limits<int32_t>::min() ||
        x > std::numeric_limits<int32_t>::max() ||
        y < std::numeric_limits<int32_t>::min() ||
        y > std::numeric_limits<int32_t>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_x = static_cast<int32_t>(x);
    *out_y = static_cast<int32_t>(y);
    return SAO_STATUS_OK;
}

static void destroy_layer_handle(sao_ui_layer_handle_t layer) noexcept {
    if (layer == nullptr) return;
    (void)sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr, nullptr);
    sao_ui_layer_destroy(layer);
}

static std::string next_layer_name() {
    const uint64_t sequence = g_popup_layer_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return "popup." + std::to_string(sequence);
}

static void SAO_UI_CALL popup_layer_cursor(float layer_x, float layer_y, void* user_data);
static void SAO_UI_CALL popup_layer_button(
    int32_t button, int32_t action, int32_t mods,
    float layer_x, float layer_y, void* user_data);

static sao_status_t sync_visible_layer_locked(sao_ui_popup_s* popup) {
    if (popup->compositor == nullptr) return SAO_STATUS_OK;
    try {
        VisualSnapshot snapshot;
        sao_ui_layer_handle_t layer = nullptr;
        PopupInputBinding* binding = nullptr;
        {
            std::lock_guard lock(popup->mu);
            if (!popup->visible || popup->levels.empty()) return SAO_STATUS_OK;
            const sao_status_t snapshot_status = build_visual_snapshot_locked(popup, &snapshot);
            if (snapshot_status != SAO_STATUS_OK) return snapshot_status;
            layer = popup->layer;
            binding = popup->input_binding.get();
        }

        PopupFrame frame;
        sao_status_t status = render_popup_frame(snapshot, &frame);
        if (status != SAO_STATUS_OK) return status;
        int32_t layer_x = 0;
        int32_t layer_y = 0;
        status = resolve_layer_origin(
            popup->compositor, frame.screen_bounds, &layer_x, &layer_y);
        if (status != SAO_STATUS_OK) return status;

        if (layer == nullptr) {
            const std::string layer_name = next_layer_name();
            SaoLayerConfig config{};
            config.struct_size = sizeof(SaoLayerConfig);
            config.name_utf8 = layer_name.c_str();
            config.x = layer_x;
            config.y = layer_y;
            config.width = static_cast<int32_t>(frame.width);
            config.height = static_cast<int32_t>(frame.height);
            config.z_order = kPopupZOrder;
            config.click_through = false;
            config.rect_hit = false;
            config.bgra_swizzle = true;
            sao_ui_layer_handle_t created = nullptr;
            status = sao_ui_layer_create(popup->compositor, &config, &created);
            SaoUiLayerEffects effects{};
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_effects_init(
                    SAO_UI_LAYER_EFFECT_PRESET_POPUP, &effects);
            }
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_effects(created, &effects);
            if (status == SAO_STATUS_OK) status = sao_ui_layer_set_visible(created, false);
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_input_callbacks(
                    created, &popup_layer_cursor, nullptr, &popup_layer_button,
                    nullptr, binding);
            }
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_update_bgra(
                    created, frame.pixels.data(), frame.width, frame.height, frame.stride);
            }
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_input_rects(
                    created, frame.input_rects.empty() ? nullptr : frame.input_rects.data(),
                    frame.input_rects.size());
            }
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_input_enabled(created, !frame.input_rects.empty());
            }
            if (status == SAO_STATUS_OK) status = sao_ui_layer_set_visible(created, true);
            if (status != SAO_STATUS_OK) {
                destroy_layer_handle(created);
                return status;
            }
            {
                std::lock_guard lock(popup->mu);
                popup->layer = created;
                popup->layer_screen_bounds = frame.screen_bounds;
                popup->layer_bounds_valid = true;
            }
            return SAO_STATUS_OK;
        }

        status = sao_ui_layer_set_input_rects(layer, nullptr, 0);
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_update_bgra(
                layer, frame.pixels.data(), frame.width, frame.height, frame.stride);
        }
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_geometry(
                layer, layer_x, layer_y,
                static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height));
        }
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_input_rects(
                layer, frame.input_rects.empty() ? nullptr : frame.input_rects.data(),
                frame.input_rects.size());
        }
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_input_enabled(layer, !frame.input_rects.empty());
        }
        if (status == SAO_STATUS_OK) status = sao_ui_layer_set_visible(layer, true);
        if (status != SAO_STATUS_OK) {
            {
                std::lock_guard lock(popup->mu);
                if (popup->layer == layer) {
                    popup->layer = nullptr;
                    popup->layer_bounds_valid = false;
                }
            }
            destroy_layer_handle(layer);
            return status;
        }
        {
            std::lock_guard lock(popup->mu);
            popup->layer_screen_bounds = frame.screen_bounds;
            popup->layer_bounds_valid = true;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

static void invoke_result_noexcept(const PendingResult& result) noexcept {
    if (result.callback == nullptr) return;
    try {
        result.callback(result.entry_id, result.dismissed,
                        result.screen_x, result.screen_y, result.user_data);
    } catch (...) {
    }
}

static bool local_to_screen_locked(
    const sao_ui_popup_s* popup, float layer_x, float layer_y,
    int32_t* out_x, int32_t* out_y) {
    if (!popup->layer_bounds_valid || out_x == nullptr || out_y == nullptr ||
        !std::isfinite(layer_x) || !std::isfinite(layer_y)) {
        return false;
    }
    const int64_t x = static_cast<int64_t>(popup->layer_screen_bounds.x) +
        static_cast<int64_t>(std::floor(layer_x));
    const int64_t y = static_cast<int64_t>(popup->layer_screen_bounds.y) +
        static_cast<int64_t>(std::floor(layer_y));
    if (x < std::numeric_limits<int32_t>::min() ||
        x > std::numeric_limits<int32_t>::max() ||
        y < std::numeric_limits<int32_t>::min() ||
        y > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out_x = static_cast<int32_t>(x);
    *out_y = static_cast<int32_t>(y);
    return true;
}

static void SAO_UI_CALL popup_layer_cursor(
    float layer_x, float layer_y, void* user_data) {
    const auto binding = acquire_binding(user_data);
    if (!binding) return;
    try {
        std::unique_lock binding_lock(binding->mutex);
        sao_ui_popup_s* popup = binding->popup;
        if (popup == nullptr) return;
        std::lock_guard layer_lock(popup->layer_mu);
        bool redraw = false;
        {
            std::lock_guard lock(popup->mu);
            if (!popup->visible || popup->levels.empty()) return;
            int32_t screen_x = 0;
            int32_t screen_y = 0;
            if (!local_to_screen_locked(popup, layer_x, layer_y, &screen_x, &screen_y)) return;
            const HitTarget hit = hit_target_locked(popup, screen_x, screen_y);
            if (!hit.inside_level) return;
            OpenLevel& level = popup->levels[static_cast<size_t>(hit.depth)];
            const auto& entries = level_entries(popup, hit.depth);
            const int32_t selection = hit.index >= 0 &&
                is_selectable(entries[static_cast<size_t>(hit.index)]) ? hit.index : -1;
            if (level.selected_index != selection) {
                level.selected_index = selection;
                redraw = true;
            }
            const size_t child_index = static_cast<size_t>(hit.depth + 1);
            if (popup->levels.size() > child_index) {
                const bool keep_open = selection >= 0 &&
                    popup->levels[child_index].parent_entry_index == selection;
                if (!keep_open) {
                    popup->levels.resize(child_index);
                    redraw = true;
                }
            }
        }
        if (redraw) (void)sync_visible_layer_locked(popup);
    } catch (...) {
    }
}

static void SAO_UI_CALL popup_layer_button(
    int32_t button, int32_t action, int32_t,
    float layer_x, float layer_y, void* user_data) {
    if (button != 0 || action != 1) return;
    const auto binding = acquire_binding(user_data);
    if (!binding) return;

    PendingResult pending;
    try {
        std::unique_lock binding_lock(binding->mutex);
        sao_ui_popup_s* popup = binding->popup;
        if (popup == nullptr) return;
        std::unique_lock layer_lock(popup->layer_mu);
        sao_ui_layer_handle_t layer_to_destroy = nullptr;
        bool redraw = false;
        {
            std::lock_guard lock(popup->mu);
            if (!popup->visible || popup->levels.empty()) return;
            int32_t screen_x = 0;
            int32_t screen_y = 0;
            if (!local_to_screen_locked(popup, layer_x, layer_y, &screen_x, &screen_y)) return;
            const HitTarget hit = hit_target_locked(popup, screen_x, screen_y);
            if (hit.index < 0) return;
            OpenLevel& level = popup->levels[static_cast<size_t>(hit.depth)];
            const auto& entries = level_entries(popup, hit.depth);
            const EntryNode& entry = entries[static_cast<size_t>(hit.index)];
            const int32_t selection = is_selectable(entry) ? hit.index : -1;
            if (level.selected_index != selection) {
                level.selected_index = selection;
                redraw = true;
            }
            popup->levels.resize(static_cast<size_t>(hit.depth + 1));
            if (!is_selectable(entry)) {
                redraw = true;
            } else if (!entry.children.empty()) {
                redraw = open_submenu_locked(popup, hit.depth, hit.index) == SAO_STATUS_OK;
            } else {
                pending = {popup->callback, popup->user_data, entry.entry_id,
                           false, screen_x, screen_y};
                popup->visible = false;
                popup->callback = nullptr;
                popup->user_data = nullptr;
                popup->levels.clear();
                layer_to_destroy = std::exchange(popup->layer, nullptr);
                popup->layer_bounds_valid = false;
            }
        }
        if (layer_to_destroy != nullptr) {
            destroy_layer_handle(layer_to_destroy);
        } else if (redraw) {
            (void)sync_visible_layer_locked(popup);
        }
        layer_lock.unlock();
        binding_lock.unlock();
    } catch (...) {
        return;
    }
    invoke_result_noexcept(pending);
}

}  // namespace

extern "C" const SaoUiPopupLayoutConsts* SAO_UI_CALL
sao_ui_popup_layout_constants(void) {
    static constexpr SaoUiPopupLayoutConsts kConsts = {
        /*menu_size=*/54,
        /*menu_max_size=*/70,
        /*menu_slot=*/70,
        /*menu_width=*/70,
        /*menu_max_visible=*/9,
        /*child_width=*/240,
        /*child_row_stride=*/40,
        /*child_slide_ms=*/240,
        /*hud_pad=*/40,
        /*hud_margin=*/12,
        /*gap_menu_child=*/25,
    };
    return &kConsts;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    sao_ui_popup_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        auto popup = std::make_unique<sao_ui_popup_s>();
        auto binding = std::make_shared<PopupInputBinding>();
        binding->popup = popup.get();
        if (!register_binding(binding)) return SAO_STATUS_ERR_UNKNOWN;
        popup->compositor = compositor;
        popup->theme = theme;
        popup->input_binding = std::move(binding);
        *out_handle = popup.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_popup_destroy(sao_ui_popup_handle_t handle) {
    if (handle == nullptr) return;
    const std::shared_ptr<PopupInputBinding> binding = handle->input_binding;
    if (binding) {
        std::lock_guard binding_lock(binding->mutex);
        if (binding->popup == handle) binding->popup = nullptr;
    }
    unregister_binding(binding.get());
    sao_ui_layer_handle_t layer = nullptr;
    {
        std::lock_guard layer_lock(handle->layer_mu);
        std::lock_guard lock(handle->mu);
        handle->visible = false;
        handle->callback = nullptr;
        handle->user_data = nullptr;
        handle->levels.clear();
        layer = std::exchange(handle->layer, nullptr);
        handle->layer_bounds_valid = false;
    }
    destroy_layer_handle(layer);
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_show(
    sao_ui_popup_handle_t handle,
    const SaoUiPopupSpec* spec,
    sao_ui_popup_result_callback_t callback,
    void* user_data) {
    if (handle == nullptr || spec == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!valid_theme_override(spec->theme_override)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        EntryNode replacement_root;
        deep_copy_entries(spec->entries, spec->entry_count, &replacement_root.children);
        OpenLevel root_level;
        sao_status_t status = make_root_level(replacement_root.children, *spec, &root_level);
        if (status != SAO_STATUS_OK) return status;

        std::lock_guard layer_lock(handle->layer_mu);
        sao_ui_layer_handle_t old_layer = nullptr;
        {
            std::lock_guard lock(handle->mu);
            old_layer = std::exchange(handle->layer, nullptr);
            handle->layer_bounds_valid = false;
            handle->root = std::move(replacement_root);
            handle->spec_snapshot = *spec;
            handle->spec_snapshot.entries = nullptr;
            handle->spec_snapshot.entry_count = 0;
            handle->callback = callback;
            handle->user_data = user_data;
            handle->visible = true;
            handle->levels.clear();
            handle->levels.push_back(std::move(root_level));
        }
        destroy_layer_handle(old_layer);
        status = sync_visible_layer_locked(handle);
        if (status == SAO_STATUS_OK) return SAO_STATUS_OK;

        sao_ui_layer_handle_t failed_layer = nullptr;
        {
            std::lock_guard lock(handle->mu);
            handle->visible = false;
            handle->callback = nullptr;
            handle->user_data = nullptr;
            handle->levels.clear();
            failed_layer = std::exchange(handle->layer, nullptr);
            handle->layer_bounds_valid = false;
        }
        destroy_layer_handle(failed_layer);
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_hide(sao_ui_popup_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PendingResult pending;
    sao_ui_layer_handle_t layer = nullptr;
    {
        std::lock_guard layer_lock(handle->layer_mu);
        {
            std::lock_guard lock(handle->mu);
            if (!handle->visible) return SAO_STATUS_OK;
            pending = {handle->callback, handle->user_data, -1, true, -1, -1};
            handle->visible = false;
            handle->callback = nullptr;
            handle->user_data = nullptr;
            handle->levels.clear();
            layer = std::exchange(handle->layer, nullptr);
            handle->layer_bounds_valid = false;
        }
        destroy_layer_handle(layer);
    }
    invoke_result_noexcept(pending);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_is_visible(
    sao_ui_popup_handle_t handle, bool* out_visible) {
    if (handle == nullptr || out_visible == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mu);
    *out_visible = handle->visible;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_refresh_entries(
    sao_ui_popup_handle_t handle,
    const SaoUiPopupEntry* entries, size_t count) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard layer_lock(handle->layer_mu);
        SaoUiPopupSpec spec{};
        {
            std::lock_guard lock(handle->mu);
            spec = handle->spec_snapshot;
        }
        EntryNode replacement_root;
        deep_copy_entries(entries, count, &replacement_root.children);
        OpenLevel root_level;
        sao_status_t status = make_root_level(replacement_root.children, spec, &root_level);
        if (status != SAO_STATUS_OK) return status;
        bool visible = false;
        {
            std::lock_guard lock(handle->mu);
            handle->root = std::move(replacement_root);
            handle->levels.clear();
            handle->levels.push_back(std::move(root_level));
            visible = handle->visible;
        }
        return visible ? sync_visible_layer_locked(handle) : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

static EntryNode* find_by_id_mut(EntryNode& root, int32_t entry_id) {
    for (EntryNode& child : root.children) {
        if (child.entry_id == entry_id) return &child;
        EntryNode* deep = find_by_id_mut(child, entry_id);
        if (deep != nullptr) return deep;
    }
    return nullptr;
}

static void reconcile_disabled_selection_locked(sao_ui_popup_s* popup) {
    for (size_t depth = 0; depth < popup->levels.size(); ++depth) {
        OpenLevel& level = popup->levels[depth];
        const auto& entries = level_entries(popup, static_cast<int32_t>(depth));
        if (level.selected_index >= 0 &&
            (level.selected_index >= static_cast<int32_t>(entries.size()) ||
             !is_selectable(entries[static_cast<size_t>(level.selected_index)]))) {
            level.selected_index = popup->spec_snapshot.allow_keyboard_nav
                ? advance_selection(entries, level.selected_index, +1) : -1;
        }
        const size_t child_depth = depth + 1;
        if (child_depth >= popup->levels.size()) continue;
        const int32_t parent_index = popup->levels[child_depth].parent_entry_index;
        if (parent_index < 0 || parent_index >= static_cast<int32_t>(entries.size()) ||
            !is_selectable(entries[static_cast<size_t>(parent_index)]) ||
            entries[static_cast<size_t>(parent_index)].children.empty()) {
            popup->levels.resize(child_depth);
            return;
        }
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_set_entry_checked(
    sao_ui_popup_handle_t handle, int32_t entry_id, bool checked) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard layer_lock(handle->layer_mu);
    bool visible = false;
    {
        std::lock_guard lock(handle->mu);
        EntryNode* node = find_by_id_mut(handle->root, entry_id);
        if (node == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
        node->checked = checked;
        visible = handle->visible;
    }
    return visible ? sync_visible_layer_locked(handle) : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_set_entry_enabled(
    sao_ui_popup_handle_t handle, int32_t entry_id, bool enabled) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard layer_lock(handle->layer_mu);
    bool visible = false;
    {
        std::lock_guard lock(handle->mu);
        EntryNode* node = find_by_id_mut(handle->root, entry_id);
        if (node == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
        node->enabled = enabled;
        if (!enabled) reconcile_disabled_selection_locked(handle);
        visible = handle->visible;
    }
    return visible ? sync_visible_layer_locked(handle) : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_key_press(
    sao_ui_popup_handle_t handle, SaoUiPopupNavKey key) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (key < SAO_UI_POPUP_KEY_UP || key > SAO_UI_POPUP_KEY_ESC)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PendingResult pending;
    sao_status_t status = SAO_STATUS_OK;
    try {
        std::unique_lock layer_lock(handle->layer_mu);
        sao_ui_layer_handle_t layer_to_destroy = nullptr;
        bool redraw = true;
        {
            std::lock_guard lock(handle->mu);
            if (!handle->visible || handle->levels.empty())
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (key != SAO_UI_POPUP_KEY_ESC && !handle->spec_snapshot.allow_keyboard_nav)
                return SAO_STATUS_ERR_ACCESS_DENIED;
            const int32_t depth = static_cast<int32_t>(handle->levels.size()) - 1;
            OpenLevel& top = handle->levels.back();
            const auto& entries = level_entries(handle, depth);

            switch (key) {
            case SAO_UI_POPUP_KEY_UP:
                top.selected_index = advance_selection(entries, top.selected_index, -1);
                break;
            case SAO_UI_POPUP_KEY_DOWN:
                top.selected_index = advance_selection(entries, top.selected_index, +1);
                break;
            case SAO_UI_POPUP_KEY_LEFT:
                if (handle->levels.size() > 1) handle->levels.pop_back();
                break;
            case SAO_UI_POPUP_KEY_RIGHT:
                if (top.selected_index >= 0 &&
                    top.selected_index < static_cast<int32_t>(entries.size())) {
                    const EntryNode& entry = entries[static_cast<size_t>(top.selected_index)];
                    if (is_selectable(entry) && !entry.children.empty())
                        status = open_submenu_locked(handle, depth, top.selected_index);
                }
                break;
            case SAO_UI_POPUP_KEY_ENTER:
                if (top.selected_index >= 0 &&
                    top.selected_index < static_cast<int32_t>(entries.size())) {
                    const EntryNode& entry = entries[static_cast<size_t>(top.selected_index)];
                    if (is_selectable(entry)) {
                        if (!entry.children.empty()) {
                            status = open_submenu_locked(handle, depth, top.selected_index);
                        } else {
                            pending = {handle->callback, handle->user_data, entry.entry_id,
                                       false, -1, -1};
                            handle->visible = false;
                            handle->callback = nullptr;
                            handle->user_data = nullptr;
                            handle->levels.clear();
                            layer_to_destroy = std::exchange(handle->layer, nullptr);
                            handle->layer_bounds_valid = false;
                            redraw = false;
                        }
                    }
                }
                break;
            case SAO_UI_POPUP_KEY_ESC:
                pending = {handle->callback, handle->user_data, -1, true, -1, -1};
                handle->visible = false;
                handle->callback = nullptr;
                handle->user_data = nullptr;
                handle->levels.clear();
                layer_to_destroy = std::exchange(handle->layer, nullptr);
                handle->layer_bounds_valid = false;
                redraw = false;
                break;
            default:
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }
        if (layer_to_destroy != nullptr) {
            destroy_layer_handle(layer_to_destroy);
        } else if (redraw && status == SAO_STATUS_OK) {
            status = sync_visible_layer_locked(handle);
        }
        layer_lock.unlock();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    invoke_result_noexcept(pending);
    return status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_popup_hit_test(
    sao_ui_popup_handle_t handle,
    int32_t x, int32_t y,
    int32_t* out_entry_id, int32_t* out_submenu_depth) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_entry_id != nullptr) *out_entry_id = -1;
    if (out_submenu_depth != nullptr) *out_submenu_depth = 0;
    std::lock_guard<std::mutex> lk(handle->mu);
    if (!handle->visible || handle->levels.empty()) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    const HitTarget hit = hit_target_locked(handle, x, y);
    if (out_entry_id != nullptr) *out_entry_id = hit.entry_id;
    if (out_submenu_depth != nullptr && hit.inside_level)
        *out_submenu_depth = hit.depth;
    return SAO_STATUS_OK;
}
