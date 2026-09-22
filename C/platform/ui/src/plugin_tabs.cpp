#include "sao/ui/plugin_tabs.h"

#include "../assets/classic/classic_icons.h"
#include "classic_text_roles.h"
#include "layer_paint_internal.h"
#include "panel_theme_internal.h"
#include "widget_paint_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kHeader = 34;
constexpr int32_t kRow = 44;
constexpr int32_t kStride = 47;
constexpr size_t kMaxVisible = 12;
constexpr size_t kNoIndex = std::numeric_limits<size_t>::max();
constexpr size_t kTextBudget = 256U * 1024U;

struct Action {
    std::string name;
    std::string icon;
    int32_t id{};
    bool enabled{};
    bool keep_open{};
    bool close_before{};
    bool operator==(const Action&) const = default;
};

struct Tab {
    std::string id;
    std::string name;
    std::string icon;
    std::vector<Action> actions;
    bool enabled{};
    uint8_t status{SAO_UI_PLUGIN_TAB_STATUS_UNKNOWN};
    bool operator==(const Tab&) const = default;
};

struct Column {
    int32_t x{}, y{}, width{1}, height{1};
    size_t first{}, capacity{}, rows{};
    bool operator==(const Column&) const = default;
};

struct Tabs {
    sao_ui_compositor_handle_t compositor{};
    sao_ui_plugin_tabs_handle_t handle{};
    const std::thread::id owner{std::this_thread::get_id()};
    sao_ui_plugin_tab_action_fn_t action_fn{};
    void* user_data{};
    std::array<sao_ui_layer_handle_t, 2> layers{};
    std::array<Column, 2> columns{};
    std::array<Column, 2> published{};
    std::array<bool, 2> shown{};
    std::array<bool, 2> has_paint{};
    std::array<size_t, 2> hovered{kNoIndex, kNoIndex};
    std::array<size_t, 2> pressed{kNoIndex, kNoIndex};
    std::array<float, 2> wheel{};
    std::vector<Tab> items;
    size_t selected{kNoIndex};
    uint64_t model_revision{}, navigation_revision{};
    uint64_t theme_generation{std::numeric_limits<uint64_t>::max()};
    SaoUiThemeId theme_id{SAO_UI_THEME_COUNT};
    int32_t viewport_width{1280}, viewport_height{720};
    int32_t x{24}, y{};
    float grab_x{}, grab_y{};
    bool positioned{}, requested{}, dragging{}, dirty{true}, publishing{}, retiring{}, invoking{};
    size_t callback_depth{};
    sao_status_t pending_status{SAO_STATUS_OK};
    sao_status_t last_action_status{SAO_STATUS_OK};
};

std::mutex g_registry_mutex;
std::unordered_map<sao_ui_plugin_tabs_handle_t, std::shared_ptr<Tabs>> g_registry;
uintptr_t g_next_handle{};

std::shared_ptr<Tabs> acquire(sao_ui_plugin_tabs_handle_t handle) {
    std::lock_guard lock(g_registry_mutex);
    const auto found = g_registry.find(handle);
    return found == g_registry.end() ? std::shared_ptr<Tabs>{} : found->second;
}

sao_status_t owner_status(const Tabs& tabs) noexcept {
    if (tabs.owner != std::this_thread::get_id())
        return SAO_STATUS_ERR_ACCESS_DENIED;
    return sao_ui_compositor_require_owner_thread(tabs.compositor);
}

sao_status_t ready_status(const std::shared_ptr<Tabs>& tabs) noexcept {
    if (!tabs)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto status = owner_status(*tabs);
    if (status != SAO_STATUS_OK)
        return status;
    return tabs->retiring || tabs->publishing ? SAO_STATUS_ERR_CANCELLED : SAO_STATUS_OK;
}

void remember(Tabs& tabs, sao_status_t status) noexcept {
    if (tabs.pending_status == SAO_STATUS_OK)
        tabs.pending_status = status;
}

struct CallbackLease {
    std::shared_ptr<Tabs> tabs;
    explicit CallbackLease(void* token) : tabs(acquire(static_cast<sao_ui_plugin_tabs_handle_t>(token))) {
        if (tabs && owner_status(*tabs) == SAO_STATUS_OK && !tabs->retiring)
            ++tabs->callback_depth;
        else
            tabs.reset();
    }
    ~CallbackLease() {
        if (tabs)
            --tabs->callback_depth;
    }
};

bool valid_utf8(std::string_view text) noexcept {
    for (size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset++]);
        if (lead < 0x80U)
            continue;
        const size_t extra = lead >= 0xC2U && lead <= 0xDFU ? 1U
                           : lead >= 0xE0U && lead <= 0xEFU ? 2U
                           : lead >= 0xF0U && lead <= 0xF4U ? 3U : 0U;
        if (!extra || extra > text.size() - offset)
            return false;
        uint32_t point = lead & (0x7FU >> extra);
        for (size_t i = 0; i < extra; ++i) {
            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xC0U) != 0x80U)
                return false;
            point = (point << 6U) | (next & 0x3FU);
        }
        if ((extra == 1 && point < 0x80U) || (extra == 2 && point < 0x800U) ||
            (extra == 3 && point < 0x10000U) || point > 0x10FFFFU ||
            (point >= 0xD800U && point <= 0xDFFFU))
            return false;
    }
    return true;
}

