#include "sao/ui/entity_shell.h"

#include "sao/ui/compositor.h"
#include "sao/ui/menu.h"
#include "sao/ui/theme.h"

#if defined(_WIN32)
#include "entity_authority_frames.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_menu_compute_button_layout(sao_ui_menu_handle_t handle, int32_t button_index, int32_t* out_x,
                                  int32_t* out_y, int32_t* out_w, int32_t* out_h);
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
}

namespace {

constexpr int32_t kMarginRight = 20;
constexpr int32_t kMarginBottom = 20;
constexpr int32_t kMenuWidth = 442;
constexpr int32_t kMenuHeight = 430;
constexpr int32_t kMenuSlot = 70;
constexpr int32_t kMenuPad = 40;
constexpr int32_t kMenuColumnCenter = kMenuPad + kMenuSlot / 2;
constexpr int32_t kAboutIndex = 4;
constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kMouseLeave = 0x02A3;

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

Pixel premultiply(Color color) {
    return {
        static_cast<uint8_t>((static_cast<uint32_t>(color.b) * color.a + 127U) / 255U),
        static_cast<uint8_t>((static_cast<uint32_t>(color.g) * color.a + 127U) / 255U),
        static_cast<uint8_t>((static_cast<uint32_t>(color.r) * color.a + 127U) / 255U),
        color.a,
    };
}

void blend(Raster& raster, int32_t x, int32_t y, Color color) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return;
    }
    const Pixel source = premultiply(color);
    Pixel& destination = raster.pixels[static_cast<size_t>(y) * raster.width + x];
    const uint32_t inverse = 255U - source.a;
    destination.b = static_cast<uint8_t>(
        source.b + (static_cast<uint32_t>(destination.b) * inverse + 127U) / 255U);
    destination.g = static_cast<uint8_t>(
        source.g + (static_cast<uint32_t>(destination.g) * inverse + 127U) / 255U);
    destination.r = static_cast<uint8_t>(
        source.r + (static_cast<uint32_t>(destination.r) * inverse + 127U) / 255U);
    destination.a = static_cast<uint8_t>(
        source.a + (static_cast<uint32_t>(destination.a) * inverse + 127U) / 255U);
}

void fill_rect(Raster& raster, int32_t x, int32_t y, int32_t width, int32_t height, Color color) {
    const int32_t left = std::max(0, x);
    const int32_t top = std::max(0, y);
    const int32_t right = std::min(static_cast<int32_t>(raster.width), x + width);
    const int32_t bottom = std::min(static_cast<int32_t>(raster.height), y + height);
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px)
            blend(raster, px, py, color);
    }
}

void fill_circle(Raster& raster, int32_t center_x, int32_t center_y, int32_t radius, Color color) {
    const int32_t radius_squared = radius * radius;
    for (int32_t y = center_y - radius; y <= center_y + radius; ++y) {
        for (int32_t x = center_x - radius; x <= center_x + radius; ++x) {
            const int32_t dx = x - center_x;
            const int32_t dy = y - center_y;
            if (dx * dx + dy * dy <= radius_squared)
                blend(raster, x, y, color);
        }
    }
}

void stroke_circle(Raster& raster, int32_t center_x, int32_t center_y, int32_t radius,
                   int32_t thickness, Color color) {
    const int32_t outer = radius * radius;
    const int32_t inner_radius = std::max(0, radius - thickness);
    const int32_t inner = inner_radius * inner_radius;
    for (int32_t y = center_y - radius; y <= center_y + radius; ++y) {
        for (int32_t x = center_x - radius; x <= center_x + radius; ++x) {
            const int32_t dx = x - center_x;
            const int32_t dy = y - center_y;
            const int32_t distance = dx * dx + dy * dy;
            if (distance <= outer && distance >= inner)
                blend(raster, x, y, color);
        }
    }
}

