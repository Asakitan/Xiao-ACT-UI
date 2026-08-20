// SAO AI Editor - kernel-map command-palette wiring.

#pragma once

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::native {
class ExtensionHost;
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::kernel_map {

class Bridge;

inline bool parse_kernel_map_uint64_text(std::string_view text,
                                         uint64_t& out) noexcept {
    out = 0;
    if (text.empty()) return false;
    int base = 10;
    if (text.size() >= 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return false;
    for (const char ch : text) {
        const bool valid = base == 16
            ? ((ch >= '0' && ch <= '9') ||
               (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F'))
            : (ch >= '0' && ch <= '9');
        if (!valid) return false;
    }
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        value, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

inline bool parse_kernel_map_uint64(const nlohmann::json& value,
                                     uint64_t& out) noexcept {
    return value.is_string() &&
           parse_kernel_map_uint64_text(
               std::string_view(value.get_ref<const std::string&>()), out);
}

inline bool parse_kernel_map_uint32(const nlohmann::json& value,
                                    uint32_t& out) noexcept {
    uint64_t parsed = 0;
    if (value.is_number_unsigned()) {
        parsed = value.get<uint64_t>();
    } else if (value.is_number_integer()) {
        const int64_t signed_value = value.get<int64_t>();
        if (signed_value < 0) return false;
        parsed = static_cast<uint64_t>(signed_value);
    } else {
        return false;
    }
    if (parsed > UINT32_MAX) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

std::shared_ptr<Bridge> shared_bridge_handle();

int32_t register_kernel_map_commands(
    sao::ai_editor::native::ExtensionHost& host,
    const std::shared_ptr<Bridge>& bridge);

int32_t unregister_kernel_map_commands(
    sao::ai_editor::native::ExtensionHost& host);

void abandon_kernel_map_commands(
    sao::ai_editor::native::ExtensionHost& host);

int32_t handle_kernel_map_command(std::string_view command_id,
                                   const nlohmann::json& args,
                                   nlohmann::json& out);

int32_t dispatch_kernel_map_command(Bridge& bridge,
                                    std::string_view command_id,
                                    const nlohmann::json& args,
                                    nlohmann::json& out);

}  // namespace sao::ai_editor::kernel_map
