// SAO AI Editor - kernel-map command-palette wiring implementation.

#include "kernel_map_commands.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "sao/rt_io/kernel_map_wire/wire.h"
#include "extension_host.h"

namespace sao::ai_editor::kernel_map {
namespace {
using Json = nlohmann::json;

struct CommandRegistrationState final {
    sao::ai_editor::native::ExtensionHost* host = nullptr;
    uint64_t owner = 0;
    std::shared_ptr<Bridge> bridge;
    std::array<std::optional<
                   sao::ai_editor::native::ExtensionHost::NativeCommandRegistration>,
               6>
        rollback_snapshot{};
    bool rollback_pending = false;
};

std::mutex& command_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<sao::ai_editor::native::ExtensionHost*, uint64_t>&
command_owners() {
    static std::unordered_map<sao::ai_editor::native::ExtensionHost*, uint64_t>
        owners;
    return owners;
}

std::unordered_map<sao::ai_editor::native::ExtensionHost*,
                   CommandRegistrationState>&
command_states() {
    static std::unordered_map<sao::ai_editor::native::ExtensionHost*,
                              CommandRegistrationState>
        states;
    return states;
}

uint64_t next_owner() {
    static uint64_t value = 1;
    return value++;
}

std::shared_ptr<Bridge> resolve_bridge() {
    std::lock_guard<std::mutex> guard(command_mutex());
    for (const auto& [host, state] : command_states()) {
        (void)host;
        if (state.bridge != nullptr) {
            return state.bridge;
        }
    }
    return shared_bridge_handle();
}

std::string format_hex_u64(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::nouppercase << value;
    return stream.str();
}

bool require_object(const Json& args, Json& out) {
    if (args.is_object()) {
        return true;
    }
    out = Json{{"ok", false}, {"error", "arguments must be an object"}};
    return false;
}

bool parse_uint64_field(const Json& args, std::string_view name,
                        uint64_t& out, Json& reply) {
    const std::string key(name);
    if (!args.contains(key)) {
        return true;
    }
    if (!parse_kernel_map_uint64(args[key], out)) {
        reply = Json{{"ok", false},
                     {"error", key +
                                  " must be a decimal or 0x-prefixed hexadecimal string"}};
        return false;
    }
    return true;
}

bool parse_uint32_field(const Json& args, std::string_view name,
                        uint32_t& out, Json& reply) {
    const std::string key(name);
    if (!args.contains(key)) {
        return true;
    }
    if (!parse_kernel_map_uint32(args[key], out)) {
        reply = Json{{"ok", false},
                     {"error", key + " must be a non-negative uint32 integer"}};
        return false;
    }
    return true;
}

int32_t read_driver_file(std::string_view path, std::vector<uint8_t>& out) {
    out.clear();
    std::ifstream stream(std::string(path), std::ios::binary | std::ios::ate);
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

int32_t cmd_activate(Bridge& bridge, const Json& args, Json& out) {
    if (!require_object(args, out)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    uint64_t slot_va = 0;
    uint64_t seed = 0;
    uint32_t timeout_ms = 60000;
    if (!parse_uint64_field(args, "invoke_result_slot_va", slot_va, out) ||
        !parse_uint64_field(args, "pool_tag_seed", seed, out) ||
        !parse_uint32_field(args, "idle_timeout_ms", timeout_ms, out)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = bridge.activate(slot_va, timeout_ms, seed);
    out = Json{{"ok", status == SAO_AI_EDITOR_OK}, {"status", status}};
    return status;
}

int32_t cmd_deactivate(Bridge& bridge, const Json& args, Json& out) {
    if (!require_object(args, out)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = bridge.deactivate();
    out = Json{{"ok", status == SAO_AI_EDITOR_OK}, {"status", status}};
    return status;
}

int32_t cmd_status(Bridge& bridge, const Json& args, Json& out) {
    if (!require_object(args, out)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    BridgeStatus snapshot{};
    const int32_t status = bridge.status(snapshot);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    out = Json{{"ok", true}, {"active", snapshot.active},
               {"map_count", snapshot.map_count}};
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_map(Bridge& bridge, const Json& args, Json& out) {
    if (!args.is_object() || !args.contains("driver_path") ||
        !args["driver_path"].is_string()) {
        out = Json{{"ok", false},
                   {"error", "driver_path (string) is required"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string path = args["driver_path"].get<std::string>();
    std::vector<uint8_t> bytes;
    const int32_t read_status = read_driver_file(path, bytes);
    if (read_status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"error", "failed to read driver file"},
                   {"status", read_status}};
        return read_status;
    }
    if (bytes.empty()) {
        out = Json{{"ok", false}, {"error", "driver file is empty"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    MapResult result{};
    const int32_t status = bridge.map(bytes.data(),
                                      static_cast<uint32_t>(bytes.size()),
                                      result);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    out = Json{{"ok", true},
               {"target_base_hex", format_hex_u64(result.target_base)},
               {"entry_status", result.entry_status}};
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_unmap(Bridge& bridge, const Json& args, Json& out) {
    if (!args.is_object() || !args.contains("target_base")) {
        out = Json{{"ok", false}, {"error", "target_base is required"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    uint64_t base = 0;
    if (!parse_kernel_map_uint64(args["target_base"], base) || base == 0) {
        out = Json{{"ok", false},
                   {"error", "target_base must be a non-zero decimal or 0x-prefixed hexadecimal string"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = bridge.unmap(base);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    out = Json{{"ok", true}};
    return SAO_AI_EDITOR_OK;
}

int32_t cmd_enumerate(Bridge& bridge, const Json& args, Json& out) {
    if (!require_object(args, out)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::vector<uint64_t> bases;
    const int32_t status = bridge.enumerate(bases);
    if (status != SAO_AI_EDITOR_OK) {
        out = Json{{"ok", false}, {"status", status}};
        return status;
    }
    Json values = Json::array();
    for (const uint64_t base : bases) {
        values.push_back(format_hex_u64(base));
    }
    out = Json{{"ok", true}, {"bases", std::move(values)}};
    return SAO_AI_EDITOR_OK;
}

constexpr std::array<std::string_view, 6> kCommandIds{
    "sao.kernelMap.activate", "sao.kernelMap.deactivate",
    "sao.kernelMap.status", "sao.kernelMap.map", "sao.kernelMap.unmap",
    "sao.kernelMap.enumerate"};

using CommandSnapshot = std::array<
    std::optional<sao::ai_editor::native::ExtensionHost::NativeCommandRegistration>,
    kCommandIds.size()>;

int32_t restore_command_snapshot(
    sao::ai_editor::native::ExtensionHost& host, uint64_t owner,
    const CommandSnapshot& snapshot) {
    int32_t first_error = SAO_AI_EDITOR_OK;
    for (size_t i = 0; i < kCommandIds.size(); ++i) {
        const auto current = host.snapshot_native_command(kCommandIds[i]);
        if (snapshot[i].has_value() && current.has_value() &&
            current->owner == snapshot[i]->owner &&
            current->owner != owner) {
            // Registration failed before touching this foreign-owned slot;
            // it already equals the prior ownership state and needs no
            // transaction-owner restore operation.
            continue;
        }
        const int32_t status = host.restore_native_command(
            kCommandIds[i], snapshot[i], owner);
        if (status != SAO_AI_EDITOR_OK && first_error == SAO_AI_EDITOR_OK) {
            first_error = status;
        }
    }
    return first_error;
}

bool snapshot_is_empty(const CommandSnapshot& snapshot) {
    for (const auto& prior : snapshot) {
        if (prior.has_value()) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::shared_ptr<Bridge> shared_bridge_handle() {
    return std::shared_ptr<Bridge>(&shared_bridge(), [](Bridge*) {});
}

int32_t register_kernel_map_commands(
    sao::ai_editor::native::ExtensionHost& host,
    const std::shared_ptr<Bridge>& bridge) {
    if (bridge == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> guard(command_mutex());
    auto& states = command_states();
    auto& owners = command_owners();
    auto found = states.find(&host);
    if (found != states.end() && found->second.rollback_pending) {
        const int32_t recovery_status = restore_command_snapshot(
            host, found->second.owner, found->second.rollback_snapshot);
        if (recovery_status != SAO_AI_EDITOR_OK) {
            return recovery_status;
        }
        if (snapshot_is_empty(found->second.rollback_snapshot)) {
            owners.erase(&host);
            states.erase(found);
            found = states.end();
        } else {
            found->second.rollback_pending = false;
        }
    }
    auto owner_entry = owners.find(&host);
    if (owner_entry == owners.end()) {
        owner_entry = owners.emplace(&host, next_owner()).first;
    }
    const uint64_t owner = owner_entry->second;

    std::array<std::optional<sao::ai_editor::native::ExtensionHost::NativeCommandRegistration>,
               kCommandIds.size()> prior{};
    for (size_t i = 0; i < kCommandIds.size(); ++i) {
        prior[i] = host.snapshot_native_command(kCommandIds[i]);
    }

    size_t registered = 0;
    for (; registered < kCommandIds.size(); ++registered) {
        const std::string id(kCommandIds[registered]);
        const int32_t status = host.register_native_command(
            id,
            [bridge, id](const Json& args, Json& out) {
                return dispatch_kernel_map_command(*bridge, id, args, out);
            },
            owner);
        if (status != SAO_AI_EDITOR_OK) {
            const int32_t rollback_status = restore_command_snapshot(
                host, owner, prior);
            if (rollback_status != SAO_AI_EDITOR_OK) {
                const std::shared_ptr<Bridge> recovery_bridge =
                    found != states.end() ? found->second.bridge : bridge;
                states[&host] = CommandRegistrationState{
                    &host, owner, recovery_bridge, prior, true};
                owners[&host] = owner;
                return rollback_status;
            }
            if (found == states.end()) {
                owners.erase(&host);
            }
            return status;
        }
    }

    states[&host] = CommandRegistrationState{&host, owner, bridge, {}, false};
    return SAO_AI_EDITOR_OK;
}

int32_t unregister_kernel_map_commands(
    sao::ai_editor::native::ExtensionHost& host) {
    std::lock_guard<std::mutex> guard(command_mutex());
    auto& states = command_states();
    auto& owners = command_owners();
    auto state_entry = states.find(&host);
    const auto owner_entry = owners.find(&host);
    if (owner_entry == owners.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const uint64_t owner = owner_entry->second;
    if (state_entry != states.end() && state_entry->second.rollback_pending) {
        const int32_t recovery_status = restore_command_snapshot(
            host, owner, state_entry->second.rollback_snapshot);
        if (recovery_status != SAO_AI_EDITOR_OK) {
            return recovery_status;
        }
        if (snapshot_is_empty(state_entry->second.rollback_snapshot)) {
            states.erase(state_entry);
            owners.erase(owner_entry);
            return SAO_AI_EDITOR_OK;
        }
        state_entry->second.rollback_pending = false;
    }
    int32_t first_error = SAO_AI_EDITOR_OK;
    for (const auto id : kCommandIds) {
        const int32_t status = host.unregister_native_command(id, owner);
        if (status != SAO_AI_EDITOR_OK &&
            status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
            first_error == SAO_AI_EDITOR_OK) {
            first_error = status;
        }
    }
    if (first_error == SAO_AI_EDITOR_OK) {
        states.erase(&host);
        owners.erase(owner_entry);
    }
    return first_error;
}

void abandon_kernel_map_commands(
    sao::ai_editor::native::ExtensionHost& host) {
    std::lock_guard<std::mutex> guard(command_mutex());
    command_states().erase(&host);
    command_owners().erase(&host);
}

int32_t dispatch_kernel_map_command(Bridge& bridge,
                                    std::string_view command_id,
                                    const Json& args,
                                    Json& out) {
    try {
        if (command_id == "sao.kernelMap.activate") {
            return cmd_activate(bridge, args, out);
        }
        if (command_id == "sao.kernelMap.deactivate") {
            return cmd_deactivate(bridge, args, out);
        }
        if (command_id == "sao.kernelMap.status") {
            return cmd_status(bridge, args, out);
        }
        if (command_id == "sao.kernelMap.map") {
            return cmd_map(bridge, args, out);
        }
        if (command_id == "sao.kernelMap.unmap") {
            return cmd_unmap(bridge, args, out);
        }
        if (command_id == "sao.kernelMap.enumerate") {
            return cmd_enumerate(bridge, args, out);
        }
        out = Json{{"ok", false}, {"error", "unknown kernelMap command"}};
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    } catch (const std::exception& error) {
        out = Json{{"ok", false}, {"error", error.what()}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        out = Json{{"ok", false}, {"error", "kernelMap command failed"}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t handle_kernel_map_command(std::string_view command_id,
                                  const Json& args,
                                  Json& out) {
    try {
        const auto bridge = resolve_bridge();
        return dispatch_kernel_map_command(*bridge, command_id, args, out);
    } catch (const std::exception& error) {
        out = Json{{"ok", false}, {"error", error.what()}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        out = Json{{"ok", false}, {"error", "kernelMap command failed"}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

}  // namespace sao::ai_editor::kernel_map