bool copy_text(const char* source, size_t limit, size_t& budget, std::string& target) {
    if (!source)
        return true;
    size_t length = 0;
    while (length <= limit && source[length])
        ++length;
    if (length > limit || length > budget || !valid_utf8({source, length}))
        return false;
    target.assign(source, length);
    budget -= length;
    return true;
}

bool valid_flag(const bool& flag) noexcept {
    unsigned char byte{};
    static_assert(sizeof(bool) == sizeof(byte));
    std::memcpy(&byte, &flag, sizeof(byte));
    return byte <= 1;
}

sao_status_t copy_items(const SaoUiPluginTab* items, size_t count, std::vector<Tab>& result) {
    if (count > 256 || (count && !items))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    size_t budget = kTextBudget;
    size_t actions = 0;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& source = items[i];
        if (source.struct_size < sizeof(SaoUiPluginTab))
            return SAO_STATUS_ERR_ABI_MISMATCH;
        if (!valid_flag(source.enabled) || source.status > SAO_UI_PLUGIN_TAB_STATUS_DISABLED ||
            source.action_count > 1024 - actions ||
            (source.action_count && !source.actions) ||
            std::any_of(std::begin(source.reserved), std::end(source.reserved),
                        [](uint8_t value) { return value != 0; }))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        Tab tab;
        if (!copy_text(source.plugin_id_utf8, 127, budget, tab.id) || tab.id.empty() ||
            !copy_text(source.name_utf8, 4096, budget, tab.name) || tab.name.empty() ||
            !copy_text(source.icon_utf8, 128, budget, tab.icon))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (std::any_of(result.begin(), result.end(), [&tab](const Tab& old) { return old.id == tab.id; }))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        tab.enabled = source.enabled;
        tab.status = source.status;
        actions += source.action_count;
        tab.actions.reserve(source.action_count);
        for (size_t j = 0; j < source.action_count; ++j) {
            const auto& input = source.actions[j];
            if (input.struct_size < sizeof(SaoUiPluginTabAction))
                return SAO_STATUS_ERR_ABI_MISMATCH;
            if (!valid_flag(input.enabled) || !valid_flag(input.keep_open) ||
                !valid_flag(input.close_before) || input.reserved)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            Action action;
            if (!copy_text(input.name_utf8, 4096, budget, action.name) || action.name.empty() ||
                !copy_text(input.icon_utf8, 128, budget, action.icon))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            action.id = input.action_id;
            action.enabled = input.enabled;
            action.keep_open = input.keep_open;
            action.close_before = input.close_before;
            tab.actions.push_back(std::move(action));
        }
        result.push_back(std::move(tab));
    }
    return SAO_STATUS_OK;
}

bool has_children(const Tabs& tabs) noexcept {
    return tabs.selected < tabs.items.size() && tabs.items[tabs.selected].enabled;
}

void cancel_interaction(Tabs& tabs) noexcept {
    tabs.dragging = false;
    tabs.pressed.fill(kNoIndex);
    tabs.hovered.fill(kNoIndex);
    tabs.wheel.fill(0.0F);
}

size_t find_tab(const std::vector<Tab>& items, std::string_view id) noexcept {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id)
            return i;
    return kNoIndex;
}

void size_column(Column& column, size_t count, int32_t viewport_height) noexcept {
    const int32_t available = std::max(0, viewport_height - kHeader);
    column.capacity = std::min(kMaxVisible, static_cast<size_t>(
        available >= kRow ? (available - kRow) / kStride + 1 : available > 0 ? 1 : 0));
    const size_t last = count > column.capacity ? count - column.capacity : 0;
    column.first = std::min(column.first, last);
    column.rows = std::min(column.capacity, count - column.first);
    column.height = std::min(viewport_height, kHeader + (column.rows
        ? static_cast<int32_t>(column.rows - 1) * kStride + kRow : 0));
}

sao_status_t layout(Tabs& tabs) noexcept {
    int32_t width = tabs.viewport_width;
    int32_t height = tabs.viewport_height;
    const auto host = sao_ui_compositor_host(tabs.compositor);
    if (host) {
        SaoOverlayHostClientRect rect{};
        const auto status = sao_ui_overlay_host_get_client_rect(host, &rect);
        if (status != SAO_STATUS_OK)
            return status;
        width = std::max(1, rect.width);
        height = std::max(1, rect.height);
        if (!sao_ui_overlay_host_visible(host) && tabs.dragging) {
            cancel_interaction(tabs);
            tabs.dirty = true;
        }
    } else {
        sao::ui::detail::MenuSceneFrame scene{};
        if (sao::ui::detail::read_menu_scene(tabs.compositor, scene)) {
            width = static_cast<int32_t>(std::clamp(scene.viewport_width, 1.0F, 32768.0F));
            height = static_cast<int32_t>(std::clamp(scene.viewport_height, 1.0F, 32768.0F));
        }
    }
    if (width != tabs.viewport_width || height != tabs.viewport_height) {
        cancel_interaction(tabs);
        tabs.dirty = true;
    }
    tabs.viewport_width = width;
    tabs.viewport_height = height;
    const auto previous = tabs.columns;
    auto& root = tabs.columns[0];
    auto& child = tabs.columns[1];
    const bool children = has_children(tabs) && width >= 2;
    const int32_t gap = width >= 48 ? 12 : 0;
    root.width = children ? std::min(224, std::max(1, (width - gap) / 2)) : std::min(224, width);
    child.width = std::min(240, std::max(1, width - root.width - gap));
    size_column(root, tabs.items.size(), height);
    size_column(child, children ? std::max(size_t{1}, tabs.items[tabs.selected].actions.size()) : 0, height);
    if (!tabs.positioned && !tabs.items.empty()) {
        tabs.y = (height - root.height) / 2;
        tabs.positioned = true;
    }
    tabs.x = std::clamp(tabs.x, 0, width - root.width);
    tabs.y = std::clamp(tabs.y, 0, height - root.height);
    if (children && static_cast<int64_t>(tabs.x) + root.width + gap + child.width > width &&
        tabs.x < child.width + gap)
        tabs.x = std::max(0, width - root.width - gap - child.width);
    root.x = tabs.x;
    root.y = tabs.y;
    child.x = static_cast<int64_t>(root.x) + root.width + gap + child.width <= width
        ? root.x + root.width + gap : std::max(0, root.x - gap - child.width);
    int32_t anchor = root.y;
    if (children && tabs.selected >= root.first && tabs.selected - root.first < root.rows)
        anchor += static_cast<int32_t>(tabs.selected - root.first) * kStride;
    child.y = std::clamp(anchor, 0, height - child.height);
    if (previous != tabs.columns)
        tabs.dirty = true;
    return SAO_STATUS_OK;
}

