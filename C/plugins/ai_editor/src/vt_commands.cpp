// SAO AI Editor - VT command-palette wiring implementation.

#include "vt_commands.h"

#include "extension_host.h"
#include "vt_bridge.h"

#include "sao/ai_editor/ai_editor_status.h"

#include <array>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sao::ai_editor::vt {
namespace {

using Json = nlohmann::json;
using Host = sao::ai_editor::native::ExtensionHost;
using Registration = Host::NativeCommandRegistration;

struct CommandRegistrationState final {
    Host* host = nullptr;
    uint64_t owner = 0;
    std::shared_ptr<Bridge> bridge;
    std::array<std::optional<Registration>, 8> rollback_snapshot{};
    bool rollback_pending = false;
};

constexpr std::array<std::string_view, 8> kCommandIds{
    "sao.vt.status", "sao.vt.capabilities", "sao.vt.probe",
    "sao.vt.hookPage", "sao.vt.hideRegion", "sao.vt.unhook",
    "sao.vt.readPhys", "sao.vt.writePhys"};

using CommandSnapshot = std::array<std::optional<Registration>, kCommandIds.size()>;

std::mutex& command_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<Host*, uint64_t>& command_owners() {
    static std::unordered_map<Host*, uint64_t> owners;
    return owners;
}

std::unordered_map<Host*, CommandRegistrationState>& command_states() {
    static std::unordered_map<Host*, CommandRegistrationState> states;
    return states;
}

uint64_t next_owner() {
    static uint64_t value = 1;
    return value++;
}

bool snapshot_is_empty(const CommandSnapshot& snapshot) noexcept {
    for (const auto& prior : snapshot) {
        if (prior.has_value()) return false;
    }
    return true;
}

int32_t restore_command_snapshot(Host& host, uint64_t owner,
                                 const CommandSnapshot& snapshot) noexcept {
    try {
        int32_t first_error = SAO_AI_EDITOR_OK;
        for (size_t index = 0; index < kCommandIds.size(); ++index) {
            const auto current = host.snapshot_native_command(kCommandIds[index]);
            if (snapshot[index].has_value() && current.has_value() &&
                current->owner == snapshot[index]->owner &&
                current->owner != owner) {
                continue;
            }
            const int32_t status = host.restore_native_command(
                kCommandIds[index], snapshot[index], owner);
            if (status != SAO_AI_EDITOR_OK && first_error == SAO_AI_EDITOR_OK)
                first_error = status;
        }
        return first_error;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t registration_exception_status() noexcept {
    return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
}

}  // namespace

int32_t register_vt_commands(Host& host,
                             const std::shared_ptr<Bridge>& bridge) noexcept {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;

    try {
        std::lock_guard<std::mutex> guard(command_mutex());
        auto& states = command_states();
        auto& owners = command_owners();
        auto found = states.find(&host);
        if (found != states.end() && found->second.bridge.get() != bridge.get())
            return SAO_AI_EDITOR_ERR_BUSY;
        if (found != states.end() && found->second.rollback_pending) {
            const int32_t recovery_status = restore_command_snapshot(
                host, found->second.owner, found->second.rollback_snapshot);
            if (recovery_status != SAO_AI_EDITOR_OK) return recovery_status;
            if (snapshot_is_empty(found->second.rollback_snapshot)) {
                owners.erase(&host);
                states.erase(found);
                found = states.end();
            } else {
                found->second.rollback_pending = false;
            }
        }

        auto owner_entry = owners.find(&host);
        if (owner_entry == owners.end())
            owner_entry = owners.emplace(&host, next_owner()).first;
        const uint64_t owner = owner_entry->second;

        CommandSnapshot prior{};
        for (size_t index = 0; index < kCommandIds.size(); ++index)
            prior[index] = host.snapshot_native_command(kCommandIds[index]);

        int32_t registration_status = SAO_AI_EDITOR_OK;
        try {
            for (const auto command_id : kCommandIds) {
                const std::string id(command_id);
                registration_status = host.register_native_command(
                    id,
                    [bridge, id](const Json& args, Json& out) {
                        return dispatch_vt_command(*bridge, id, args, out);
                    },
                    owner);
                if (registration_status != SAO_AI_EDITOR_OK) break;
            }
        } catch (...) {
            registration_status = registration_exception_status();
        }

        if (registration_status != SAO_AI_EDITOR_OK) {
            const int32_t rollback_status =
                restore_command_snapshot(host, owner, prior);
            if (rollback_status != SAO_AI_EDITOR_OK) {
                const std::shared_ptr<Bridge> recovery_bridge =
                    found != states.end() ? found->second.bridge : bridge;
                states[&host] = CommandRegistrationState{
                    &host, owner, recovery_bridge, prior, true};
                owners[&host] = owner;
                return rollback_status;
            }
            if (found == states.end()) owners.erase(&host);
            return registration_status;
        }

        try {
            states[&host] = CommandRegistrationState{&host, owner, bridge, {}, false};
        } catch (...) {
            const int32_t rollback_status =
                restore_command_snapshot(host, owner, prior);
            if (rollback_status != SAO_AI_EDITOR_OK) {
                states[&host] = CommandRegistrationState{
                    &host, owner, bridge, prior, true};
                owners[&host] = owner;
                return rollback_status;
            }
            if (found == states.end()) owners.erase(&host);
            return registration_exception_status();
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return registration_exception_status();
    }
}

int32_t unregister_vt_commands(Host& host) noexcept {
    try {
        std::lock_guard<std::mutex> guard(command_mutex());
        auto& states = command_states();
        auto& owners = command_owners();
        auto state_entry = states.find(&host);
        const auto owner_entry = owners.find(&host);
        if (owner_entry == owners.end()) return SAO_AI_EDITOR_ERR_NOT_FOUND;

        const uint64_t owner = owner_entry->second;
        if (state_entry != states.end() && state_entry->second.rollback_pending) {
            const int32_t recovery_status = restore_command_snapshot(
                host, owner, state_entry->second.rollback_snapshot);
            if (recovery_status != SAO_AI_EDITOR_OK) return recovery_status;
            if (snapshot_is_empty(state_entry->second.rollback_snapshot)) {
                states.erase(state_entry);
                owners.erase(owner_entry);
                return SAO_AI_EDITOR_OK;
            }
            state_entry->second.rollback_pending = false;
        }

        int32_t first_error = SAO_AI_EDITOR_OK;
        for (const auto command_id : kCommandIds) {
            const int32_t status = host.unregister_native_command(command_id, owner);
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
    } catch (...) {
        return registration_exception_status();
    }
}

void abandon_vt_commands(Host& host) noexcept {
    try {
        std::lock_guard<std::mutex> guard(command_mutex());
        command_states().erase(&host);
        command_owners().erase(&host);
    } catch (...) {
    }
}

int32_t dispatch_vt_command(Bridge& bridge, std::string_view command_id,
                            const Json& args, Json& out) {
    try {
        if (command_id == "sao.vt.status")
            return bridge.execute("vt.status", args, out);
        if (command_id == "sao.vt.capabilities")
            return bridge.execute("vt.capabilities", args, out);
        if (command_id == "sao.vt.probe")
            return bridge.execute("vt.probe", args, out);
        if (command_id == "sao.vt.hookPage")
            return bridge.execute("vt.hookPage", args, out);
        if (command_id == "sao.vt.hideRegion")
            return bridge.execute("vt.hideRegion", args, out);
        if (command_id == "sao.vt.unhook")
            return bridge.execute("vt.unhook", args, out);
        if (command_id == "sao.vt.readPhys")
            return bridge.execute("vt.readPhys", args, out);
        if (command_id == "sao.vt.writePhys")
            return bridge.execute("vt.writePhys", args, out);
        out = Json{{"ok", false},
                   {"available", false},
                   {"statusCode", SAO_AI_EDITOR_ERR_NOT_FOUND},
                   {"reason", "unknown_command"},
                   {"unknown", true},
                   {"partial", false}};
        return SAO_AI_EDITOR_OK;
    } catch (const std::exception& error) {
        out = Json{{"ok", false}, {"error", error.what()}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        out = Json{{"ok", false}, {"error", "VT command failed"}};
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

}  // namespace sao::ai_editor::vt
