// SAO AI Editor - VT command-palette wiring.

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::native {
class ExtensionHost;
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::vt {

class Bridge;

int32_t register_vt_commands(
    sao::ai_editor::native::ExtensionHost& host,
    const std::shared_ptr<Bridge>& bridge) noexcept;

int32_t unregister_vt_commands(
    sao::ai_editor::native::ExtensionHost& host) noexcept;

void abandon_vt_commands(
    sao::ai_editor::native::ExtensionHost& host) noexcept;

int32_t dispatch_vt_command(Bridge& bridge,
                            std::string_view command_id,
                            const nlohmann::json& args,
                            nlohmann::json& out);

}  // namespace sao::ai_editor::vt