void draw_line(Raster& raster, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness,
               Color color) {
    const int32_t steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (steps == 0) {
        fill_circle(raster, x0, y0, std::max(1, thickness / 2), color);
        return;
    }
    for (int32_t step = 0; step <= steps; ++step) {
        const float ratio = static_cast<float>(step) / steps;
        const int32_t x =
            static_cast<int32_t>(std::lround(x0 + static_cast<float>(x1 - x0) * ratio));
        const int32_t y =
            static_cast<int32_t>(std::lround(y0 + static_cast<float>(y1 - y0) * ratio));
        fill_circle(raster, x, y, std::max(1, thickness / 2), color);
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
constexpr Glyph kBlankGlyph{};

const Glyph& glyph_for(unsigned char character) {
    if (character >= 'A' && character <= 'Z') {
        return kUppercaseGlyphs[character - 'A'];
    }
    return character == '?' ? kQuestionGlyph : kBlankGlyph;
}

void draw_text(Raster& raster, int32_t x, int32_t y, std::string_view text, int32_t scale,
               Color color) {
    int32_t cursor = x;
    for (const unsigned char character : text) {
        const Glyph& glyph = glyph_for(character);
        for (int32_t row = 0; row < static_cast<int32_t>(glyph.size()); ++row) {
            for (int32_t column = 0; column < 5; ++column) {
                if ((glyph[row] & (1U << (4 - column))) != 0U) {
                    fill_rect(raster, cursor + column * scale, y + row * scale, scale, scale,
                              color);
                }
            }
        }
        cursor += scale * 6;
    }
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

Raster rasterize_menu(int32_t hover_index, int32_t pressed_index) {
#if defined(_WIN32)
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
    return load_authority_frame(resource_id, kMenuWidth, kMenuHeight);
#else
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

    constexpr std::array<std::string_view, 5> labels{"CONTROL", "TOOLS", "PLUGINS", "SKINS",
                                                     "ABOUT"};
    constexpr std::array<std::string_view, 5> icons{"C", "T", "P", "S", "?"};
    for (int32_t index = 0; index < static_cast<int32_t>(labels.size()); ++index) {
        const bool hover = index == hover_index;
        const bool pressed = index == pressed_index;
        const int32_t size = hover || pressed ? 70 : 54;
        const int32_t center_x = kMenuColumnCenter;
        const int32_t center_y = kMenuPad + index * kMenuSlot + kMenuSlot / 2;
        fill_circle(raster, center_x, center_y, size / 2,
                    pressed ? Color{244, 235, 215, 235}
                    : hover ? Color{237, 247, 250, 235}
                            : Color{247, 248, 248, 220});
        stroke_circle(raster, center_x, center_y, size / 2, 2,
                      index == kAboutIndex ? Color{212, 156, 23, 245} : Color{88, 152, 190, 225});
        draw_text(raster, center_x - 5, center_y - 7, icons[index], 2, Color{100, 99, 100, 255});
        draw_text(raster, 118, center_y - 7, labels[index], 1,
                  pressed ? Color{98, 88, 70, 255} : Color{100, 99, 100, 255});
        draw_line(raster, 113, center_y + 15, kMenuWidth - 19, center_y + 15, 1,
                  Color{188, 196, 202, 145});
    }
    return raster;
#endif
}

sao_status_t first_failure(sao_status_t current, sao_status_t candidate) {
    return current == SAO_STATUS_OK && candidate != SAO_STATUS_OK ? candidate : current;
}

} // namespace

struct sao_ui_entity_shell_s {
    sao_ui_overlay_host_handle_t host{};
    sao_ui_compositor_handle_t compositor{};
    sao_ui_theme_handle_t theme{};
    sao_ui_menu_handle_t menu{};
    sao_ui_nervegear_handle_t nervegear{};
    sao_ui_layer_handle_t nervegear_layer{};
    sao_ui_layer_handle_t menu_layer{};
    SaoUiEntityShellConfig config{};
    Raster nervegear_raster;
    Raster menu_raster;
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
    bool online{};
    bool overlay_visible{};
    bool menu_visible{};
    bool visual_dirty{true};
    bool input_region_settle_pending{};
    uint64_t frame_count{};
    uint64_t action_count{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::thread::id owner_thread;
    mutable std::mutex mutex;
};

namespace {

void destroy_members(sao_ui_entity_shell_s* shell) {
    sao_ui_layer_destroy(shell->menu_layer);
    shell->menu_layer = nullptr;
    sao_ui_layer_destroy(shell->nervegear_layer);
    shell->nervegear_layer = nullptr;
    sao_ui_nervegear_destroy(shell->nervegear);
    shell->nervegear = nullptr;
    sao_ui_menu_destroy(shell->menu);
    shell->menu = nullptr;
    sao_ui_theme_destroy(shell->theme);
    shell->theme = nullptr;
    sao_ui_compositor_destroy(shell->compositor);
    shell->compositor = nullptr;
}

sao_status_t set_menu_visibility_locked(sao_ui_entity_shell_s* shell, bool visible) {
    const bool changed = shell->menu_visible != visible;
    const sao_status_t status =
        visible ? sao_ui_menu_show(shell->menu, shell->menu_x + kMenuColumnCenter,
                                   shell->menu_y + kMenuPad)
                : sao_ui_menu_hide(shell->menu);
    if (status != SAO_STATUS_OK)
        return status;
    shell->menu_visible = visible;
    shell->visual_dirty = shell->visual_dirty || changed;
    if (!visible) {
        shell->menu_hover_index = -1;
        shell->menu_pressed_index = -1;
        (void)sao_ui_menu_set_hover(shell->menu, -1);
    }
    return SAO_STATUS_OK;
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
    const bool nervegear_active = shell->online && shell->overlay_visible;
    SaoUiMenuPhase menu_phase = SAO_UI_MENU_PHASE_CLOSED;
    float menu_progress = 0.0F;
    sao_status_t status = sao_ui_menu_get_phase(shell->menu, &menu_phase);
    status =
        first_failure(status, sao_ui_menu_get_transition_progress(shell->menu, &menu_progress));
    const bool menu_active = nervegear_active && menu_phase != SAO_UI_MENU_PHASE_CLOSED;
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
                                       shell->menu_layer, menu_active && shell->menu_visible));
    return status;
}

sao_status_t raster_and_upload_locked(sao_ui_entity_shell_s* shell) {
    SaoUiNerveGearState state = SAO_UI_NG_STATE_IDLE;
    sao_status_t status = sao_ui_nervegear_get_state(shell->nervegear, &state);
    if (status != SAO_STATUS_OK)
        return status;
    try {
        shell->nervegear_raster = rasterize_nervegear(state);
        shell->menu_raster = rasterize_menu(shell->menu_hover_index, shell->menu_pressed_index);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    status = sao_ui_layer_update_bgra(
        shell->nervegear_layer,
        reinterpret_cast<const uint8_t*>(shell->nervegear_raster.pixels.data()),
        shell->nervegear_raster.width, shell->nervegear_raster.height,
        shell->nervegear_raster.width * sizeof(Pixel));
    if (status != SAO_STATUS_OK)
        return status;
    return sao_ui_layer_update_bgra(
        shell->menu_layer, reinterpret_cast<const uint8_t*>(shell->menu_raster.pixels.data()),
        shell->menu_raster.width, shell->menu_raster.height,
        shell->menu_raster.width * sizeof(Pixel));
}

sao_status_t commit_visual_state_locked(sao_ui_entity_shell_s* shell) {
    if (!shell->visual_dirty) {
        if (shell->host == nullptr || !shell->input_region_settle_pending) {
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
    status = first_failure(status, raster_and_upload_locked(shell));
    if (shell->host != nullptr) {
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

sao_status_t sync_frame_locked(sao_ui_entity_shell_s* shell, uint32_t elapsed_ms) {
    SaoUiNerveGearState previous_state = SAO_UI_NG_STATE_IDLE;
    SaoUiMenuPhase previous_menu_phase = SAO_UI_MENU_PHASE_CLOSED;
    float previous_menu_progress = 0.0F;
    sao_status_t status = refresh_host_geometry_locked(shell);
    status = first_failure(status, sao_ui_nervegear_get_state(shell->nervegear, &previous_state));
    status = first_failure(status, sao_ui_menu_get_phase(shell->menu, &previous_menu_phase));
    status = first_failure(
        status, sao_ui_menu_get_transition_progress(shell->menu, &previous_menu_progress));
    status = first_failure(
        status, sao_ui_nervegear_tick(shell->nervegear, static_cast<int32_t>(elapsed_ms)));
    status = first_failure(status, sao_ui_menu_tick(shell->menu, static_cast<int32_t>(elapsed_ms)));
    SaoUiNerveGearState current_state = previous_state;
    SaoUiMenuPhase current_menu_phase = previous_menu_phase;
    float current_menu_progress = previous_menu_progress;
    status = first_failure(status, sao_ui_nervegear_get_state(shell->nervegear, &current_state));
    status = first_failure(status, sao_ui_menu_get_phase(shell->menu, &current_menu_phase));
    status = first_failure(
        status, sao_ui_menu_get_transition_progress(shell->menu, &current_menu_progress));
    shell->visual_dirty = shell->visual_dirty || previous_state != current_state ||
                          previous_menu_phase != current_menu_phase ||
                          previous_menu_progress != current_menu_progress;
    status = first_failure(status, commit_visual_state_locked(shell));
    shell->last_status = status;
    return status;
}

bool local_nervegear_hit_locked(sao_ui_entity_shell_s* shell, int32_t screen_x, int32_t screen_y) {
    bool hit = false;
    return sao_ui_nervegear_hit_test(shell->nervegear, screen_x, screen_y, &hit) == SAO_STATUS_OK &&
           hit;
}

int32_t menu_hit_locked(sao_ui_entity_shell_s* shell, int32_t screen_x, int32_t screen_y) {
    int32_t index = -1;
    int32_t child = -1;
    if (!shell->menu_visible ||
        sao_ui_menu_hit_test(shell->menu, screen_x - shell->origin_x, screen_y - shell->origin_y,
                             &index, &child) != SAO_STATUS_OK) {
        return -1;
    }
    return index;
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL
sao_ui_entity_shell_create(sao_ui_overlay_host_handle_t host, const SaoUiEntityShellConfig* config,
                           sao_ui_entity_shell_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
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

    sao_status_t status = sao_ui_compositor_create(host, nullptr, &shell->compositor);
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
        const std::array<SaoUiMenuItem, 5> items{{
            {"Control", "C", 10, false, {false, false, false}},
            {"Tools", "T", 11, false, {false, false, false}},
            {"Plugins", "P", 12, false, {false, false, false}},
            {"Skins", "S", 13, false, {false, false, false}},
            {"About", "?", SAO_UI_ENTITY_ACTION_OPEN_ABOUT, true, {false, false, false}},
        }};
        status = sao_ui_menu_set_items(shell->menu, items.data(), items.size());
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
        SaoLayerConfig layer{};
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
        SaoLayerConfig layer{};
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
        std::array<SaoUiLayerInputRect, 5> input_rects{};
        for (int32_t index = 0; index < static_cast<int32_t>(input_rects.size()); ++index) {
            int32_t x = 0;
            int32_t y = 0;
            int32_t width = 0;
            int32_t height = 0;
            status = sao_ui_menu_compute_button_layout(shell->menu, index, &x, &y, &width, &height);
            if (status != SAO_STATUS_OK)
                break;
            input_rects[static_cast<size_t>(index)] = {x - shell->menu_x, y - shell->menu_y, width,
                                                       height};
        }
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_input_rects(shell->menu_layer, input_rects.data(),
                                                  input_rects.size());
        }
    }
    if (status == SAO_STATUS_OK) {
        status = sao_ui_layer_set_input_enabled(shell->menu_layer, false);
    }
    if (status == SAO_STATUS_OK)
        status = raster_and_upload_locked(shell.get());
    if (status != SAO_STATUS_OK) {
        destroy_members(shell.get());
        return status;
    }
    *out_handle = shell.release();
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_entity_shell_destroy(sao_ui_entity_shell_handle_t handle) {
    if (handle == nullptr)
        return;
    if (handle->host != nullptr) {
        (void)sao_ui_overlay_host_set_mouse(handle->host, nullptr, nullptr);
        (void)sao_ui_overlay_host_set_hit_test(handle->host, nullptr, nullptr);
    }
    if (std::this_thread::get_id() != handle->owner_thread)
        return;
    (void)sao_ui_entity_shell_take_offline(handle);
    destroy_members(handle);
    delete handle;
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
    if (handle->host != nullptr) {
        status = first_failure(status, sao_ui_overlay_host_set_visible(handle->host, true));
    }
    if (status != SAO_STATUS_OK) {
        if (handle->host != nullptr) {
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
    if (handle->host != nullptr) {
        status = first_failure(status, sao_ui_overlay_host_set_visible(handle->host, false));
    }
    handle->last_status = status;
    return status;
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
    (void)wheel_delta;
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    sao_ui_entity_action_fn_t action_fn = nullptr;
    void* action_user_data = nullptr;
    sao_status_t status = SAO_STATUS_OK;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!handle->online || !handle->overlay_visible) {
            return SAO_STATUS_OK;
        }

        const bool nervegear_hit = local_nervegear_hit_locked(handle, screen_x, screen_y);
        const int32_t menu_index = menu_hit_locked(handle, screen_x, screen_y);
        SaoUiNerveGearState previous_state = SAO_UI_NG_STATE_IDLE;
        status = sao_ui_nervegear_get_state(handle->nervegear, &previous_state);
        const int32_t previous_hover = handle->menu_hover_index;
        const int32_t previous_pressed = handle->menu_pressed_index;
        const bool previous_menu_visible = handle->menu_visible;
        if (message == kMouseMove) {
            status = nervegear_hit ? sao_ui_nervegear_on_mouse_enter(handle->nervegear)
                                   : sao_ui_nervegear_on_mouse_leave(handle->nervegear);
            if (handle->menu_hover_index != menu_index) {
                handle->menu_hover_index = menu_index;
                status = first_failure(status, sao_ui_menu_set_hover(handle->menu, menu_index));
            }
        } else if (message == kMouseLeave) {
            status = sao_ui_nervegear_on_mouse_leave(handle->nervegear);
            handle->menu_hover_index = -1;
            status = first_failure(status, sao_ui_menu_set_hover(handle->menu, -1));
        } else if (message == kLeftButtonDown && button == 0) {
            if (nervegear_hit) {
                status = sao_ui_nervegear_on_mouse_enter(handle->nervegear);
                status = first_failure(status, sao_ui_nervegear_on_mouse_down(handle->nervegear));
            }
            handle->menu_pressed_index = menu_index;
        } else if (message == kLeftButtonUp && button == 0) {
            if (nervegear_hit) {
                status = sao_ui_nervegear_on_mouse_up(handle->nervegear);
                if (status == SAO_STATUS_OK && !handle->menu_visible) {
                    status = set_menu_visibility_locked(handle, true);
                }
                status = first_failure(
                    status, sao_ui_nervegear_transition(handle->nervegear, SAO_UI_NG_STATE_IDLE));
                status = first_failure(status, sao_ui_nervegear_on_mouse_enter(handle->nervegear));
            } else if (menu_index >= 0 && menu_index == handle->menu_pressed_index) {
                status = sao_ui_menu_activate(handle->menu, menu_index);
                if (status == SAO_STATUS_OK && menu_index == kAboutIndex) {
                    status = set_menu_visibility_locked(handle, false);
                    if (status == SAO_STATUS_OK && handle->config.action_fn != nullptr) {
                        ++handle->action_count;
                        action_fn = handle->config.action_fn;
                        action_user_data = handle->config.action_user_data;
                    }
                }
            }
            handle->menu_pressed_index = -1;
        }
        SaoUiNerveGearState current_state = previous_state;
        status =
            first_failure(status, sao_ui_nervegear_get_state(handle->nervegear, &current_state));
        handle->visual_dirty = handle->visual_dirty || previous_state != current_state ||
                               previous_hover != handle->menu_hover_index ||
                               previous_pressed != handle->menu_pressed_index ||
                               previous_menu_visible != handle->menu_visible;
        status = first_failure(status, commit_visual_state_locked(handle));
        handle->last_status = status;
    }
    if (status == SAO_STATUS_OK && action_fn != nullptr) {
        sao_status_t action_status = SAO_STATUS_ERR_UNKNOWN;
        try {
            action_status = action_fn(SAO_UI_ENTITY_ACTION_OPEN_ABOUT, action_user_data);
        } catch (...) {
            action_status = SAO_STATUS_ERR_UNKNOWN;
        }
        std::lock_guard<std::mutex> lock(handle->mutex);
        handle->last_status = action_status;
        return action_status;
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
    *out_hit = menu_hit_locked(handle, screen_x, screen_y) >= 0;
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
    if (handle->host != nullptr) {
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

extern "C" sao_status_t SAO_UI_CALL sao_ui_entity_shell_snapshot_bgra(
    sao_ui_entity_shell_handle_t handle, uint8_t* out_bgra_pixels, size_t capacity,
    uint32_t* out_width, uint32_t* out_height, size_t* out_bytes) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(handle->mutex);
    return sao_ui_compositor_snapshot_bgra(handle->compositor, out_bgra_pixels, capacity, out_width,
                                           out_height, out_bytes);
}
