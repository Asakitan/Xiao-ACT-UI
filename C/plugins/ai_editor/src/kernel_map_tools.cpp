// SAO AI Editor - kernel-map tool-registry wiring implementation.
//
// TODO(kernel_map): NativeToolRegistry has no custom-execute seam yet;
// see kernel_map_tools.h for the full rationale.  The four
// registrations below still fire so the tools appear in tools/list;
// the command-palette wiring in kernel_map_commands.* is the
// operator-executable path in the meantime.

#include "kernel_map_tools.h"

#include <mutex>
#include <string_view>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "native_tool_registry.h"

namespace sao::ai_editor::kernel_map {

namespace {

using Json = nlohmann::json;

// Guards the idempotent registration so a caller that wires the
// runtime twice (test isolation, unit-test constructor re-runs)
// never sees a spurious INVALID_ARGUMENT from the underlying
// register_custom path.  register_custom itself is documented as
// "re-registering the same custom name updates the descriptor" so
// double-registration would already be OK on the happy path; the
// mutex just makes the "already there" case observable to the
// caller without racing another thread.
std::mutex& registration_mutex() {
    static std::mutex m;
    return m;
}

Json empty_object_schema() {
    return Json{{"type", "object"}, {"properties", Json::object()}};
}

}  // namespace

void register_kernel_map_tools(sao::ai_editor::native::NativeToolRegistry& registry,
                                Bridge& bridge) {
    // The Bridge reference is intentionally captured but not stored -
    // the registry has no custom-exec hook to hand it to (see the
    // header TODO), and holding a reference here would just be dead
    // weight.  The commands module owns the executable path.
    (void)bridge;

    std::lock_guard<std::mutex> guard(registration_mutex());

    // kernelMap.status - read-only, no args.
    (void)registry.register_custom(
        std::string_view("kernelMap.status"),
        std::string_view("Report whether the kernel map adapter is active "
                         "and how many drivers are currently mapped."),
        empty_object_schema(),
        /*read_only=*/true);

    // kernelMap.enumerate - read-only, no args.
    (void)registry.register_custom(
        std::string_view("kernelMap.enumerate"),
        std::string_view("List kernel virtual addresses of currently mapped "
                         "driver images."),
        empty_object_schema(),
        /*read_only=*/true);

    // kernelMap.map - mutating.  driver_path is required; there is
    // deliberately NO flags field - the bridge always sets
    // NO_INVOKE_ENTRY.  The description spells that out so the AI has
    // no reason to look for an invoke toggle.
    {
        Json properties = Json::object();
        properties["driver_path"] = Json{
            {"type", "string"},
            {"description",
             "Absolute path to a PE32+ AMD64 driver file (.sys) that the "
             "runtime should read and hand to the mapper."},
        };
        Json schema{{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", Json::array({"driver_path"})}};
        (void)registry.register_custom(
            std::string_view("kernelMap.map"),
            std::string_view(
                "Load a driver PE into non-paged pool via the manual "
                "mapper.  The DriverEntry callback is never invoked; the "
                "image is only projected, relocated, and imports "
                "resolved.  Use kernelMap.unmap to free the allocation."),
            schema,
            /*read_only=*/false);
    }

    // kernelMap.unmap - mutating.  target_base is required.  Accept
    // both decimal + 0x-hex string on the description; the command
    // handler does the parsing (the tool schema surface is
    // declarative and cannot express the union).
    {
        Json properties = Json::object();
        properties["target_base"] = Json{
            {"type", "string"},
            {"description",
             "Kernel VA as decimal or 0x-prefixed hex."},
        };
        Json schema{{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", Json::array({"target_base"})}};
        (void)registry.register_custom(
            std::string_view("kernelMap.unmap"),
            std::string_view(
                "Free a driver image previously loaded by kernelMap.map, "
                "using the base returned there."),
            schema,
            /*read_only=*/false);
    }

    // Lifecycle actions (activate / deactivate) are intentionally NOT
    // registered here; those live behind the sao.kernelMap.* command
    // IDs so the AI cannot toggle the ring0 channel mid-conversation.
}

}  // namespace sao::ai_editor::kernel_map
