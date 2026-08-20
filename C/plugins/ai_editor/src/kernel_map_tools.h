// SAO AI Editor - kernel-map tool-registry wiring.

#pragma once

#include <cstdint>
#include <memory>

namespace sao::ai_editor::native {
class NativeToolRegistry;
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::kernel_map {

class Bridge;

int32_t register_kernel_map_tools(
    sao::ai_editor::native::NativeToolRegistry& registry,
    const std::shared_ptr<Bridge>& bridge);

int32_t unregister_kernel_map_tools(
    sao::ai_editor::native::NativeToolRegistry& registry,
    const std::shared_ptr<Bridge>& bridge);

void abandon_kernel_map_tools(
    sao::ai_editor::native::NativeToolRegistry& registry);

#if defined(SAO_AI_EDITOR_TESTING)
void set_kernel_map_tool_failure_injection(
    int32_t register_fail_call, int32_t register_fail_count,
    int32_t unregister_fail_call, int32_t unregister_fail_count,
    int32_t failure_status);
void clear_kernel_map_tool_failure_injection();
#endif

}  // namespace sao::ai_editor::kernel_map