SaoUiLayerInputRect row_rect(const Column& column, size_t slot) noexcept {
    const int32_t y = kHeader + static_cast<int32_t>(slot) * kStride;
    return {0, y, column.width, std::max(0, std::min(kRow, column.height - y))};
}

size_t hit_row(const Column& column, float x, float y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x >= column.width ||
        y < kHeader || y >= column.height)
        return kNoIndex;
    const auto slot = static_cast<size_t>((y - kHeader) / kStride);
    if (slot >= column.rows || y >= kHeader + static_cast<float>(slot * kStride) + kRow)
        return kNoIndex;
    return column.first + slot;
}

bool header_hit(const Column& column, float x, float y) noexcept {
    return std::isfinite(x) && std::isfinite(y) && x >= 0 && x < column.width &&
           y >= 0 && y < std::min(kHeader - 3, column.height);
}

struct PaintDeleter {
    void operator()(sao_ui_paint_ctx_s* context) const noexcept { sao_ui_paint_ctx_destroy(context); }
};

class Painter {
  public:
    sao_ui_paint_ctx_handle_t context{};
    sao_status_t status{SAO_STATUS_OK};
    void merge(sao_status_t next) noexcept {
        if (status == SAO_STATUS_OK)
            status = next;
    }
    void rounded(float x, float y, float width, float height, uint32_t fill, uint32_t border) {
        if (status != SAO_STATUS_OK || width <= 0 || height <= 0)
            return;
        merge(sao::ui::detail::paint_rounded_rect(context, x, y, width, height, 5.0F, fill));
        if (width > 1 && height > 1)
            merge(sao::ui::detail::paint_rounded_rect_stroke(context, x + 0.5F, y + 0.5F,
                width - 1, height - 1, 5.0F, 1.0F, border));
    }
    void line(float x, float y, float end_x, float end_y, uint32_t color) {
        if (status == SAO_STATUS_OK)
            merge(sao_ui_paint_ctx_stroke_line(context, x, y, end_x, end_y, 1.0F, color));
    }
    void text(float x, float y, float width, float height, const char* value,
              float size, uint32_t color) {
        if (status != SAO_STATUS_OK || width <= 0 || height <= 0)
            return;
        const std::string fitted = sao::ui::detail::ellipsize_ui_text(value, size, width);
        const auto clip = sao_ui_paint_ctx_push_clip(context, x, y, width, height);
        merge(clip);
        if (clip == SAO_STATUS_OK) {
            merge(sao_ui_paint_ctx_draw_utf8(context, x, y, fitted.c_str(), size, color));
            merge(sao_ui_paint_ctx_pop_clip(context));
        }
    }
    void icon(const std::string& value, float x, float y, uint32_t color) {
        if (status != SAO_STATUS_OK)
            return;
        using sao::ui::classic::IconId;
        std::string_view token(value);
        const bool named = token.starts_with("sao:");
        if (named)
            token.remove_prefix(4);
        for (const auto& definition : sao::ui::classic::kDefinitions) {
            if (token == definition.name) {
                merge(sao::ui::classic::paint_classic_icon(context, definition.id, x, y, 18.0F, color));
                return;
            }
        }
        IconId fallback = token == "license" ? IconId::Lock
                        : token == "light" ? IconId::Sun
                        : token == "dark" ? IconId::Moon
                        : token == "exit" || token == "cancel" ? IconId::Close : IconId::Plugins;
        const bool glyph = !named && !value.empty();
        if (glyph)
            text(x - 1, y - 2, 26, 26, value.c_str(), 17, color);
        else
            merge(sao::ui::classic::paint_classic_icon(context, fallback, x, y, 18.0F, color));
    }
};

