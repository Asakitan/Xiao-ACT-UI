#pragma once

#include "sao/ui/d2d_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Small, dependency-free path table for the classic SAO-style monochrome
// symbols.  The geometry is deliberately shared by the native paint context
// and the raster menu adapter in entity_shell.cpp.
namespace sao::ui::classic {

enum class IconId : uint8_t {
    Menu,
    Confirm,
    Cancel,
    Settings,
    Tools,
    Plugins,
    User,
    Notification,
    Back,
    Home,
    Folder,
    Search,
    Lock,
    Info,
    Download,
    Expand,
    Refresh,
    Close,
    Chat,
    Keyboard,
    Workshop,
    Process,
};

struct Stroke {
    float x1;
    float y1;
    float x2;
    float y2;
};

struct Definition {
    IconId id;
    const char* name;
    const Stroke* strokes;
    size_t stroke_count;
};

template <size_t N> constexpr size_t stroke_count(const Stroke (&)[N]) noexcept {
    return N;
}

inline constexpr Stroke kMenu[] = {
    {4.0F, 6.5F, 20.0F, 6.5F},
    {4.0F, 12.0F, 20.0F, 12.0F},
    {4.0F, 17.5F, 20.0F, 17.5F},
};
inline constexpr Stroke kConfirm[] = {
    {12.0F, 3.4F, 20.6F, 12.0F}, {20.6F, 12.0F, 12.0F, 20.6F}, {12.0F, 20.6F, 3.4F, 12.0F},
    {3.4F, 12.0F, 12.0F, 3.4F},  {7.7F, 12.1F, 10.6F, 15.0F},  {10.6F, 15.0F, 16.3F, 9.2F},
};
inline constexpr Stroke kCancel[] = {
    {12.0F, 3.4F, 20.6F, 12.0F}, {20.6F, 12.0F, 12.0F, 20.6F}, {12.0F, 20.6F, 3.4F, 12.0F},
    {3.4F, 12.0F, 12.0F, 3.4F},  {8.6F, 8.6F, 15.4F, 15.4F},   {15.4F, 8.6F, 8.6F, 15.4F},
};
inline constexpr Stroke kSettings[] = {
    {9.6F, 4.2F, 10.2F, 3.2F},    {10.2F, 3.2F, 13.8F, 3.2F},   {13.8F, 3.2F, 14.4F, 4.2F},
    {15.4F, 4.8F, 16.4F, 4.7F},   {16.4F, 4.7F, 18.2F, 6.5F},   {18.2F, 6.5F, 18.1F, 7.7F},
    {18.7F, 8.7F, 19.6F, 9.3F},   {19.6F, 9.3F, 19.6F, 12.9F},  {19.6F, 12.9F, 18.7F, 13.5F},
    {18.1F, 14.5F, 18.2F, 15.7F}, {18.2F, 15.7F, 16.4F, 17.5F}, {16.4F, 17.5F, 15.4F, 17.4F},
    {14.4F, 18.0F, 13.8F, 19.0F}, {13.8F, 19.0F, 10.2F, 19.0F}, {10.2F, 19.0F, 9.6F, 18.0F},
    {8.6F, 17.4F, 7.6F, 17.5F},   {7.6F, 17.5F, 5.8F, 15.7F},   {5.8F, 15.7F, 5.9F, 14.5F},
    {5.3F, 13.5F, 4.4F, 12.9F},   {4.4F, 12.9F, 4.4F, 9.3F},    {4.4F, 9.3F, 5.3F, 8.7F},
    {5.9F, 7.7F, 5.8F, 6.5F},     {5.8F, 6.5F, 7.6F, 4.7F},     {7.6F, 4.7F, 8.6F, 4.8F},
    {9.3F, 8.4F, 10.5F, 7.7F},    {10.5F, 7.7F, 13.5F, 7.7F},   {13.5F, 7.7F, 14.7F, 8.4F},
    {14.7F, 8.4F, 15.4F, 9.6F},   {15.4F, 9.6F, 15.4F, 12.6F},  {15.4F, 12.6F, 14.7F, 13.8F},
    {14.7F, 13.8F, 13.5F, 14.5F}, {13.5F, 14.5F, 10.5F, 14.5F}, {10.5F, 14.5F, 9.3F, 13.8F},
    {9.3F, 13.8F, 8.6F, 12.6F},   {8.6F, 12.6F, 8.6F, 9.6F},    {8.6F, 9.6F, 9.3F, 8.4F},
};
inline constexpr Stroke kTools[] = {
    {14.7F, 5.1F, 15.9F, 3.9F}, {15.9F, 3.9F, 18.0F, 3.0F}, {18.0F, 3.0F, 15.0F, 6.0F},
    {15.0F, 6.0F, 16.1F, 7.1F}, {16.1F, 7.1F, 19.1F, 4.1F}, {19.1F, 4.1F, 18.2F, 6.2F},
    {18.2F, 6.2F, 17.0F, 7.4F}, {17.0F, 7.4F, 8.1F, 16.8F}, {8.1F, 16.8F, 6.6F, 18.3F},
    {6.6F, 18.3F, 4.5F, 16.2F}, {4.5F, 16.2F, 6.0F, 14.7F}, {6.0F, 14.7F, 14.7F, 6.0F},
    {5.5F, 16.5F, 7.5F, 18.5F},
};
inline constexpr Stroke kPlugins[] = {
    {9.0F, 6.4F, 9.0F, 4.1F},     {9.0F, 4.1F, 11.2F, 1.9F},    {11.2F, 1.9F, 13.4F, 4.1F},
    {13.4F, 4.1F, 13.4F, 6.4F},   {13.4F, 6.4F, 16.0F, 6.4F},   {16.0F, 6.4F, 18.2F, 8.6F},
    {18.2F, 8.6F, 16.0F, 10.8F},  {16.0F, 10.8F, 13.4F, 10.8F}, {13.4F, 10.8F, 13.4F, 13.4F},
    {13.4F, 13.4F, 15.6F, 13.4F}, {15.6F, 13.4F, 17.8F, 15.6F}, {17.8F, 15.6F, 15.6F, 17.8F},
    {15.6F, 17.8F, 13.4F, 17.8F}, {13.4F, 17.8F, 13.4F, 20.1F}, {13.4F, 20.1F, 9.0F, 20.1F},
    {9.0F, 20.1F, 9.0F, 17.8F},   {9.0F, 17.8F, 6.8F, 17.8F},   {6.8F, 17.8F, 4.6F, 15.6F},
    {4.6F, 15.6F, 6.8F, 13.4F},   {6.8F, 13.4F, 9.0F, 13.4F},   {9.0F, 13.4F, 9.0F, 10.8F},
    {9.0F, 10.8F, 6.4F, 10.8F},   {6.4F, 10.8F, 4.2F, 8.6F},    {4.2F, 8.6F, 6.4F, 6.4F},
    {6.4F, 6.4F, 9.0F, 6.4F},
};
inline constexpr Stroke kUser[] = {
    {12.0F, 4.3F, 13.6F, 4.7F},   {13.6F, 4.7F, 14.8F, 5.9F},   {14.8F, 5.9F, 15.2F, 7.5F},
    {15.2F, 7.5F, 14.8F, 9.1F},   {14.8F, 9.1F, 13.6F, 10.3F},  {13.6F, 10.3F, 12.0F, 10.7F},
    {12.0F, 10.7F, 10.4F, 10.3F}, {10.4F, 10.3F, 9.2F, 9.1F},   {9.2F, 9.1F, 8.8F, 7.5F},
    {8.8F, 7.5F, 9.2F, 5.9F},     {9.2F, 5.9F, 10.4F, 4.7F},    {10.4F, 4.7F, 12.0F, 4.3F},
    {5.2F, 20.0F, 5.8F, 17.7F},   {5.8F, 17.7F, 7.2F, 15.8F},   {7.2F, 15.8F, 9.2F, 14.5F},
    {9.2F, 14.5F, 12.0F, 14.0F},  {12.0F, 14.0F, 14.8F, 14.5F}, {14.8F, 14.5F, 16.8F, 15.8F},
    {16.8F, 15.8F, 18.2F, 17.7F}, {18.2F, 17.7F, 18.8F, 20.0F},
};
inline constexpr Stroke kNotification[] = {
    {6.5F, 16.8F, 17.5F, 16.8F},  {17.5F, 16.8F, 16.3F, 15.0F}, {16.3F, 15.0F, 16.3F, 10.0F},
    {16.3F, 10.0F, 15.1F, 6.8F},  {15.1F, 6.8F, 12.0F, 5.7F},   {12.0F, 5.7F, 8.9F, 6.8F},
    {8.9F, 6.8F, 7.7F, 10.0F},    {7.7F, 10.0F, 7.7F, 15.0F},   {7.7F, 15.0F, 6.5F, 16.8F},
    {10.0F, 19.2F, 10.6F, 20.0F}, {10.6F, 20.0F, 13.4F, 20.0F}, {13.4F, 20.0F, 14.0F, 19.2F},
    {12.0F, 3.2F, 12.0F, 5.7F},
};
inline constexpr Stroke kBack[] = {
    {13.8F, 5.5F, 7.3F, 12.0F}, {7.3F, 12.0F, 13.8F, 18.5F}, {7.8F, 12.0F, 20.2F, 12.0F}};
inline constexpr Stroke kHome[] = {{3.8F, 10.8F, 12.0F, 3.8F},   {12.0F, 3.8F, 20.2F, 10.8F},
                                   {3.8F, 10.8F, 3.8F, 19.1F},   {3.8F, 19.1F, 20.2F, 19.1F},
                                   {20.2F, 19.1F, 20.2F, 10.8F}, {9.3F, 19.1F, 9.3F, 14.5F},
                                   {9.3F, 14.5F, 14.7F, 14.5F},  {14.7F, 14.5F, 14.7F, 19.1F}};
inline constexpr Stroke kFolder[] = {
    {3.5F, 6.2F, 9.5F, 6.2F},    {9.5F, 6.2F, 11.3F, 8.2F},    {11.3F, 8.2F, 20.5F, 8.2F},
    {20.5F, 8.2F, 20.5F, 17.8F}, {20.5F, 17.8F, 18.9F, 19.4F}, {18.9F, 19.4F, 5.1F, 19.4F},
    {5.1F, 19.4F, 3.5F, 17.8F},  {3.5F, 17.8F, 3.5F, 6.2F},    {3.5F, 8.2F, 20.5F, 8.2F}};
inline constexpr Stroke kSearch[] = {
    {10.5F, 4.8F, 12.8F, 5.3F},   {12.8F, 5.3F, 14.7F, 6.8F},   {14.7F, 6.8F, 15.8F, 8.5F},
    {15.8F, 8.5F, 16.2F, 10.5F},  {16.2F, 10.5F, 15.8F, 12.5F}, {15.8F, 12.5F, 14.7F, 14.2F},
    {14.7F, 14.2F, 12.8F, 15.7F}, {12.8F, 15.7F, 10.5F, 16.2F}, {10.5F, 16.2F, 8.2F, 15.7F},
    {8.2F, 15.7F, 6.3F, 14.2F},   {6.3F, 14.2F, 5.2F, 12.5F},   {5.2F, 12.5F, 4.8F, 10.5F},
    {4.8F, 10.5F, 5.2F, 8.5F},    {5.2F, 8.5F, 6.3F, 6.8F},     {6.3F, 6.8F, 8.2F, 5.3F},
    {8.2F, 5.3F, 10.5F, 4.8F},    {15.0F, 15.0F, 20.0F, 20.0F},
};
inline constexpr Stroke kLock[] = {
    {5.2F, 10.0F, 18.8F, 10.0F}, {18.8F, 10.0F, 18.8F, 20.0F}, {18.8F, 20.0F, 5.2F, 20.0F},
    {5.2F, 20.0F, 5.2F, 10.0F},  {8.0F, 10.0F, 8.0F, 7.5F},    {8.0F, 7.5F, 9.0F, 4.9F},
    {9.0F, 4.9F, 12.0F, 3.5F},   {12.0F, 3.5F, 15.0F, 4.9F},   {15.0F, 4.9F, 16.0F, 7.5F},
    {16.0F, 7.5F, 16.0F, 10.0F}, {12.0F, 14.0F, 12.0F, 16.6F}};
inline constexpr Stroke kInfo[] = {
    {12.0F, 3.5F, 14.2F, 3.8F},   {14.2F, 3.8F, 16.1F, 4.9F},   {16.1F, 4.9F, 17.6F, 6.4F},
    {17.6F, 6.4F, 18.7F, 8.3F},   {18.7F, 8.3F, 19.0F, 12.0F},  {19.0F, 12.0F, 18.7F, 15.7F},
    {18.7F, 15.7F, 17.6F, 17.6F}, {17.6F, 17.6F, 16.1F, 19.1F}, {16.1F, 19.1F, 14.2F, 20.2F},
    {14.2F, 20.2F, 12.0F, 20.5F}, {12.0F, 20.5F, 9.8F, 20.2F},  {9.8F, 20.2F, 7.9F, 19.1F},
    {7.9F, 19.1F, 6.4F, 17.6F},   {6.4F, 17.6F, 5.3F, 15.7F},   {5.3F, 15.7F, 5.0F, 12.0F},
    {5.0F, 12.0F, 5.3F, 8.3F},    {5.3F, 8.3F, 6.4F, 6.4F},     {6.4F, 6.4F, 7.9F, 4.9F},
    {7.9F, 4.9F, 9.8F, 3.8F},     {9.8F, 3.8F, 12.0F, 3.5F},    {12.0F, 9.5F, 12.0F, 15.7F},
    {12.0F, 7.0F, 12.0F, 7.1F},
};
inline constexpr Stroke kDownload[] = {{12.0F, 3.5F, 12.0F, 15.0F},
                                       {7.8F, 10.8F, 12.0F, 15.0F},
                                       {12.0F, 15.0F, 16.2F, 10.8F},
                                       {4.5F, 19.5F, 19.5F, 19.5F}};
inline constexpr Stroke kExpand[] = {
    {9.0F, 4.0F, 4.0F, 4.0F},     {4.0F, 4.0F, 4.0F, 9.0F},     {15.0F, 4.0F, 20.0F, 4.0F},
    {20.0F, 4.0F, 20.0F, 9.0F},   {9.0F, 20.0F, 4.0F, 20.0F},   {4.0F, 20.0F, 4.0F, 15.0F},
    {15.0F, 20.0F, 20.0F, 20.0F}, {20.0F, 20.0F, 20.0F, 15.0F}, {4.0F, 4.0F, 10.0F, 10.0F},
    {20.0F, 4.0F, 14.0F, 10.0F},  {4.0F, 20.0F, 10.0F, 14.0F},  {20.0F, 20.0F, 14.0F, 14.0F},
};
inline constexpr Stroke kRefresh[] = {
    {19.2F, 8.8F, 17.2F, 6.5F},   {17.2F, 6.5F, 14.5F, 5.0F},   {14.5F, 5.0F, 11.5F, 4.5F},
    {11.5F, 4.5F, 8.5F, 5.1F},    {8.5F, 5.1F, 6.0F, 7.0F},     {6.0F, 7.0F, 3.8F, 9.2F},
    {3.8F, 9.2F, 7.9F, 9.2F},     {4.8F, 15.2F, 6.8F, 17.5F},   {6.8F, 17.5F, 9.5F, 19.0F},
    {9.5F, 19.0F, 12.5F, 19.5F},  {12.5F, 19.5F, 15.5F, 18.9F}, {15.5F, 18.9F, 18.0F, 17.0F},
    {18.0F, 17.0F, 20.2F, 14.8F}, {16.1F, 14.8F, 20.2F, 14.8F},
};
inline constexpr Stroke kClose[] = {{6.5F, 6.5F, 17.5F, 17.5F}, {17.5F, 6.5F, 6.5F, 17.5F}};
inline constexpr Stroke kChat[] = {
    {5.0F, 5.0F, 19.0F, 5.0F},   {19.0F, 5.0F, 19.0F, 15.0F}, {19.0F, 15.0F, 14.0F, 15.0F},
    {14.0F, 15.0F, 9.0F, 19.0F}, {9.0F, 19.0F, 9.0F, 15.0F},  {9.0F, 15.0F, 5.0F, 15.0F},
    {5.0F, 15.0F, 5.0F, 5.0F},   {8.0F, 9.0F, 10.0F, 9.0F},   {12.0F, 9.0F, 14.0F, 9.0F},
    {16.0F, 9.0F, 17.0F, 9.0F},
};
inline constexpr Stroke kKeyboard[] = {
    {3.5F, 6.0F, 20.5F, 6.0F},    {20.5F, 6.0F, 20.5F, 18.0F}, {20.5F, 18.0F, 3.5F, 18.0F},
    {3.5F, 18.0F, 3.5F, 6.0F},    {6.0F, 9.0F, 8.0F, 9.0F},    {10.0F, 9.0F, 12.0F, 9.0F},
    {14.0F, 9.0F, 16.0F, 9.0F},   {6.0F, 12.0F, 8.0F, 12.0F},  {10.0F, 12.0F, 12.0F, 12.0F},
    {14.0F, 12.0F, 16.0F, 12.0F}, {6.0F, 15.0F, 18.0F, 15.0F},
};
inline constexpr Stroke kWorkshop[] = {
    {4.0F, 8.0F, 12.0F, 3.5F},    {12.0F, 3.5F, 20.0F, 8.0F},  {4.0F, 8.0F, 12.0F, 12.5F},
    {12.0F, 12.5F, 20.0F, 8.0F},  {4.0F, 8.0F, 4.0F, 17.5F},   {4.0F, 17.5F, 12.0F, 21.0F},
    {12.0F, 21.0F, 20.0F, 17.5F}, {20.0F, 17.5F, 20.0F, 8.0F}, {12.0F, 12.5F, 12.0F, 21.0F},
    {8.0F, 6.0F, 8.0F, 10.0F},    {8.0F, 10.0F, 12.0F, 12.5F},
};
inline constexpr Stroke kProcess[] = {
    {4.0F, 4.5F, 20.0F, 4.5F},    {20.0F, 4.5F, 20.0F, 19.5F}, {20.0F, 19.5F, 4.0F, 19.5F},
    {4.0F, 19.5F, 4.0F, 4.5F},    {4.0F, 8.0F, 20.0F, 8.0F},   {7.0F, 12.0F, 9.0F, 12.0F},
    {11.0F, 12.0F, 17.0F, 12.0F}, {7.0F, 15.5F, 9.0F, 15.5F},  {11.0F, 15.5F, 17.0F, 15.5F},
};

inline constexpr Definition kDefinitions[] = {
    {IconId::Menu, "menu", kMenu, stroke_count(kMenu)},
    {IconId::Confirm, "confirm", kConfirm, stroke_count(kConfirm)},
    {IconId::Cancel, "cancel", kCancel, stroke_count(kCancel)},
    {IconId::Settings, "settings", kSettings, stroke_count(kSettings)},
    {IconId::Tools, "tools", kTools, stroke_count(kTools)},
    {IconId::Plugins, "plugins", kPlugins, stroke_count(kPlugins)},
    {IconId::User, "user", kUser, stroke_count(kUser)},
    {IconId::Notification, "notification", kNotification, stroke_count(kNotification)},
    {IconId::Back, "back", kBack, stroke_count(kBack)},
    {IconId::Home, "home", kHome, stroke_count(kHome)},
    {IconId::Folder, "folder", kFolder, stroke_count(kFolder)},
    {IconId::Search, "search", kSearch, stroke_count(kSearch)},
    {IconId::Lock, "lock", kLock, stroke_count(kLock)},
    {IconId::Info, "info", kInfo, stroke_count(kInfo)},
    {IconId::Download, "download", kDownload, stroke_count(kDownload)},
    {IconId::Expand, "expand", kExpand, stroke_count(kExpand)},
    {IconId::Refresh, "refresh", kRefresh, stroke_count(kRefresh)},
    {IconId::Close, "close", kClose, stroke_count(kClose)},
    {IconId::Chat, "chat", kChat, stroke_count(kChat)},
    {IconId::Keyboard, "keyboard", kKeyboard, stroke_count(kKeyboard)},
    {IconId::Workshop, "workshop", kWorkshop, stroke_count(kWorkshop)},
    {IconId::Process, "process", kProcess, stroke_count(kProcess)},
};

inline const Definition& definition(IconId id) noexcept {
    for (const auto& candidate : kDefinitions) {
        if (candidate.id == id)
            return candidate;
    }
    return kDefinitions[0];
}

// Native consumer: all drawing is routed through the existing paint context
// ABI. Coordinates are in the caller's local space and size is the full icon
// box in pixels.
inline sao_status_t paint_classic_icon(sao_ui_paint_ctx_handle_t ctx, IconId id, float x, float y,
                                       float size, uint32_t argb) noexcept {
    if (ctx == nullptr || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(size) ||
        size <= 0.0F)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto& icon = definition(id);
    const float scale = size / 24.0F;
    const float stroke_width = std::max(1.0F, scale * 1.65F);
    for (size_t index = 0; index < icon.stroke_count; ++index) {
        const Stroke& stroke = icon.strokes[index];
        const sao_status_t status = sao_ui_paint_ctx_stroke_line(
            ctx, x + stroke.x1 * scale, y + stroke.y1 * scale, x + stroke.x2 * scale,
            y + stroke.y2 * scale, stroke_width, argb);
        if (status != SAO_STATUS_OK)
            return status;
    }
    return SAO_STATUS_OK;
}

} // namespace sao::ui::classic
