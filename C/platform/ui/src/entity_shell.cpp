#include "sao/ui/entity_shell.h"

#include "sao/ui/compositor.h"
#include "sao/ui/menu.h"
#include "sao/ui/theme.h"

#include "menu_visual_internal.h"

#if defined(_WIN32)
#include "entity_authority_frames.h"
#include "entity_text_renderer_win.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_tick(sao_ui_menu_handle_t handle, int32_t dt_ms);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_get_transition_progress(sao_ui_menu_handle_t handle,
                                                                        float* out_progress);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_tick(sao_ui_nervegear_handle_t handle,
                                                          int32_t dt_ms);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_hit_test(sao_ui_nervegear_handle_t handle,
                                                              int32_t px, int32_t py,
                                                              bool* out_hit);
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_enter(sao_ui_nervegear_handle_t handle);
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_leave(sao_ui_nervegear_handle_t handle);
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_on_mouse_down(sao_ui_nervegear_handle_t handle);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_on_mouse_up(sao_ui_nervegear_handle_t handle);
sao_status_t SAO_UI_CALL sao_ui_compositor_current_input_position(float* out_layer_x,
                                                                  float* out_layer_y);
}
namespace {

constexpr int32_t kMarginRight = 20;
constexpr int32_t kMarginBottom = 20;
constexpr int32_t kMenuWidth = 442;
constexpr int32_t kMenuHeight = 430;
constexpr int32_t kMenuSlot = 70;
constexpr int32_t kMenuPad = 40;
constexpr int32_t kMenuColumnCenter = kMenuPad + kMenuSlot / 2;
constexpr int32_t kRootItemCount = 5;
constexpr size_t kMaxChildrenPerRoot = 256;
constexpr size_t kMaxTotalChildren = 1024;
constexpr size_t kMaxTreeUtf8Bytes = 64U * 1024U;
constexpr size_t kMaxLabelBytes = 4096;
constexpr size_t kMaxIconBytes = 256;
constexpr int32_t kChildOriginX = 135;
constexpr int32_t kChildOriginY = 40;
constexpr int32_t kChildLineCenterX = 140;
constexpr int32_t kChildRowX = 162;
constexpr int32_t kChildRowHeight = 44;
constexpr int32_t kChildRowStride = 47;
constexpr int32_t kChildTargetWidth = 240;
constexpr int32_t kChildPhysicalCapacity =
    (kMenuHeight - kChildOriginY - kChildRowHeight) / kChildRowStride + 1;
constexpr int32_t kChildIconFontSize = 12;
constexpr int32_t kChildLabelFontSize = 10;
constexpr int32_t kChildFallbackIconWidth = 12;
constexpr int32_t kChildIconGap = 5;
constexpr int32_t kChildRowPadRight = 8;
constexpr int32_t kChildCaretWidth = 12;
constexpr int32_t kChildCaretGap = 3;
constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kMouseWheel = 0x020A;
constexpr uint32_t kMouseLeave = 0x02A3;
constexpr int32_t kWheelDeltaPerNotch = 120;

static_assert(kChildPhysicalCapacity == 8);

// Convert a desktop-space screen coordinate into a host-local coordinate,
// dividing by host_dpi/96 before subtracting the host origin. Windows exposes
// a single scalar DPI per top-level HWND, so both axes share host_dpi. When
// host_dpi is the standard 96 this reduces to a pure subtraction and cannot
// regress unscaled callers. host_dpi == 0 is coerced to 96 for safety.
inline int64_t screen_to_host_dpi(int32_t screen_pt, int32_t host_origin,
                                  uint32_t host_dpi) noexcept {
    const uint32_t dpi = host_dpi == 0u ? 96u : host_dpi;
    // screen_pt and host_origin share the desktop physical coordinate space.
    // Subtract in physical space first to get the host-local delta, then
    // scale that delta down to host logical pixels. Integer math avoids
    // floating drift on 120/144 DPI monitors and never touches signed
    // overflow because desktop coordinates fit in int32.
    const int64_t desktop_local =
        static_cast<int64_t>(screen_pt) - static_cast<int64_t>(host_origin);
    if (dpi == 96u) {
        return desktop_local;
    }
    return desktop_local * static_cast<int64_t>(96) /
           static_cast<int64_t>(dpi);
}

struct OwnedMenuItem {
    std::string name;
    std::string icon;
    int32_t action_id{-1};
    bool can_activate{};
};

struct OwnedRootItem {
    std::string id;
    std::string name;
    std::string icon;
    int32_t action_id{-1};
    bool can_activate{};
    std::vector<OwnedMenuItem> children;
};

bool operator==(const OwnedMenuItem& left, const OwnedMenuItem& right) {
    return left.name == right.name && left.icon == right.icon &&
           left.action_id == right.action_id && left.can_activate == right.can_activate;
}

bool operator==(const OwnedRootItem& left, const OwnedRootItem& right) {
    return left.id == right.id && left.name == right.name && left.icon == right.icon &&
           left.action_id == right.action_id && left.can_activate == right.can_activate &&
           left.children == right.children;
}

std::vector<OwnedRootItem> make_default_roots(bool nervgear_mode) {
    std::vector<OwnedRootItem> roots;
    roots.reserve(kRootItemCount);
    roots.push_back({
        "Control",
        "Control",
        "C",
        10,
        true,
        {
            {"置顶: OFF", "⬆", SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, false},
            {nervgear_mode ? "NervGear: ON" : "NervGear: OFF", "◈",
             SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR, true},
            {"──────────", "─", -1, false},
            {"Streaming Mode: OFF", "◈", SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE, true},
            {"鱼眼背景: 程序生成", "◆", SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL, false},
            {"鱼眼背景: 实时截屏", "◇", SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE, false},
            {"──────────", "─", -1, false},
            {"保存设置", "✓", SAO_UI_ENTITY_ACTION_SAVE_SETTINGS, true},
        },
    });
    roots.push_back({
        "Tools",
        "Tools",
        "T",
        11,
        true,
        {
            {"AI Editor (LLM)", "✦", SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, true},
            {"Workshop", "◇", SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, false},
            {"Process Selector", "⚙", SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR, false},
        },
    });
    roots.push_back({
        "Plugins",
        "Plugins",
        "P",
        12,
        true,
        {
            {"插件管理面板 Manage", "⚙", SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER, false},
            {"重载全部插件 Reload", "↻", SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, true},
            {"无已启用面板插件 (去 Manage 启用)", "·", -1, false},
        },
    });
    roots.push_back({
        "Skins",
        "Skins",
        "S",
        13,
        true,
        {
            {"全部 Light", "🎨", SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, true},
            {"全部 Dark", "🌙", SAO_UI_ENTITY_ACTION_SET_ALL_DARK, true},
        },
    });
    roots.push_back({"About", "About", "?", SAO_UI_ENTITY_ACTION_OPEN_ABOUT, true, {}});
    return roots;
}

bool is_default_root_view(const std::vector<OwnedRootItem>& roots, size_t first_visible) {
    if (first_visible != 0 || roots.size() != static_cast<size_t>(kRootItemCount))
        return false;
    const auto defaults = make_default_roots(true);
    for (size_t index = 0; index < defaults.size(); ++index) {
        if (roots[index].id != defaults[index].id || roots[index].name != defaults[index].name ||
            roots[index].icon != defaults[index].icon ||
            roots[index].action_id != defaults[index].action_id ||
            roots[index].can_activate != defaults[index].can_activate) {
            return false;
        }
    }
    return true;
}

bool valid_utf8(std::string_view text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<uint8_t>(text[offset]);
        if (first < 0x80U) {
            ++offset;
            continue;
        }
        size_t length = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
            value = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
            value = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
            value = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (length > text.size() - offset)
            return false;
        for (size_t index = 1; index < length; ++index) {
            const auto continuation = static_cast<uint8_t>(text[offset + index]);
            if ((continuation & 0xC0U) != 0x80U)
                return false;
            value = (value << 6U) | (continuation & 0x3FU);
        }
        if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
            return false;
        }
        offset += length;
    }
    return true;
}