sao_status_t record_column(const Tabs& tabs, size_t index,
                          const sao::ui::detail::PanelResolvedTheme& theme,
                          std::shared_ptr<const sao::ui::detail::PaintDisplayList>& result) {
    const auto& column = tabs.columns[index];
    sao_ui_paint_ctx_handle_t raw{};
    auto status = sao::ui::detail::create_recording_paint_context(
        static_cast<uint32_t>(column.width), static_cast<uint32_t>(column.height), &raw);
    if (status != SAO_STATUS_OK)
        return status;
    std::unique_ptr<sao_ui_paint_ctx_s, PaintDeleter> context(raw);
    Painter paint{raw};
    paint.merge(sao_ui_paint_ctx_begin_frame(raw));
    const sao::ui::detail::ScopedPanelPaintTheme theme_scope(theme);
    const sao::ui::detail::ScopedTextRole text_scope(sao::ui::detail::ClassicTextRole::Body);
    const auto color = [&theme](SaoUiColorToken token) { return theme.colors[static_cast<size_t>(token)]; };
    const auto opaque = [](uint32_t value) { return value | 0xFF000000U; };
    const auto border = color(SAO_UI_TOKEN_CHILD_LINE);
    const auto accent = color(theme.high_contrast ? SAO_UI_TOKEN_FOCUS_RING : SAO_UI_TOKEN_APP_ACCENT);
    const bool child = index == 1;
    const std::string title = child ? "插件 › " + tabs.items[tabs.selected].name : "PLUGINS";
    paint.rounded(0, 0, static_cast<float>(column.width), static_cast<float>(std::min(kHeader - 3, column.height)),
                  opaque(color(SAO_UI_TOKEN_CIRCLE_BG)), border);
    paint.text(10, 7, static_cast<float>(column.width - 40), 21, title.c_str(), 13, color(SAO_UI_TOKEN_APP_TEXT));
    if (column.width >= 48) {
        if (child)
            paint.merge(sao::ui::classic::paint_classic_icon(raw, sao::ui::classic::IconId::Close,
                static_cast<float>(column.width - 26), 7, 18, color(SAO_UI_TOKEN_APP_TEXT_2)));
        else
            for (int32_t offset = 0; offset < 3; ++offset)
                paint.line(static_cast<float>(column.width - 25), static_cast<float>(11 + offset * 4),
                           static_cast<float>(column.width - 12), static_cast<float>(11 + offset * 4), border);
    }
    for (size_t slot = 0; slot < column.rows; ++slot) {
        const size_t row = column.first + slot;
        const Tab& tab = tabs.items[child ? tabs.selected : row];
        const bool empty = child && tab.actions.empty();
        const Action* action = child && !empty ? &tab.actions[row] : nullptr;
        const bool enabled = !empty && tab.enabled && (!action || (action->enabled && tabs.action_fn));
        const bool selected = !child && row == tabs.selected;
        const bool pressed = enabled && tabs.pressed[index] == row;
        const bool hovered = enabled && tabs.hovered[index] == row;
        const bool highlighted = selected || pressed;
        const auto background = color(!enabled ? SAO_UI_TOKEN_DISABLED_BG
            : highlighted ? (theme.high_contrast ? SAO_UI_TOKEN_SELECTION : SAO_UI_TOKEN_CIRCLE_ACTIVE_BG)
            : hovered ? SAO_UI_TOKEN_CHILD_HOVER : SAO_UI_TOKEN_CHILD_BG);
        const auto foreground = color(!enabled ? SAO_UI_TOKEN_DISABLED_FG
            : highlighted ? (theme.high_contrast ? SAO_UI_TOKEN_WHITE : SAO_UI_TOKEN_CIRCLE_ACTIVE_ICON)
            : hovered ? SAO_UI_TOKEN_CHILD_HOVER_FG : SAO_UI_TOKEN_CHILD_TEXT);
        const auto secondary = !enabled || highlighted || hovered || theme.high_contrast
            ? foreground : color(SAO_UI_TOKEN_APP_TEXT_2);
        const auto content = tab.status == SAO_UI_PLUGIN_TAB_STATUS_DISABLED ? secondary : foreground;
        const char* status_label = tab.status == SAO_UI_PLUGIN_TAB_STATUS_ACTIVE ? "已启用"
            : tab.status == SAO_UI_PLUGIN_TAB_STATUS_DISABLED ? "已停用" : nullptr;
        const auto edge = !enabled ? color(SAO_UI_TOKEN_DISABLED_BORDER) : highlighted || hovered ? accent : border;
        const auto rect = row_rect(column, slot);
        const float y = static_cast<float>(rect.y);
        paint.rounded(0, y, static_cast<float>(column.width), static_cast<float>(rect.height), opaque(background), edge);
        if (paint.status == SAO_STATUS_OK && rect.height > 0) {
            const auto clip = sao_ui_paint_ctx_push_clip(raw, 0, y, static_cast<float>(column.width), static_cast<float>(rect.height));
            paint.merge(clip);
            if (clip == SAO_STATUS_OK) {
                paint.icon(empty ? "sao:info" : action ? action->icon : tab.icon, 12, y + 13, content);
                const float text_width = static_cast<float>(column.width - 58);
                const float pressed_offset = pressed ? 1.0F : 0.0F;
                if (!child && status_label) {
                    paint.text(42, y + 3 + pressed_offset, text_width, 21, tab.name.c_str(), 13, content);
                    paint.text(42, y + 25 + pressed_offset, text_width, 17, status_label, 11, secondary);
                } else {
                    paint.text(42, y + 12 + pressed_offset, text_width, 24,
                               empty ? (tab.status == SAO_UI_PLUGIN_TAB_STATUS_DISABLED
                                   ? "Plugin disabled" : "No actions")
                                   : action ? action->name.c_str() : tab.name.c_str(),
                               13, content);
                }
                if (!child && enabled && column.width >= 72) {
                    const float caret = static_cast<float>(column.width - 13);
                    paint.line(caret, y + 18, caret + 3, y + 22, foreground);
                    paint.line(caret + 3, y + 22, caret, y + 26, foreground);
                }
                paint.merge(sao_ui_paint_ctx_pop_clip(raw));
            }
        }
    }
    const size_t total = child ? tabs.items[tabs.selected].actions.size() : tabs.items.size();
    if (total > column.rows && column.rows && column.width >= 8) {
        const float extent = static_cast<float>(column.height - kHeader);
        const float thumb = std::max(4.0F, extent * static_cast<float>(column.rows) / static_cast<float>(total));
        const float top = kHeader + (extent - thumb) * static_cast<float>(column.first) /
                                      static_cast<float>(total - column.rows);
        for (size_t slot = 0; slot < column.rows; ++slot) {
            const auto rect = row_rect(column, slot);
            const float start = std::max(top, static_cast<float>(rect.y + 2));
            const float end = std::min(top + thumb, static_cast<float>(rect.y + rect.height - 2));
            if (end > start)
                paint.line(static_cast<float>(column.width - 3), start,
                           static_cast<float>(column.width - 3), end, accent);
        }
    }
    paint.merge(sao_ui_paint_ctx_end_frame(raw));
    if (paint.status == SAO_STATUS_OK)
        paint.merge(sao::ui::detail::seal_recording_paint_context(raw, &result));
    return paint.status;
}

