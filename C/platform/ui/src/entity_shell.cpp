#include "sao/ui/entity_shell.h"
#include "sao/ui/d2d_effects.h"

#include "sao/ui/compositor.h"
#include "sao/ui/menu.h"
#include "sao/ui/theme.h"

#include "../assets/classic/classic_icons.h"
#include "classic_text_roles.h"
#include "layer_paint_internal.h"
#include "menu_visual_internal.h"
#include "panel_theme_internal.h"
#include "widget_paint_internal.h"

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
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
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
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_visual_budget(sao_ui_menu_handle_t handle,
                                                                  bool reduced_motion,
                                                                  bool fps_pressure);
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
constexpr int32_t kChildRowHeight = 44;
constexpr int32_t kChildRowStride = 47;
constexpr int32_t kChildTargetWidth = 240;

int32_t entity_child_row_x() noexcept {
    return sao::ui::menu_visual::visual_child_row_x(
        kMenuColumnCenter, kMenuSlot, sao::ui::menu_visual::kVisualDefaultChildRadius);
}
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
constexpr uint32_t kRightButtonDown = 0x0204;
constexpr uint32_t kRightButtonUp = 0x0205;
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
    return desktop_local * static_cast<int64_t>(96) / static_cast<int64_t>(dpi);
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
        "",
        10,
        true,
        {
            {"置顶: OFF", "", SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, false},
            {nervgear_mode ? "NervGear: ON" : "NervGear: OFF", "",
             SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR, true},
            {"──────────", "", -1, false},
            {"Streaming Mode: OFF", "", SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE, true},
            {"鱼眼背景: 程序生成", "", SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL, false},
            {"鱼眼背景: 实时截屏", "", SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE, false},
            {"──────────", "", -1, false},
            {"保存设置", "", SAO_UI_ENTITY_ACTION_SAVE_SETTINGS, true},
        },
    });
    roots.push_back({
        "Tools",
        "Tools",
        "",
        11,
        true,
        {
            {"AI Editor (LLM)", "", SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, true},
            {"Workshop", "", SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, false},
            {"Process Selector", "", SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR, false},
        },
    });
    roots.push_back({
        "Plugins",
        "Plugins",
        "",
        12,
        true,
        {
            {"插件管理面板 Manage", "", SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER, false},
            {"重载全部插件 Reload", "", SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, true},
            {"无已启用面板插件 (去 Manage 启用)", "", -1, false},
        },
    });
    roots.push_back({
        "Skins",
        "Skins",
        "",
        13,
        true,
        {
            {"全部 Light", "", SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, true},
            {"全部 Dark", "", SAO_UI_ENTITY_ACTION_SET_ALL_DARK, true},
        },
    });
    roots.push_back({"About", "About", "", SAO_UI_ENTITY_ACTION_OPEN_ABOUT, true, {}});
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
    if (clamped_radius == 0 || (px >= x + clamped_radius && px < x + width - clamped_radius) ||
        (py >= y + clamped_radius && py < y + height - clamped_radius)) {
        return true;
    }
    const int32_t center_x =
        px < x + clamped_radius ? x + clamped_radius - 1 : x + width - clamped_radius;
    const int32_t center_y =
        py < y + clamped_radius ? y + clamped_radius - 1 : y + height - clamped_radius;
    const int32_t dx = px - center_x;
    const int32_t dy = py - center_y;
    return dx * dx + dy * dy <= clamped_radius * clamped_radius;
}

void fill_rounded_rect(Raster& raster, int32_t x, int32_t y, int32_t width, int32_t height,
                       int32_t radius, Color color,
                       BlendRounding rounding = BlendRounding::Nearest) {
    for (int32_t py = std::max(0, y);
         py < std::min(static_cast<int32_t>(raster.height), y + height); ++py) {
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

std::optional<sao::ui::classic::IconId> classic_icon_for_action(int32_t action_id) noexcept {
    using sao::ui::classic::IconId;
    switch (action_id) {
    case SAO_UI_ENTITY_ACTION_OPEN_ABOUT:
        return IconId::Info;
    case SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST:
        return IconId::Expand;
    case SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR:
        return IconId::Lock;
    case SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE:
        return IconId::Notification;
    case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS:
        return IconId::Confirm;
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL:
        return IconId::Tools;
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE:
        return IconId::Search;
    case SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR:
        return IconId::Chat;
    case SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP:
        return IconId::Workshop;
    case SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR:
        return IconId::Process;
    case SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER:
        return IconId::Plugins;
    case SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS:
        return IconId::Refresh;
    case SAO_UI_ENTITY_ACTION_PLUGIN_STATUS:
        return IconId::Notification;
    case SAO_UI_ENTITY_ACTION_OPEN_LICENSE_ACTIVATION:
        return IconId::Lock;
    case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
        return IconId::Home;
    case SAO_UI_ENTITY_ACTION_SET_ALL_DARK:
        return IconId::Lock;
    default:
        return std::nullopt;
    }
}

void draw_classic_icon(Raster& raster, sao::ui::classic::IconId id, float x, float y, float size,
                       Color color, BlendRounding rounding = BlendRounding::Nearest) {
    const auto& definition = sao::ui::classic::definition(id);
    const float scale = size / 24.0F;
    const int32_t thickness = std::max(1, static_cast<int32_t>(std::lround(scale * 1.65F)));
    for (size_t index = 0; index < definition.stroke_count; ++index) {
        const auto& stroke = definition.strokes[index];
        draw_line(raster, static_cast<int32_t>(std::lround(x + stroke.x1 * scale)),
                  static_cast<int32_t>(std::lround(y + stroke.y1 * scale)),
                  static_cast<int32_t>(std::lround(x + stroke.x2 * scale)),
                  static_cast<int32_t>(std::lround(y + stroke.y2 * scale)), thickness, color,
                  rounding);
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
        return static_cast<uint8_t>(
            static_cast<int32_t>(static_cast<double>(start) +
                                 (static_cast<double>(end) - static_cast<double>(start)) * t));
    };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b), alpha};
}

uint8_t scaled_alpha(double base, double opacity) {
    return static_cast<uint8_t>(std::clamp(static_cast<int32_t>(base * opacity), 0, 255));
}

Color panel_color(SaoUiColorToken token) noexcept {
    const uint32_t argb = sao::ui::detail::panel_theme_color(token);
    return {static_cast<uint8_t>((argb >> 16U) & 0xFFU), static_cast<uint8_t>((argb >> 8U) & 0xFFU),
            static_cast<uint8_t>(argb & 0xFFU), static_cast<uint8_t>((argb >> 24U) & 0xFFU)};
}

Color alpha_color(Color color, uint8_t alpha) noexcept {
    color.a = alpha;
    return color;
}

Color fade_color(Color color, double opacity) noexcept {
    color.a = scaled_alpha(color.a, opacity);
    return color;
}

bool menu_decoration_enabled(const sao::ui::menu_visual::Snapshot& snapshot,
                             bool high_contrast) noexcept {
    return !high_contrast && !snapshot.reduced_motion && !snapshot.fps_pressure;
}

bool root_row_disabled(const sao::ui::menu_visual::RootRowSnapshot& row) {
    return !row.can_activate || row.state == SAO_UI_MENU_BTN_DISABLED;
}

bool child_row_disabled(const sao::ui::menu_visual::ChildRowSnapshot& row) {
    return !row.can_activate || row.state == SAO_UI_MENU_BTN_DISABLED;
}

bool windows_reduced_motion() noexcept {
#if defined(_WIN32)
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) != FALSE)
        return animations == FALSE;
#endif
    return false;
}

int32_t adaptive_corner_radius(int32_t width, int32_t height) {
    const int32_t maximum = std::max(0, std::min(width, height) / 2);
    return std::clamp((height + 3) / 4, 2, std::max(2, maximum));
}