sao_status_t copy_validated_utf8(const char* input, size_t max_bytes, bool require_nonempty,
                                 size_t* total_bytes, std::string* output) {
    if (input == nullptr || total_bytes == nullptr || output == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    size_t length = 0;
    while (length <= max_bytes && input[length] != '\0')
        ++length;
    if (length > max_bytes || (require_nonempty && length == 0) ||
        length > kMaxTreeUtf8Bytes - *total_bytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::string_view text(input, length);
    if (!valid_utf8(text))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    output->assign(text);
    *total_bytes += length;
    return SAO_STATUS_OK;
}

sao_status_t add_tree_bytes(size_t bytes, size_t* total_bytes) {
    if (total_bytes == nullptr || bytes > kMaxTreeUtf8Bytes - *total_bytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *total_bytes += bytes;
    return SAO_STATUS_OK;
}

sao_status_t measure_tree_without_children(const std::vector<OwnedRootItem>& roots,
                                           size_t excluded_root_index, size_t* total_children,
                                           size_t* total_bytes) {
    if (total_children == nullptr || total_bytes == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *total_children = 0;
    *total_bytes = 0;
    for (size_t root_index = 0; root_index < roots.size(); ++root_index) {
        const auto& root = roots[root_index];
        sao_status_t status = add_tree_bytes(root.id.size(), total_bytes);
        status = status == SAO_STATUS_OK ? add_tree_bytes(root.name.size(), total_bytes) : status;
        status = status == SAO_STATUS_OK ? add_tree_bytes(root.icon.size(), total_bytes) : status;
        if (status != SAO_STATUS_OK)
            return status;
        if (root_index == excluded_root_index)
            continue;
        if (root.children.size() > kMaxTotalChildren - *total_children)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *total_children += root.children.size();
        for (const auto& child : root.children) {
            status = add_tree_bytes(child.name.size(), total_bytes);
            status =
                status == SAO_STATUS_OK ? add_tree_bytes(child.icon.size(), total_bytes) : status;
            if (status != SAO_STATUS_OK)
                return status;
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t build_root_candidate(const SaoUiEntityRootItem* roots, size_t root_count,
                                  std::vector<OwnedRootItem>* output) {
    if (output == nullptr || (roots == nullptr && root_count > 0) ||
        root_count > SAO_UI_ENTITY_ROOT_MAX_COUNT) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<OwnedRootItem> candidate;
        candidate.reserve(root_count);
        size_t total_bytes = 0;
        size_t total_children = 0;
        for (size_t root_index = 0; root_index < root_count; ++root_index) {
            const auto& source = roots[root_index];
            // SaoUiEntityRootItem has no per-element stride field, so we
            // require exact size equality. SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE
            // and sizeof(SaoUiEntityRootItem) are compile-time locked equal.
            if (source.struct_size != SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE)
                return SAO_STATUS_ERR_ABI_MISMATCH;
            if ((source.children == nullptr && source.child_count > 0) ||
                source.child_count > kMaxChildrenPerRoot ||
                source.child_count > kMaxTotalChildren - total_children) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            OwnedRootItem root{};
            sao_status_t status =
                copy_validated_utf8(source.root_id_utf8, SAO_UI_ENTITY_ROOT_ID_CAPACITY - 1U, true,
                                    &total_bytes, &root.id);
            if (status == SAO_STATUS_OK) {
                status = copy_validated_utf8(source.name_utf8, kMaxLabelBytes, true, &total_bytes,
                                             &root.name);
            }
            if (status == SAO_STATUS_OK) {
                status = copy_validated_utf8(source.icon_utf8, kMaxIconBytes, false, &total_bytes,
                                             &root.icon);
            }
            if (status != SAO_STATUS_OK)
                return status;
            if (std::any_of(candidate.begin(), candidate.end(),
                            [&root](const auto& existing) { return existing.id == root.id; })) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
            if (std::any_of(candidate.begin(), candidate.end(),
                            [&root](const auto& existing) { return existing.name == root.name; })) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
            root.action_id = source.action_id;
            root.can_activate = source.can_activate;
            root.children.reserve(source.child_count);
            for (size_t child_index = 0; child_index < source.child_count; ++child_index) {
                const auto& child_source = source.children[child_index];
                OwnedMenuItem child{};
                status = copy_validated_utf8(child_source.name_utf8, kMaxLabelBytes, true,
                                             &total_bytes, &child.name);
                if (status == SAO_STATUS_OK) {
                    status = copy_validated_utf8(child_source.icon_utf8, kMaxIconBytes, false,
                                                 &total_bytes, &child.icon);
                }
                if (status != SAO_STATUS_OK)
                    return status;
                child.action_id = child_source.action_id;
                child.can_activate = child_source.can_activate;
                root.children.push_back(std::move(child));
            }
            total_children += source.child_count;
            candidate.push_back(std::move(root));
        }
        *output = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

struct Pixel {
    uint8_t b{};
    uint8_t g{};
    uint8_t r{};
    uint8_t a{};
};

static_assert(sizeof(Pixel) == 4U);

struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct Raster {
    uint32_t width{};
    uint32_t height{};
    std::vector<Pixel> pixels;
};

enum class BlendRounding {
    Nearest,
    Floor,
};

constexpr BlendRounding kChildBlendRounding = BlendRounding::Floor;

Pixel premultiply(Color color, BlendRounding rounding = BlendRounding::Nearest) {
    const uint32_t bias = rounding == BlendRounding::Nearest ? 127U : 0U;
    return {
        static_cast<uint8_t>((static_cast<uint32_t>(color.b) * color.a + bias) / 255U),
        static_cast<uint8_t>((static_cast<uint32_t>(color.g) * color.a + bias) / 255U),
        static_cast<uint8_t>((static_cast<uint32_t>(color.r) * color.a + bias) / 255U),
        color.a,
    };
}

void blend(Raster& raster, int32_t x, int32_t y, Color color,
           BlendRounding rounding = BlendRounding::Nearest) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return;
    }
    const Pixel source = premultiply(color, rounding);
    Pixel& destination = raster.pixels[static_cast<size_t>(y) * raster.width + x];
    const uint32_t inverse = 255U - source.a;
    const uint32_t bias = rounding == BlendRounding::Nearest ? 127U : 0U;
    destination.b = static_cast<uint8_t>(
        source.b + (static_cast<uint32_t>(destination.b) * inverse + bias) / 255U);
    destination.g = static_cast<uint8_t>(
        source.g + (static_cast<uint32_t>(destination.g) * inverse + bias) / 255U);
    destination.r = static_cast<uint8_t>(
        source.r + (static_cast<uint32_t>(destination.r) * inverse + bias) / 255U);
    destination.a = static_cast<uint8_t>(
        source.a + (static_cast<uint32_t>(destination.a) * inverse + bias) / 255U);
}

void fill_rect(Raster& raster, int32_t x, int32_t y, int32_t width, int32_t height, Color color,
               BlendRounding rounding = BlendRounding::Nearest) {
    const int32_t left = std::max(0, x);
    const int32_t top = std::max(0, y);
    const int32_t right = std::min(static_cast<int32_t>(raster.width), x + width);
    const int32_t bottom = std::min(static_cast<int32_t>(raster.height), y + height);
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px)
            blend(raster, px, py, color, rounding);
    }
}

bool rounded_rect_contains(int32_t px, int32_t py, int32_t x, int32_t y, int32_t width,
                           int32_t height, int32_t radius) {
    if (px < x || py < y || px >= x + width || py >= y + height)
        return false;
    const int32_t clamped_radius = std::clamp(radius, 0, std::min(width, height) / 2);
    if (clamped_radius == 0 ||
        (px >= x + clamped_radius && px < x + width - clamped_radius) ||
        (py >= y + clamped_radius && py < y + height - clamped_radius)) {
        return true;
    }
    const int32_t center_x = px < x + clamped_radius ? x + clamped_radius - 1
                                                     : x + width - clamped_radius;
    const int32_t center_y = py < y + clamped_radius ? y + clamped_radius - 1
                                                     : y + height - clamped_radius;
    const int32_t dx = px - center_x;
    const int32_t dy = py - center_y;
    return dx * dx + dy * dy <= clamped_radius * clamped_radius;
}

void fill_rounded_rect(Raster& raster, int32_t x, int32_t y, int32_t width, int32_t height,
                       int32_t radius, Color color,
                       BlendRounding rounding = BlendRounding::Nearest) {
    for (int32_t py = std::max(0, y); py < std::min(static_cast<int32_t>(raster.height), y + height);
         ++py) {
        for (int32_t px = std::max(0, x);
             px < std::min(static_cast<int32_t>(raster.width), x + width); ++px) {
            if (rounded_rect_contains(px, py, x, y, width, height, radius))
                blend(raster, px, py, color, rounding);
        }
    }
}

void stroke_rounded_rect(Raster& raster, int32_t x, int32_t y, int32_t width, int32_t height,
                         int32_t radius, int32_t thickness, Color color,
                         BlendRounding rounding = BlendRounding::Nearest) {
    for (int32_t inset = 0; inset < thickness; ++inset) {
        const int32_t inner_x = x + inset;
        const int32_t inner_y = y + inset;
        const int32_t inner_width = width - inset * 2;
        const int32_t inner_height = height - inset * 2;
        if (inner_width <= 0 || inner_height <= 0)
            break;
        for (int32_t py = std::max(0, inner_y);
             py < std::min(static_cast<int32_t>(raster.height), inner_y + inner_height); ++py) {
            for (int32_t px = std::max(0, inner_x);
                 px < std::min(static_cast<int32_t>(raster.width), inner_x + inner_width); ++px) {
                if (!rounded_rect_contains(px, py, inner_x, inner_y, inner_width, inner_height,
                                           std::max(0, radius - inset))) {
                    continue;
                }
                if (rounded_rect_contains(px, py, inner_x + 1, inner_y + 1, inner_width - 2,
                                          inner_height - 2, std::max(0, radius - inset - 1))) {
                    continue;
                }
                blend(raster, px, py, color, rounding);
            }
        }
    }
}

void fill_circle(Raster& raster, int32_t center_x, int32_t center_y, int32_t radius, Color color,
                 BlendRounding rounding = BlendRounding::Nearest) {
    const int32_t radius_squared = radius * radius;
    for (int32_t y = center_y - radius; y <= center_y + radius; ++y) {
        for (int32_t x = center_x - radius; x <= center_x + radius; ++x) {
            const int32_t dx = x - center_x;
            const int32_t dy = y - center_y;
            if (dx * dx + dy * dy <= radius_squared)
                blend(raster, x, y, color, rounding);
        }
    }
}

void stroke_circle(Raster& raster, int32_t center_x, int32_t center_y, int32_t radius,
                   int32_t thickness, Color color,
                   BlendRounding rounding = BlendRounding::Nearest) {
    const int32_t outer = radius * radius;
    const int32_t inner_radius = std::max(0, radius - thickness);
    const int32_t inner = inner_radius * inner_radius;
    for (int32_t y = center_y - radius; y <= center_y + radius; ++y) {
        for (int32_t x = center_x - radius; x <= center_x + radius; ++x) {
            const int32_t dx = x - center_x;
            const int32_t dy = y - center_y;
            const int32_t distance = dx * dx + dy * dy;
            if (distance <= outer && distance >= inner)
                blend(raster, x, y, color, rounding);
        }
    }
}

void draw_line(Raster& raster, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness,
               Color color, BlendRounding rounding = BlendRounding::Nearest) {
    const int32_t steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (steps == 0) {
        fill_circle(raster, x0, y0, std::max(1, thickness / 2), color, rounding);
        return;
    }
    for (int32_t step = 0; step <= steps; ++step) {
        const float ratio = static_cast<float>(step) / steps;
        const int32_t x =
            static_cast<int32_t>(std::lround(x0 + static_cast<float>(x1 - x0) * ratio));
        const int32_t y =
            static_cast<int32_t>(std::lround(y0 + static_cast<float>(y1 - y0) * ratio));
        fill_circle(raster, x, y, std::max(1, thickness / 2), color, rounding);
    }
}

using Glyph = std::array<uint8_t, 7>;

constexpr std::array<Glyph, 26> kUppercaseGlyphs{{
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
}};

constexpr Glyph kQuestionGlyph{0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
constexpr Glyph kDotGlyph{0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};
constexpr Glyph kBlankGlyph{};

const Glyph& glyph_for(unsigned char character) {
    if (character >= 'a' && character <= 'z')
        character = static_cast<unsigned char>(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z') {
        return kUppercaseGlyphs[character - 'A'];
    }
    if (character == '?')
        return kQuestionGlyph;
    return character == '.' ? kDotGlyph : kBlankGlyph;
}

bool next_utf8_code_point(std::string_view text, size_t* offset, uint32_t* code_point) {
    if (offset == nullptr || code_point == nullptr || *offset >= text.size())
        return false;
    const auto first = static_cast<uint8_t>(text[*offset]);
    if (first < 0x80U) {
        *code_point = first;
        ++*offset;
        return true;
    }

    size_t length = 0;
    uint32_t value = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        length = 2;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        *code_point = '?';
        do {
            ++*offset;
        } while (*offset < text.size() && (static_cast<uint8_t>(text[*offset]) & 0xC0U) == 0x80U);
        return true;
    }
    if (length > text.size() - *offset) {
        *code_point = '?';
        do {
            ++*offset;
        } while (*offset < text.size() && (static_cast<uint8_t>(text[*offset]) & 0xC0U) == 0x80U);
        return true;
    }
    for (size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<uint8_t>(text[*offset + index]);
        if ((continuation & 0xC0U) != 0x80U) {
            *code_point = '?';
            *offset += index;
            return true;
        }
        value = (value << 6U) | (continuation & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
        *code_point = '?';
        *offset += length;
        return true;
    }
    *code_point = value;
    *offset += length;
    return true;
}

void draw_text(Raster& raster, int32_t x, int32_t y, std::string_view text, int32_t scale,
               Color color, BlendRounding rounding = BlendRounding::Nearest);

void draw_text_clipped(Raster& raster, int32_t x, int32_t y, int32_t max_width,
                       std::string_view text, int32_t scale, Color color,
                       BlendRounding rounding = BlendRounding::Nearest, bool ellipsis = false);

Color lerp_rgb(Color from, Color to, float amount, uint8_t alpha) {
    const double t = std::clamp(static_cast<double>(amount), 0.0, 1.0);
    const auto channel = [t](uint8_t start, uint8_t end) {
        return static_cast<uint8_t>(static_cast<int32_t>(
            static_cast<double>(start) +
            (static_cast<double>(end) - static_cast<double>(start)) * t));
    };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b), alpha};
}

uint8_t scaled_alpha(double base, double opacity) {
    return static_cast<uint8_t>(
        std::clamp(static_cast<int32_t>(base * opacity), 0, 255));
}

void draw_child_overlay(Raster& raster, const sao::ui::menu_visual::Snapshot& snapshot,
                        size_t first_visible_child_index) {
    constexpr Color kChildBackground{248, 248, 248, 255};
    constexpr Color kChildHover{244, 238, 225, 255};
    constexpr Color kChildText{100, 99, 100, 255};
    constexpr Color kChildHoverText{98, 88, 70, 255};
    constexpr Color kChildIcon{143, 149, 155, 255};
    constexpr Color kActiveBorder{243, 175, 18, 255};

    if (snapshot.active_root_idx >= 0 && snapshot.active_root_idx < kRootItemCount) {
        const int32_t center_y = kMenuPad + snapshot.active_root_idx * kMenuSlot + kMenuSlot / 2;
        stroke_circle(raster, kMenuColumnCenter, center_y, 31, 2,
                      Color{kActiveBorder.r, kActiveBorder.g, kActiveBorder.b, 235},
                      kChildBlendRounding);
    }

    const size_t available_row_count =
        first_visible_child_index < snapshot.rows.size()
            ? snapshot.rows.size() - first_visible_child_index
            : 0U;
    const size_t visible_row_count =
        std::min(available_row_count, static_cast<size_t>(kChildPhysicalCapacity));
    const int32_t row_count = static_cast<int32_t>(visible_row_count);
    if (row_count <= 0)
        return;
#if defined(_WIN32)
    std::vector<sao::ui::entity_text::TextCommand> text_commands;
    text_commands.reserve(static_cast<size_t>(row_count) * 2U);
#endif
    const double opacity = 1.0 - std::clamp(static_cast<double>(snapshot.fade_t), 0.0, 1.0);
    const int32_t line_height = row_count * kChildRowStride - 3;
    const int32_t line_top = kChildOriginY + 5;
    const uint8_t line_alpha = scaled_alpha(210.0, opacity);
    draw_line(raster, kChildLineCenterX, line_top + 5, kChildLineCenterX,
              line_top + line_height - 5, 4, Color{212, 208, 208, line_alpha},
              kChildBlendRounding);
    draw_line(raster, kChildLineCenterX, line_top + 5, kChildLineCenterX,
              line_top + line_height - 5, 2, Color{156, 153, 153, line_alpha},
              kChildBlendRounding);
    fill_circle(raster, kChildLineCenterX, line_top + 5, 2,
                Color{176, 176, 176, line_alpha}, kChildBlendRounding);
    fill_circle(raster, kChildLineCenterX, line_top + line_height - 5, 2,
                Color{176, 176, 176, line_alpha}, kChildBlendRounding);

    constexpr int32_t kArrowCenterX = kChildOriginX + 10 + 3 + 6;
    const int32_t arrow_center_y = kChildOriginY + 5 + line_height / 2;
    for (int32_t glow_radius = 6; glow_radius > 0; glow_radius -= 2) {
        const int32_t glow_amount = static_cast<int32_t>(15.0 * (1.0 - glow_radius / 6.0));
        fill_circle(raster, kArrowCenterX, arrow_center_y, glow_radius,
                    Color{static_cast<uint8_t>(static_cast<int32_t>(glow_amount * 3.5) & 0xFF),
                          static_cast<uint8_t>(static_cast<int32_t>(glow_amount * 2.2) & 0xFF),
                          static_cast<uint8_t>(static_cast<int32_t>(glow_amount * 0.3) & 0xFF),
                          scaled_alpha(150.0, opacity)},
                    kChildBlendRounding);
    }
    fill_circle(raster, kArrowCenterX, arrow_center_y, 3,
                Color{201, 184, 150, scaled_alpha(220.0, opacity)}, kChildBlendRounding);
    stroke_circle(raster, kArrowCenterX, arrow_center_y, 3, 1,
                  Color{212, 200, 168, scaled_alpha(220.0, opacity)}, kChildBlendRounding);

    for (int32_t slot = 0; slot < row_count; ++slot) {
        const size_t logical_index = first_visible_child_index + static_cast<size_t>(slot);
        const auto& row = snapshot.rows[logical_index];
        const int32_t row_width = std::clamp(row.visible_width_px, 0, kChildTargetWidth);
        if (row_width <= 1)
            continue;
        const float hover = std::clamp(row.hover_t, 0.0F, 1.0F);
        const int32_t row_y = kChildOriginY + slot * kChildRowStride;
        const int32_t radius = std::min({8, std::max(2, kChildRowHeight / 4),
                                         std::max(2, row_width / 2)});
        Color background = lerp_rgb(kChildBackground, kChildHover, hover,
                                    scaled_alpha(218.0 + 18.0 * hover, opacity));
        Color foreground = lerp_rgb(kChildText, kChildHoverText, hover,
                                    scaled_alpha(255.0, opacity));
        Color icon = lerp_rgb(kChildIcon, kChildHoverText, hover,
                              scaled_alpha(245.0, opacity));
        Color indicator = lerp_rgb(kChildBackground, kActiveBorder, hover,
                                   scaled_alpha(235.0, opacity));
        fill_rounded_rect(raster, kChildRowX, row_y, row_width, kChildRowHeight, radius,
                          background, kChildBlendRounding);
        stroke_rounded_rect(raster, kChildRowX, row_y, row_width, kChildRowHeight, radius, 1,
                            Color{kActiveBorder.r, kActiveBorder.g, kActiveBorder.b,
                                  scaled_alpha(72.0 * hover, opacity)},
                            kChildBlendRounding);
        fill_rounded_rect(raster, kChildRowX, row_y, 2, kChildRowHeight, 2, indicator,
                          kChildBlendRounding);

        const int32_t icon_x = kChildRowX + 2 + 8;
        const int32_t icon_y = row_y + (kChildRowHeight - kChildIconFontSize) / 2 - 2;
        const int32_t label_x = icon_x + kChildFallbackIconWidth + kChildIconGap;
        const int32_t label_y = row_y + (kChildRowHeight - kChildLabelFontSize) / 2 - 2;
        const int32_t caret_x = kChildRowX + row_width - kChildRowPadRight - 6;
        const int32_t label_max_width =
            std::max(0, caret_x - kChildCaretWidth - kChildCaretGap - label_x);
        const std::string_view icon_text(row.icon_utf8.data());
#if defined(_WIN32)
        if (!icon_text.empty()) {
            text_commands.push_back({icon_x,
                                     icon_y,
                                     kChildFallbackIconWidth,
                                     kChildRowHeight,
                                     std::string(icon_text),
                                     sao::ui::entity_text::FontRole::Icon,
                                     static_cast<float>(kChildIconFontSize),
                                     {icon.r, icon.g, icon.b, icon.a},
                                     false});
        }
        const std::string_view label_text(row.name_utf8.data());
        if (!label_text.empty() && label_max_width > 4) {
            text_commands.push_back({label_x,
                                     label_y,
                                     label_max_width,
                                     kChildRowHeight,
                                     std::string(label_text),
                                     sao::ui::entity_text::FontRole::Label,
                                     static_cast<float>(kChildLabelFontSize),
                                     {foreground.r, foreground.g, foreground.b, foreground.a},
                                     true});
        }
#else
        if (!icon_text.empty()) {
            draw_text_clipped(raster, icon_x, icon_y, kChildFallbackIconWidth, icon_text, 1, icon,
                              kChildBlendRounding);
        }
        if (label_max_width > 4) {
            draw_text_clipped(raster, label_x, label_y, label_max_width,
                              std::string_view(row.name_utf8.data()), 1, foreground,
                              kChildBlendRounding, true);
        }
#endif
        if (hover > 0.05F && row_width >= 18) {
            const Color caret = lerp_rgb(kChildBackground, kChildHoverText, hover,
                                         scaled_alpha(255.0, opacity));
            const int32_t caret_y = row_y + 15;
            draw_line(raster, caret_x, caret_y, caret_x + 3, caret_y + 3, 1, caret,
                      kChildBlendRounding);
            draw_line(raster, caret_x + 3, caret_y + 3, caret_x, caret_y + 6, 1, caret,
                      kChildBlendRounding);
        }
    }
#if defined(_WIN32)
    const sao::ui::entity_text::BgraSurface surface{
        reinterpret_cast<uint8_t*>(raster.pixels.data()), raster.width, raster.height,
        raster.width * sizeof(Pixel)};
    if (!sao::ui::entity_text::render_text(surface, text_commands)) {
        for (const auto& command : text_commands) {
            draw_text_clipped(raster, command.x, command.y, command.max_width, command.utf8, 1,
                              {command.color.r, command.color.g, command.color.b, command.color.a},
                              kChildBlendRounding, command.ellipsis);
        }
    }
#endif
}

void draw_text(Raster& raster, int32_t x, int32_t y, std::string_view text, int32_t scale,
               Color color, BlendRounding rounding) {
    draw_text_clipped(raster, x, y, std::numeric_limits<int32_t>::max(), text, scale, color,
                      rounding);
}

void draw_text_clipped(Raster& raster, int32_t x, int32_t y, int32_t max_width,
                       std::string_view text, int32_t scale, Color color, BlendRounding rounding,
                       bool ellipsis) {
    if (scale <= 0 || max_width <= 0)
        return;
    const int64_t glyph_width = static_cast<int64_t>(scale) * 5;
    const int64_t advance = static_cast<int64_t>(scale) * 6;
    if (glyph_width > max_width)
        return;
    const size_t capacity = static_cast<size_t>(
        1 + (static_cast<int64_t>(max_width) - glyph_width) / advance);
    std::vector<uint32_t> code_points;
    code_points.reserve(text.size());
    size_t offset = 0;
    uint32_t code_point = 0;
    while (next_utf8_code_point(text, &offset, &code_point))
        code_points.push_back(code_point);

    const bool clipped = code_points.size() > capacity;
    const size_t dot_count = ellipsis && clipped ? std::min<size_t>(3U, capacity) : 0U;
    const size_t text_count = clipped ? capacity - dot_count : code_points.size();
    int32_t cursor = x;
    const auto draw_code_point = [&](uint32_t value) {
        const unsigned char character =
            value <= 0x7FU ? static_cast<unsigned char>(value) : '?';
        const Glyph& glyph = glyph_for(character);
        for (int32_t row = 0; row < static_cast<int32_t>(glyph.size()); ++row) {
            for (int32_t column = 0; column < 5; ++column) {
                if ((glyph[row] & (1U << (4 - column))) != 0U) {
                    fill_rect(raster, cursor + column * scale, y + row * scale, scale, scale, color,
                              rounding);
                }
            }
        }
        cursor += scale * 6;
    };
    for (size_t index = 0; index < text_count; ++index)
        draw_code_point(code_points[index]);
    for (size_t index = 0; index < dot_count; ++index)
        draw_code_point('.');
}

Raster make_raster(uint32_t width, uint32_t height) {
    return {width, height, std::vector<Pixel>(static_cast<size_t>(width) * height)};
}

#if defined(_WIN32)
Raster load_authority_frame(int32_t resource_id, uint32_t width, uint32_t height) {
    static int module_anchor = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_anchor), &module)) {
        throw std::runtime_error("UI module handle unavailable");
    }
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) {
        throw std::runtime_error("Entity authority resource unavailable");
    }
    const DWORD resource_bytes = SizeofResource(module, resource);
    const size_t expected_bytes = static_cast<size_t>(width) * height * sizeof(Pixel);
    if (resource_bytes != expected_bytes) {
        throw std::runtime_error("Entity authority resource size mismatch");
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* bytes = loaded == nullptr ? nullptr : LockResource(loaded);
    if (bytes == nullptr) {
        throw std::runtime_error("Entity authority resource load failed");
    }
    Raster raster = make_raster(width, height);
    std::memcpy(raster.pixels.data(), bytes, expected_bytes);
    return raster;
}
#endif

