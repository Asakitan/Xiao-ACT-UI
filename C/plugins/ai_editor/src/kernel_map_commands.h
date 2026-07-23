// SAO AI Editor - kernel-map command-palette wiring.
//
// TODO(kernel_map): ExtensionHost::execute_command is a
// forward-to-Node-runtime pipe (see extension_host.cpp: `execute_command`
// packs command_id + args into a `commands.execute` JSON-RPC call and
// hands it to the shared NodeRuntime).  There is no C++ side "register
// command handler" sink today - every command is expected to be
// handled inside a Node extension that binds to it via
// commands.registerCommand.  As a result the C++ registration
// function below cannot install a real dispatch entry that the AI
// editor host will pick up on its own; the module exposes a
// handle_kernel_map_command() static so a future ExtensionHost
// enhancement can plug native commands in through a real seam, and
// the same helper can be called directly from a Node-side shim (see
// assets/extension_host_shim.js) as an interim path.  DO NOT modify
// ExtensionHost from here to add the seam; that is a shared-owner
// change.  Follow-up ticket needed.

#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::native {
class ExtensionHost;
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::kernel_map {

class Bridge;

// Register the sao.kernelMap.* command IDs with the extension host.
// Today this is a no-op stub that only records the registration in a
// module-local table so a shim reachable via
// handle_kernel_map_command() can dispatch to Bridge.  Idempotent -
// callers can re-run the registration cheaply.  See the header TODO
// for the seam gap.
void register_kernel_map_commands(sao::ai_editor::native::ExtensionHost& host,
                                   Bridge& bridge);

// Dispatch a sao.kernelMap.* command by id.  Returns the AI editor
// status code and fills `out` with a JSON-friendly reply.  Unknown
// command ids resolve to SAO_AI_EDITOR_ERR_NOT_FOUND without touching
// the bridge.  Made public so a Node-side shim (or a future
// ExtensionHost seam) can route into the native dispatcher without
// having to know the individual bridge entry points.
int32_t handle_kernel_map_command(std::string_view command_id,
                                   const nlohmann::json& args,
                                   nlohmann::json& out);

}  // namespace sao::ai_editor::kernel_map