sao_status_t hide_column(Tabs& tabs, size_t index) noexcept {
    auto status = sao_ui_layer_set_input_enabled(tabs.layers[index], false);
    const auto hidden = sao_ui_layer_set_visible(tabs.layers[index], false);
    tabs.shown[index] = false;
    return status == SAO_STATUS_OK ? hidden : status;
}

sao_status_t sync(Tabs& tabs) {
    if (tabs.publishing || tabs.retiring)
        return SAO_STATUS_ERR_CANCELLED;
    auto status = layout(tabs);
    if (status != SAO_STATUS_OK)
        return status;
    const auto theme = sao::ui::detail::resolve_process_theme();
    if (tabs.theme_generation != theme.generation || tabs.theme_id != theme.theme_id)
        tabs.dirty = true;
    const bool visible = tabs.requested && !tabs.items.empty();
    const std::array<bool, 2> wanted{visible, visible && has_children(tabs) && tabs.viewport_width >= 2};
    if (!tabs.dirty && wanted == tabs.shown)
        return SAO_STATUS_OK;
    std::array<std::shared_ptr<const sao::ui::detail::PaintDisplayList>, 2> lists{};
    for (size_t i = 0; i < 2; ++i) {
        if (wanted[i]) {
            status = record_column(tabs, i, theme, lists[i]);
            if (status != SAO_STATUS_OK)
                break;
        }
    }
    struct Publishing {
        Tabs& tabs;
        explicit Publishing(Tabs& value) : tabs(value) { tabs.publishing = true; }
        ~Publishing() { tabs.publishing = false; }
    } publishing(tabs);
    tabs.dirty = false;
    for (size_t i = 0; i < 2 && status == SAO_STATUS_OK; ++i) {
        if (!wanted[i]) {
            if (tabs.shown[i])
                status = hide_column(tabs, i);
            continue;
        }
        const auto& column = tabs.columns[i];
        const auto& old = tabs.published[i];
        const bool resized = !tabs.has_paint[i] || old.width != column.width || old.height != column.height;
        if (resized) {
            status = sao_ui_layer_set_input_enabled(tabs.layers[i], false);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_input_rects(tabs.layers[i], nullptr, 0);
        }
        if (status == SAO_STATUS_OK)
            status = sao::ui::detail::submit_layer_paint(tabs.layers[i], lists[i],
                static_cast<uint32_t>(column.width), static_cast<uint32_t>(column.height));
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_position(tabs.layers[i], column.x, column.y);
        if (status == SAO_STATUS_OK && (resized || old.rows != column.rows)) {
            std::array<SaoUiLayerInputRect, kMaxVisible + 1> rects{};
            size_t count = 1;
            rects[0] = {0, 0, column.width, std::min(kHeader - 3, column.height)};
            for (size_t slot = 0; slot < column.rows; ++slot) {
                const auto rect = row_rect(column, slot);
                if (rect.height > 0)
                    rects[count++] = rect;
            }
            status = sao_ui_layer_set_input_rects(tabs.layers[i], rects.data(), count);
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_input_enabled(tabs.layers[i], true);
        if (status == SAO_STATUS_OK && !tabs.shown[i])
            status = sao_ui_layer_set_visible(tabs.layers[i], true);
        if (status == SAO_STATUS_OK) {
            tabs.published[i] = column;
            tabs.has_paint[i] = true;
            tabs.shown[i] = true;
        }
    }
    if (status != SAO_STATUS_OK) {
        cancel_interaction(tabs);
        for (size_t i = 0; i < 2; ++i)
            (void)hide_column(tabs, i);
        tabs.dirty = true;
        return status;
    }
    tabs.theme_generation = theme.generation;
    tabs.theme_id = theme.theme_id;
    return SAO_STATUS_OK;
}

void close_children(Tabs& tabs) noexcept {
    tabs.selected = kNoIndex;
    tabs.columns[1].first = 0;
    ++tabs.navigation_revision;
    cancel_interaction(tabs);
    tabs.dirty = true;
}

template <size_t Index> void SAO_UI_CALL cursor(float x, float y, void* user) {
    try {
        CallbackLease lease(user);
        if (!lease.tabs || lease.tabs->publishing || !lease.tabs->shown[Index])
            return;
        auto& tabs = *lease.tabs;
        if (!std::isfinite(x) || !std::isfinite(y))
            return;
        if constexpr (Index == 0) {
            if (tabs.dragging) {
                // Captured coordinates track the moving layer, so recover host pixels before applying the grab offset.
                const double next_x = static_cast<double>(tabs.published[0].x) + x - tabs.grab_x;
                const double next_y = static_cast<double>(tabs.published[0].y) + y - tabs.grab_y;
                tabs.x = static_cast<int32_t>(std::clamp(next_x, 0.0, static_cast<double>(tabs.viewport_width - tabs.columns[0].width)));
                tabs.y = static_cast<int32_t>(std::clamp(next_y, 0.0, static_cast<double>(tabs.viewport_height - tabs.columns[0].height)));
                remember(tabs, layout(tabs));
                for (size_t i = 0; i < 2; ++i) {
                    if (tabs.shown[i]) {
                        const auto status = sao_ui_layer_set_position(tabs.layers[i], tabs.columns[i].x, tabs.columns[i].y);
                        remember(tabs, status);
                        if (status == SAO_STATUS_OK) {
                            tabs.published[i].x = tabs.columns[i].x;
                            tabs.published[i].y = tabs.columns[i].y;
                        }
                    }
                }
                return;
            }
        }
        const size_t hovered = hit_row(tabs.columns[Index], x, y);
        if (tabs.hovered[Index] != hovered) {
            tabs.hovered[Index] = hovered;
            tabs.dirty = true;
        }
    } catch (...) {
    }
}

template <size_t Index> void SAO_UI_CALL leave(void* user) {
    try {
        CallbackLease lease(user);
        if (!lease.tabs)
            return;
        auto& tabs = *lease.tabs;
        if (tabs.hovered[Index] != kNoIndex || tabs.pressed[Index] != kNoIndex ||
            (Index == 0 && tabs.dragging)) {
            tabs.hovered[Index] = kNoIndex;
            tabs.pressed[Index] = kNoIndex;
            if constexpr (Index == 0)
                tabs.dragging = false;
            tabs.dirty = true;
        }
    } catch (...) {
    }
}

template <size_t Index> void SAO_UI_CALL button(int32_t which, int32_t action, int32_t,
                                              float x, float y, void* user) {
    try {
        CallbackLease lease(user);
        if (!lease.tabs || lease.tabs->publishing || !lease.tabs->shown[Index] || which != 0)
            return;
        auto& tabs = *lease.tabs;
        const bool header = header_hit(tabs.columns[Index], x, y);
        const size_t row = hit_row(tabs.columns[Index], x, y);
        if (action == 1) {
            tabs.pressed[Index] = header ? kNoIndex - 1 : row;
            tabs.hovered[Index] = row;
            if constexpr (Index == 0) {
                if (header) {
                    tabs.dragging = true;
                    tabs.grab_x = x;
                    tabs.grab_y = y;
                }
            }
            tabs.dirty = true;
            return;
        }
        if (action != 0)
            return;
        const auto pressed = std::exchange(tabs.pressed[Index], kNoIndex);
        tabs.dirty = true;
        if constexpr (Index == 0) {
            if (std::exchange(tabs.dragging, false))
                return;
        } else {
            if (header && pressed == kNoIndex - 1) {
                close_children(tabs);
                remember(tabs, sync(tabs));
                return;
            }
        }
        if (row == kNoIndex || row != pressed)
            return;
        if constexpr (Index == 0) {
            if (row >= tabs.items.size() || !tabs.items[row].enabled)
                return;
            if (tabs.selected == row)
                close_children(tabs);
            else {
                tabs.selected = row;
                tabs.columns[1].first = 0;
                ++tabs.navigation_revision;
                cancel_interaction(tabs);
            }
            remember(tabs, sync(tabs));
        } else {
            if (!has_children(tabs) || row >= tabs.items[tabs.selected].actions.size())
                return;
            const auto& selected = tabs.items[tabs.selected].actions[row];
            if (!selected.enabled || !tabs.action_fn || tabs.invoking)
                return;
            const int32_t id = selected.id;
            const bool keep_open = selected.keep_open;
            const bool close_before = selected.close_before;
            const uint64_t navigation = tabs.navigation_revision;
            const auto callback = tabs.action_fn;
            void* const callback_user = tabs.user_data;
            if (!keep_open && close_before) {
                close_children(tabs);
                const auto status = sync(tabs);
                if (status != SAO_STATUS_OK) {
                    remember(tabs, status);
                    return;
                }
            }
            sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
            struct Invocation {
                Tabs& tabs;
                explicit Invocation(Tabs& value) : tabs(value) { tabs.invoking = true; }
                ~Invocation() { tabs.invoking = false; }
            } invocation(tabs);
            try {
                status = callback(id, callback_user);
            } catch (...) {
                status = SAO_STATUS_ERR_UNKNOWN;
            }
            tabs.last_action_status = status;
            if (!tabs.retiring && !keep_open && !close_before && tabs.navigation_revision == navigation) {
                close_children(tabs);
                remember(tabs, sync(tabs));
            }
        }
    } catch (...) {
    }
}

template <size_t Index> void SAO_UI_CALL scroll(float, float dy, void* user) {
    try {
        CallbackLease lease(user);
        if (!lease.tabs || lease.tabs->publishing || !lease.tabs->shown[Index] || !std::isfinite(dy))
            return;
        auto& tabs = *lease.tabs;
        if (tabs.dragging)
            return;
        auto& column = tabs.columns[Index];
        const size_t count = Index == 0 ? tabs.items.size()
            : has_children(tabs) ? tabs.items[tabs.selected].actions.size() : 0;
        tabs.wheel[Index] = std::clamp(tabs.wheel[Index] - dy, -1024.0F, 1024.0F);
        const auto steps = static_cast<int32_t>(tabs.wheel[Index]);
        if (!steps)
            return;
        tabs.wheel[Index] -= static_cast<float>(steps);
        const size_t maximum = count > column.capacity ? count - column.capacity : 0;
        const auto first = static_cast<size_t>(std::clamp(static_cast<int64_t>(column.first) + steps,
                                                         int64_t{0}, static_cast<int64_t>(maximum)));
        if (first != column.first) {
            column.first = first;
            tabs.pressed.fill(kNoIndex);
            tabs.hovered.fill(kNoIndex);
            tabs.dirty = true;
            remember(tabs, sync(tabs));
        }
    } catch (...) {
    }
}

sao_status_t detach(Tabs& tabs) noexcept {
    tabs.retiring = true;
    cancel_interaction(tabs);
    for (auto layer : tabs.layers) {
        if (!layer)
            continue;
        const auto status = sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (status != SAO_STATUS_OK)
            return status;
    }
    for (size_t i = 0; i < 2; ++i) {
        if (tabs.layers[i]) {
            const auto status = hide_column(tabs, i);
            if (status != SAO_STATUS_OK)
                return status;
        }
    }
    for (auto& layer : tabs.layers) {
        sao_ui_layer_destroy(layer);
        layer = nullptr;
    }
    return SAO_STATUS_OK;
}

}