void draw_child_overlay(Raster& raster, const sao::ui::menu_visual::Snapshot& snapshot,
                        size_t first_visible_child_index, int32_t pressed_child_index,
                        bool classic_default_icons) {
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    const bool decoration_enabled = menu_decoration_enabled(snapshot, high_contrast);
    const Color child_background = panel_color(SAO_UI_TOKEN_CHILD_BG);
    const Color child_hover = panel_color(SAO_UI_TOKEN_CHILD_HOVER);
    const Color child_hover_text = panel_color(SAO_UI_TOKEN_CHILD_HOVER_FG);
    const Color child_text = panel_color(SAO_UI_TOKEN_CHILD_TEXT);
    const Color child_line = panel_color(SAO_UI_TOKEN_CHILD_LINE);
    const Color child_icon = panel_color(SAO_UI_TOKEN_CHILD_ICON);
    const Color accent = panel_color(SAO_UI_TOKEN_APP_ACCENT);
    const Color gold = panel_color(high_contrast ? SAO_UI_TOKEN_FOCUS_RING : SAO_UI_TOKEN_APP_GOLD);
    const Color disabled_bg = panel_color(SAO_UI_TOKEN_DISABLED_BG);
    const Color disabled_border = panel_color(SAO_UI_TOKEN_DISABLED_BORDER);
    const Color disabled_fg = panel_color(SAO_UI_TOKEN_DISABLED_FG);
    const Color pressed_surface = panel_color(SAO_UI_TOKEN_PRESSED_SURFACE);
    const Color app_bg = panel_color(SAO_UI_TOKEN_APP_BG);

    if (snapshot.active_root_idx >= 0 && snapshot.active_root_idx < kRootItemCount) {
        const int32_t center_y = kMenuPad + snapshot.active_root_idx * kMenuSlot + kMenuSlot / 2;
        stroke_circle(raster, kMenuColumnCenter, center_y, 31, 2, fade_color(gold, 0.92),
                      kChildBlendRounding);
    }

    const size_t available_row_count = first_visible_child_index < snapshot.rows.size()
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
    const uint8_t rail_glow_alpha =
        decoration_enabled ? scaled_alpha(126.0, snapshot.child_rail_glow_t * opacity) : 0;
    draw_line(raster, kChildLineCenterX - 2, kChildOriginY + 4, kChildLineCenterX + 2,
              kChildOriginY + row_count * kChildRowStride - 4, 2,
              alpha_color(accent, rail_glow_alpha), kChildBlendRounding);
    const int32_t line_height = row_count * kChildRowStride - 3;
    const int32_t line_top = kChildOriginY + 5;
    const uint8_t line_alpha = scaled_alpha(220.0, opacity);
    draw_line(raster, kChildLineCenterX, line_top + 5, kChildLineCenterX,
              line_top + line_height - 5, 4, alpha_color(child_line, line_alpha),
              kChildBlendRounding);
    draw_line(raster, kChildLineCenterX, line_top + 5, kChildLineCenterX,
              line_top + line_height - 5, 2, alpha_color(accent, scaled_alpha(150.0, opacity)),
              kChildBlendRounding);
    fill_circle(raster, kChildLineCenterX, line_top + 5, 2, alpha_color(child_line, line_alpha),
                kChildBlendRounding);
    fill_circle(raster, kChildLineCenterX, line_top + line_height - 5, 2,
                alpha_color(child_line, line_alpha), kChildBlendRounding);

    constexpr int32_t kArrowCenterX = kChildOriginX + 10 + 3 + 6;
    const int32_t arrow_center_y = kChildOriginY + 5 + line_height / 2;
    if (decoration_enabled) {
        for (int32_t glow_radius = 6; glow_radius > 0; glow_radius -= 2) {
            const int32_t glow_amount = static_cast<int32_t>(15.0 * (1.0 - glow_radius / 6.0));
            fill_circle(raster, kArrowCenterX, arrow_center_y, glow_radius,
                        {static_cast<uint8_t>(std::clamp(glow_amount * 4, 0, 255)),
                         static_cast<uint8_t>(std::clamp(glow_amount * 2, 0, 255)),
                         static_cast<uint8_t>(glow_amount), scaled_alpha(150.0, opacity)},
                        kChildBlendRounding);
        }
    }
    fill_circle(raster, kArrowCenterX, arrow_center_y, 3, fade_color(gold, opacity),
                kChildBlendRounding);
    stroke_circle(raster, kArrowCenterX, arrow_center_y, 3, 1, fade_color(accent, opacity),
                  kChildBlendRounding);

    for (int32_t slot = 0; slot < row_count; ++slot) {
        const size_t logical_index = first_visible_child_index + static_cast<size_t>(slot);
        const auto& row = snapshot.rows[logical_index];
        const int32_t row_width = std::clamp(row.visible_width_px, 0, kChildTargetWidth);
        if (row_width <= 1)
            continue;
        const bool disabled = child_row_disabled(row);
        const bool pressed =
            !disabled && static_cast<int32_t>(logical_index) == pressed_child_index;
        const float hover = std::clamp(row.hover_t, 0.0F, 1.0F);
        const int32_t row_y = kChildOriginY + slot * kChildRowStride + (pressed ? 1 : 0);
        const int32_t radius = adaptive_corner_radius(row_width, kChildRowHeight);
        Color background = disabled  ? fade_color(disabled_bg, opacity)
                           : pressed ? fade_color(pressed_surface, opacity)
                                     : lerp_rgb(child_background, child_hover, hover,
                                                scaled_alpha(255.0, opacity));
        Color foreground =
            disabled ? fade_color(disabled_fg, opacity)
                     : lerp_rgb(child_text, child_hover_text, hover, scaled_alpha(255.0, opacity));
        Color icon =
            disabled ? fade_color(disabled_fg, opacity)
                     : lerp_rgb(child_icon, child_hover_text, hover, scaled_alpha(245.0, opacity));
        Color indicator =
            disabled  ? fade_color(disabled_border, opacity)
            : pressed ? fade_color(gold, opacity)
                      : lerp_rgb(child_background, accent, hover, scaled_alpha(235.0, opacity));
        fill_rounded_rect(raster, entity_child_row_x(), row_y, row_width, kChildRowHeight, radius,
                          background, kChildBlendRounding);
        stroke_rounded_rect(
            raster, entity_child_row_x(), row_y, row_width, kChildRowHeight, radius, 1,
            pressed ? fade_color(gold, opacity)
                    : lerp_rgb(child_line, accent, hover, scaled_alpha(145.0, opacity)),
            kChildBlendRounding);
        fill_rounded_rect(raster, entity_child_row_x(), row_y, 2, kChildRowHeight, 2, indicator,
                          kChildBlendRounding);
        if (!disabled && row_width > 20) {
            const uint8_t glass_alpha =
                decoration_enabled ? scaled_alpha(30.0 + 30.0 * hover, opacity) : 0;
            draw_line(raster, entity_child_row_x() + 8, row_y + 1,
                      entity_child_row_x() + row_width - 8, row_y + 1, 1,
                      alpha_color(panel_color(SAO_UI_TOKEN_WHITE), glass_alpha),
                      kChildBlendRounding);
        }
        if (!disabled && decoration_enabled && (hover > 0.05F || pressed)) {
            const uint8_t spine_alpha =
                scaled_alpha(34.0 + 52.0 * hover + (pressed ? 24.0 : 0.0), opacity);
            draw_line(raster, kChildLineCenterX + 3, row_y + kChildRowHeight / 2,
                      entity_child_row_x(), row_y + kChildRowHeight / 2, 1,
                      alpha_color(accent, spine_alpha), kChildBlendRounding);
        }

        const int32_t icon_x = entity_child_row_x() + 2 + 8;
        const int32_t icon_y = row_y + (kChildRowHeight - kChildIconFontSize) / 2 - 2;
        const int32_t label_x = icon_x + kChildFallbackIconWidth + kChildIconGap;
        const int32_t label_y = row_y + (kChildRowHeight - kChildLabelFontSize) / 2 - 2;
        const int32_t caret_x = entity_child_row_x() + row_width - kChildRowPadRight - 6;
        const int32_t label_max_width =
            std::max(0, caret_x - kChildCaretWidth - kChildCaretGap - label_x);
        if (row_width >= 24) {
            const Color icon_well = pressed ? panel_color(SAO_UI_TOKEN_SELECTION) : app_bg;
            fill_rounded_rect(raster, icon_x - 4, row_y + 7, 20, 30, 5,
                              alpha_color(icon_well, scaled_alpha(120.0, opacity)),
                              kChildBlendRounding);
            stroke_rounded_rect(raster, icon_x - 4, row_y + 7, 20, 30, 5, 1,
                                alpha_color(accent, scaled_alpha(72.0 + 80.0 * hover, opacity)),
                                kChildBlendRounding);
        }
        const std::string_view icon_text(row.icon_utf8.data());
        const auto classic_id = classic_default_icons && icon_text.empty()
                                    ? classic_icon_for_action(row.action_id)
                                    : std::nullopt;
        if (classic_id.has_value()) {
            draw_classic_icon(raster, *classic_id, static_cast<float>(icon_x - 2),
                              static_cast<float>(row_y + 12), 18.0F, icon, kChildBlendRounding);
        }
#if defined(_WIN32)
        if (!classic_id.has_value() && !icon_text.empty()) {
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
        if (!classic_id.has_value() && !icon_text.empty()) {
            draw_text_clipped(raster, icon_x, icon_y, kChildFallbackIconWidth, icon_text, 1, icon,
                              kChildBlendRounding);
        }
        if (label_max_width > 4) {
            draw_text_clipped(raster, label_x, label_y, label_max_width,
                              std::string_view(row.name_utf8.data()), 1, foreground,
                              kChildBlendRounding, true);
        }
#endif
        if (!disabled && hover > 0.05F && row_width >= 18) {
            const Color caret =
                lerp_rgb(child_text, child_hover_text, hover, scaled_alpha(255.0, opacity));
            const int32_t caret_y = row_y + 15;
            draw_line(raster, caret_x, caret_y, caret_x + 3, caret_y + 3, 1, caret,
                      kChildBlendRounding);
            draw_line(raster, caret_x + 3, caret_y + 3, caret_x, caret_y + 6, 1, caret,
                      kChildBlendRounding);
        }
        if (slot + 1 < row_count && row_width > 16) {
            draw_line(raster, entity_child_row_x() + 8, row_y + kChildRowHeight + 1,
                      entity_child_row_x() + std::max(8, row_width - 8),
                      row_y + kChildRowHeight + 1, 1,
                      alpha_color(child_line, scaled_alpha(80.0, opacity)), kChildBlendRounding);
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
    const size_t capacity =
        static_cast<size_t>(1 + (static_cast<int64_t>(max_width) - glyph_width) / advance);
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
        const unsigned char character = value <= 0x7FU ? static_cast<unsigned char>(value) : '?';
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

struct ImmutableAuthorityFrame {
    uint32_t width{};
    uint32_t height{};
    std::shared_ptr<const std::vector<Pixel>> pixels;
};

struct AuthorityFrameCache {
    std::array<ImmutableAuthorityFrame, 9> frames{};
};

const AuthorityFrameCache& authority_frame_cache() {
    static const AuthorityFrameCache cache = [] {
        AuthorityFrameCache result{};
        const auto load = [&result](size_t index, int32_t resource_id, uint32_t width,
                                    uint32_t height) {
            Raster raster = load_authority_frame(resource_id, width, height);
            result.frames[index] = {
                width, height,
                std::make_shared<const std::vector<Pixel>>(std::move(raster.pixels))};
        };
        load(0, SAO_UI_ENTITY_AUTHORITY_NERVEGEAR_IDLE, SAO_UI_NERVEGEAR_SIZE,
             SAO_UI_NERVEGEAR_SIZE);
        load(1, SAO_UI_ENTITY_AUTHORITY_NERVEGEAR_HOVER, SAO_UI_NERVEGEAR_SIZE,
             SAO_UI_NERVEGEAR_SIZE);
        load(2, SAO_UI_ENTITY_AUTHORITY_NERVEGEAR_PRESSED, SAO_UI_NERVEGEAR_SIZE,
             SAO_UI_NERVEGEAR_SIZE);
        load(3, SAO_UI_ENTITY_AUTHORITY_MENU_IDLE, kMenuWidth, kMenuHeight);
        load(4, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_0, kMenuWidth, kMenuHeight);
        load(5, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_1, kMenuWidth, kMenuHeight);
        load(6, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_2, kMenuWidth, kMenuHeight);
        load(7, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_3, kMenuWidth, kMenuHeight);
        load(8, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_4, kMenuWidth, kMenuHeight);
        return result;
    }();
    return cache;
}

Raster copy_authority_frame(const ImmutableAuthorityFrame& frame) {
    if (frame.pixels == nullptr)
        throw std::runtime_error("Entity authority frame cache unavailable");
    return {frame.width, frame.height, *frame.pixels};
}

Raster blend_authority_menu(const sao::ui::menu_visual::Snapshot& snapshot, int32_t pressed_index) {
    constexpr std::array<int32_t, 5> kHoverResources{
        SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_0, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_1,
        SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_2, SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_3,
        SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_4,
    };
    int32_t selected_index = pressed_index;
    if (selected_index < 0) {
        const size_t count = std::min(snapshot.roots.size(), kHoverResources.size());
        for (size_t index = 0; index < count; ++index) {
            if (snapshot.roots[index].state == SAO_UI_MENU_BTN_HOVER) {
                selected_index = static_cast<int32_t>(index);
                break;
            }
        }
    }
    const int32_t resource_id =
        selected_index >= 0 && selected_index < static_cast<int32_t>(kHoverResources.size())
            ? kHoverResources[static_cast<size_t>(selected_index)]
            : SAO_UI_ENTITY_AUTHORITY_MENU_IDLE;
    const auto& cache = authority_frame_cache();
    const size_t cache_index =
        resource_id == SAO_UI_ENTITY_AUTHORITY_MENU_IDLE
            ? 3U
            : 4U + static_cast<size_t>(resource_id - SAO_UI_ENTITY_AUTHORITY_MENU_HOVER_0);
    return copy_authority_frame(cache.frames[cache_index]);
}
#endif

void draw_classic_default_root_icons(Raster& raster, const sao::ui::menu_visual::Snapshot& snapshot,
                                     const std::vector<OwnedRootItem>& roots) {
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    const Color circle_bg = panel_color(SAO_UI_TOKEN_CIRCLE_BG);
    const Color circle_hover_bg = panel_color(SAO_UI_TOKEN_CIRCLE_HOVER_BG);
    const Color circle_active_bg = panel_color(SAO_UI_TOKEN_CIRCLE_ACTIVE_BG);
    const Color circle_icon = panel_color(SAO_UI_TOKEN_CIRCLE_ICON);
    const Color circle_hover_icon = panel_color(SAO_UI_TOKEN_CIRCLE_HOVER_ICON);
    const Color circle_active_icon = panel_color(SAO_UI_TOKEN_CIRCLE_ACTIVE_ICON);
    const Color disabled_bg = panel_color(SAO_UI_TOKEN_DISABLED_BG);
    const Color disabled_fg = panel_color(SAO_UI_TOKEN_DISABLED_FG);
    const size_t visible = std::min(roots.size(), static_cast<size_t>(kRootItemCount));
    for (size_t index = 0; index < visible; ++index) {
        const auto id = classic_icon_for_action(roots[index].action_id);
        if (!id.has_value())
            continue;
        const bool has_visual = index < snapshot.roots.size();
        const auto& row =
            has_visual ? snapshot.roots[index] : sao::ui::menu_visual::RootRowSnapshot{};
        const bool disabled = has_visual && root_row_disabled(row);
        const bool active = has_visual && row.state == SAO_UI_MENU_BTN_ACTIVE;
        const float hover = has_visual ? std::clamp(row.hover_t, 0.0F, 1.0F) : 0.0F;
        const float fisheye = has_visual ? std::clamp(row.fisheye_t, 0.0F, 1.0F) : 0.0F;
        const float stagger = has_visual ? std::clamp(row.stagger_t, 0.0F, 1.0F) : 0.0F;
        SaoUiMenuLayout visual_layout{};
        visual_layout.button_size = sao::ui::menu_visual::kVisualButtonBaseSize;
        visual_layout.button_max_size = sao::ui::menu_visual::kVisualButtonMaxSize;
        const int32_t size =
            sao::ui::menu_visual::visual_button_diameter(visual_layout, fisheye * stagger);
        const int32_t center_y = kMenuPad + static_cast<int32_t>(index) * kMenuSlot + kMenuSlot / 2;
        const Color well = disabled        ? disabled_bg
                           : active        ? circle_active_bg
                           : hover > 0.01F ? lerp_rgb(circle_bg, circle_hover_bg, hover, 255)
                                           : circle_bg;
        const Color icon = disabled        ? disabled_fg
                           : active        ? circle_active_icon
                           : hover > 0.01F ? circle_hover_icon
                                           : circle_icon;
        fill_circle(raster, kMenuColumnCenter, center_y, std::max(8, size / 2 - 9),
                    alpha_color(well, high_contrast ? 255 : 235));
        const float icon_size = std::max(20.0F, static_cast<float>(size) * 0.54F);
        draw_classic_icon(raster, *id, static_cast<float>(kMenuColumnCenter) - icon_size * 0.5F,
                          static_cast<float>(center_y) - icon_size * 0.5F, icon_size, icon);
    }
}

Raster rasterize_nervegear(SaoUiNerveGearState state) {
#if defined(_WIN32)
    Raster raster =
        copy_authority_frame(authority_frame_cache().frames[state == SAO_UI_NG_STATE_PRESSED ? 2U
                                                            : state == SAO_UI_NG_STATE_HOVER ? 1U
                                                                                             : 0U]);
    if (state == SAO_UI_NG_STATE_LINKING || state == SAO_UI_NG_STATE_LINKED) {
        stroke_circle(raster, 36, 36, state == SAO_UI_NG_STATE_LINKING ? 34 : 32, 2,
                      Color{104, 228, 255, 165});
        draw_line(raster, 8, 36, 20, 36, 1, Color{212, 156, 23, 145});
        draw_line(raster, 52, 36, 64, 36, 1, Color{212, 156, 23, 145});
    }
    return raster;
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

Raster rasterize_generic_menu(int32_t pressed_index, const sao::ui::menu_visual::Snapshot& snapshot,
                              const std::vector<OwnedRootItem>& roots,
                              size_t first_visible_root_index) {
    Raster raster = make_raster(kMenuWidth, kMenuHeight);
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    const bool decoration_enabled = menu_decoration_enabled(snapshot, high_contrast);
    const Color app_bg = panel_color(SAO_UI_TOKEN_APP_BG);
    const Color panel_card = panel_color(SAO_UI_TOKEN_APP_CARD);
    const Color border = panel_color(SAO_UI_TOKEN_APP_BORDER);
    const Color text = panel_color(SAO_UI_TOKEN_APP_TEXT);
    const Color text_secondary = panel_color(SAO_UI_TOKEN_APP_TEXT_2);
    const Color accent = panel_color(SAO_UI_TOKEN_APP_ACCENT);
    const Color cyan = panel_color(SAO_UI_TOKEN_CORNER_CYAN);
    const Color gold = panel_color(high_contrast ? SAO_UI_TOKEN_FOCUS_RING : SAO_UI_TOKEN_APP_GOLD);
    const Color circle_bg = panel_color(SAO_UI_TOKEN_CIRCLE_BG);
    const Color circle_hover_bg = panel_color(SAO_UI_TOKEN_CIRCLE_HOVER_BG);
    const Color circle_hover_icon = panel_color(SAO_UI_TOKEN_CIRCLE_HOVER_ICON);
    const Color circle_active_bg = panel_color(SAO_UI_TOKEN_CIRCLE_ACTIVE_BG);
    const Color circle_active_icon = panel_color(SAO_UI_TOKEN_CIRCLE_ACTIVE_ICON);
    const Color circle_icon = panel_color(SAO_UI_TOKEN_CIRCLE_ICON);
    const Color circle_border = panel_color(SAO_UI_TOKEN_CIRCLE_BORDER);
    const Color disabled_bg = panel_color(SAO_UI_TOKEN_DISABLED_BG);
    const Color disabled_border = panel_color(SAO_UI_TOKEN_DISABLED_BORDER);
    const Color disabled_fg = panel_color(SAO_UI_TOKEN_DISABLED_FG);
    const Color pressed_surface = panel_color(SAO_UI_TOKEN_PRESSED_SURFACE);
    const float lens = std::clamp(snapshot.backdrop_lens_t, 0.0F, 1.0F);
    if (decoration_enabled) {
        for (int32_t radius = 48; radius >= 26; radius -= 6) {
            const uint8_t alpha = static_cast<uint8_t>(12.0F * lens * (1.0F - radius / 60.0F));
            fill_circle(raster, kMenuColumnCenter, kMenuPad, radius, alpha_color(cyan, alpha));
        }
    }
    const uint8_t panel_alpha =
        high_contrast ? 255 : static_cast<uint8_t>(std::max(190, static_cast<int>(panel_card.a)));
    fill_rounded_rect(raster, 15, 15, kMenuWidth - 30, kMenuHeight - 30, 16,
                      alpha_color(panel_card, panel_alpha));
    stroke_rounded_rect(raster, 15, 15, kMenuWidth - 30, kMenuHeight - 30, 16, 1,
                        alpha_color(border, high_contrast ? 255 : 220));
    fill_rect(raster, 34, 34, 3, kMenuHeight - 68, alpha_color(accent, high_contrast ? 255 : 220));
    fill_rect(raster, kMenuWidth - 37, 34, 3, kMenuHeight - 68,
              alpha_color(gold, high_contrast ? 255 : 220));
    draw_line(raster, 34, 34, 58, 34, 2, alpha_color(cyan, high_contrast ? 255 : 235));
    draw_line(raster, 34, 34, 34, 58, 2, alpha_color(cyan, high_contrast ? 255 : 235));
    draw_line(raster, kMenuWidth - 34, kMenuHeight - 34, kMenuWidth - 58, kMenuHeight - 34, 2,
              alpha_color(gold, high_contrast ? 255 : 235));
    draw_line(raster, kMenuWidth - 34, kMenuHeight - 34, kMenuWidth - 34, kMenuHeight - 58, 2,
              alpha_color(gold, high_contrast ? 255 : 235));

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
        const bool has_visual = slot < snapshot.roots.size();
        const sao::ui::menu_visual::RootRowSnapshot row =
            has_visual ? snapshot.roots[slot] : sao::ui::menu_visual::RootRowSnapshot{};
        const bool disabled = has_visual && root_row_disabled(row);
        const bool active = has_visual && row.state == SAO_UI_MENU_BTN_ACTIVE;
        const float hover = has_visual ? std::clamp(row.hover_t, 0.0F, 1.0F) : 0.0F;
        const float fisheye = has_visual ? std::clamp(row.fisheye_t, 0.0F, 1.0F) : 0.0F;
        const float stagger = has_visual ? std::clamp(row.stagger_t, 0.0F, 1.0F) : 0.0F;
        const float trail = has_visual ? std::clamp(row.selection_trail_t, 0.0F, 1.0F) : 0.0F;
        const float pulse = has_visual ? std::clamp(row.pressed_pulse_t, 0.0F, 1.0F) : 0.0F;
        const bool pressed = !disabled && physical_index == pressed_index;
        const bool hovered = hover > 0.01F || (has_visual && row.state == SAO_UI_MENU_BTN_HOVER);
        SaoUiMenuLayout visual_layout{};
        visual_layout.button_size = sao::ui::menu_visual::kVisualButtonBaseSize;
        visual_layout.button_max_size = sao::ui::menu_visual::kVisualButtonMaxSize;
        const int32_t size =
            sao::ui::menu_visual::visual_button_diameter(visual_layout, fisheye * stagger);
        const int32_t settled_y = kMenuPad + physical_index * kMenuSlot + kMenuSlot / 2;
        const int32_t center_y =
            kMenuPad + kMenuSlot / 2 +
            static_cast<int32_t>(std::lround((settled_y - (kMenuPad + kMenuSlot / 2)) * stagger)) +
            (pressed ? 2 : 0);
        if (trail > 0.01F)
            stroke_circle(raster, kMenuColumnCenter, center_y,
                          size / 2 + static_cast<int32_t>(std::lround(8.0F * trail)), 2,
                          alpha_color(gold, scaled_alpha(220.0, trail)));
        if (pulse > 0.01F)
            stroke_circle(raster, kMenuColumnCenter, center_y,
                          size / 2 + static_cast<int32_t>(std::lround(12.0F * (1.0F - pulse))), 2,
                          alpha_color(gold, scaled_alpha(230.0, pulse)));
        Color fill = disabled  ? disabled_bg
                     : active  ? circle_active_bg
                     : hovered ? lerp_rgb(circle_bg, circle_hover_bg, hover, 255)
                               : circle_bg;
        const double fill_opacity = high_contrast ? 1.0 : 0.86 + 0.14 * stagger;
        fill_circle(
            raster, kMenuColumnCenter, center_y, size / 2,
            alpha_color(fill, scaled_alpha(std::max(180, static_cast<int>(fill.a)), fill_opacity)));
        const int32_t well_radius = std::max(2, size / 2 - 7);
        const Color well = active ? circle_active_bg : app_bg;
        fill_circle(raster, kMenuColumnCenter, center_y, well_radius,
                    alpha_color(well, scaled_alpha(high_contrast ? 220.0 : 150.0, fill_opacity)));
        Color edge = disabled ? disabled_border : active ? gold : hovered ? accent : circle_border;
        stroke_circle(raster, kMenuColumnCenter, center_y, size / 2, active || hovered ? 2 : 1,
                      alpha_color(edge, high_contrast ? 255 : (active ? 245 : 220)));
        if (hovered)
            stroke_circle(raster, kMenuColumnCenter, center_y, size / 2 + 2, 1,
                          alpha_color(accent, high_contrast ? 255 : scaled_alpha(150.0, hover)));
        if (active)
            stroke_circle(raster, kMenuColumnCenter, center_y, size / 2 + 3, 2,
                          alpha_color(gold, high_contrast ? 255 : 205));

        const Color icon_color = disabled  ? disabled_fg
                                 : active  ? circle_active_icon
                                 : hovered ? circle_hover_icon
                                           : circle_icon;
        const auto classic_id =
            root.icon.empty() ? classic_icon_for_action(root.action_id) : std::nullopt;
        if (classic_id.has_value()) {
            const int32_t icon_cover_radius = std::max(6, size / 2 - 10);
            fill_circle(
                raster, kMenuColumnCenter, center_y, icon_cover_radius,
                alpha_color(well, scaled_alpha(high_contrast ? 240.0 : 190.0, fill_opacity)),
                BlendRounding::Nearest);
            const float icon_size = std::max(16.0F, static_cast<float>(size) * 0.56F);
            draw_classic_icon(
                raster, *classic_id, static_cast<float>(kMenuColumnCenter) - icon_size * 0.5F,
                static_cast<float>(center_y) - icon_size * 0.5F, icon_size, icon_color);
        }
#if defined(_WIN32)
        if (!classic_id.has_value() && !root.icon.empty()) {
            text_commands.push_back({kMenuColumnCenter - 10,
                                     center_y - 12,
                                     20,
                                     24,
                                     root.icon,
                                     sao::ui::entity_text::FontRole::Icon,
                                     15.0F,
                                     {icon_color.r, icon_color.g, icon_color.b,
                                      high_contrast ? uint8_t{255} : icon_color.a},
                                     true});
        }
        if (!root.name.empty()) {
            const Color label_color = disabled ? disabled_fg : active ? circle_active_icon : text;
            text_commands.push_back({118,
                                     center_y - 8,
                                     kMenuWidth - 140,
                                     18,
                                     root.name,
                                     sao::ui::entity_text::FontRole::Label,
                                     11.0F,
                                     {label_color.r, label_color.g, label_color.b,
                                      high_contrast ? uint8_t{255} : label_color.a},
                                     true});
        }
#else
        if (!classic_id.has_value() && !root.icon.empty())
            draw_text_clipped(raster, kMenuColumnCenter - 5, center_y - 7, 12, root.icon, 2,
                              icon_color);
        draw_text_clipped(raster, 118, center_y - 7, kMenuWidth - 140, root.name, 1,
                          disabled ? disabled_fg
                          : active ? circle_active_icon
                                   : text,
                          BlendRounding::Nearest, true);
#endif
        const Color separator = hovered || active ? accent : border;
        draw_line(raster, 113, center_y + 15, kMenuWidth - 19, center_y + 15, 1,
                  alpha_color(separator, high_contrast ? 220 : 145));
        if (slot + 1 < visible) {
            draw_line(raster, 118, center_y + 28, kMenuWidth - 26, center_y + 28, 1,
                      alpha_color(text_secondary, high_contrast ? 150 : 58));
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
                              BlendRounding::Nearest, command.ellipsis);
        }
    }
#endif
    return raster;
}

void draw_authority_menu_accents(Raster& raster, const sao::ui::menu_visual::Snapshot& snapshot,
                                 int32_t pressed_index) {
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    const bool decoration_enabled = menu_decoration_enabled(snapshot, high_contrast);
    const Color accent = panel_color(SAO_UI_TOKEN_CORNER_CYAN);
    const Color gold = panel_color(high_contrast ? SAO_UI_TOKEN_FOCUS_RING : SAO_UI_TOKEN_APP_GOLD);
    const int32_t pulse_index =
        snapshot.pressed_pulse_idx >= 0 ? snapshot.pressed_pulse_idx : pressed_index;
    const size_t visible = std::min(snapshot.roots.size(), static_cast<size_t>(kRootItemCount));
    for (size_t index = 0; index < visible; ++index) {
        const auto& row = snapshot.roots[index];
        const int32_t center_y = kMenuPad + static_cast<int32_t>(index) * kMenuSlot + kMenuSlot / 2;
        const bool active = row.state == SAO_UI_MENU_BTN_ACTIVE;
        if (row.hover_t > 0.02F)
            stroke_circle(raster, kMenuColumnCenter, center_y, 31, 1,
                          alpha_color(accent, high_contrast ? 255 : 130));
        if (active)
            stroke_circle(raster, kMenuColumnCenter, center_y, 32, 2,
                          alpha_color(gold, high_contrast ? 255 : 220));
        if (decoration_enabled && row.selection_trail_t > 0.01F)
            stroke_circle(raster, kMenuColumnCenter, center_y,
                          34 + static_cast<int32_t>(std::lround(6.0F * row.selection_trail_t)), 2,
                          alpha_color(gold, scaled_alpha(205.0, row.selection_trail_t)));
        if (decoration_enabled && static_cast<int32_t>(index) == pulse_index &&
            row.pressed_pulse_t > 0.01F)
            stroke_circle(
                raster, kMenuColumnCenter, center_y,
                35 + static_cast<int32_t>(std::lround(10.0F * (1.0F - row.pressed_pulse_t))), 2,
                alpha_color(gold, scaled_alpha(220.0, row.pressed_pulse_t)));
    }
}

void draw_finite_menu_effects(Raster& raster, const sao::ui::menu_visual::Snapshot& snapshot) {
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    if (!menu_decoration_enabled(snapshot, high_contrast))
        return;
    const Color cyan = panel_color(SAO_UI_TOKEN_CORNER_CYAN);
    const Color gold = panel_color(SAO_UI_TOKEN_CORNER_GOLD);
    const float spark_t = std::max(snapshot.open_spark_t, snapshot.selection_spark_t);
    const uint32_t spark_count =
        std::min<uint32_t>(24U, snapshot.open_spark_count + snapshot.selection_spark_count);
    for (uint32_t spark = 0; spark < spark_count; ++spark) {
        const float angle = static_cast<float>(spark) * 0.78539816339F;
        const float distance = 42.0F + 24.0F * (1.0F - spark_t);
        const int32_t x0 =
            kMenuColumnCenter + static_cast<int32_t>(std::lround(28.0F * std::cos(angle)));
        const int32_t y0 = kMenuPad + static_cast<int32_t>(std::lround(28.0F * std::sin(angle)));
        const int32_t x1 =
            kMenuColumnCenter + static_cast<int32_t>(std::lround(distance * std::cos(angle)));
        const int32_t y1 = kMenuPad + static_cast<int32_t>(std::lround(distance * std::sin(angle)));
        draw_line(
            raster, x0, y0, x1, y1, 1,
            alpha_color((spark & 1U) == 0U ? cyan : gold, static_cast<uint8_t>(180.0F * spark_t)));
    }

    const float sweep = std::clamp(snapshot.ambient_phase_t, 0.0F, 1.0F);
    if (sweep <= 0.0F || sweep >= 1.0F)
        return;
    const float envelope = std::sin(3.14159265359F * sweep);
    const int32_t scanline_y = 38 + static_cast<int32_t>(std::lround(sweep * (kMenuHeight - 76)));
    draw_line(raster, 38, scanline_y, kMenuWidth - 38, scanline_y, 1,
              alpha_color(cyan, scaled_alpha(118.0, envelope)));
    if (scanline_y > 45)
        draw_line(raster, 38, scanline_y - 7, kMenuWidth - 38, scanline_y - 7, 1,
                  alpha_color(cyan, scaled_alpha(32.0, envelope)));
    if (scanline_y < kMenuHeight - 45)
        draw_line(raster, 38, scanline_y + 7, kMenuWidth - 38, scanline_y + 7, 1,
                  alpha_color(cyan, scaled_alpha(24.0, envelope)));
    const int32_t cyan_marker_y =
        34 + static_cast<int32_t>(std::lround(sweep * (kMenuHeight - 68)));
    const float gold_sweep = std::fmod(sweep + 0.5F, 1.0F);
    const int32_t gold_marker_y =
        34 + static_cast<int32_t>(std::lround(gold_sweep * (kMenuHeight - 68)));
    draw_line(raster, 31, cyan_marker_y, 38, cyan_marker_y, 1,
              alpha_color(cyan, scaled_alpha(110.0, envelope)));
    draw_line(raster, kMenuWidth - 38, gold_marker_y, kMenuWidth - 31, gold_marker_y, 1,
              alpha_color(gold, scaled_alpha(96.0, envelope)));
}

Raster rasterize_menu(int32_t pressed_index, int32_t pressed_child_index,
                      const sao::ui::menu_visual::Snapshot& snapshot,
                      size_t first_visible_child_index, const std::vector<OwnedRootItem>& roots,
                      size_t first_visible_root_index) {
    const bool authority = is_default_root_view(roots, first_visible_root_index);
#if defined(_WIN32)
    Raster raster = authority ? blend_authority_menu(snapshot, pressed_index)
                              : rasterize_generic_menu(pressed_index, snapshot, roots,
                                                       first_visible_root_index);
#else
    Raster raster =
        rasterize_generic_menu(pressed_index, snapshot, roots, first_visible_root_index);
#endif
#if defined(_WIN32)
    if (authority)
        draw_classic_default_root_icons(raster, snapshot, roots);
#endif
    if (authority)
        draw_authority_menu_accents(raster, snapshot, pressed_index);
    draw_finite_menu_effects(raster, snapshot);
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    const Color cyan = panel_color(SAO_UI_TOKEN_CORNER_CYAN);
    const Color gold =
        panel_color(high_contrast ? SAO_UI_TOKEN_FOCUS_RING : SAO_UI_TOKEN_CORNER_GOLD);
    draw_line(raster, 34, 34, 54, 34, 2, alpha_color(cyan, high_contrast ? 255 : 210));
    draw_line(raster, 34, 34, 34, 54, 2, alpha_color(cyan, high_contrast ? 255 : 210));
    draw_line(raster, kMenuWidth - 34, kMenuHeight - 34, kMenuWidth - 54, kMenuHeight - 34, 2,
              alpha_color(gold, high_contrast ? 255 : 210));
    draw_line(raster, kMenuWidth - 34, kMenuHeight - 34, kMenuWidth - 34, kMenuHeight - 54, 2,
              alpha_color(gold, high_contrast ? 255 : 210));
    draw_child_overlay(raster, snapshot, first_visible_child_index, pressed_child_index, authority);
    return raster;
}

// CPU raster helpers above remain only for legacy resource inspection and
// compatibility diagnostics. Entity's live layers use the value-only paint
// recording path below.

// The normal Entity surface records primitive values only.  The compositor
// replays this immutable list directly into its D3D/D2D target; it does not
// retain the shell, menu, widgets, callbacks, or a CPU pixel buffer.
uint32_t paint_argb(Color color) noexcept {
    return (static_cast<uint32_t>(color.a) << 24U) | (static_cast<uint32_t>(color.r) << 16U) |
           (static_cast<uint32_t>(color.g) << 8U) | color.b;
}

void record_status(sao_status_t* current, sao_status_t candidate) noexcept {
    if (*current == SAO_STATUS_OK && candidate != SAO_STATUS_OK)
        *current = candidate;
}

void record_line(sao_ui_paint_ctx_handle_t context, sao_status_t* status, float x1, float y1,
                 float x2, float y2, float width, Color color) noexcept {
    if (*status == SAO_STATUS_OK)
        record_status(status, sao_ui_paint_ctx_stroke_line(context, x1, y1, x2, y2, width,
                                                           paint_argb(color)));
}

void record_ellipse(sao_ui_paint_ctx_handle_t context, sao_status_t* status, float center_x,
                    float center_y, float radius, Color color) noexcept {
    if (*status == SAO_STATUS_OK)
        record_status(status, sao_ui_paint_ctx_fill_ellipse(context, center_x - radius,
                                                            center_y - radius, radius * 2.0F,
                                                            radius * 2.0F, paint_argb(color)));
}

void record_ellipse_ring(sao_ui_paint_ctx_handle_t context, sao_status_t* status, float center_x,
                         float center_y, float radius, float thickness, Color edge,
                         Color interior) noexcept {
    record_ellipse(context, status, center_x, center_y, radius, edge);
    if (*status == SAO_STATUS_OK && radius > thickness)
        record_ellipse(context, status, center_x, center_y, radius - thickness, interior);
}

void record_text_clipped(sao_ui_paint_ctx_handle_t context, sao_status_t* status, float x, float y,
                         float width, float height, std::string_view text, float size,
                         Color color) noexcept {
    if (*status != SAO_STATUS_OK || text.empty() || width <= 0.0F || height <= 0.0F)
        return;
    const sao::ui::detail::ScopedTextRole body_role(sao::ui::detail::ClassicTextRole::Body);
    const sao_status_t push_status = sao_ui_paint_ctx_push_clip(context, x, y, width, height);
    record_status(status, push_status);
    if (push_status == SAO_STATUS_OK) {
        record_status(status, sao_ui_paint_ctx_draw_utf8(context, x, y, text.data(), size,
                                                         paint_argb(color)));
        record_status(status, sao_ui_paint_ctx_pop_clip(context));
    }
}

void record_classic_icon(sao_ui_paint_ctx_handle_t context, sao_status_t* status,
                         std::optional<sao::ui::classic::IconId> icon, float x, float y, float size,
                         Color color) noexcept {
    if (*status == SAO_STATUS_OK && icon.has_value())
        record_status(status, sao::ui::classic::paint_classic_icon(context, *icon, x, y, size,
                                                                   paint_argb(color)));
}

std::optional<sao::ui::classic::IconId> record_root_icon(const OwnedRootItem& root) noexcept {
    const auto semantic_icon =
        [](std::string_view token) -> std::optional<sao::ui::classic::IconId> {
        using sao::ui::classic::IconId;
        constexpr std::string_view prefix = "sao:";
        if (token.starts_with(prefix))
            token.remove_prefix(prefix.size());
        if (token == "settings")
            return IconId::Settings;
        if (token == "tools")
            return IconId::Tools;
        if (token == "user")
            return IconId::User;
        if (token == "home")
            return IconId::Home;
        if (token == "chat")
            return IconId::Chat;
        if (token == "keyboard")
            return IconId::Keyboard;
        if (token == "plugins")
            return IconId::Plugins;
        if (token == "workshop")
            return IconId::Workshop;
        if (token == "process")
            return IconId::Process;
        if (token == "license")
            return IconId::Lock;
        if (token == "info")
            return IconId::Info;
        return std::nullopt;
    };
    if (const auto token_icon = semantic_icon(root.icon); token_icon.has_value())
        return token_icon;
    if (!root.icon.empty())
        return std::nullopt;
    if (const auto action_icon = classic_icon_for_action(root.action_id); action_icon.has_value())
        return action_icon;
    using sao::ui::classic::IconId;
    if (root.id == "Control" || root.name == "Control")
        return IconId::Settings;
    if (root.id == "Tools" || root.name == "Tools")
        return IconId::Tools;
    if (root.id == "Plugins" || root.name == "Plugins")
        return IconId::Plugins;
    if (root.id == "Skins" || root.name == "Skins")
        return IconId::Home;
    if (root.id == "About" || root.name == "About")
        return IconId::Info;
    return std::nullopt;
}

std::optional<sao::ui::classic::IconId> record_semantic_icon(std::string_view token,
                                                             int32_t action_id) noexcept {
    using sao::ui::classic::IconId;
    constexpr std::string_view prefix = "sao:";
    if (token.starts_with(prefix))
        token.remove_prefix(prefix.size());
    if (token == "settings")
        return IconId::Settings;
    if (token == "tools")
        return IconId::Tools;
    if (token == "user")
        return IconId::User;
    if (token == "home")
        return IconId::Home;
    if (token == "chat")
        return IconId::Chat;
    if (token == "keyboard")
        return IconId::Keyboard;
    if (token == "plugins")
        return IconId::Plugins;
    if (token == "workshop")
        return IconId::Workshop;
    if (token == "process")
        return IconId::Process;
    if (token == "license")
        return IconId::Lock;
    if (token == "info")
        return IconId::Info;
    return token.empty() ? classic_icon_for_action(action_id) : std::nullopt;
}

sao_status_t record_nervegear_paint(sao_ui_paint_ctx_handle_t context, SaoUiNerveGearState state,
                                    bool enabled) noexcept {
    sao_status_t status = SAO_STATUS_OK;
    const bool hover = state == SAO_UI_NG_STATE_HOVER;
    const bool pressed = state == SAO_UI_NG_STATE_PRESSED;
    const bool linking = state == SAO_UI_NG_STATE_LINKING;
    const bool linked = state == SAO_UI_NG_STATE_LINKED;
    const Color orange = alpha_color(panel_color(SAO_UI_TOKEN_APP_ACCENT), enabled ? 245 : 120);
    const Color orange_soft = alpha_color(orange, enabled ? (hover ? 210 : 145) : 82);
    const Color body = alpha_color(panel_color(pressed ? SAO_UI_TOKEN_CIRCLE_ACTIVE_BG
                                               : hover ? SAO_UI_TOKEN_CIRCLE_HOVER_BG
                                                       : SAO_UI_TOKEN_CIRCLE_BG),
                                   enabled ? 246 : 165);
    const Color glyph = panel_color(pressed ? SAO_UI_TOKEN_CIRCLE_ACTIVE_ICON
                                    : hover ? SAO_UI_TOKEN_CIRCLE_HOVER_ICON
                                            : SAO_UI_TOKEN_CIRCLE_ICON);
    record_ellipse(context, &status, 36.0F, 36.0F, 31.0F, body);
    record_ellipse_ring(context, &status, 36.0F, 36.0F, 31.0F, pressed ? 3.5F : 2.0F, orange, body);
    record_ellipse_ring(context, &status, 36.0F, 36.0F, 22.0F, 1.2F, orange_soft, body);
    record_line(context, &status, 19.0F, 53.0F, 53.0F, 53.0F, pressed ? 3.0F : 2.0F, orange);
    record_line(context, &status, 36.0F, 21.0F, 47.0F, 32.0F, 1.7F, glyph);
    record_line(context, &status, 47.0F, 32.0F, 36.0F, 43.0F, 1.7F, glyph);
    record_line(context, &status, 36.0F, 43.0F, 25.0F, 32.0F, 1.7F, glyph);
    record_line(context, &status, 25.0F, 32.0F, 36.0F, 21.0F, 1.7F, glyph);
    if (hover || linking || linked)
        record_ellipse_ring(context, &status, 36.0F, 36.0F, linking ? 34.0F : 33.0F, 1.2F,
                            fade_color(orange, linking ? 0.78 : 0.50), body);
    if (linking || linked) {
        record_line(context, &status, 8.0F, 36.0F, 20.0F, 36.0F, 1.0F, fade_color(orange, 0.60));
        record_line(context, &status, 52.0F, 36.0F, 64.0F, 36.0F, 1.0F, fade_color(orange, 0.60));
        record_text_clipped(context, &status, 23.0F, 31.0F, 26.0F, 12.0F, linked ? "ON" : "LINK",
                            linked ? 7.0F : 5.5F, orange);
    }
    return status;
}

void record_menu_child_rows(sao_ui_paint_ctx_handle_t context, sao_status_t* status,
                            const sao::ui::menu_visual::Snapshot& snapshot,
                            size_t first_visible_child_index,
                            int32_t pressed_child_index) noexcept {
    const Color child_background = alpha_color(panel_color(SAO_UI_TOKEN_APP_CARD), 228);
    const Color child_text = panel_color(SAO_UI_TOKEN_APP_TEXT);
    const Color child_line = alpha_color(panel_color(SAO_UI_TOKEN_APP_BORDER), 190);
    const Color child_icon = panel_color(SAO_UI_TOKEN_APP_TEXT_2);
    const Color orange = panel_color(SAO_UI_TOKEN_APP_ACCENT);
    const Color disabled_bg = panel_color(SAO_UI_TOKEN_DISABLED_BG);
    const Color disabled_border = panel_color(SAO_UI_TOKEN_DISABLED_BORDER);
    const Color disabled_fg = panel_color(SAO_UI_TOKEN_DISABLED_FG);
    const float opacity = 1.0F - std::clamp(snapshot.fade_t, 0.0F, 1.0F);
    const size_t available = first_visible_child_index < snapshot.rows.size()
                                 ? snapshot.rows.size() - first_visible_child_index
                                 : 0U;
    const int32_t count =
        static_cast<int32_t>(std::min(available, static_cast<size_t>(kChildPhysicalCapacity)));
    if (count <= 0 || opacity <= 0.01F)
        return;
    const int32_t line_top = kChildOriginY + 5;
    const int32_t line_height = count * kChildRowStride - 3;
    record_line(context, status, kChildLineCenterX, line_top + 5, kChildLineCenterX,
                static_cast<float>(line_top + line_height - 5), 1.0F,
                fade_color(child_line, opacity));
    record_line(context, status, kChildLineCenterX, line_top + 5, kChildLineCenterX,
                static_cast<float>(line_top + line_height - 5), 1.0F,
                fade_color(orange, opacity * 0.52F));
    record_ellipse(context, status, static_cast<float>(kChildLineCenterX),
                   static_cast<float>(line_top + 5), 2.0F, fade_color(orange, opacity));
    for (int32_t slot = 0; slot < count; ++slot) {
        const size_t index = first_visible_child_index + static_cast<size_t>(slot);
        const auto& row = snapshot.rows[index];
        const int32_t row_width = std::clamp(row.visible_width_px, 0, kChildTargetWidth);
        if (row_width <= 1)
            continue;
        const bool disabled = child_row_disabled(row);
        const bool pressed = !disabled && static_cast<int32_t>(index) == pressed_child_index;
        const float hover = std::clamp(row.hover_t, 0.0F, 1.0F);
        const int32_t row_y = kChildOriginY + slot * kChildRowStride + (pressed ? 1 : 0);
        const int32_t visual_y = row_y + 5;
        const int32_t visual_height = kChildRowHeight - 10;
        const float radius = static_cast<float>(adaptive_corner_radius(row_width, visual_height));
        const Color background = disabled                   ? fade_color(disabled_bg, opacity)
                                 : pressed || hover > 0.05F ? fade_color(orange, opacity)
                                                            : fade_color(child_background, opacity);
        const Color foreground = disabled ? fade_color(disabled_fg, opacity)
                                 : pressed || hover > 0.05F
                                     ? Color{255, 255, 255, scaled_alpha(255.0, opacity)}
                                     : fade_color(child_text, opacity);
        const Color icon = disabled ? fade_color(disabled_fg, opacity)
                           : pressed || hover > 0.05F
                               ? Color{255, 255, 255, scaled_alpha(255.0, opacity)}
                               : fade_color(child_icon, opacity);
        if (*status == SAO_STATUS_OK)
            record_status(status,
                          sao_ui_paint_ctx_fill_rounded_rect(
                              context, static_cast<float>(entity_child_row_x()),
                              static_cast<float>(visual_y), static_cast<float>(row_width),
                              static_cast<float>(visual_height), radius, paint_argb(background)));
        if (*status == SAO_STATUS_OK)
            record_status(
                status,
                sao::ui::detail::paint_rounded_rect_stroke(
                    context, static_cast<float>(entity_child_row_x()), static_cast<float>(visual_y),
                    static_cast<float>(row_width), static_cast<float>(visual_height), radius, 1.0F,
                    paint_argb(pressed || hover > 0.05F ? fade_color(orange, opacity)
                                                        : fade_color(child_line, opacity))));
        record_line(context, status, static_cast<float>(entity_child_row_x() + 1),
                    static_cast<float>(visual_y + 3), static_cast<float>(entity_child_row_x() + 1),
                    static_cast<float>(visual_y + visual_height - 3), 2.0F,
                    disabled                   ? fade_color(disabled_border, opacity)
                    : pressed || hover > 0.05F ? fade_color(orange, opacity)
                                               : fade_color(child_line, opacity));
        const int32_t icon_x = entity_child_row_x() + 10;
        const int32_t label_x = icon_x + kChildFallbackIconWidth + kChildIconGap;
        const int32_t caret_x = entity_child_row_x() + row_width - kChildRowPadRight - 6;
        const int32_t label_width =
            std::max(0, caret_x - kChildCaretWidth - kChildCaretGap - label_x);
        const std::string_view icon_text(row.icon_utf8.data());
        const auto classic = record_semantic_icon(icon_text, row.action_id);
        record_classic_icon(context, status, classic, static_cast<float>(icon_x - 2),
                            static_cast<float>(visual_y + 5), 18.0F, icon);
        if (!classic.has_value())
            record_text_clipped(context, status, static_cast<float>(icon_x),
                                static_cast<float>(visual_y + 7), 14.0F, 18.0F, icon_text, 11.0F,
                                icon);
        record_text_clipped(context, status, static_cast<float>(label_x),
                            static_cast<float>(visual_y + 7), static_cast<float>(label_width),
                            18.0F, std::string_view(row.name_utf8.data()), 10.0F, foreground);
        if (!disabled && hover > 0.05F && row_width >= 18) {
            record_line(context, status, static_cast<float>(caret_x),
                        static_cast<float>(row_y + 15), static_cast<float>(caret_x + 3),
                        static_cast<float>(row_y + 18), 1.0F, foreground);
            record_line(context, status, static_cast<float>(caret_x + 3),
                        static_cast<float>(row_y + 18), static_cast<float>(caret_x),
                        static_cast<float>(row_y + 21), 1.0F, foreground);
        }
        if (hover > 0.05F || pressed)
            record_line(context, status, static_cast<float>(kChildLineCenterX + 3),
                        static_cast<float>(visual_y + visual_height / 2),
                        static_cast<float>(entity_child_row_x()),
                        static_cast<float>(visual_y + visual_height / 2), 1.0F,
                        fade_color(orange, opacity * std::max(hover, pressed ? 1.0F : 0.0F)));
    }
}

void record_menu_paint(sao_ui_paint_ctx_handle_t context, sao_status_t* status,
                       int32_t pressed_index, int32_t pressed_child_index,
                       const sao::ui::menu_visual::Snapshot& snapshot,
                       size_t first_visible_child_index, const std::vector<OwnedRootItem>& roots,
                       size_t first_visible_root_index) noexcept {
    const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
    const bool decorations = menu_decoration_enabled(snapshot, high_contrast);
    const Color paper = alpha_color(panel_color(SAO_UI_TOKEN_APP_CARD), high_contrast ? 255 : 242);
    const Color paper_border = panel_color(SAO_UI_TOKEN_CIRCLE_BORDER);
    const Color text = panel_color(SAO_UI_TOKEN_APP_TEXT);
    const Color orange =
        panel_color(high_contrast ? SAO_UI_TOKEN_FOCUS_RING : SAO_UI_TOKEN_APP_ACCENT);
    const Color inactive_icon = panel_color(SAO_UI_TOKEN_CIRCLE_ICON);
    const Color disabled_bg = panel_color(SAO_UI_TOKEN_DISABLED_BG);
    const Color disabled_border = panel_color(SAO_UI_TOKEN_DISABLED_BORDER);
    const Color disabled_fg = panel_color(SAO_UI_TOKEN_DISABLED_FG);
    const size_t available =
        first_visible_root_index < roots.size() ? roots.size() - first_visible_root_index : 0U;
    const size_t visible = std::min(available, static_cast<size_t>(kRootItemCount));
    const bool child_menu_visible =
        snapshot.displayed_parent_idx >= 0 && !snapshot.rows.empty() && snapshot.fade_t < 0.98F;
    for (size_t slot = 0; slot < visible; ++slot) {
        const auto& root = roots[first_visible_root_index + slot];
        const auto row = slot < snapshot.roots.size() ? snapshot.roots[slot]
                                                      : sao::ui::menu_visual::RootRowSnapshot{};
        const bool disabled = root_row_disabled(row);
        const bool active = row.state == SAO_UI_MENU_BTN_ACTIVE;
        const float hover = std::clamp(row.hover_t, 0.0F, 1.0F);
        const float stagger = std::clamp(row.stagger_t, 0.0F, 1.0F);
        SaoUiMenuLayout layout{};
        layout.button_size = sao::ui::menu_visual::kVisualButtonBaseSize;
        layout.button_max_size = sao::ui::menu_visual::kVisualButtonMaxSize;
        const int32_t diameter = sao::ui::menu_visual::visual_button_diameter(
            layout, std::clamp(row.fisheye_t, 0.0F, 1.0F) * stagger);
        const int32_t settled_y = kMenuPad + static_cast<int32_t>(slot) * kMenuSlot + kMenuSlot / 2;
        const int32_t center_y =
            kMenuPad + kMenuSlot / 2 +
            static_cast<int32_t>(std::lround((settled_y - (kMenuPad + kMenuSlot / 2)) * stagger)) +
            ((!disabled && static_cast<int32_t>(slot) == pressed_index) ? 2 : 0);
        const bool pressed = !disabled && static_cast<int32_t>(slot) == pressed_index;
        const bool selected = active || pressed || hover > 0.01F;
        const Color fill = disabled ? disabled_bg : selected ? orange : paper;
        const Color edge = disabled ? disabled_border : selected ? orange : paper_border;
        const Color icon = disabled   ? disabled_fg
                           : selected ? Color{255, 255, 255, 255}
                                      : inactive_icon;
        record_ellipse(context, status, static_cast<float>(kMenuColumnCenter),
                       static_cast<float>(center_y), static_cast<float>(diameter) * 0.5F, fill);
        record_ellipse_ring(context, status, static_cast<float>(kMenuColumnCenter),
                            static_cast<float>(center_y), static_cast<float>(diameter) * 0.5F,
                            selected ? 2.0F : 1.0F, edge, fill);
        if (row.selection_trail_t > 0.01F)
            record_ellipse_ring(context, status, static_cast<float>(kMenuColumnCenter),
                                static_cast<float>(center_y),
                                static_cast<float>(diameter) * 0.5F + 5.0F * row.selection_trail_t,
                                1.0F, fade_color(orange, row.selection_trail_t), fill);
        const auto classic = record_root_icon(root);
        record_classic_icon(context, status, classic, static_cast<float>(kMenuColumnCenter - 12),
                            static_cast<float>(center_y - 12), 24.0F, icon);
        if (!classic.has_value())
            record_text_clipped(context, status, static_cast<float>(kMenuColumnCenter - 11),
                                static_cast<float>(center_y - 9), 22.0F, 18.0F, root.icon, 14.0F,
                                icon);
        if (!child_menu_visible) {
            const float label_x = 118.0F;
            const float label_y = static_cast<float>(center_y - 14);
            const float label_width = 152.0F;
            if (*status == SAO_STATUS_OK)
                record_status(status, sao_ui_paint_ctx_fill_rounded_rect(
                                          context, label_x, label_y, label_width, 28.0F, 8.0F,
                                          paint_argb(selected ? fade_color(orange, 0.17)
                                                              : fade_color(paper, 0.78))));
            if (*status == SAO_STATUS_OK)
                record_status(status, sao::ui::detail::paint_rounded_rect_stroke(
                                          context, label_x, label_y, label_width, 28.0F, 8.0F, 1.0F,
                                          paint_argb(selected ? fade_color(orange, 0.75)
                                                              : fade_color(paper_border, 0.82))));
            record_text_clipped(context, status, label_x + 12.0F, label_y + 8.0F,
                                label_width - 24.0F, 17.0F, root.name, 14.0F,
                                disabled ? disabled_fg : text);
        }
    }
    if (decorations && child_menu_visible) {
        const float sweep = std::clamp(snapshot.ambient_phase_t, 0.0F, 1.0F);
        const float envelope = std::sin(3.14159265359F * sweep);
        const float scan_y = 38.0F + sweep * static_cast<float>(kMenuHeight - 76);
        record_line(context, status, static_cast<float>(entity_child_row_x()), scan_y,
                    static_cast<float>(entity_child_row_x() + kChildTargetWidth), scan_y, 1.0F,
                    fade_color(orange, 0.20F * envelope));
    }
    record_menu_child_rows(context, status, snapshot, first_visible_child_index,
                           pressed_child_index);
}

sao_status_t
record_entity_layer(uint32_t width, uint32_t height,
                    const std::function<void(sao_ui_paint_ctx_handle_t, sao_status_t*)>& paint,
                    std::shared_ptr<const sao::ui::detail::PaintDisplayList>* out) noexcept {
    if (out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = {};
    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_status_t status = sao::ui::detail::create_recording_paint_context(width, height, &context);
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_begin_frame(context);
    if (status == SAO_STATUS_OK)
        paint(context, &status);
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_end_frame(context);
    if (status == SAO_STATUS_OK)
        status = sao::ui::detail::seal_recording_paint_context(context, out);
    if (context != nullptr)
        sao_ui_paint_ctx_destroy(context);
    return status;
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
    bool menu_raster_dirty{true};
    bool nervegear_raster_dirty{true};
    bool menu_raster_valid{};
    bool nervegear_raster_valid{};
    uint64_t menu_raster_signature{};
    uint64_t nervegear_raster_signature{};
    uint64_t raster_theme_generation{};
    bool menu_effects_applied{};
    SaoUiLayerEffects applied_menu_effects{};
    bool input_region_settle_pending{};
    bool destroy_pending{};
    bool destroy_finalizer_scheduled{};
    bool teardown_started{};
    std::atomic_bool suppress_layer_input_callbacks{};
    uint32_t callback_depth{};
    uint64_t frame_count{};
    uint64_t visual_time_ms{};
    uint64_t action_count{};
    bool reduced_motion{};
    bool fps_pressure{};
    uint64_t root_tree_revision{1};
    sao_status_t last_status{SAO_STATUS_OK};
    std::thread::id owner_thread;
    mutable std::mutex mutex;
};

void invalidate_menu_raster_locked(sao_ui_entity_shell_s* shell) {
    shell->menu_raster_dirty = true;
}

void invalidate_nervegear_raster_locked(sao_ui_entity_shell_s* shell) {
    shell->nervegear_raster_dirty = true;
}

void invalidate_rasters_locked(sao_ui_entity_shell_s* shell) {
    shell->menu_raster_dirty = true;
    shell->nervegear_raster_dirty = true;
    shell->menu_raster_valid = false;
    shell->nervegear_raster_valid = false;
}

void signature_mix(uint64_t* value, uint64_t next) noexcept {
    *value ^= next + 0x9E3779B97F4A7C15ULL + (*value << 6U) + (*value >> 2U);
}

void signature_mix_float(uint64_t* value, float next) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &next, sizeof(bits));
    signature_mix(value, bits);
}

void signature_mix_string(uint64_t* value, std::string_view next) noexcept {
    signature_mix(value, next.size());
    for (const unsigned char character : next)
        signature_mix(value, character);
}

uint64_t compute_menu_raster_signature(const sao_ui_entity_shell_s* shell,
                                       const sao::ui::menu_visual::Snapshot& snapshot,
                                       uint64_t theme_generation) noexcept {
    uint64_t signature = 0xCBF29CE484222325ULL;
    signature_mix(&signature, theme_generation);
    signature_mix(&signature, shell->roots.size());
    for (const auto& root : shell->roots) {
        signature_mix_string(&signature, root.id);
        signature_mix_string(&signature, root.name);
        signature_mix_string(&signature, root.icon);
        signature_mix(&signature, static_cast<uint64_t>(static_cast<int64_t>(root.action_id) + 1));
        signature_mix(&signature, root.can_activate ? 1U : 0U);
        signature_mix(&signature, root.children.size());
        for (const auto& child : root.children) {
            signature_mix_string(&signature, child.name);
            signature_mix_string(&signature, child.icon);
            signature_mix(&signature,
                          static_cast<uint64_t>(static_cast<int64_t>(child.action_id) + 1));
            signature_mix(&signature, child.can_activate ? 1U : 0U);
        }
    }
    signature_mix(&signature, shell->first_visible_root_index);
    signature_mix(&signature, shell->first_visible_child_index);
    signature_mix(&signature, static_cast<uint64_t>(shell->menu_pressed_index + 1));
    signature_mix(&signature, static_cast<uint64_t>(shell->menu_pressed_child_index + 1));
    signature_mix(&signature, shell->menu_visible ? 1U : 0U);
    signature_mix(&signature, snapshot.revision);
    signature_mix(&signature, static_cast<uint64_t>(snapshot.phase));
    signature_mix(&signature, static_cast<uint64_t>(snapshot.active_root_idx + 1));
    signature_mix(&signature, static_cast<uint64_t>(snapshot.displayed_parent_idx + 1));
    signature_mix(&signature, static_cast<uint64_t>(snapshot.child_hover_idx + 1));
    signature_mix_float(&signature, snapshot.fade_t);
    signature_mix_float(&signature, snapshot.transition_eased_t);
    signature_mix_float(&signature, snapshot.center_diffusion_t);
    signature_mix_float(&signature, snapshot.close_suction_t);
    signature_mix(&signature, static_cast<uint64_t>(snapshot.selection_trail_idx + 1));
    signature_mix_float(&signature, snapshot.selection_trail_t);
    signature_mix(&signature, static_cast<uint64_t>(snapshot.pressed_pulse_idx + 1));
    signature_mix_float(&signature, snapshot.pressed_pulse_t);
    signature_mix_float(&signature, snapshot.child_rail_glow_t);
    signature_mix_float(&signature, snapshot.backdrop_lens_t);
    signature_mix_float(&signature, snapshot.ambient_phase_t);
    signature_mix_float(&signature, snapshot.open_spark_t);
    signature_mix_float(&signature, snapshot.selection_spark_t);
    signature_mix(&signature, snapshot.open_spark_count);
    signature_mix(&signature, snapshot.selection_spark_count);
    signature_mix(&signature, snapshot.reduced_motion ? 1U : 0U);
    signature_mix(&signature, snapshot.fps_pressure ? 1U : 0U);
    signature_mix(&signature, snapshot.roots.size());
    for (const auto& row : snapshot.roots) {
        signature_mix(&signature, row.can_activate ? 1U : 0U);
        signature_mix(&signature, static_cast<uint64_t>(row.state));
        signature_mix_float(&signature, row.hover_t);
        signature_mix_float(&signature, row.fisheye_t);
        signature_mix_float(&signature, row.stagger_t);
        signature_mix_float(&signature, row.selection_trail_t);
        signature_mix_float(&signature, row.pressed_pulse_t);
    }
    signature_mix(&signature, snapshot.rows.size());
    for (const auto& row : snapshot.rows) {
        signature_mix(&signature, row.can_activate ? 1U : 0U);
        signature_mix(&signature, static_cast<uint64_t>(row.state));
        signature_mix(&signature, static_cast<uint64_t>(row.visible_width_px));
        signature_mix_float(&signature, row.hover_t);
        signature_mix_float(&signature, row.stagger_t);
        signature_mix_float(&signature, row.radial_t);
        signature_mix_float(&signature, row.selection_trail_t);
        signature_mix_float(&signature, row.pressed_pulse_t);
    }
    return signature;
}

uint64_t compute_nervegear_raster_signature(const sao_ui_entity_shell_s* shell,
                                            SaoUiNerveGearState state,
                                            uint64_t theme_generation) noexcept {
    uint64_t signature = 0x84222325CBF29CE4ULL;
    signature_mix(&signature, theme_generation);
    signature_mix(&signature, static_cast<uint64_t>(state));
    signature_mix(&signature, shell->nervgear_mode ? 1U : 0U);
    return signature;
}

bool layer_effects_equal(const SaoUiLayerEffects& left, const SaoUiLayerEffects& right) noexcept {
    if (left.struct_size != right.struct_size || left.flags != right.flags ||
        left.blur_sigma != right.blur_sigma || left.shadow_sigma != right.shadow_sigma ||
        left.shadow_offset_x != right.shadow_offset_x ||
        left.shadow_offset_y != right.shadow_offset_y || left.shadow_argb != right.shadow_argb ||
        left.reserved != right.reserved) {
        return false;
    }
    for (size_t index = 0; index < 20U; ++index) {
        if (left.color_matrix[index] != right.color_matrix[index])
            return false;
    }
    return true;
}

sao_status_t apply_menu_effects_locked(sao_ui_entity_shell_s* shell) {
    if (shell->menu_layer == nullptr || shell->host == nullptr)
        return SAO_STATUS_OK;
    SaoUiLayerEffects effects{};
    sao_status_t status = sao_ui_layer_effects_init(SAO_UI_LAYER_EFFECT_PRESET_MENU, &effects);
    if (status != SAO_STATUS_OK)
        return status;
    // Classic menus keep the underlying colors neutral; focus carries orange.
    effects.flags &= ~SAO_UI_LAYER_EFFECT_COLOR_MATRIX;
    if (shell->reduced_motion || shell->fps_pressure ||
        sao::ui::detail::panel_theme_high_contrast()) {
        effects.flags &= ~SAO_UI_LAYER_EFFECT_BACKDROP_BLUR;
        effects.blur_sigma = 0.0F;
    } else {
        effects.blur_sigma *= 0.82F;
    }
    if (shell->menu_effects_applied && layer_effects_equal(shell->applied_menu_effects, effects))
        return SAO_STATUS_OK;
    shell->menu_effects_applied = false;
    status = sao_ui_layer_set_effects(shell->menu_layer, &effects);
    if (status == SAO_STATUS_OK) {
        shell->applied_menu_effects = effects;
        shell->menu_effects_applied = true;
    }
    return status;
}

namespace {

// Read the overlay host's current DPI without holding shell->mutex. Returns
// 96 when the shell has no host or the host has never received WM_DPICHANGED.
uint32_t host_dpi_snapshot(const sao_ui_entity_shell_s* shell) noexcept {
    if (shell == nullptr || shell->host == nullptr)
        return 96u;
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
    const int64_t screen_x =
        static_cast<int64_t>(origin_x) + layer_x + static_cast<int32_t>(local_x);
    const int64_t screen_y =
        static_cast<int64_t>(origin_y) + layer_y + static_cast<int32_t>(local_y);
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
                    status =
                        sao::ui::menu_visual::activate_root(shell->menu, physical_index, false);
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

sao_status_t sync_child_viewport_locked(sao_ui_entity_shell_s* shell,
                                        const sao::ui::menu_visual::Snapshot& snapshot) {
    const bool child_visible = is_child_phase(snapshot.phase) &&
                               snapshot.displayed_parent_idx >= 0 && !snapshot.rows.empty();
    const int32_t next_parent = child_visible ? snapshot.displayed_parent_idx : -1;
    const bool parent_changed = shell->displayed_child_parent_index != next_parent;
    const size_t next_first = parent_changed
                                  ? 0U
                                  : std::min(shell->first_visible_child_index,
                                             max_first_visible_child_index(snapshot.rows.size()));
    if (!parent_changed && next_first == shell->first_visible_child_index)
        return SAO_STATUS_OK;

    shell->displayed_child_parent_index = next_parent;
    shell->first_visible_child_index = next_first;
    shell->visual_dirty = true;
    invalidate_menu_raster_locked(shell);
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
        const int32_t half_span =
            static_cast<int32_t>(std::floor(std::sqrt(static_cast<double>(remaining))));
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
    const sao_status_t status =
        sao_ui_layer_set_input_callbacks(*layer, nullptr, nullptr, nullptr, nullptr, nullptr);
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
    sao_status_t status = detach_and_destroy_layer(&shell->menu_layer, force_construction_cleanup);
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
    sao_status_t status = visible ? sao_ui_menu_show(shell->menu, shell->menu_x + kMenuColumnCenter,
                                                     shell->menu_y + kMenuPad)
                                  : sao_ui_menu_hide(shell->menu);
    if (status != SAO_STATUS_OK)
        return status;
    shell->menu_visible = visible;
    shell->visual_dirty = shell->visual_dirty || changed;
    if (changed)
        invalidate_menu_raster_locked(shell);
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
    if (status == SAO_STATUS_OK) {
        shell->visual_dirty = true;
        invalidate_rasters_locked(shell);
    }
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

sao_status_t build_menu_input_rects_locked(sao_ui_entity_shell_s* shell,
                                           const sao::ui::menu_visual::Snapshot& snapshot,
                                           std::vector<SaoUiLayerInputRect>* out_rects) {
    if (out_rects == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<SaoUiLayerInputRect> next_rects;
    next_rects.reserve(kRootItemCount + kChildPhysicalCapacity);
    for (int32_t index = 0; index < static_cast<int32_t>(visible_root_count(shell)); ++index) {
        if (static_cast<size_t>(index) < snapshot.roots.size() &&
            root_row_disabled(snapshot.roots[static_cast<size_t>(index)])) {
            continue;
        }
        next_rects.push_back({kMenuPad, kMenuPad + index * kMenuSlot, kMenuSlot, kMenuSlot});
    }
    const size_t available_child_count =
        shell->first_visible_child_index < snapshot.rows.size()
            ? snapshot.rows.size() - shell->first_visible_child_index
            : 0U;
    const size_t visible_child_count =
        std::min(available_child_count, static_cast<size_t>(kChildPhysicalCapacity));
    const int32_t child_count = static_cast<int32_t>(visible_child_count);
    const bool child_fade_visible = snapshot.fade_t < 0.50F;
    for (int32_t slot = 0; slot < child_count; ++slot) {
        if (!child_fade_visible)
            break;
        const size_t logical_index = shell->first_visible_child_index + static_cast<size_t>(slot);
        const int32_t width =
            std::clamp(snapshot.rows[logical_index].visible_width_px, 0, kChildTargetWidth);
        if (width <= 1)
            continue;
        next_rects.push_back(
            {entity_child_row_x(), kChildOriginY + slot * kChildRowStride, width, kChildRowHeight});
    }
    *out_rects = std::move(next_rects);
    return SAO_STATUS_OK;
}

sao_status_t raster_and_upload_locked(sao_ui_entity_shell_s* shell) {
    const uint64_t theme_generation = sao::ui::detail::process_theme_generation();
    if (shell->raster_theme_generation != theme_generation) {
        shell->raster_theme_generation = theme_generation;
        invalidate_rasters_locked(shell);
    }
    sao::ui::menu_visual::Snapshot menu_snapshot{};
    sao_status_t status = sao::ui::menu_visual::get_snapshot(shell->menu, &menu_snapshot);
    menu_snapshot.reduced_motion = shell->reduced_motion;
    menu_snapshot.fps_pressure = shell->fps_pressure;
    if (status != SAO_STATUS_OK)
        return status;
    status = sync_child_viewport_locked(shell, menu_snapshot);
    if (status != SAO_STATUS_OK)
        return status;
    SaoUiNerveGearState state = SAO_UI_NG_STATE_IDLE;
    status = sao_ui_nervegear_get_state(shell->nervegear, &state);
    if (status != SAO_STATUS_OK)
        return status;

    const uint64_t next_nervegear_signature =
        compute_nervegear_raster_signature(shell, state, theme_generation);
    const uint64_t next_menu_signature =
        compute_menu_raster_signature(shell, menu_snapshot, theme_generation);
    const bool update_nervegear = shell->nervegear_raster_dirty || !shell->nervegear_raster_valid ||
                                  shell->nervegear_raster_signature != next_nervegear_signature;
    const bool update_menu = shell->menu_raster_dirty || !shell->menu_raster_valid ||
                             shell->menu_raster_signature != next_menu_signature;
    std::shared_ptr<const sao::ui::detail::PaintDisplayList> next_nervegear_paint;
    std::shared_ptr<const sao::ui::detail::PaintDisplayList> next_menu_paint;
    std::vector<SaoUiLayerInputRect> next_menu_input_rects;
    try {
        if (update_nervegear) {
            status = record_entity_layer(
                SAO_UI_NERVEGEAR_SIZE, SAO_UI_NERVEGEAR_SIZE,
                [state, enabled = shell->nervgear_mode](sao_ui_paint_ctx_handle_t context,
                                                        sao_status_t* paint_status) {
                    record_status(paint_status, record_nervegear_paint(context, state, enabled));
                },
                &next_nervegear_paint);
        }
        if (update_menu) {
            status = first_failure(
                status,
                record_entity_layer(
                    kMenuWidth, kMenuHeight,
                    [pressed = shell->menu_pressed_index,
                     pressed_child = shell->menu_pressed_child_index, snapshot = menu_snapshot,
                     first_child = shell->first_visible_child_index, roots = shell->roots,
                     first_root = shell->first_visible_root_index](
                        sao_ui_paint_ctx_handle_t context, sao_status_t* paint_status) {
                        record_menu_paint(context, paint_status, pressed, pressed_child, snapshot,
                                          first_child, roots, first_root);
                    },
                    &next_menu_paint));
        }
        status = first_failure(
            status, build_menu_input_rects_locked(shell, menu_snapshot, &next_menu_input_rects));
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (status != SAO_STATUS_OK)
        return status;
    if (update_nervegear) {
        status = sao::ui::detail::submit_layer_paint(shell->nervegear_layer,
                                                     std::move(next_nervegear_paint),
                                                     SAO_UI_NERVEGEAR_SIZE, SAO_UI_NERVEGEAR_SIZE);
        if (status != SAO_STATUS_OK) {
            shell->nervegear_raster_valid = false;
            shell->nervegear_raster_dirty = true;
            return status;
        }
        shell->nervegear_raster_signature = next_nervegear_signature;
        shell->nervegear_raster_valid = true;
        shell->nervegear_raster_dirty = false;
    }
    if (update_menu) {
        status = sao::ui::detail::submit_layer_paint(shell->menu_layer, std::move(next_menu_paint),
                                                     kMenuWidth, kMenuHeight);
        if (status != SAO_STATUS_OK) {
            shell->menu_raster_valid = false;
            shell->menu_raster_dirty = true;
            return status;
        }
        shell->menu_raster_signature = next_menu_signature;
        shell->menu_raster_valid = true;
        shell->menu_raster_dirty = false;
    }
    if (!input_rects_equal(shell->menu_input_rects, next_menu_input_rects)) {
        status = sao_ui_layer_set_input_rects(shell->menu_layer, next_menu_input_rects.data(),
                                              next_menu_input_rects.size());
        if (status != SAO_STATUS_OK)
            return status;
        shell->menu_input_rects = std::move(next_menu_input_rects);
    }
    shell->nervegear_raster_dirty = false;
    shell->menu_raster_dirty = false;
    return SAO_STATUS_OK;
}

sao_status_t commit_visual_state_locked(sao_ui_entity_shell_s* shell) {
    const uint64_t theme_generation = sao::ui::detail::process_theme_generation();
    if (shell->raster_theme_generation != theme_generation) {
        shell->raster_theme_generation = theme_generation;
        invalidate_rasters_locked(shell);
        shell->visual_dirty = true;
    }
    (void)apply_menu_effects_locked(shell);
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
    if (status == SAO_STATUS_OK) {
        shell->visual_dirty = false;
        shell->menu_raster_dirty = false;
        shell->nervegear_raster_dirty = false;
    }
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
            invalidate_menu_raster_locked(shell);
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
    shell->visual_time_ms += std::min<uint32_t>(elapsed_ms, 1000U);
    const bool next_fps_pressure = elapsed_ms > 34U;
    const bool next_reduced_motion = windows_reduced_motion();
    const bool visual_budget_changed =
        shell->fps_pressure != next_fps_pressure || shell->reduced_motion != next_reduced_motion;
    shell->fps_pressure = next_fps_pressure;
    shell->reduced_motion = next_reduced_motion;
    (void)sao_ui_menu_set_visual_budget(shell->menu, shell->reduced_motion, shell->fps_pressure);
    if (visual_budget_changed) {
        shell->visual_dirty = true;
        invalidate_menu_raster_locked(shell);
    }
    SaoUiNerveGearState previous_state = SAO_UI_NG_STATE_IDLE;
    SaoUiMenuPhase previous_menu_phase = SAO_UI_MENU_PHASE_CLOSED;
    float previous_menu_progress = 0.0F;
    sao::ui::menu_visual::Snapshot previous_menu_visual{};
    sao_status_t status = refresh_host_geometry_locked(shell);
    status = first_failure(status, sao_ui_nervegear_get_state(shell->nervegear, &previous_state));
    status = first_failure(status, sao_ui_menu_get_phase(shell->menu, &previous_menu_phase));
    status = first_failure(
        status, sao_ui_menu_get_transition_progress(shell->menu, &previous_menu_progress));
    status = first_failure(status,
                           sao::ui::menu_visual::get_snapshot(shell->menu, &previous_menu_visual));
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
    status = first_failure(status,
                           sao::ui::menu_visual::get_snapshot(shell->menu, &current_menu_visual));
    const bool nervegear_changed = previous_state != current_state;
    const bool menu_changed = previous_menu_phase != current_menu_phase ||
                              previous_menu_progress != current_menu_progress ||
                              previous_menu_visual.revision != current_menu_visual.revision;
    if (nervegear_changed)
        invalidate_nervegear_raster_locked(shell);
    if (menu_changed)
        invalidate_menu_raster_locked(shell);
    shell->visual_dirty = shell->visual_dirty || nervegear_changed || menu_changed;
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
                               const sao::ui::menu_visual::Snapshot& snapshot, int32_t screen_x,
                               int32_t screen_y, bool include_disabled, MenuHit* out_hit) {
    if (!shell->menu_visible || !is_child_phase(snapshot.phase) ||
        snapshot.displayed_parent_idx < 0 || snapshot.rows.empty() || snapshot.fade_t >= 0.50F) {
        return false;
    }
    const uint32_t host_dpi = host_dpi_snapshot(shell);
    // DPI-aware: scale the desktop screen point down before subtracting the
    // host origin, then subtract the layer origin (already in host-local
    // logical pixels).
    const int64_t local_x = screen_to_host_dpi(screen_x, shell->origin_x, host_dpi) - shell->menu_x;
    const int64_t local_y = screen_to_host_dpi(screen_y, shell->origin_y, host_dpi) - shell->menu_y;
    if (local_y < kChildOriginY)
        return false;
    const int64_t slot = (local_y - kChildOriginY) / kChildRowStride;
    const int64_t slot_y = kChildOriginY + slot * kChildRowStride;
    if (slot < 0 || slot >= kChildPhysicalCapacity || local_y >= slot_y + kChildRowHeight)
        return false;
    const size_t logical_index = shell->first_visible_child_index + static_cast<size_t>(slot);
    if (logical_index >= snapshot.rows.size())
        return false;
    if (!include_disabled && child_row_disabled(snapshot.rows[logical_index]))
        return false;
    const int32_t width =
        std::clamp(snapshot.rows[logical_index].visible_width_px, 0, kChildTargetWidth);
    if (width <= 1)
        return false;
    if (local_x < entity_child_row_x() || local_x >= entity_child_row_x() + width)
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
    const int64_t local_x = screen_to_host_dpi(screen_x, shell->origin_x, host_dpi) - shell->menu_x;
    const int64_t local_y = screen_to_host_dpi(screen_y, shell->origin_y, host_dpi) - shell->menu_y;
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
        invalidate_menu_raster_locked(shell);
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
    if (max_first == 0 ||
        !child_viewport_hit_locked(shell, snapshot, screen_x, screen_y, true, nullptr))
        return SAO_STATUS_OK;

    const int64_t notches = wheel_delta / kWheelDeltaPerNotch;
    const int64_t requested = static_cast<int64_t>(shell->first_visible_child_index) - notches;
    const size_t next_first =
        static_cast<size_t>(std::clamp<int64_t>(requested, 0, static_cast<int64_t>(max_first)));
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
        invalidate_menu_raster_locked(shell);
        *out_changed = true;
        return SAO_STATUS_OK;
    }
    shell->first_visible_child_index = next_first;
    status = clear_child_interaction_locked(shell);
    if (status == SAO_STATUS_OK) {
        shell->visual_dirty = true;
        invalidate_menu_raster_locked(shell);
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
        if (should_destroy && !shell->destroy_finalizer_scheduled && shell->compositor != nullptr) {
            shell->destroy_finalizer_scheduled = true;
            schedule_finalizer = true;
            should_destroy = false;
        }
    }
    if (schedule_finalizer) {
        const sao_status_t finalizer_status =
            sao_ui_compositor_post_input(shell->compositor, &deferred_entity_destroy, shell);
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

bool menu_surface_hit_locked(sao_ui_entity_shell_s* shell, int32_t screen_x, int32_t screen_y) {
    if (!shell->menu_visible)
        return false;
    const uint32_t host_dpi = host_dpi_snapshot(shell);
    const int64_t host_x = screen_to_host_dpi(screen_x, shell->origin_x, host_dpi);
    const int64_t host_y = screen_to_host_dpi(screen_y, shell->origin_y, host_dpi);
    const int64_t local_x = host_x - shell->menu_x;
    const int64_t local_y = host_y - shell->menu_y;
    return local_x >= 0 && local_y >= 0 && local_x < kMenuWidth && local_y < kMenuHeight;
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
    if (child_viewport_hit_locked(shell, snapshot, screen_x, screen_y, false, &hit))
        return hit;

    int32_t ignored_child = -1;
    if (sao_ui_menu_hit_test(shell->menu, static_cast<int32_t>(host_x),
                             static_cast<int32_t>(host_y), &hit.parent,
                             &ignored_child) != SAO_STATUS_OK ||
        ignored_child >= 0) {
        return {};
    }
    hit.child = -1;
    return hit;
}

} // namespace

extern "C" SAO_UI_API void SAO_UI_CALL sao_ui_test_fail_next_entity_shell_construction(void) {
    g_fail_next_entity_shell_construction.store(true, std::memory_order_release);
}

static sao_status_t create_entity_shell(sao_ui_overlay_host_handle_t host,
                                        sao_ui_compositor_handle_t borrowed_compositor,
                                        const SaoUiEntityShellConfig* config,
                                        sao_ui_entity_shell_handle_t* out_handle) {
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
                nervegear_input_rects =
                    circular_input_rects(local_center_x, local_center_y, radius,
                                         SAO_UI_NERVEGEAR_SIZE, SAO_UI_NERVEGEAR_SIZE);
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
        status = sao_ui_layer_set_input_rects(shell->nervegear_layer, nervegear_input_rects.data(),
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
    if (status == SAO_STATUS_OK && sao_ui_compositor_host(shell->compositor) != nullptr)
        status = apply_menu_effects_locked(shell.get());
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_visible(shell->menu_layer, false);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_enabled(shell->menu_layer, false);
    }
    if (status == SAO_STATUS_OK)
        status = raster_and_upload_locked(shell.get());
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_callbacks(shell->nervegear_layer, &entity_layer_cursor,
                                                  &entity_layer_leave, &entity_layer_button,
                                                  &entity_layer_scroll, &shell->nervegear_input);
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_callbacks(shell->menu_layer, &entity_layer_cursor,
                                                  &entity_layer_leave, &entity_layer_button,
                                                  &entity_layer_scroll, &shell->menu_input);
    }
    if (status == SAO_STATUS_OK &&
        g_fail_next_entity_shell_construction.exchange(false, std::memory_order_acq_rel)) {
        status = SAO_STATUS_ERR_UNKNOWN;
    }
    if (status != SAO_STATUS_OK) {
        const sao_status_t cleanup_status = destroy_members(shell.get(), !shell->owns_compositor);
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
    invalidate_rasters_locked(handle);
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
    invalidate_nervegear_raster_locked(handle);
    status =
        first_failure(status, sao_ui_nervegear_transition(handle->nervegear, SAO_UI_NG_STATE_IDLE));
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

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_set_children(sao_ui_entity_shell_handle_t handle, const char* parent_name_utf8,
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
                status = first_failure(status, sao::ui::menu_visual::set_child_hover(
                                                   handle->menu, handle->menu_hover_child_parent,
                                                   handle->menu_hover_child_index));
            }
        } else if (message == kMouseLeave) {
            status = sao_ui_nervegear_on_mouse_leave(handle->nervegear);
            handle->menu_hover_index = -1;
            handle->menu_hover_child_parent = -1;
            handle->menu_hover_child_index = -1;
            status = first_failure(status, sao_ui_menu_set_hover(handle->menu, -1));
            status =
                first_failure(status, sao::ui::menu_visual::set_child_hover(handle->menu, -1, -1));
        } else if (message == kRightButtonUp && button == 1) {
            // Right-click dismisses the open menu overlay from anywhere.
            if (handle->menu_visible) {
                status = first_failure(status, set_menu_visibility_locked(handle, false));
            }
        } else if (message == kLeftButtonDown && button == 0) {
            if (nervegear_hit) {
                status = sao_ui_nervegear_on_mouse_enter(handle->nervegear);
                status = first_failure(status, sao_ui_nervegear_on_mouse_down(handle->nervegear));
            }
            // Background click dismiss: only a click outside the complete
            // menu surface closes it. Disabled rows remain fail-closed
            // without collapsing the current navigation state.
            if (!nervegear_hit && handle->menu_visible &&
                !menu_surface_hit_locked(handle, screen_x, screen_y)) {
                status = first_failure(status, set_menu_visibility_locked(handle, false));
            }
            handle->menu_pressed_index = menu_index;
            handle->menu_pressed_child_parent = menu_hit.child >= 0 ? menu_hit.parent : -1;
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
        const bool nervegear_changed = previous_state != current_state;
        const bool menu_changed = previous_hover != handle->menu_hover_index ||
                                  previous_pressed != handle->menu_pressed_index ||
                                  previous_child_hover != handle->menu_hover_child_index ||
                                  previous_child_pressed != handle->menu_pressed_child_index ||
                                  previous_menu_visible != handle->menu_visible;
        if (nervegear_changed)
            invalidate_nervegear_raster_locked(handle);
        if (menu_changed)
            invalidate_menu_raster_locked(handle);
        handle->visual_dirty = handle->visual_dirty || nervegear_changed || menu_changed;
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

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_handle_key(
    sao_ui_entity_shell_handle_t handle, uint32_t virtual_key, uint32_t modifiers) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    (void)modifiers;
    PendingEntityAction pending_action{};
    sao_status_t status = SAO_STATUS_OK;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!handle->online || !handle->overlay_visible) {
            return SAO_STATUS_OK;
        }
        const int32_t count = static_cast<int32_t>(visible_root_count(handle));
        const int32_t current = handle->menu_hover_index;
        switch (virtual_key) {
        case 0x25U:   // Left — previous root
        case 0x26U: { // Up — previous root
            if (count <= 0)
                return SAO_STATUS_ERR_NOT_FOUND;
            int32_t target = current < 0 ? 0 : current - 1;
            if (target < 0)
                target = count - 1;
            handle->menu_hover_index = target;
            status = sao_ui_menu_set_hover(handle->menu, target);
            break;
        }
        case 0x27U:   // Right — next root
        case 0x28U: { // Down — next root
            if (count <= 0)
                return SAO_STATUS_ERR_NOT_FOUND;
            int32_t target = current < 0 ? 0 : current + 1;
            if (target >= count)
                target = 0;
            handle->menu_hover_index = target;
            status = sao_ui_menu_set_hover(handle->menu, target);
            break;
        }
        case 0x0dU:   // Enter
        case 0x20U: { // Space — activate the hovered root
            if (count <= 0)
                return SAO_STATUS_ERR_NOT_FOUND;
            const int32_t menu_index = handle->menu_hover_index < 0 ? 0 : handle->menu_hover_index;
            const int32_t logical_index = physical_to_logical_root_locked(handle, menu_index);
            if (logical_index >= 0) {
                const auto& root = handle->roots[static_cast<size_t>(logical_index)];
                status = sao_ui_menu_activate(handle->menu, menu_index);
                if (status == SAO_STATUS_OK && root.can_activate && root.children.empty() &&
                    root.action_id >= 0) {
                    const auto action = static_cast<SaoUiEntityAction>(root.action_id);
                    if (action == SAO_UI_ENTITY_ACTION_OPEN_ABOUT ||
                        action == SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR) {
                        status = first_failure(status, set_menu_visibility_locked(handle, false));
                    }
                    if (status == SAO_STATUS_OK && handle->config.action_fn != nullptr) {
                        pending_action.callback = handle->config.action_fn;
                        pending_action.user_data = handle->config.action_user_data;
                        pending_action.action = action;
                    }
                }
            }
            break;
        }
        case 0x1bU: // Escape — close the menu overlay
            if (handle->menu_visible) {
                status = first_failure(status, set_menu_visibility_locked(handle, false));
            }
            break;
        default:
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }
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
    invalidate_rasters_locked(handle);
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