Raster rasterize_nervegear(SaoUiNerveGearState state) {
#if defined(_WIN32)
    const int32_t resource_id =
        state == SAO_UI_NG_STATE_PRESSED ? SAO_UI_ENTITY_AUTHORITY_NERVEGEAR_PRESSED
        : state == SAO_UI_NG_STATE_HOVER ? SAO_UI_ENTITY_AUTHORITY_NERVEGEAR_HOVER
                                         : SAO_UI_ENTITY_AUTHORITY_NERVEGEAR_IDLE;
    return load_authority_frame(resource_id, SAO_UI_NERVEGEAR_SIZE, SAO_UI_NERVEGEAR_SIZE);
#else
    Raster raster = make_raster(SAO_UI_NERVEGEAR_SIZE, SAO_UI_NERVEGEAR_SIZE);
    const bool hover = state == SAO_UI_NG_STATE_HOVER;
    const bool pressed = state == SAO_UI_NG_STATE_PRESSED;
    const Color cyan{104, 228, 255, 242};
    const Color cyan_soft{77, 232, 244, static_cast<uint8_t>(hover ? 185 : 135)};
    const Color gold{212, 156, 23, 242};
    const Color body = pressed ? Color{4, 11, 17, 242}
                       : hover ? Color{13, 36, 46, 242}
                               : Color{8, 18, 27, 242};
    fill_circle(raster, 36, 36, 30, body);
    stroke_circle(raster, 36, 36, 30, pressed ? 4 : 3, cyan);
    stroke_circle(raster, 36, 36, 22, 2, cyan_soft);
    draw_line(raster, 19, 53, 53, 53, pressed ? 4 : 3, gold);
    draw_line(raster, 36, 21, 47, 32, 2, cyan);
    draw_line(raster, 47, 32, 36, 43, 2, cyan);
    draw_line(raster, 36, 43, 25, 32, 2, gold);
    draw_line(raster, 25, 32, 36, 21, 2, gold);
    draw_line(raster, 22, 18, 45, 11, 2, Color{255, 255, 255, 95});
    if (hover)
        stroke_circle(raster, 36, 36, 33, 2, Color{104, 228, 255, 130});
    return raster;
#endif
}

Raster rasterize_generic_menu(int32_t hover_index, int32_t pressed_index,
                              const std::vector<OwnedRootItem>& roots,
                              size_t first_visible_root_index) {
    Raster raster = make_raster(kMenuWidth, kMenuHeight);
    fill_rect(raster, 15, 15, kMenuWidth - 30, kMenuHeight - 30, Color{248, 248, 248, 205});
    fill_rect(raster, 34, 34, 2, kMenuHeight - 68, Color{104, 228, 255, 235});
    fill_rect(raster, kMenuWidth - 36, 34, 2, kMenuHeight - 68, Color{212, 156, 23, 240});
    draw_line(raster, 34, 34, 54, 34, 2, Color{104, 228, 255, 240});
    draw_line(raster, 34, 34, 34, 54, 2, Color{104, 228, 255, 240});
    draw_line(raster, kMenuWidth - 34, kMenuHeight - 34, kMenuWidth - 54, kMenuHeight - 34, 2,
              Color{212, 156, 23, 240});
    draw_line(raster, kMenuWidth - 34, kMenuHeight - 34, kMenuWidth - 34, kMenuHeight - 54, 2,
              Color{212, 156, 23, 240});

    const size_t available =
        first_visible_root_index < roots.size() ? roots.size() - first_visible_root_index : 0U;
    const size_t visible = std::min(available, static_cast<size_t>(kRootItemCount));
#if defined(_WIN32)
    std::vector<sao::ui::entity_text::TextCommand> text_commands;
    text_commands.reserve(visible * 2U);
#endif
    for (size_t slot = 0; slot < visible; ++slot) {
        const auto& root = roots[first_visible_root_index + slot];
        const int32_t physical_index = static_cast<int32_t>(slot);
        const bool hover = physical_index == hover_index;
        const bool pressed = physical_index == pressed_index;
        const int32_t size = hover || pressed ? 70 : 54;
        const int32_t center_y = kMenuPad + physical_index * kMenuSlot + kMenuSlot / 2;
        fill_circle(raster, kMenuColumnCenter, center_y, size / 2,
                    pressed ? Color{244, 235, 215, 235}
                    : hover ? Color{237, 247, 250, 235}
                            : Color{247, 248, 248, 220});
        stroke_circle(raster, kMenuColumnCenter, center_y, size / 2, 2,
                      root.action_id == SAO_UI_ENTITY_ACTION_OPEN_ABOUT ? Color{212, 156, 23, 245}
                                                                        : Color{88, 152, 190, 225});
#if defined(_WIN32)
        if (!root.icon.empty()) {
            text_commands.push_back({kMenuColumnCenter - 10,
                                     center_y - 12,
                                     20,
                                     24,
                                     root.icon,
                                     sao::ui::entity_text::FontRole::Icon,
                                     15.0F,
                                     {100, 99, 100, 255},
                                     true});
        }
        if (!root.name.empty()) {
            text_commands.push_back({118,
                                     center_y - 8,
                                     kMenuWidth - 140,
                                     18,
                                     root.name,
                                     sao::ui::entity_text::FontRole::Label,
                                     11.0F,
                                     {100, 99, 100, 255},
                                     true});
        }
#else
        draw_text_clipped(raster, kMenuColumnCenter - 5, center_y - 7, 12, root.icon, 2,
                          Color{100, 99, 100, 255});
        draw_text_clipped(raster, 118, center_y - 7, kMenuWidth - 140, root.name, 1,
                          pressed ? Color{98, 88, 70, 255} : Color{100, 99, 100, 255},
                          BlendRounding::Nearest, true);
#endif
        draw_line(raster, 113, center_y + 15, kMenuWidth - 19, center_y + 15, 1,
                  Color{188, 196, 202, 145});
    }
#if defined(_WIN32)
    const sao::ui::entity_text::BgraSurface surface{
        reinterpret_cast<uint8_t*>(raster.pixels.data()), raster.width, raster.height,
        raster.width * sizeof(Pixel)};
    if (!sao::ui::entity_text::render_text(surface, text_commands)) {
        for (const auto& command : text_commands) {
            draw_text_clipped(raster, command.x, command.y, command.max_width, command.utf8, 1,
                              {command.color.r, command.color.g, command.color.b, command.color.a},
                              BlendRounding::Nearest, command.ellipsis);
        }
    }
#endif
    return raster;
}

Raster rasterize_menu(int32_t hover_index, int32_t pressed_index,
                      const sao::ui::menu_visual::Snapshot& snapshot,
                      size_t first_visible_child_index, const std::vector<OwnedRootItem>& roots,
                      size_t first_visible_root_index) {
#if defined(_WIN32)
    Raster raster;
    if (is_default_root_view(roots, first_visible_root_index)) {
        const int32_t selected_index = pressed_index >= 0 ? pressed_index : hover_index;
        constexpr std::array<int32_t, 5> kHoverResources{
            SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_0, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_1,
            SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_2, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_3,
            SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_4,
        };
        const int32_t resource_id =
            selected_index >= 0 && selected_index < static_cast<int32_t>(kHoverResources.size())
                ? kHoverResources[static_cast<size_t>(selected_index)]
                : SAO_UI_ENTITY_AUTHORITY_MENU_IDLE;
        raster = load_authority_frame(resource_id, kMenuWidth, kMenuHeight);
    } else {
        raster =
            rasterize_generic_menu(hover_index, pressed_index, roots, first_visible_root_index);
    }
#else
    Raster raster =
        rasterize_generic_menu(hover_index, pressed_index, roots, first_visible_root_index);
#endif
    draw_child_overlay(raster, snapshot, first_visible_child_index);
    return raster;
}

sao_status_t first_failure(sao_status_t current, sao_status_t candidate) {
    return current == SAO_STATUS_OK && candidate != SAO_STATUS_OK ? candidate : current;
}

} // namespace

struct EntityLayerInputBinding {
    sao_ui_entity_shell_s* shell{};
    bool menu{};
    float last_x{};
    float last_y{};
    bool has_cursor{};
};