extern "C" sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_create(
    sao_ui_compositor_handle_t compositor, sao_ui_plugin_tab_action_fn_t action_fn,
    void* user_data, sao_ui_plugin_tabs_handle_t* out_handle) {
    if (!out_handle)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (!compositor)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto owner = sao_ui_compositor_require_owner_thread(compositor);
    if (owner != SAO_STATUS_OK)
        return owner;
    std::shared_ptr<Tabs> tabs;
    try {
        tabs = std::make_shared<Tabs>();
        tabs->compositor = compositor;
        tabs->action_fn = action_fn;
        tabs->user_data = user_data;
        {
            std::lock_guard lock(g_registry_mutex);
            if (g_next_handle >= std::numeric_limits<uintptr_t>::max() / 16)
                return SAO_STATUS_ERR_UNKNOWN;
            // Non-reused tokens fence callbacks copied before layer retirement.
            tabs->handle = reinterpret_cast<sao_ui_plugin_tabs_handle_t>(++g_next_handle * 16);
            g_registry.emplace(tabs->handle, tabs);
        }
        sao_status_t status = SAO_STATUS_OK;
        for (size_t i = 0; i < 2 && status == SAO_STATUS_OK; ++i) {
            const std::string name = "sao.plugin-tabs." + std::to_string(reinterpret_cast<uintptr_t>(tabs->handle)) + "." + std::to_string(i);
            SaoLayerConfig config{};
            config.struct_size = sizeof(config);
            config.name_utf8 = name.c_str();
            config.width = 1;
            config.height = 1;
            config.z_order = 1000 + static_cast<int32_t>(i);
            config.bgra_swizzle = true;
            status = sao_ui_layer_create(compositor, &config, &tabs->layers[i]);
            if (status == SAO_STATUS_OK)
                status = hide_column(*tabs, i);
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_input_callbacks(tabs->layers[0], &cursor<0>, &leave<0>, &button<0>, &scroll<0>, tabs->handle);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_input_callbacks(tabs->layers[1], &cursor<1>, &leave<1>, &button<1>, &scroll<1>, tabs->handle);
        if (status == SAO_STATUS_OK) {
            *out_handle = tabs->handle;
            return SAO_STATUS_OK;
        }
        const auto cleanup = detach(*tabs);
        if (cleanup != SAO_STATUS_OK) {
            *out_handle = tabs->handle;
            return cleanup;
        }
        std::lock_guard lock(g_registry_mutex);
        g_registry.erase(tabs->handle);
        return status;
    } catch (...) {
        if (tabs && tabs->handle) {
            if (detach(*tabs) != SAO_STATUS_OK)
                *out_handle = tabs->handle;
            else {
                std::lock_guard lock(g_registry_mutex);
                g_registry.erase(tabs->handle);
            }
        }
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_set_items(
    sao_ui_plugin_tabs_handle_t handle, const SaoUiPluginTab* items, size_t count) {
    try {
        const auto tabs = acquire(handle);
        auto status = ready_status(tabs);
        if (status != SAO_STATUS_OK)
            return status;
        std::vector<Tab> candidate;
        status = copy_items(items, count, candidate);
        if (status != SAO_STATUS_OK || candidate == tabs->items)
            return status;
        const size_t selected = tabs->selected < tabs->items.size()
            ? find_tab(candidate, tabs->items[tabs->selected].id) : kNoIndex;
        size_t first = tabs->columns[0].first < tabs->items.size()
            ? find_tab(candidate, tabs->items[tabs->columns[0].first].id) : kNoIndex;
        if (first == kNoIndex)
            first = tabs->columns[0].first;
        size_t child_first = tabs->columns[1].first;
        if (selected < candidate.size() && tabs->selected < tabs->items.size()) {
            const auto& old = tabs->items[tabs->selected].actions;
            if (child_first < old.size()) {
                const int32_t id = old[child_first].id;
                const auto occurrence = static_cast<size_t>(std::count_if(old.begin(), old.begin() + child_first,
                    [id](const Action& action) { return action.id == id; }));
                size_t seen = 0;
                const auto& next = candidate[selected].actions;
                for (size_t i = 0; i < next.size(); ++i) {
                    if (next[i].id == id && seen++ == occurrence) {
                        child_first = i;
                        break;
                    }
                }
            }
        }
        if (tabs->model_revision == std::numeric_limits<uint64_t>::max())
            return SAO_STATUS_ERR_UNKNOWN;
        tabs->items.swap(candidate);
        tabs->selected = selected < tabs->items.size() && tabs->items[selected].enabled ? selected : kNoIndex;
        if (tabs->selected == kNoIndex)
            ++tabs->navigation_revision;
        tabs->columns[0].first = first;
        tabs->columns[1].first = tabs->selected == kNoIndex ? 0 : child_first;
        ++tabs->model_revision;
        cancel_interaction(*tabs);
        tabs->dirty = true;
        return sync(*tabs);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_set_visible(
    sao_ui_plugin_tabs_handle_t handle, bool visible) {
    try {
        const auto tabs = acquire(handle);
        if (tabs && tabs->retiring && !visible) {
            const auto status = owner_status(*tabs);
            if (status != SAO_STATUS_OK)
                return status;
            tabs->requested = false;
            cancel_interaction(*tabs);
            for (size_t i = 0; i < tabs->layers.size(); ++i) {
                if (tabs->layers[i]) {
                    const auto hidden = hide_column(*tabs, i);
                    if (hidden != SAO_STATUS_OK)
                        return hidden;
                }
            }
            return SAO_STATUS_OK;
        }
        const auto status = ready_status(tabs);
        if (status != SAO_STATUS_OK)
            return status;
        if (tabs->requested != visible) {
            tabs->requested = visible;
            tabs->dirty = true;
            if (!visible) {
                cancel_interaction(*tabs);
                ++tabs->navigation_revision;
            }
        }
        return sync(*tabs);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_tick(
    sao_ui_plugin_tabs_handle_t handle, uint32_t elapsed_ms) {
    (void)elapsed_ms;
    try {
        const auto tabs = acquire(handle);
        const auto status = ready_status(tabs);
        if (status != SAO_STATUS_OK)
            return status;
        const auto pending = std::exchange(tabs->pending_status, SAO_STATUS_OK);
        const auto updated = sync(*tabs);
        return updated == SAO_STATUS_OK ? pending : updated;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_get_snapshot(
    sao_ui_plugin_tabs_handle_t handle, SaoUiPluginTabsSnapshot* out_snapshot) {
    if (!out_snapshot)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_snapshot->struct_size < sizeof(SaoUiPluginTabsSnapshot))
        return SAO_STATUS_ERR_ABI_MISMATCH;
    try {
        const auto tabs = acquire(handle);
        if (!tabs)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const auto status = owner_status(*tabs);
        if (status != SAO_STATUS_OK)
            return status;
        SaoUiPluginTabsSnapshot snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        snapshot.x = tabs->columns[0].x;
        snapshot.y = tabs->columns[0].y;
        snapshot.width = tabs->columns[0].width;
        snapshot.height = tabs->columns[0].height;
        snapshot.tab_count = tabs->items.size();
        snapshot.first_visible = tabs->columns[0].first;
        snapshot.visible = !tabs->retiring && tabs->requested && !tabs->items.empty() && tabs->shown[0];
        snapshot.dragging = tabs->dragging;
        snapshot.last_action_status = tabs->last_action_status;
        if (tabs->selected < tabs->items.size()) {
            const auto& id = tabs->items[tabs->selected].id;
            std::memcpy(snapshot.selected_plugin_id_utf8, id.c_str(), id.size() + 1);
        }
        *out_snapshot = snapshot;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_try_destroy(sao_ui_plugin_tabs_handle_t handle) {
    if (!handle)
        return SAO_STATUS_OK;
    try {
        const auto tabs = acquire(handle);
        if (!tabs)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        auto status = owner_status(*tabs);
        if (status != SAO_STATUS_OK)
            return status;
        if (tabs->callback_depth || tabs->publishing)
            return SAO_UI_STATUS_ERR_BUSY;
        status = sao_ui_compositor_destroy_preflight(tabs->compositor);
        if (status != SAO_STATUS_OK)
            return status;
        status = detach(*tabs);
        if (status != SAO_STATUS_OK)
            return status;
        std::lock_guard lock(g_registry_mutex);
        g_registry.erase(handle);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
