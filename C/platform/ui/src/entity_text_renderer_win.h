#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sao::ui::entity_text {

enum class FontRole : uint8_t {
    Icon,
    Label,
};

struct TextColor {
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
    uint8_t a{};
};

struct TextCommand {
    int32_t x{};
    int32_t y{};
    int32_t max_width{};
    int32_t max_height{};
    std::string utf8;
    FontRole font_role{FontRole::Label};
    float pixel_size{};
    TextColor color{};
    bool ellipsis{};
};

struct BgraSurface {
    uint8_t* pixels{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
};

bool render_text(BgraSurface target, const std::vector<TextCommand>& commands) noexcept;

} // namespace sao::ui::entity_text