struct sao_ui_entity_shell_s {
    sao_ui_overlay_host_handle_t host{};
    sao_ui_compositor_handle_t compositor{};
    bool owns_compositor{true};
    sao_ui_theme_handle_t theme{};
    sao_ui_menu_handle_t menu{};
    sao_ui_nervegear_handle_t nervegear{};
    sao_ui_layer_handle_t nervegear_layer{};
    sao_ui_layer_handle_t menu_layer{};
    SaoUiEntityShellConfig config{};
    Raster nervegear_raster;
    Raster menu_raster;
    std::vector<SaoUiLayerInputRect> menu_input_rects;
    EntityLayerInputBinding nervegear_input{};
    EntityLayerInputBinding menu_input{};
    int32_t origin_x{};
    int32_t origin_y{};
    int32_t width{};
    int32_t height{};
    int32_t nervegear_x{};
    int32_t nervegear_y{};
    int32_t menu_x{};
    int32_t menu_y{};
    int32_t menu_hover_index{-1};
    int32_t menu_pressed_index{-1};
    int32_t menu_hover_child_parent{-1};
    int32_t menu_hover_child_index{-1};
    int32_t menu_pressed_child_parent{-1};
    int32_t menu_pressed_child_index{-1};
    std::vector<OwnedRootItem> roots;
    std::string active_root_id;
    size_t first_visible_root_index{};
    size_t first_visible_child_index{};
    int32_t displayed_child_parent_index{-1};
    bool online{};
    bool overlay_visible{};
    bool menu_visible{};
    bool nervgear_mode{true};
    bool visual_dirty{true};
    bool input_region_settle_pending{};
    bool destroy_pending{};
    bool destroy_finalizer_scheduled{};
    bool teardown_started{};
    std::atomic_bool suppress_layer_input_callbacks{};
    uint32_t callback_depth{};
    uint64_t frame_count{};
    uint64_t action_count{};
    uint64_t root_tree_revision{1};
    sao_status_t last_status{SAO_STATUS_OK};
    std::thread::id owner_thread;
    mutable std::mutex mutex;
};

