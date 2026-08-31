#pragma once

#include <cstdint>
#include <memory>

namespace sao::ai_editor::native {
class NativeToolRegistry;
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::vt {

class Bridge;

int32_t register_vt_tools(
    sao::ai_editor::native::NativeToolRegistry& registry,
    const std::shared_ptr<Bridge>& bridge) noexcept;

int32_t unregister_vt_tools(
    sao::ai_editor::native::NativeToolRegistry& registry,
    const std::shared_ptr<Bridge>& bridge) noexcept;

void abandon_vt_tools(sao::ai_editor::native::NativeToolRegistry& registry) noexcept;

}  // namespace sao::ai_editor::vt
