// SAO AI Editor - kernel-map command-palette wiring implementation.
//
// TODO(kernel_map): ExtensionHost has no C++-side "register command
// handler" seam; see kernel_map_commands.h for the full rationale.
// register_kernel_map_commands() records the association in a module
// static so a Node-side shim (or a future seam) can hand a command
// invocation to handle_kernel_map_command() and route to the Bridge.
// This is the operator-executable path for driving the kernel_map
// wire from the AI editor host today.

#include "kernel_map_commands.h"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <ios>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "sao/rt_io/kernel_map_wire/wire.h"
#include "extension_host.h"

namespace sao::ai_editor::kernel_map {

namespace {

using Json = nlohmann::json;

// Guard the "installed" state so a caller that wires the runtime
// twice does not race the module-local bridge pointer.
std::mutex& command_mutex() {
    static std::mutex m;
    return m;
}

// The module-local bridge pointer resolved by
// register_kernel_map_commands.  handle_kernel_map_command() falls
// back to shared_bridge() when this is null so tests that skip the
// registration step still land on a working bridge.
Bridge*& registered_bridge_slot() {
    static Bridge* slot = nullptr;
    return slot;
}

Bridge& resolve_bridge() {
    std::lock_guard<std::mutex> guard(command_mutex());
    if (registered_bridge_slot() != nullptr) {
        return *registered_bridge_slot();
    }
    return shared_bridge();
}

std::string format_hex_u64(uint64_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::nouppercase << v;
    return oss.str();
}

// Parse a numeric string accepting both `0x`-prefixed hex and plain
// decimal.  Returns true on success and writes the result to `out`.
// Empty inputs / trailing junk are rejected so `"0x123abc oops"` does
// not silently become 0x123abc.
bool parse_u64(std::string_view text, uint64_t& out) {
    out = 0;
    if (text.empty()) {
        return false;
    }
    std::string_view remainder = text;
    int base = 10;
    if (remainder.size() >= 2 &&
        remainder[0] == '0' &&
        (remainder[1] == 'x' || remainder[1] == 'X')) {
        base = 16;
        remainder.remove_prefix(2);
    }
    if (remainder.empty()) {
        return false;
    }
    uint64_t value = 0;
    const auto* first = remainder.data();
    const auto* last = remainder.data() + remainder.size();
    const auto result = std::from_chars(first, last, value, base);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

// Read a file at `path` into memory.  Rejects anything larger than
// the wire-layer's 32 MiB cap before allocation (matches the cap
// enforced inside Bridge::map).  On success returns SAO_AI_EDITOR_OK
// and moves the bytes into `out`.
int32_t read_driver_file(std::string_view path, std::vector<uint8_t>& out) {
    out.clear();
    std::ifstream stream{std::string(path),
                          std::ios::binary | std::ios::ate};
    if (!stream) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const std::streamsize length = stream.tellg();
    if (length < 0) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (static_cast<uint64_t>(length) >
        static_cast<uint64_t>(SAO_RT_IO_KMOP_MAX_DRIVER_BYTES)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(length));
    if (length != 0 &&
        !stream.read(reinterpret_cast<char*>(out.data()), length)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_activate(const Json& args, Json& out) {
    // Every field is optional so an operator can activate with sane
    // defaults from the command palette without typing JSON.  The
    // proxy layer will surface NOT_INITIALIZED if the slot VA is 0,
    // which is desirable in a stock build.
    const uint64_t slot_va = args.value(
        "invoke_result_slot_va",
        static_cast<uint64_t>(0));
    const uint32_t timeout_ms = args.value(
        "idle_timeout_ms", static_cast<uint32_t>(60000));
    const uint64_t seed = args.value(
        "pool_tag_seed", static_cast<uint64_t>(0));
    const int32_t status =
        resolve_bridge().activate(slot_va, timeout_ms, seed);
    out = Json{{"ok", status == SAO_AI_EDITOR_OK},
               {"status", status}};
    return status;
}

int32_t cmd_deactivate(const Json&, Json& out) {
    const int32_t status = resolve_bridge().deactivate();
    out = Json{{"ok", status == SAO_AI_EDITOR_OK},
               {"status", status}};
    return status;
}

int32_t cmd_status(const Json&, Json& out) {
    BridgeStatus snap{};
    const int32_t status = resolve_bridge().status(snap);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    out = Json{{"ok", true},
               {"active", snap.active},
               {"map_count", snap.map_count}};
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_map(const Json& args, Json& out) {
    if (!args.is_object() ||
        !args.contains("driver_path") ||
        !args["driver_path"].is_string()) {
        out = Json{{"ok", false},
                   {"error", "driver_path (string) is required"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string driver_path =
        args["driver_path"].get<std::string>();
    std::vector<uint8_t> bytes;
    const int32_t read_status = read_driver_file(driver_path, bytes);
    if (read_status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false},
                   {"error", "failed to read driver file"},
                   {"status", read_status}};
        return read_status;
    }
    if (bytes.empty()) {
        out = Json{{"ok", false},
                   {"error", "driver file is empty"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    MapResult result{};
    const int32_t status = resolve_bridge().map(
        bytes.data(), static_cast<uint32_t>(bytes.size()), result);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    out = Json{{"ok", true},
               {"target_base_hex", format_hex_u64(result.target_base)},
               {"entry_status", result.entry_status}};
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_unmap(const Json& args, Json& out) {
    if (!args.is_object() ||
        !args.contains("target_base")) {
        out = Json{{"ok", false},
                   {"error", "target_base is required"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    uint64_t base = 0;
    // Accept either a JSON number (safe for values <= 2^53) or a
    // decimal / hex string.  A stringified value is required for
    // full-range 64-bit addresses because JS number precision only
    // covers ~53 bits.
    const auto& field = args["target_base"];
    if (field.is_string()) {
        if (!parse_u64(field.get_ref<const std::string&>(), base)) {
            out = Json{{"ok", false},
                       {"error", "target_base could not be parsed"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    } else if (field.is_number_unsigned()) {
        base = field.get<uint64_t>();
    } else if (field.is_number_integer()) {
        const int64_t signed_base = field.get<int64_t>();
        if (signed_base <= 0) {
            out = Json{{"ok", false},
                       {"error", "target_base must be positive"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        base = static_cast<uint64_t>(signed_base);
    } else {
        out = Json{{"ok", false},
                   {"error", "target_base must be string or number"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (base == 0) {
        out = Json{{"ok", false},
                   {"error", "target_base must not be zero"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = resolve_bridge().unmap(base);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    out = Json::object();
    out["ok"] = true;
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_enumerate(const Json&, Json& out) {
    std::vector<uint64_t> bases;
    const int32_t status = resolve_bridge().enumerate(bases);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    Json arr = Json::array();
    for (const auto& base : bases) {
        arr.push_back(format_hex_u64(base));
    }
    out = Json{{"ok", true}, {"bases", std::move(arr)}};
    return SAO_AI_EDITOR_OK;
}

}  // namespace

void register_kernel_map_commands(sao::ai_editor::native::ExtensionHost& host,
                                   Bridge& bridge) {
    // ExtensionHost has no C++ command sink today (see header TODO);
    // reference the parameter to keep the signature intentional and
    // record the bridge so handle_kernel_map_command() can find it
    // without going through shared_bridge().
    (void)host;
    std::lock_guard<std::mutex> guard(command_mutex());
    registered_bridge_slot() = &bridge;
}

int32_t handle_kernel_map_command(std::string_view command_id,
                                   const Json& args,
                                   Json& out) {
    if (command_id == "sao.kernelMap.activate") {
        return cmd_activate(args, out);
    }
    if (command_id == "sao.kernelMap.deactivate") {
        return cmd_deactivate(args, out);
    }
    if (command_id == "sao.kernelMap.status") {
        return cmd_status(args, out);
    }
    if (command_id == "sao.kernelMap.map") {
        return cmd_map(args, out);
    }
    if (command_id == "sao.kernelMap.unmap") {
        return cmd_unmap(args, out);
    }
    if (command_id == "sao.kernelMap.enumerate") {
        return cmd_enumerate(args, out);
    }
    out = Json{{"ok", false},
               {"error", "unknown kernelMap command"}};
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

}  // namespace sao::ai_editor::kernel_map
