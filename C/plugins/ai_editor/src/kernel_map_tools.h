// SAO AI Editor - kernel-map tool-registry wiring.
//
// TODO(kernel_map): NativeToolRegistry has no custom-execute seam yet -
// register_custom() only records a descriptor and the built-in
// execute() dispatch echoes back the request payload (see
// native_tool_registry.cpp: the `is_custom` branch of execute() just
// wraps the arguments into `{custom, name, arguments}` and returns
// SAO_AI_EDITOR_OK).  As a result the four kernelMap.* entries below
// only surface via tools/list; a tools/call kernelMap.status
// invocation cannot reach the Bridge through the registry today.
// Follow-up ticket needed (peer agent owns NativeToolRegistry) - the
// registration functions here register the descriptors anyway so the
// tools appear in tools/list, and the command-palette wiring in
// kernel_map_commands.* is the executable path until the registry
// gains a real custom-exec hook.  DO NOT patch the registry to add a
// custom dispatch case from this side of the wall; that is out of
// scope and would trip the "no cross-owner modifications" rule.

#pragma once

namespace sao::ai_editor::native {
class NativeToolRegistry;
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::kernel_map {

class Bridge;

// Register the four kernelMap.* tools with `registry`.  Idempotent -
// re-registering the same tool name updates the descriptor in place
// (that is the built-in NativeToolRegistry::register_custom
// contract).  `bridge` is stored for future use once the registry
// grows a real custom-exec hook; today the descriptors are
// declarative-only.  The reference is captured by function pointer,
// not copied, so the caller keeps ownership.
void register_kernel_map_tools(sao::ai_editor::native::NativeToolRegistry& registry,
                                Bridge& bridge);

}  // namespace sao::ai_editor::kernel_map