namespace {

// Read the overlay host's current DPI without holding shell->mutex. Returns
// 96 when the shell has no host or the host has never received WM_DPICHANGED.
uint32_t host_dpi_snapshot(const sao_ui_entity_shell_s* shell) noexcept {
    if (shell == nullptr || shell->host == nullptr) return 96u;
    const uint32_t dpi = sao_ui_overlay_host_current_dpi(shell->host);
    return dpi == 0u ? 96u : dpi;
}

bool entity_input_screen_point(EntityLayerInputBinding* binding, float local_x, float local_y,
                               int32_t* out_x, int32_t* out_y) {
    if (binding == nullptr || binding->shell == nullptr || out_x == nullptr || out_y == nullptr)
        return false;
    int32_t layer_x = 0;
    int32_t layer_y = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    {
        std::lock_guard lock(binding->shell->mutex);
        origin_x = binding->shell->origin_x;
        origin_y = binding->shell->origin_y;
        layer_x = binding->menu ? binding->shell->menu_x : binding->shell->nervegear_x;
        layer_y = binding->menu ? binding->shell->menu_y : binding->shell->nervegear_y;
    }
    const int64_t screen_x = static_cast<int64_t>(origin_x) + layer_x +
                             static_cast<int32_t>(local_x);
    const int64_t screen_y = static_cast<int64_t>(origin_y) + layer_y +
                             static_cast<int32_t>(local_y);
    if (screen_x < INT32_MIN || screen_x > INT32_MAX || screen_y < INT32_MIN ||
        screen_y > INT32_MAX) {
        return false;
    }
    *out_x = static_cast<int32_t>(screen_x);
    *out_y = static_cast<int32_t>(screen_y);
    return true;
}

void SAO_UI_CALL entity_layer_cursor(float layer_x, float layer_y, void* user_data) {
    auto* binding = static_cast<EntityLayerInputBinding*>(user_data);
    if (binding == nullptr || binding->shell == nullptr ||
        binding->shell->suppress_layer_input_callbacks.load(std::memory_order_acquire)) {
        return;
    }
    int32_t screen_x = 0;
    int32_t screen_y = 0;
    if (!entity_input_screen_point(binding, layer_x, layer_y, &screen_x, &screen_y))
        return;
    binding->last_x = layer_x;
    binding->last_y = layer_y;
    binding->has_cursor = true;
    (void)sao_ui_entity_shell_handle_mouse(binding->shell, kMouseMove, screen_x, screen_y, -1, 0);
}

void SAO_UI_CALL entity_layer_leave(void* user_data) {
    auto* binding = static_cast<EntityLayerInputBinding*>(user_data);
    if (binding == nullptr || binding->shell == nullptr)
        return;
    binding->has_cursor = false;
    if (binding->shell->suppress_layer_input_callbacks.load(std::memory_order_acquire))
        return;
    (void)sao_ui_entity_shell_handle_mouse(binding->shell, kMouseLeave, 0, 0, -1, 0);
}

void SAO_UI_CALL entity_layer_button(int32_t button, int32_t action, int32_t, float layer_x,
                                     float layer_y, void* user_data) {
    auto* binding = static_cast<EntityLayerInputBinding*>(user_data);
    if (binding == nullptr || binding->shell == nullptr ||
        binding->shell->suppress_layer_input_callbacks.load(std::memory_order_acquire)) {
        return;
    }
    int32_t screen_x = 0;
    int32_t screen_y = 0;
    if (!entity_input_screen_point(binding, layer_x, layer_y, &screen_x, &screen_y))
        return;
    uint32_t message = 0;
    if (button == 0)
        message = action == 0 ? kLeftButtonUp : kLeftButtonDown;
    else if (button == 1)
        message = action == 0 ? 0x0205U : 0x0204U;
    else if (button == 2)
        message = action == 0 ? 0x0208U : 0x0207U;
    if (message != 0)
        (void)sao_ui_entity_shell_handle_mouse(binding->shell, message, screen_x, screen_y, button,
                                               0);
}

void SAO_UI_CALL entity_layer_scroll(float, float dy, void* user_data) {
    auto* binding = static_cast<EntityLayerInputBinding*>(user_data);
    if (binding == nullptr || binding->shell == nullptr || !std::isfinite(dy) ||
        binding->shell->suppress_layer_input_callbacks.load(std::memory_order_acquire)) {
        return;
    }
    float local_x = binding->last_x;
    float local_y = binding->last_y;
    if (sao_ui_compositor_current_input_position(&local_x, &local_y) != SAO_STATUS_OK &&
        !binding->has_cursor) {
        std::lock_guard lock(binding->shell->mutex);
        local_x = static_cast<float>((binding->menu ? kMenuWidth : SAO_UI_NERVEGEAR_SIZE) / 2);
        local_y = static_cast<float>((binding->menu ? kMenuHeight : SAO_UI_NERVEGEAR_SIZE) / 2);
    }
    int32_t screen_x = 0;
    int32_t screen_y = 0;
    if (!entity_input_screen_point(binding, local_x, local_y, &screen_x, &screen_y))
        return;
    const double raw_delta = static_cast<double>(dy) * kWheelDeltaPerNotch;
    if (raw_delta < INT32_MIN || raw_delta > INT32_MAX)
        return;
    (void)sao_ui_entity_shell_handle_mouse(binding->shell, kMouseWheel, screen_x, screen_y, -1,
                                           static_cast<int32_t>(std::lround(raw_delta)));
}

size_t visible_root_count(const sao_ui_entity_shell_s* shell) {
    if (shell->first_visible_root_index >= shell->roots.size())
        return 0;
    return std::min(shell->roots.size() - shell->first_visible_root_index,
                    static_cast<size_t>(kRootItemCount));
}

size_t max_first_visible_root_index(size_t root_count) {
    const size_t capacity = static_cast<size_t>(kRootItemCount);
    return root_count > capacity ? root_count - capacity : 0U;
}

int32_t find_root_by_id(const std::vector<OwnedRootItem>& roots, std::string_view id) {
    const auto found =
        std::find_if(roots.begin(), roots.end(), [id](const auto& root) { return root.id == id; });
    if (found == roots.end())
        return -1;
    return static_cast<int32_t>(std::distance(roots.begin(), found));
}

int32_t find_root_by_name(const std::vector<OwnedRootItem>& roots, std::string_view name) {
    const auto found = std::find_if(roots.begin(), roots.end(),
                                    [name](const auto& root) { return root.name == name; });
    if (found == roots.end())
        return -1;
    return static_cast<int32_t>(std::distance(roots.begin(), found));
}

int32_t physical_to_logical_root_locked(const sao_ui_entity_shell_s* shell,
                                        int32_t physical_index) {
    if (physical_index < 0 || static_cast<size_t>(physical_index) >= visible_root_count(shell)) {
        return -1;
    }
    const size_t logical_index =
        shell->first_visible_root_index + static_cast<size_t>(physical_index);
    return logical_index <= static_cast<size_t>(std::numeric_limits<int32_t>::max())
               ? static_cast<int32_t>(logical_index)
               : -1;
}

std::vector<SaoUiMenuItem> make_menu_items(const std::vector<OwnedMenuItem>& items) {
    std::vector<SaoUiMenuItem> descriptors;
    descriptors.reserve(items.size());
    for (const auto& item : items) {
        descriptors.push_back({item.name.c_str(),
                               item.icon.c_str(),
                               item.action_id,
                               item.can_activate,
                               {false, false, false}});
    }
    return descriptors;
}

sao_status_t apply_visible_roots_locked(sao_ui_entity_shell_s* shell) {
    try {
        const size_t count = visible_root_count(shell);
        std::vector<SaoUiMenuItem> descriptors;
        descriptors.reserve(count);
        for (size_t slot = 0; slot < count; ++slot) {
            const auto& root = shell->roots[shell->first_visible_root_index + slot];
            descriptors.push_back({root.id.c_str(),
                                   root.icon.c_str(),
                                   root.action_id,
                                   root.can_activate,
                                   {false, false, false}});
        }
        sao_status_t status = sao_ui_menu_set_items(
            shell->menu, descriptors.empty() ? nullptr : descriptors.data(), descriptors.size());
        if (status != SAO_STATUS_OK)
            return status;
        for (size_t slot = 0; slot < count; ++slot) {
            const auto& root = shell->roots[shell->first_visible_root_index + slot];
            const auto children = make_menu_items(root.children);
            status = sao_ui_menu_set_children(shell->menu, root.id.c_str(),
                                              children.empty() ? nullptr : children.data(),
                                              children.size());
            if (status != SAO_STATUS_OK)
                return status;
        }
        if (!shell->active_root_id.empty()) {
            const int32_t logical_index = find_root_by_id(shell->roots, shell->active_root_id);
            if (logical_index >= 0 &&
                static_cast<size_t>(logical_index) >= shell->first_visible_root_index &&
                static_cast<size_t>(logical_index) < shell->first_visible_root_index + count) {
                const int32_t physical_index = static_cast<int32_t>(
                    static_cast<size_t>(logical_index) - shell->first_visible_root_index);
                sao::ui::menu_visual::Snapshot snapshot{};
                status = sao::ui::menu_visual::get_snapshot(shell->menu, &snapshot);
                if (status == SAO_STATUS_OK && snapshot.active_root_idx != physical_index)
                    status = sao_ui_menu_activate(shell->menu, physical_index);
                if (status != SAO_STATUS_OK)
                    return status;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

bool is_child_phase(SaoUiMenuPhase phase) {
    return phase == SAO_UI_MENU_PHASE_CHILD_OPENING || phase == SAO_UI_MENU_PHASE_CHILD_OPEN ||
           phase == SAO_UI_MENU_PHASE_CHILD_CLOSING;
}

size_t max_first_visible_child_index(size_t row_count) {
    const size_t capacity = static_cast<size_t>(kChildPhysicalCapacity);
    return row_count > capacity ? row_count - capacity : 0U;
}

sao_status_t clear_child_interaction_locked(sao_ui_entity_shell_s* shell) {
    shell->menu_pressed_index = -1;
    shell->menu_hover_child_parent = -1;
    shell->menu_hover_child_index = -1;
    shell->menu_pressed_child_parent = -1;
    shell->menu_pressed_child_index = -1;
    return sao::ui::menu_visual::set_child_hover(shell->menu, -1, -1);
}

sao_status_t sync_child_viewport_locked(
    sao_ui_entity_shell_s* shell, const sao::ui::menu_visual::Snapshot& snapshot) {
    const bool child_visible = is_child_phase(snapshot.phase) &&
                               snapshot.displayed_parent_idx >= 0 && !snapshot.rows.empty();
    const int32_t next_parent = child_visible ? snapshot.displayed_parent_idx : -1;
    const bool parent_changed = shell->displayed_child_parent_index != next_parent;
    const size_t next_first =
        parent_changed ? 0U
                       : std::min(shell->first_visible_child_index,
                                  max_first_visible_child_index(snapshot.rows.size()));
    if (!parent_changed && next_first == shell->first_visible_child_index)
        return SAO_STATUS_OK;

    shell->displayed_child_parent_index = next_parent;
    shell->first_visible_child_index = next_first;
    shell->visual_dirty = true;
    return clear_child_interaction_locked(shell);
}

std::vector<SaoUiLayerInputRect> circular_input_rects(int32_t center_x, int32_t center_y,
                                                      int32_t radius, int32_t width,
                                                      int32_t height) {
    std::vector<SaoUiLayerInputRect> rects;
    rects.reserve(static_cast<size_t>(height));
    const int64_t radius_squared = static_cast<int64_t>(radius) * radius;
    for (int32_t y = 0; y < height; ++y) {
        const int64_t dy = static_cast<int64_t>(y) - center_y;
        const int64_t remaining = radius_squared - dy * dy;
        if (remaining < 0)
            continue;
        const int32_t half_span = static_cast<int32_t>(std::floor(std::sqrt(
            static_cast<double>(remaining))));
        const int32_t left = std::max(0, center_x - half_span);
        const int32_t right = std::min(width - 1, center_x + half_span);
        if (left <= right)
            rects.push_back({left, y, right - left + 1, 1});
    }
    return rects;
}

sao_status_t detach_and_destroy_layer(sao_ui_layer_handle_t* layer,
                                      bool force_construction_cleanup = false) {
    if (layer == nullptr || *layer == nullptr)
        return SAO_STATUS_OK;
    const sao_status_t status = sao_ui_layer_set_input_callbacks(
        *layer, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_HANDLE_INVALID &&
        !force_construction_cleanup) {
        return status;
    }
    if (status != SAO_STATUS_ERR_HANDLE_INVALID)
        sao_ui_layer_destroy(*layer);
    *layer = nullptr;
    return SAO_STATUS_OK;
}

sao_status_t destroy_members(sao_ui_entity_shell_s* shell,
                             bool force_construction_cleanup = false) {
    shell->suppress_layer_input_callbacks.store(true, std::memory_order_release);
    sao_status_t status =
        detach_and_destroy_layer(&shell->menu_layer, force_construction_cleanup);
    status = first_failure(
        status, detach_and_destroy_layer(&shell->nervegear_layer, force_construction_cleanup));
    if (status != SAO_STATUS_OK)
        return status;

    if (shell->nervegear != nullptr) {
        sao_ui_nervegear_destroy(shell->nervegear);
        shell->nervegear = nullptr;
    }
    if (shell->menu != nullptr) {
        sao_ui_menu_destroy(shell->menu);
        shell->menu = nullptr;
    }
    if (shell->theme != nullptr) {
        sao_ui_theme_destroy(shell->theme);
        shell->theme = nullptr;
    }
    if (shell->owns_compositor && shell->compositor != nullptr) {
        status = sao_ui_compositor_try_destroy(shell->compositor);
        if (status != SAO_STATUS_OK)
            return status;
    }
    shell->compositor = nullptr;
    return SAO_STATUS_OK;
}

std::mutex g_failed_owned_construction_mutex;
std::vector<std::unique_ptr<sao_ui_entity_shell_s>> g_failed_owned_construction_shells;
std::atomic_bool g_fail_next_entity_shell_construction{};

void retry_failed_owned_construction_cleanup() {
    std::lock_guard lock(g_failed_owned_construction_mutex);
    for (auto it = g_failed_owned_construction_shells.begin();
         it != g_failed_owned_construction_shells.end();) {
        if ((*it)->owner_thread == std::this_thread::get_id() &&
            destroy_members(it->get()) == SAO_STATUS_OK) {
            it = g_failed_owned_construction_shells.erase(it);
        } else {
            ++it;
        }
    }
}

void quarantine_failed_owned_construction(std::unique_ptr<sao_ui_entity_shell_s> shell) {
    if (shell == nullptr || !shell->owns_compositor || shell->compositor == nullptr)
        return;
    shell->suppress_layer_input_callbacks.store(true, std::memory_order_release);
    std::lock_guard lock(g_failed_owned_construction_mutex);
    try {
        g_failed_owned_construction_shells.push_back(std::move(shell));
    } catch (...) {
        (void)shell.release();
        throw;
    }
}

void SAO_UI_CALL deferred_entity_destroy(void* user_data) {
    auto* shell = static_cast<sao_ui_entity_shell_s*>(user_data);
    if (shell == nullptr)
        return;
    {
        std::lock_guard lock(shell->mutex);
        shell->destroy_finalizer_scheduled = false;
    }
    (void)sao_ui_entity_shell_try_destroy(shell);
}

sao_status_t set_menu_visibility_locked(sao_ui_entity_shell_s* shell, bool visible) {
    const bool changed = shell->menu_visible != visible;
    sao_status_t status =
        visible ? sao_ui_menu_show(shell->menu, shell->menu_x + kMenuColumnCenter,
                                   shell->menu_y + kMenuPad)
                : sao_ui_menu_hide(shell->menu);
    if (status != SAO_STATUS_OK)
        return status;
    shell->menu_visible = visible;
    shell->visual_dirty = shell->visual_dirty || changed;
    if (!visible) {
        shell->active_root_id.clear();
        shell->menu_hover_index = -1;
        shell->menu_pressed_index = -1;
        shell->first_visible_child_index = 0;
        shell->displayed_child_parent_index = -1;
        status = first_failure(status, sao_ui_menu_set_hover(shell->menu, -1));
        status = first_failure(status, clear_child_interaction_locked(shell));
    }
    return status;
}

sao_status_t refresh_host_geometry_locked(sao_ui_entity_shell_s* shell) {
    if (shell->host == nullptr)
        return SAO_STATUS_OK;
    SaoOverlayHostState host_state{};
    sao_status_t status = sao_ui_overlay_host_get_state(shell->host, &host_state);
    if (status != SAO_STATUS_OK)
        return status;
    const auto& geometry = host_state.geometry;
    if (geometry.width <= 0 || geometry.height <= 0)
        return SAO_STATUS_OK;
    if (shell->origin_x == geometry.x && shell->origin_y == geometry.y &&
        shell->width == geometry.width && shell->height == geometry.height) {
        return SAO_STATUS_OK;
    }

    shell->origin_x = geometry.x;
    shell->origin_y = geometry.y;
    shell->width = geometry.width;
    shell->height = geometry.height;
    shell->nervegear_x = std::max(0, shell->width - kMarginRight - SAO_UI_NERVEGEAR_SIZE);
    shell->nervegear_y = std::max(0, shell->height - kMarginBottom - SAO_UI_NERVEGEAR_SIZE);
    const int32_t menu_max_x = std::max(0, shell->width - kMenuWidth);
    const int32_t menu_max_y = std::max(0, shell->height - kMenuHeight);
    shell->menu_x =
        std::clamp(shell->nervegear_x + SAO_UI_NERVEGEAR_SIZE - kMenuWidth, 0, menu_max_x);
    shell->menu_y = std::clamp(shell->nervegear_y - kMenuHeight - 12, 0, menu_max_y);

    SaoUiMenuLayout layout{};
    layout.center_x = shell->menu_x + kMenuColumnCenter;
    layout.center_y = shell->menu_y + kMenuPad;
    layout.inner_radius = 60;
    layout.outer_radius = 160;
    layout.child_ring_radius = 240;
    layout.button_size = 54;
    layout.button_max_size = 70;
    layout.slot_size = kMenuSlot;
    layout.max_visible = 9;
    status = sao_ui_menu_set_layout(shell->menu, &layout);
    status = first_failure(status, sao_ui_nervegear_set_position(
                                       shell->nervegear, shell->origin_x + shell->nervegear_x,
                                       shell->origin_y + shell->nervegear_y));
    status =
        first_failure(status, sao_ui_layer_set_position(shell->nervegear_layer, shell->nervegear_x,
                                                        shell->nervegear_y));
    status = first_failure(
        status, sao_ui_layer_set_position(shell->menu_layer, shell->menu_x, shell->menu_y));
    if (status == SAO_STATUS_OK)
        shell->visual_dirty = true;
    return status;
}

sao_status_t apply_layer_state_locked(sao_ui_entity_shell_s* shell) {
    const bool callbacks_were_suppressed =
        shell->suppress_layer_input_callbacks.exchange(true, std::memory_order_acq_rel);
    const bool overlay_active = shell->online && shell->overlay_visible;
    const bool nervegear_active = overlay_active && shell->nervgear_mode;
    SaoUiMenuPhase menu_phase = SAO_UI_MENU_PHASE_CLOSED;
    float menu_progress = 0.0F;
    sao_status_t status = sao_ui_menu_get_phase(shell->menu, &menu_phase);
    status =
        first_failure(status, sao_ui_menu_get_transition_progress(shell->menu, &menu_progress));
    const bool menu_active = overlay_active && menu_phase != SAO_UI_MENU_PHASE_CLOSED;
    const float menu_layer_alpha = shell->menu_visible && menu_phase == SAO_UI_MENU_PHASE_OPENING
                                       ? std::max(menu_progress, 1.0F / 255.0F)
                                       : menu_progress;
    status =
        first_failure(status, sao_ui_layer_set_visible(shell->nervegear_layer, nervegear_active));
    status = first_failure(status, sao_ui_layer_set_alpha(shell->menu_layer, menu_layer_alpha));
    status = first_failure(
        status, sao_ui_layer_set_input_enabled(shell->nervegear_layer, nervegear_active));
    status = first_failure(status, sao_ui_layer_set_visible(shell->menu_layer, menu_active));
    status = first_failure(status, sao_ui_layer_set_input_enabled(
                                       shell->menu_layer, menu_active && shell->menu_visible &&
                                                              !shell->roots.empty()));
    shell->suppress_layer_input_callbacks.store(callbacks_were_suppressed,
                                                 std::memory_order_release);
    return status;
}

bool input_rects_equal(const std::vector<SaoUiLayerInputRect>& left,
                       const std::vector<SaoUiLayerInputRect>& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](const SaoUiLayerInputRect& a, const SaoUiLayerInputRect& b) {
                          return a.x == b.x && a.y == b.y && a.width == b.width &&
                                 a.height == b.height;
                      });
}

sao_status_t build_menu_input_rects_locked(
    sao_ui_entity_shell_s* shell, const sao::ui::menu_visual::Snapshot& snapshot,
    std::vector<SaoUiLayerInputRect>* out_rects) {
    if (out_rects == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<SaoUiLayerInputRect> next_rects;
    next_rects.reserve(kRootItemCount + kChildPhysicalCapacity);
    for (int32_t index = 0; index < static_cast<int32_t>(visible_root_count(shell)); ++index) {
        next_rects.push_back(
            {kMenuPad, kMenuPad + index * kMenuSlot, kMenuSlot, kMenuSlot});
    }
    const size_t available_child_count =
        shell->first_visible_child_index < snapshot.rows.size()
            ? snapshot.rows.size() - shell->first_visible_child_index
            : 0U;
    const size_t visible_child_count =
        std::min(available_child_count, static_cast<size_t>(kChildPhysicalCapacity));
    const int32_t child_count = static_cast<int32_t>(visible_child_count);
    for (int32_t slot = 0; slot < child_count; ++slot) {
        const size_t logical_index =
            shell->first_visible_child_index + static_cast<size_t>(slot);
        const int32_t width =
            std::clamp(snapshot.rows[logical_index].visible_width_px, 0, kChildTargetWidth);
        if (width <= 1)
            continue;
        next_rects.push_back(
            {kChildRowX, kChildOriginY + slot * kChildRowStride, width, kChildRowHeight});
    }
    *out_rects = std::move(next_rects);
    return SAO_STATUS_OK;
}

sao_status_t raster_and_upload_locked(sao_ui_entity_shell_s* shell) {
    sao::ui::menu_visual::Snapshot menu_snapshot{};
    sao_status_t status = sao::ui::menu_visual::get_snapshot(shell->menu, &menu_snapshot);
    if (status != SAO_STATUS_OK)
        return status;
    status = sync_child_viewport_locked(shell, menu_snapshot);
    if (status != SAO_STATUS_OK)
        return status;
    SaoUiNerveGearState state = SAO_UI_NG_STATE_IDLE;
    status = sao_ui_nervegear_get_state(shell->nervegear, &state);
    if (status != SAO_STATUS_OK)
        return status;
    Raster next_nervegear_raster;
    Raster next_menu_raster;
    std::vector<SaoUiLayerInputRect> next_menu_input_rects;
    try {
        next_nervegear_raster = rasterize_nervegear(state);
        next_menu_raster = rasterize_menu(shell->menu_hover_index, shell->menu_pressed_index,
                                          menu_snapshot, shell->first_visible_child_index,
                                          shell->roots, shell->first_visible_root_index);
        status = build_menu_input_rects_locked(shell, menu_snapshot, &next_menu_input_rects);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_layer_update_bgra(
        shell->nervegear_layer,
        reinterpret_cast<const uint8_t*>(next_nervegear_raster.pixels.data()),
        next_nervegear_raster.width, next_nervegear_raster.height,
        next_nervegear_raster.width * sizeof(Pixel));
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_layer_update_bgra(
        shell->menu_layer, reinterpret_cast<const uint8_t*>(next_menu_raster.pixels.data()),
        next_menu_raster.width, next_menu_raster.height,
        next_menu_raster.width * sizeof(Pixel));
    if (status != SAO_STATUS_OK)
        return status;
    if (!input_rects_equal(shell->menu_input_rects, next_menu_input_rects)) {
        status = sao_ui_layer_set_input_rects(shell->menu_layer, next_menu_input_rects.data(),
                                              next_menu_input_rects.size());
        if (status != SAO_STATUS_OK)
            return status;
        shell->menu_input_rects = std::move(next_menu_input_rects);
    }
    shell->nervegear_raster = std::move(next_nervegear_raster);
    shell->menu_raster = std::move(next_menu_raster);
    return SAO_STATUS_OK;
}

sao_status_t commit_visual_state_locked(sao_ui_entity_shell_s* shell) {
    if (!shell->visual_dirty) {
        if (!shell->owns_compositor || shell->host == nullptr ||
            !shell->input_region_settle_pending) {
            return SAO_STATUS_OK;
        }
        const sao_status_t settle_status = sao_ui_compositor_sync_host_rgn(shell->compositor);
        if (settle_status == SAO_STATUS_OK) {
            shell->input_region_settle_pending = false;
        }
        shell->last_status = settle_status;
        return settle_status;
    }
    sao_status_t status = apply_layer_state_locked(shell);
    if (status != SAO_STATUS_OK) {
        shell->last_status = status;
        return status;
    }
    status = raster_and_upload_locked(shell);
    if (status != SAO_STATUS_OK) {
        shell->last_status = status;
        return status;
    }
    if (shell->owns_compositor && shell->host != nullptr) {
        status = first_failure(status, sao_ui_compositor_present(shell->compositor));
        const sao_status_t region_status = sao_ui_compositor_sync_host_rgn(shell->compositor);
        status = first_failure(status, region_status);
        if (region_status == SAO_STATUS_OK) {
            shell->input_region_settle_pending = true;
        }
        status = first_failure(status, sao_ui_compositor_sync_host_input_mode(shell->compositor));
        status = first_failure(status, sao_ui_compositor_enforce_z_order(shell->compositor));
    }
    ++shell->frame_count;
    if (status == SAO_STATUS_OK)
        shell->visual_dirty = false;
    shell->last_status = status;
    return status;
}

struct RootInteractionState {
    size_t first_visible_root_index{};
    size_t first_visible_child_index{};
    int32_t displayed_child_parent_index{-1};
    int32_t menu_hover_index{-1};
    int32_t menu_pressed_index{-1};
    int32_t menu_hover_child_parent{-1};
    int32_t menu_hover_child_index{-1};
    int32_t menu_pressed_child_parent{-1};
    int32_t menu_pressed_child_index{-1};
};

RootInteractionState capture_root_interaction(const sao_ui_entity_shell_s* shell) {
    return {
        shell->first_visible_root_index,     shell->first_visible_child_index,
        shell->displayed_child_parent_index, shell->menu_hover_index,
        shell->menu_pressed_index,           shell->menu_hover_child_parent,
        shell->menu_hover_child_index,       shell->menu_pressed_child_parent,
        shell->menu_pressed_child_index,
    };
}

void restore_root_interaction(sao_ui_entity_shell_s* shell, const RootInteractionState& state) {
    shell->first_visible_root_index = state.first_visible_root_index;
    shell->first_visible_child_index = state.first_visible_child_index;
    shell->displayed_child_parent_index = state.displayed_child_parent_index;
    shell->menu_hover_index = state.menu_hover_index;
    shell->menu_pressed_index = state.menu_pressed_index;
    shell->menu_hover_child_parent = state.menu_hover_child_parent;
    shell->menu_hover_child_index = state.menu_hover_child_index;
    shell->menu_pressed_child_parent = state.menu_pressed_child_parent;
    shell->menu_pressed_child_index = state.menu_pressed_child_index;
}

sao_status_t replace_roots_locked(sao_ui_entity_shell_s* shell,
                                  std::vector<OwnedRootItem> candidate) {
    if (candidate == shell->roots) {
        shell->last_status = SAO_STATUS_OK;
        return SAO_STATUS_OK;
    }
    if (shell->root_tree_revision == std::numeric_limits<uint64_t>::max()) {
        shell->last_status = SAO_STATUS_ERR_UNKNOWN;
        return SAO_STATUS_ERR_UNKNOWN;
    }

    try {
        const std::vector<OwnedRootItem> previous_roots = shell->roots;
        const std::string previous_active_id = shell->active_root_id;
        const RootInteractionState previous_interaction = capture_root_interaction(shell);
        sao::ui::menu_visual::ChildMenuSnapshot previous_active_menu{};
        bool previous_active_menu_captured = false;
        if (!previous_active_id.empty()) {
            const int32_t logical_index = find_root_by_id(previous_roots, previous_active_id);
            if (logical_index >= 0 &&
                static_cast<size_t>(logical_index) >= shell->first_visible_root_index &&
                static_cast<size_t>(logical_index) <
                    shell->first_visible_root_index + visible_root_count(shell)) {
                previous_active_menu_captured = sao::ui::menu_visual::get_child_menu_snapshot(
                                                    shell->menu, previous_active_id.c_str(),
                                                    &previous_active_menu) == SAO_STATUS_OK;
            }
        }

        shell->roots = std::move(candidate);
        if (find_root_by_id(shell->roots, shell->active_root_id) < 0)
            shell->active_root_id.clear();
        shell->first_visible_root_index = std::min(
            shell->first_visible_root_index, max_first_visible_root_index(shell->roots.size()));
        shell->menu_hover_index = -1;
        shell->menu_pressed_index = -1;
        shell->menu_hover_child_parent = -1;
        shell->menu_hover_child_index = -1;
        shell->menu_pressed_child_parent = -1;
        shell->menu_pressed_child_index = -1;

        sao_status_t status = apply_visible_roots_locked(shell);
        if (status == SAO_STATUS_OK) {
            sao::ui::menu_visual::Snapshot snapshot{};
            status = sao::ui::menu_visual::get_snapshot(shell->menu, &snapshot);
            if (status == SAO_STATUS_OK)
                status = sync_child_viewport_locked(shell, snapshot);
        }
        if (status == SAO_STATUS_OK)
            status = clear_child_interaction_locked(shell);
        if (status == SAO_STATUS_OK) {
            shell->visual_dirty = true;
            status = commit_visual_state_locked(shell);
        }
        if (status == SAO_STATUS_OK) {
            ++shell->root_tree_revision;
            shell->last_status = SAO_STATUS_OK;
            return SAO_STATUS_OK;
        }

        const sao_status_t failure_status = status;
        shell->roots = previous_roots;
        shell->active_root_id = previous_active_id;
        restore_root_interaction(shell, previous_interaction);
        (void)apply_visible_roots_locked(shell);
        if (previous_active_menu_captured) {
            (void)sao::ui::menu_visual::restore_child_menu_snapshot(
                shell->menu, previous_active_id.c_str(), previous_active_menu);
        }
        (void)sao_ui_menu_set_hover(shell->menu, previous_interaction.menu_hover_index);
        (void)sao::ui::menu_visual::set_child_hover(shell->menu,
                                                    previous_interaction.menu_hover_child_parent,
                                                    previous_interaction.menu_hover_child_index);
        shell->visual_dirty = true;
        (void)commit_visual_state_locked(shell);
        shell->last_status = failure_status;
        return failure_status;
    } catch (...) {
        shell->last_status = SAO_STATUS_ERR_UNKNOWN;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sync_frame_locked(sao_ui_entity_shell_s* shell, uint32_t elapsed_ms) {
    SaoUiNerveGearState previous_state = SAO_UI_NG_STATE_IDLE;
    SaoUiMenuPhase previous_menu_phase = SAO_UI_MENU_PHASE_CLOSED;
    float previous_menu_progress = 0.0F;
    sao::ui::menu_visual::Snapshot previous_menu_visual{};
    sao_status_t status = refresh_host_geometry_locked(shell);
    status = first_failure(status, sao_ui_nervegear_get_state(shell->nervegear, &previous_state));
    status = first_failure(status, sao_ui_menu_get_phase(shell->menu, &previous_menu_phase));
    status = first_failure(
        status, sao_ui_menu_get_transition_progress(shell->menu, &previous_menu_progress));
    status = first_failure(
        status, sao::ui::menu_visual::get_snapshot(shell->menu, &previous_menu_visual));
    status = first_failure(
        status, sao_ui_nervegear_tick(shell->nervegear, static_cast<int32_t>(elapsed_ms)));
    status = first_failure(status, sao_ui_menu_tick(shell->menu, static_cast<int32_t>(elapsed_ms)));
    SaoUiNerveGearState current_state = previous_state;
    SaoUiMenuPhase current_menu_phase = previous_menu_phase;
    float current_menu_progress = previous_menu_progress;
    sao::ui::menu_visual::Snapshot current_menu_visual{};
    status = first_failure(status, sao_ui_nervegear_get_state(shell->nervegear, &current_state));
    status = first_failure(status, sao_ui_menu_get_phase(shell->menu, &current_menu_phase));
    status = first_failure(
        status, sao_ui_menu_get_transition_progress(shell->menu, &current_menu_progress));
    status = first_failure(
        status, sao::ui::menu_visual::get_snapshot(shell->menu, &current_menu_visual));
    shell->visual_dirty = shell->visual_dirty || previous_state != current_state ||
                          previous_menu_phase != current_menu_phase ||
                          previous_menu_progress != current_menu_progress ||
                          previous_menu_visual.revision != current_menu_visual.revision;
    status = first_failure(status, commit_visual_state_locked(shell));
    shell->last_status = status;
    return status;
}

bool local_nervegear_hit_locked(sao_ui_entity_shell_s* shell, int32_t screen_x, int32_t screen_y) {
    if (!shell->nervgear_mode)
        return false;
    bool hit = false;
    return sao_ui_nervegear_hit_test(shell->nervegear, screen_x, screen_y, &hit) == SAO_STATUS_OK &&
           hit;
}

struct MenuHit {
    int32_t parent{-1};
    int32_t child{-1};
};

struct PendingEntityAction {
    sao_ui_entity_action_fn_t callback{};
    void* user_data{};
    SaoUiEntityAction action{SAO_UI_ENTITY_ACTION_OPEN_ABOUT};
};

bool child_viewport_hit_locked(sao_ui_entity_shell_s* shell,
                               const sao::ui::menu_visual::Snapshot& snapshot,
                               int32_t screen_x, int32_t screen_y, MenuHit* out_hit) {
    if (!shell->menu_visible || !is_child_phase(snapshot.phase) ||
        snapshot.displayed_parent_idx < 0 || snapshot.rows.empty()) {
        return false;
    }
    const uint32_t host_dpi = host_dpi_snapshot(shell);
    // DPI-aware: scale the desktop screen point down before subtracting the
    // host origin, then subtract the layer origin (already in host-local
    // logical pixels).
    const int64_t local_x =
        screen_to_host_dpi(screen_x, shell->origin_x, host_dpi) - shell->menu_x;
    const int64_t local_y =
        screen_to_host_dpi(screen_y, shell->origin_y, host_dpi) - shell->menu_y;
    if (local_y < kChildOriginY)
        return false;
    const int64_t slot = (local_y - kChildOriginY) / kChildRowStride;
    const int64_t slot_y = kChildOriginY + slot * kChildRowStride;
    if (slot < 0 || slot >= kChildPhysicalCapacity || local_y >= slot_y + kChildRowHeight)
        return false;
    const size_t logical_index =
        shell->first_visible_child_index + static_cast<size_t>(slot);
    if (logical_index >= snapshot.rows.size())
        return false;
    const int32_t width =
        std::clamp(snapshot.rows[logical_index].visible_width_px, 0, kChildTargetWidth);
    if (width <= 1)
        return false;
    if (local_x < kChildRowX || local_x >= kChildRowX + width)
        return false;
    if (out_hit != nullptr) {
        out_hit->parent = snapshot.displayed_parent_idx;
        out_hit->child = static_cast<int32_t>(logical_index);
    }
    return true;
}

bool root_column_hit_locked(const sao_ui_entity_shell_s* shell, int32_t screen_x,
                            int32_t screen_y) {
    if (!shell->menu_visible)
        return false;
    const uint32_t host_dpi = host_dpi_snapshot(shell);
    // DPI-aware: convert screen point to host-local before subtracting menu origin.
    const int64_t local_x =
        screen_to_host_dpi(screen_x, shell->origin_x, host_dpi) - shell->menu_x;
    const int64_t local_y =
        screen_to_host_dpi(screen_y, shell->origin_y, host_dpi) - shell->menu_y;
    const int64_t bottom = kMenuPad + static_cast<int64_t>(visible_root_count(shell)) * kMenuSlot;
    return local_x >= kMenuPad && local_x < kMenuPad + kMenuSlot && local_y >= kMenuPad &&
           local_y < bottom;
}

sao_status_t scroll_root_viewport_locked(sao_ui_entity_shell_s* shell, int32_t screen_x,
                                         int32_t screen_y, int32_t wheel_delta, bool* out_changed) {
    if (out_changed == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_changed = false;
    if (wheel_delta == 0 || wheel_delta % kWheelDeltaPerNotch != 0 ||
        !root_column_hit_locked(shell, screen_x, screen_y)) {
        return SAO_STATUS_OK;
    }
    const size_t max_first = max_first_visible_root_index(shell->roots.size());
    const int64_t notches = wheel_delta / kWheelDeltaPerNotch;
    const int64_t requested = static_cast<int64_t>(shell->first_visible_root_index) - notches;
    const size_t next_first =
        static_cast<size_t>(std::clamp<int64_t>(requested, 0, static_cast<int64_t>(max_first)));
    if (next_first == shell->first_visible_root_index)
        return SAO_STATUS_OK;

    const size_t previous_first = shell->first_visible_root_index;
    shell->first_visible_root_index = next_first;
    sao_status_t status = apply_visible_roots_locked(shell);
    if (status == SAO_STATUS_OK)
        status = clear_child_interaction_locked(shell);
    if (status == SAO_STATUS_OK) {
        shell->menu_hover_index = -1;
        shell->menu_pressed_index = -1;
        shell->first_visible_child_index = 0;
        shell->displayed_child_parent_index = -1;
        shell->visual_dirty = true;
        *out_changed = true;
        return SAO_STATUS_OK;
    }

    shell->first_visible_root_index = previous_first;
    const sao_status_t restore_status = apply_visible_roots_locked(shell);
    return restore_status == SAO_STATUS_OK ? status : restore_status;
}

sao_status_t scroll_child_viewport_locked(sao_ui_entity_shell_s* shell, int32_t screen_x,
                                          int32_t screen_y, int32_t wheel_delta,
                                          bool* out_changed) {
    *out_changed = false;
    if (wheel_delta == 0 || wheel_delta % kWheelDeltaPerNotch != 0)
        return SAO_STATUS_OK;
    sao::ui::menu_visual::Snapshot snapshot{};
    sao_status_t status = sao::ui::menu_visual::get_snapshot(shell->menu, &snapshot);
    if (status != SAO_STATUS_OK)
        return status;
    status = sync_child_viewport_locked(shell, snapshot);
    if (status != SAO_STATUS_OK)
        return status;
    const size_t max_first = max_first_visible_child_index(snapshot.rows.size());
    if (max_first == 0 || !child_viewport_hit_locked(shell, snapshot, screen_x, screen_y, nullptr))
        return SAO_STATUS_OK;

    const int64_t notches = wheel_delta / kWheelDeltaPerNotch;
    const int64_t requested = static_cast<int64_t>(shell->first_visible_child_index) - notches;
    const size_t next_first = static_cast<size_t>(
        std::clamp<int64_t>(requested, 0, static_cast<int64_t>(max_first)));
    if (next_first == shell->first_visible_child_index) {
        const bool pressed_changed = shell->menu_pressed_index >= 0 ||
                                     shell->menu_pressed_child_parent >= 0 ||
                                     shell->menu_pressed_child_index >= 0;
        if (!pressed_changed)
            return SAO_STATUS_OK;
        shell->menu_pressed_index = -1;
        shell->menu_pressed_child_parent = -1;
        shell->menu_pressed_child_index = -1;
        shell->visual_dirty = true;
        *out_changed = true;
        return SAO_STATUS_OK;
    }
    shell->first_visible_child_index = next_first;
    status = clear_child_interaction_locked(shell);
    if (status == SAO_STATUS_OK) {
        shell->visual_dirty = true;
        *out_changed = true;
    }
    return status;
}

sao_status_t dispatch_entity_action(sao_ui_entity_shell_s* shell,
                                    const PendingEntityAction& pending) {
    sao_status_t action_status = SAO_STATUS_ERR_UNKNOWN;
    try {
        action_status = pending.callback(pending.action, pending.user_data);
    } catch (...) {
        action_status = SAO_STATUS_ERR_UNKNOWN;
    }
    bool should_destroy = false;
    bool schedule_finalizer = false;
    {
        std::lock_guard<std::mutex> lock(shell->mutex);
        shell->last_status = action_status;
        if (shell->callback_depth > 0)
            --shell->callback_depth;
        should_destroy = shell->destroy_pending && shell->callback_depth == 0;
        if (should_destroy && !shell->destroy_finalizer_scheduled &&
            shell->compositor != nullptr) {
            shell->destroy_finalizer_scheduled = true;
            schedule_finalizer = true;
            should_destroy = false;
        }
    }
    if (schedule_finalizer) {
        const sao_status_t finalizer_status = sao_ui_compositor_post_input(
            shell->compositor, &deferred_entity_destroy, shell);
        if (finalizer_status == SAO_STATUS_OK)
            return action_status;
        std::lock_guard lock(shell->mutex);
        shell->destroy_finalizer_scheduled = false;
        should_destroy = true;
    }
    if (should_destroy)
        (void)sao_ui_entity_shell_try_destroy(shell);
    return action_status;
}

MenuHit menu_hit_locked(sao_ui_entity_shell_s* shell, int32_t screen_x, int32_t screen_y) {
    if (!shell->menu_visible)
        return {};
    const uint32_t host_dpi = host_dpi_snapshot(shell);
    // DPI-aware: convert desktop coords to host-local before further math.
    const int64_t host_x = screen_to_host_dpi(screen_x, shell->origin_x, host_dpi);
    const int64_t host_y = screen_to_host_dpi(screen_y, shell->origin_y, host_dpi);
    const int64_t local_x = host_x - shell->menu_x;
    const int64_t local_y = host_y - shell->menu_y;
    if (local_x < 0 || local_y < 0 || local_x >= kMenuWidth || local_y >= kMenuHeight ||
        host_x < std::numeric_limits<int32_t>::min() ||
        host_x > std::numeric_limits<int32_t>::max() ||
        host_y < std::numeric_limits<int32_t>::min() ||
        host_y > std::numeric_limits<int32_t>::max())
        return {};
    sao::ui::menu_visual::Snapshot snapshot{};
    if (sao::ui::menu_visual::get_snapshot(shell->menu, &snapshot) != SAO_STATUS_OK)
        return {};
    MenuHit hit{};
    if (child_viewport_hit_locked(shell, snapshot, screen_x, screen_y, &hit))
        return hit;

    int32_t ignored_child = -1;
    if (sao_ui_menu_hit_test(shell->menu, static_cast<int32_t>(host_x),
                             static_cast<int32_t>(host_y), &hit.parent, &ignored_child) !=
            SAO_STATUS_OK ||
        ignored_child >= 0) {
        return {};
    }
    hit.child = -1;
    return hit;
}

} // namespace

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_test_fail_next_entity_shell_construction(void) {
    g_fail_next_entity_shell_construction.store(true, std::memory_order_release);
}

static sao_status_t create_entity_shell(
    sao_ui_overlay_host_handle_t host, sao_ui_compositor_handle_t borrowed_compositor,
    const SaoUiEntityShellConfig* config, sao_ui_entity_shell_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (borrowed_compositor != nullptr) {
        const sao_status_t owner_status =
            sao_ui_compositor_require_owner_thread(borrowed_compositor);
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
    } else {
        retry_failed_owned_construction_cleanup();
    }
    SaoUiEntityShellConfig effective = config == nullptr ? SaoUiEntityShellConfig{} : *config;
    SaoOverlayHostState host_state{};
    if (host != nullptr) {
        const sao_status_t host_status = sao_ui_overlay_host_get_state(host, &host_state);
        if (host_status != SAO_STATUS_OK)
            return host_status;
        effective.width = host_state.geometry.width;
        effective.height = host_state.geometry.height;
        effective.origin_x = host_state.geometry.x;
        effective.origin_y = host_state.geometry.y;
    }
    if (effective.width < SAO_UI_NERVEGEAR_SIZE + kMarginRight ||
        effective.height < SAO_UI_NERVEGEAR_SIZE + kMarginBottom) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    auto shell = std::unique_ptr<sao_ui_entity_shell_s>(new (std::nothrow) sao_ui_entity_shell_s());
    if (shell == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    shell->host = host;
    shell->compositor = borrowed_compositor;
    shell->owns_compositor = borrowed_compositor == nullptr;
    shell->nervegear_input = {shell.get(), false};
    shell->menu_input = {shell.get(), true};
    shell->config = effective;
    shell->origin_x = effective.origin_x;
    shell->origin_y = effective.origin_y;
    shell->width = effective.width;
    shell->height = effective.height;
    shell->nervegear_x = std::max(0, effective.width - kMarginRight - SAO_UI_NERVEGEAR_SIZE);
    shell->nervegear_y = std::max(0, effective.height - kMarginBottom - SAO_UI_NERVEGEAR_SIZE);
    const int32_t menu_max_x = std::max(0, effective.width - kMenuWidth);
    const int32_t menu_max_y = std::max(0, effective.height - kMenuHeight);
    shell->menu_x =
        std::clamp(shell->nervegear_x + SAO_UI_NERVEGEAR_SIZE - kMenuWidth, 0, menu_max_x);
    shell->menu_y = std::clamp(shell->nervegear_y - kMenuHeight - 12, 0, menu_max_y);
    shell->owner_thread = std::this_thread::get_id();
    try {
        shell->roots = make_default_roots(shell->nervgear_mode);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    std::vector<SaoUiLayerInputRect> nervegear_input_rects;
    sao_status_t status = SAO_STATUS_OK;
    if (shell->owns_compositor)
        status = sao_ui_compositor_create(host, nullptr, &shell->compositor);
    if (status == SAO_STATUS_OK) {
        status = sao_ui_theme_create(&shell->theme);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_theme_set_active(shell->theme, SAO_UI_THEME_GLASS);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_menu_create(shell->compositor, shell->theme,
                                    SAO_UI_MENU_MODE_VERTICAL_STRIP, &shell->menu);
    }
    if (status == SAO_STATUS_OK) {
        status = apply_visible_roots_locked(shell.get());
    }
    if (status == SAO_STATUS_OK) {
        SaoUiMenuLayout layout{};
        layout.center_x = shell->menu_x + kMenuColumnCenter;
        layout.center_y = shell->menu_y + kMenuPad;
        layout.inner_radius = 60;
        layout.outer_radius = 160;
        layout.child_ring_radius = 240;
        layout.button_size = 54;
        layout.button_max_size = 70;
        layout.slot_size = kMenuSlot;
        layout.max_visible = 9;
        status = sao_ui_menu_set_layout(shell->menu, &layout);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_nervegear_create(
            shell->compositor, shell->theme, shell->origin_x + shell->nervegear_x,
            shell->origin_y + shell->nervegear_y, SAO_UI_NG_PALETTE_DARK, &shell->nervegear);
    }
    if (status == SAO_STATUS_OK) {
        int32_t center_x = 0;
        int32_t center_y = 0;
        int32_t radius = 0;
        status = sao_ui_nervegear_get_hit_shape(shell->nervegear, &center_x, &center_y, &radius);
        if (status == SAO_STATUS_OK) {
            const int32_t local_center_x = center_x - shell->origin_x - shell->nervegear_x;
            const int32_t local_center_y = center_y - shell->origin_y - shell->nervegear_y;
            try {
                nervegear_input_rects = circular_input_rects(
                    local_center_x, local_center_y, radius, SAO_UI_NERVEGEAR_SIZE,
                    SAO_UI_NERVEGEAR_SIZE);
            } catch (...) {
                status = SAO_STATUS_ERR_UNKNOWN;
            }
        }
    }
    if (status == SAO_STATUS_OK) {
        SaoLayerConfig layer{};
        layer.struct_size = sizeof(SaoLayerConfig);
        layer.name_utf8 = "entity.nervegear";
        layer.x = shell->nervegear_x;
        layer.y = shell->nervegear_y;
        layer.width = SAO_UI_NERVEGEAR_SIZE;
        layer.height = SAO_UI_NERVEGEAR_SIZE;
        layer.z_order = 20;
        layer.click_through = false;
        layer.rect_hit = false;
        layer.bgra_swizzle = true;
        layer.high_fps = true;
        layer.target_fps = 60;
        status = sao_ui_layer_create(shell->compositor, &layer, &shell->nervegear_layer);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_rects(shell->nervegear_layer,
                                              nervegear_input_rects.data(),
                                              nervegear_input_rects.size());
    }
    if (status == SAO_STATUS_OK) {
        SaoLayerConfig layer{};
        layer.struct_size = sizeof(SaoLayerConfig);
        layer.name_utf8 = "entity.menu";
        layer.x = shell->menu_x;
        layer.y = shell->menu_y;
        layer.width = kMenuWidth;
        layer.height = kMenuHeight;
        layer.z_order = 10;
        layer.click_through = false;
        layer.rect_hit = false;
        layer.bgra_swizzle = true;
        layer.high_fps = true;
        layer.target_fps = 60;
        status = sao_ui_layer_create(shell->compositor, &layer, &shell->menu_layer);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_visible(shell->menu_layer, false);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_enabled(shell->menu_layer, false);
    }
    if (status == SAO_STATUS_OK)
        status = raster_and_upload_locked(shell.get());
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_callbacks(
            shell->nervegear_layer, &entity_layer_cursor, &entity_layer_leave,
            &entity_layer_button, &entity_layer_scroll, &shell->nervegear_input);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_callbacks(
            shell->menu_layer, &entity_layer_cursor, &entity_layer_leave, &entity_layer_button,
            &entity_layer_scroll, &shell->menu_input);
    }
    if (status == SAO_STATUS_OK &&
        g_fail_next_entity_shell_construction.exchange(false, std::memory_order_acq_rel)) {
        status = SAO_STATUS_ERR_UNKNOWN;
    }
    if (status != SAO_STATUS_OK) {
        const sao_status_t cleanup_status =
            destroy_members(shell.get(), !shell->owns_compositor);
        if (cleanup_status != SAO_STATUS_OK) {
            if (shell->owns_compositor) {
                try {
                    quarantine_failed_owned_construction(std::move(shell));
                } catch (...) {
                    (void)shell.release();
                }
            }
            return cleanup_status;
        }
        return status;
    }
    *out_handle = shell.release();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_create(sao_ui_overlay_host_handle_t host, const SaoUiEntityShellConfig* config,
                           sao_ui_entity_shell_handle_t* out_handle) {
    return create_entity_shell(host, nullptr, config, out_handle);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_create_on_compositor(
    sao_ui_compositor_handle_t compositor, const SaoUiEntityShellConfig* config,
    sao_ui_entity_shell_handle_t* out_handle) {
    if (compositor == nullptr) {
        if (out_handle != nullptr)
            *out_handle = nullptr;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return create_entity_shell(sao_ui_compositor_host(compositor), compositor, config, out_handle);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_try_destroy(sao_ui_entity_shell_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_OK;
    if (std::this_thread::get_id() != handle->owner_thread)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    try {
        {
            std::lock_guard<std::mutex> lock(handle->mutex);
            if (handle->callback_depth > 0) {
                handle->destroy_pending = true;
                return SAO_STATUS_ERR_CANCELLED;
            }
        }
        if (!handle->teardown_started) {
            const sao_status_t offline_status = sao_ui_entity_shell_take_offline(handle);
            if (offline_status != SAO_STATUS_OK)
                return offline_status;
            handle->teardown_started = true;
        }
        const sao_status_t status = destroy_members(handle);
        if (status != SAO_STATUS_OK)
            return status;
        delete handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_entity_shell_destroy(sao_ui_entity_shell_handle_t handle) {
    (void)sao_ui_entity_shell_try_destroy(handle);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_bring_online(sao_ui_entity_shell_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->online)
        return SAO_STATUS_OK;
    handle->online = true;
    handle->overlay_visible = true;
    handle->visual_dirty = true;
    sao_status_t status = sao_ui_nervegear_show(handle->nervegear);
    status = first_failure(status, sao_ui_layer_set_visible(handle->nervegear_layer, true));
    status = first_failure(status, sao_ui_layer_set_input_enabled(handle->nervegear_layer, true));
    status = first_failure(status, sync_frame_locked(handle, 0));
    if (handle->owns_compositor && handle->host != nullptr) {
        status = first_failure(status, sao_ui_overlay_host_set_visible(handle->host, true));
    }
    if (status != SAO_STATUS_OK) {
        if (handle->owns_compositor && handle->host != nullptr) {
            (void)sao_ui_overlay_host_set_visible(handle->host, false);
        }
        (void)sao_ui_layer_set_input_enabled(handle->nervegear_layer, false);
        (void)sao_ui_layer_set_visible(handle->nervegear_layer, false);
        (void)sao_ui_nervegear_hide(handle->nervegear);
        handle->online = false;
        handle->overlay_visible = false;
    }
    handle->last_status = status;
    return status;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_take_offline(sao_ui_entity_shell_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    sao_status_t status = set_menu_visibility_locked(handle, false);
    handle->online = false;
    handle->overlay_visible = false;
    handle->visual_dirty = true;
    status = first_failure(status, sao_ui_nervegear_hide(handle->nervegear));
    status = first_failure(status, commit_visual_state_locked(handle));
    if (handle->owns_compositor && handle->host != nullptr) {
        status = first_failure(status, sao_ui_overlay_host_set_visible(handle->host, false));
    }
    handle->last_status = status;
    return status;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_set_nervgear_mode(sao_ui_entity_shell_handle_t handle, bool enabled) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->nervgear_mode == enabled) {
        handle->last_status = SAO_STATUS_OK;
        return SAO_STATUS_OK;
    }

    const bool previous_mode = handle->nervgear_mode;
    std::vector<OwnedRootItem> candidate;
    try {
        candidate = handle->roots;
    } catch (...) {
        handle->last_status = SAO_STATUS_ERR_UNKNOWN;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    for (auto& root : candidate) {
        for (auto& child : root.children) {
            if (child.action_id == SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR) {
                child.name = enabled ? "NervGear: ON" : "NervGear: OFF";
            }
        }
    }
    handle->nervgear_mode = enabled;
    const bool roots_changed = candidate != handle->roots;
    sao_status_t status =
        roots_changed ? replace_roots_locked(handle, std::move(candidate)) : SAO_STATUS_OK;
    if (status == SAO_STATUS_OK && !roots_changed) {
        handle->visual_dirty = true;
        status = commit_visual_state_locked(handle);
    }
    if (status == SAO_STATUS_OK) {
        handle->last_status = SAO_STATUS_OK;
        return SAO_STATUS_OK;
    }

    const sao_status_t failure_status = status;
    handle->nervgear_mode = previous_mode;
    handle->visual_dirty = true;
    (void)commit_visual_state_locked(handle);
    handle->last_status = failure_status;
    return failure_status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_set_children(
    sao_ui_entity_shell_handle_t handle, const char* parent_name_utf8,
    const SaoUiMenuItem* items, size_t item_count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (parent_name_utf8 == nullptr || (items == nullptr && item_count > 0)) {
        handle->last_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (item_count > kMaxChildrenPerRoot) {
        handle->last_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        size_t parent_bytes = 0;
        std::string parent_name;
        sao_status_t status = copy_validated_utf8(parent_name_utf8, kMaxLabelBytes, true,
                                                  &parent_bytes, &parent_name);
        if (status != SAO_STATUS_OK) {
            handle->last_status = status;
            return status;
        }
        const int32_t root_index = find_root_by_name(handle->roots, parent_name);
        if (root_index < 0) {
            handle->last_status = SAO_STATUS_ERR_NOT_FOUND;
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        size_t total_children = 0;
        size_t total_bytes = 0;
        status = measure_tree_without_children(handle->roots, static_cast<size_t>(root_index),
                                               &total_children, &total_bytes);
        if (status != SAO_STATUS_OK || item_count > kMaxTotalChildren - total_children) {
            handle->last_status = SAO_STATUS_ERR_INVALID_ARGUMENT;
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        std::vector<OwnedRootItem> candidate = handle->roots;
        auto& children = candidate[static_cast<size_t>(root_index)].children;
        children.clear();
        children.reserve(item_count);
        for (size_t index = 0; index < item_count; ++index) {
            OwnedMenuItem child{};
            const char* name = items[index].name_utf8 == nullptr ? "" : items[index].name_utf8;
            const char* icon = items[index].icon_utf8 == nullptr ? "" : items[index].icon_utf8;
            status = copy_validated_utf8(name, kMaxLabelBytes, false, &total_bytes, &child.name);
            if (status == SAO_STATUS_OK) {
                status = copy_validated_utf8(icon, kMaxIconBytes, false, &total_bytes, &child.icon);
            }
            if (status != SAO_STATUS_OK) {
                handle->last_status = status;
                return status;
            }
            child.action_id = items[index].action_id;
            child.can_activate = items[index].can_activate;
            children.push_back(std::move(child));
        }
        return replace_roots_locked(handle, std::move(candidate));
    } catch (...) {
        handle->last_status = SAO_STATUS_ERR_UNKNOWN;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_set_roots(
    sao_ui_entity_shell_handle_t handle, const SaoUiEntityRootItem* roots, size_t root_count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    std::vector<OwnedRootItem> candidate;
    const sao_status_t validation_status = build_root_candidate(roots, root_count, &candidate);
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (validation_status != SAO_STATUS_OK) {
        handle->last_status = validation_status;
        return validation_status;
    }
    return replace_roots_locked(handle, std::move(candidate));
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_tick(sao_ui_entity_shell_handle_t handle,
                                                             uint32_t elapsed_ms) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (elapsed_ms > 1000U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->online)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    return sync_frame_locked(handle, elapsed_ms);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_handle_mouse(
    sao_ui_entity_shell_handle_t handle, uint32_t message, int32_t screen_x, int32_t screen_y,
    int32_t button, int32_t wheel_delta) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    PendingEntityAction pending_action{};
    sao_status_t status = SAO_STATUS_OK;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!handle->online || !handle->overlay_visible) {
            return SAO_STATUS_OK;
        }
        if (message == kMouseWheel) {
            bool viewport_changed = false;
            status = scroll_root_viewport_locked(handle, screen_x, screen_y, wheel_delta,
                                                 &viewport_changed);
            if (status == SAO_STATUS_OK && !viewport_changed) {
                status = scroll_child_viewport_locked(handle, screen_x, screen_y, wheel_delta,
                                                      &viewport_changed);
            }
            if (status == SAO_STATUS_OK && viewport_changed)
                status = commit_visual_state_locked(handle);
            handle->last_status = status;
            return status;
        }

        const bool nervegear_hit = local_nervegear_hit_locked(handle, screen_x, screen_y);
        const MenuHit menu_hit = menu_hit_locked(handle, screen_x, screen_y);
        const int32_t menu_index = menu_hit.child >= 0 ? -1 : menu_hit.parent;
        SaoUiNerveGearState previous_state = SAO_UI_NG_STATE_IDLE;
        status = sao_ui_nervegear_get_state(handle->nervegear, &previous_state);
        const int32_t previous_hover = handle->menu_hover_index;
        const int32_t previous_pressed = handle->menu_pressed_index;
        const int32_t previous_child_hover = handle->menu_hover_child_index;
        const int32_t previous_child_pressed = handle->menu_pressed_child_index;
        const bool previous_menu_visible = handle->menu_visible;
        if (message == kMouseMove) {
            status = nervegear_hit ? sao_ui_nervegear_on_mouse_enter(handle->nervegear)
                                   : sao_ui_nervegear_on_mouse_leave(handle->nervegear);
            if (handle->menu_hover_index != menu_index) {
                handle->menu_hover_index = menu_index;
                status = first_failure(status, sao_ui_menu_set_hover(handle->menu, menu_index));
            }
            const int32_t child_parent = menu_hit.child >= 0 ? menu_hit.parent : -1;
            if (handle->menu_hover_child_parent != child_parent ||
                handle->menu_hover_child_index != menu_hit.child) {
                handle->menu_hover_child_parent = child_parent;
                handle->menu_hover_child_index = menu_hit.child;
                status = first_failure(
                    status, sao::ui::menu_visual::set_child_hover(
                                handle->menu, handle->menu_hover_child_parent,
                                handle->menu_hover_child_index));
            }
        } else if (message == kMouseLeave) {
            status = sao_ui_nervegear_on_mouse_leave(handle->nervegear);
            handle->menu_hover_index = -1;
            handle->menu_hover_child_parent = -1;
            handle->menu_hover_child_index = -1;
            status = first_failure(status, sao_ui_menu_set_hover(handle->menu, -1));
            status = first_failure(
                status, sao::ui::menu_visual::set_child_hover(handle->menu, -1, -1));
        } else if (message == kLeftButtonDown && button == 0) {
            if (nervegear_hit) {
                status = sao_ui_nervegear_on_mouse_enter(handle->nervegear);
                status = first_failure(status, sao_ui_nervegear_on_mouse_down(handle->nervegear));
            }
            handle->menu_pressed_index = menu_index;
            handle->menu_pressed_child_parent =
                menu_hit.child >= 0 ? menu_hit.parent : -1;
            handle->menu_pressed_child_index = menu_hit.child;
        } else if (message == kLeftButtonUp && button == 0) {
            if (nervegear_hit) {
                status = sao_ui_nervegear_on_mouse_up(handle->nervegear);
                if (status == SAO_STATUS_OK && !handle->menu_visible) {
                    status = set_menu_visibility_locked(handle, true);
                }
                status = first_failure(
                    status, sao_ui_nervegear_transition(handle->nervegear, SAO_UI_NG_STATE_IDLE));
                status = first_failure(status, sao_ui_nervegear_on_mouse_enter(handle->nervegear));
            } else if (menu_hit.child >= 0 &&
                       menu_hit.parent == handle->menu_pressed_child_parent &&
                       menu_hit.child == handle->menu_pressed_child_index) {
                bool activated = false;
                int32_t action_id = -1;
                status = sao::ui::menu_visual::activate_child(
                    handle->menu, menu_hit.parent, menu_hit.child, &activated, &action_id);
                if (status == SAO_STATUS_OK && activated && action_id >= 0 &&
                    handle->config.action_fn != nullptr) {
                    const auto action = static_cast<SaoUiEntityAction>(action_id);
                    if (action == SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR) {
                        status = set_menu_visibility_locked(handle, false);
                    }
                    if (status == SAO_STATUS_OK) {
                        pending_action.callback = handle->config.action_fn;
                        pending_action.user_data = handle->config.action_user_data;
                        pending_action.action = action;
                    }
                }
            } else if (menu_index >= 0 && menu_index == handle->menu_pressed_index) {
                const int32_t logical_index = physical_to_logical_root_locked(handle, menu_index);
                if (logical_index >= 0) {
                    const auto& root = handle->roots[static_cast<size_t>(logical_index)];
                    status = sao_ui_menu_activate(handle->menu, menu_index);
                    if (status == SAO_STATUS_OK && root.can_activate) {
                        sao::ui::menu_visual::Snapshot menu_snapshot{};
                        status = sao::ui::menu_visual::get_snapshot(handle->menu, &menu_snapshot);
                        if (status == SAO_STATUS_OK) {
                            handle->active_root_id =
                                menu_snapshot.active_root_idx == menu_index ? root.id : "";
                        }
                    }
                    if (status == SAO_STATUS_OK && root.can_activate && root.children.empty() &&
                        root.action_id >= 0) {
                        const auto action = static_cast<SaoUiEntityAction>(root.action_id);
                        if (action == SAO_UI_ENTITY_ACTION_OPEN_ABOUT ||
                            action == SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR) {
                            status = set_menu_visibility_locked(handle, false);
                        }
                        if (status == SAO_STATUS_OK && handle->config.action_fn != nullptr) {
                            pending_action.callback = handle->config.action_fn;
                            pending_action.user_data = handle->config.action_user_data;
                            pending_action.action = action;
                        }
                    }
                }
            }
            handle->menu_pressed_index = -1;
            handle->menu_pressed_child_parent = -1;
            handle->menu_pressed_child_index = -1;
        }
        SaoUiNerveGearState current_state = previous_state;
        status =
            first_failure(status, sao_ui_nervegear_get_state(handle->nervegear, &current_state));
        handle->visual_dirty = handle->visual_dirty || previous_state != current_state ||
                               previous_hover != handle->menu_hover_index ||
                               previous_pressed != handle->menu_pressed_index ||
                               previous_child_hover != handle->menu_hover_child_index ||
                               previous_child_pressed != handle->menu_pressed_child_index ||
                               previous_menu_visible != handle->menu_visible;
        status = first_failure(status, commit_visual_state_locked(handle));
        if (status == SAO_STATUS_OK && pending_action.callback != nullptr) {
            ++handle->action_count;
            ++handle->callback_depth;
        }
        handle->last_status = status;
    }
    if (status == SAO_STATUS_OK && pending_action.callback != nullptr) {
        return dispatch_entity_action(handle, pending_action);
    }
    return status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_hit_test(
    sao_ui_entity_shell_handle_t handle, int32_t screen_x, int32_t screen_y, bool* out_hit) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_hit == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_hit = false;
    if (!handle->online || !handle->overlay_visible)
        return SAO_STATUS_OK;
    if (local_nervegear_hit_locked(handle, screen_x, screen_y)) {
        *out_hit = true;
        return SAO_STATUS_OK;
    }
    *out_hit = menu_hit_locked(handle, screen_x, screen_y).parent >= 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_home(sao_ui_entity_shell_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->online)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    sao_status_t status = set_menu_visibility_locked(handle, !handle->menu_visible);
    status = first_failure(status, commit_visual_state_locked(handle));
    handle->last_status = status;
    return status;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_insert(sao_ui_entity_shell_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->online)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    handle->overlay_visible = !handle->overlay_visible;
    handle->visual_dirty = true;
    sao_status_t status = commit_visual_state_locked(handle);
    if (handle->owns_compositor && handle->host != nullptr) {
        status = first_failure(
            status, sao_ui_overlay_host_set_visible(handle->host, handle->overlay_visible));
    }
    handle->last_status = status;
    return status;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_get_snapshot(
    sao_ui_entity_shell_handle_t handle, SaoUiEntityShellSnapshot* out_snapshot) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_snapshot = {};
    out_snapshot->origin_x = handle->origin_x;
    out_snapshot->origin_y = handle->origin_y;
    out_snapshot->width = handle->width;
    out_snapshot->height = handle->height;
    out_snapshot->nervegear_x = handle->nervegear_x;
    out_snapshot->nervegear_y = handle->nervegear_y;
    out_snapshot->menu_x = handle->menu_x;
    out_snapshot->menu_y = handle->menu_y;
    out_snapshot->menu_width = kMenuWidth;
    out_snapshot->menu_height = kMenuHeight;
    out_snapshot->menu_hover_index = handle->menu_hover_index;
    out_snapshot->menu_pressed_index = handle->menu_pressed_index;
    (void)sao_ui_nervegear_get_state(handle->nervegear, &out_snapshot->nervegear_state);
    out_snapshot->online = handle->online;
    out_snapshot->overlay_visible = handle->overlay_visible;
    out_snapshot->menu_visible = handle->menu_visible;
    out_snapshot->frame_count = handle->frame_count;
    out_snapshot->action_count = handle->action_count;
    out_snapshot->last_status = handle->last_status;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_get_root_snapshot(
    sao_ui_entity_shell_handle_t handle, SaoUiEntityRootSnapshot* out_snapshot) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_snapshot = {};
    out_snapshot->root_count = handle->roots.size();
    out_snapshot->first_visible_root_index = handle->first_visible_root_index;
    out_snapshot->visible_root_count = visible_root_count(handle);
    out_snapshot->root_tree_revision = handle->root_tree_revision;
    const size_t copy_bytes = std::min(handle->active_root_id.size(),
                                       static_cast<size_t>(SAO_UI_ENTITY_ROOT_ID_CAPACITY - 1U));
    if (copy_bytes > 0) {
        std::memcpy(out_snapshot->active_root_id_utf8, handle->active_root_id.data(), copy_bytes);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_snapshot_bgra(
    sao_ui_entity_shell_handle_t handle, uint8_t* out_bgra_pixels, size_t capacity,
    uint32_t* out_width, uint32_t* out_height, size_t* out_bytes) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mutex);
    return sao_ui_compositor_snapshot_bgra(handle->compositor, out_bgra_pixels, capacity, out_width,
                                           out_height, out_bytes);
}
