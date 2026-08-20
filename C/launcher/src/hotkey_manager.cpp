#include "hotkey_manager.h"

#include "settings_owner_internal.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sao::launcher::hotkey {
namespace {

struct State {
    std::vector<HotkeyBinding> bindings;
    std::vector<HotkeyBinding> defaults;
    std::unordered_map<std::string, HotkeyCallback> callbacks;
    std::unordered_map<int, std::string> native_id_to_binding;
    std::unordered_map<std::string, int> binding_to_native_id;
    NativeHooks native_hooks;
    int next_native_id = 0x6B00;
    std::mutex mu;
    sao::launcher::settings_owner::SettingsOwner* owner = nullptr;
};

State& state() {
    static State value;
    return value;
}

std::string win32_error_text(uint32_t error) {
#if defined(_WIN32)
    char buffer[512]{};
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);
    std::string text = length == 0 ? "unknown error" : std::string(buffer, length);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
#else
    return "platform error " + std::to_string(error);
#endif
}

uint32_t last_error(const State& s) {
    if (s.native_hooks.last_error)
        return s.native_hooks.last_error();
#if defined(_WIN32)
    return GetLastError();
#else
    return 0;
#endif
}

bool native_register(State& s, int native_id, uint32_t modifiers, uint32_t vk,
                     uint32_t* out_error) {
    const bool success = s.native_hooks.register_hotkey
                             ? s.native_hooks.register_hotkey(native_id, modifiers, vk)
#if defined(_WIN32)
                             : RegisterHotKey(nullptr, native_id, modifiers, vk) != FALSE;
#else
                             : false;
#endif
    if (!success && out_error != nullptr)
        *out_error = last_error(s);
    return success;
}

bool native_unregister(State& s, int native_id, uint32_t* out_error) {
    const bool success = s.native_hooks.unregister_hotkey
                             ? s.native_hooks.unregister_hotkey(native_id)
#if defined(_WIN32)
                             : UnregisterHotKey(nullptr, native_id) != FALSE;
#else
                             : false;
#endif
    if (!success && out_error != nullptr)
        *out_error = last_error(s);
    return success;
}

std::string registration_error(const std::string& id, uint32_t error) {
    return id + ": RegisterHotKey failed (Win32 error " + std::to_string(error) + ": " +
           win32_error_text(error) + ")";
}

std::string unregistration_error(const std::string& id, uint32_t error) {
    return id + ": UnregisterHotKey failed (Win32 error " + std::to_string(error) + ": " +
           win32_error_text(error) + ")";
}

void erase_native_mapping(State& s, int native_id, const std::string& id) {
    const auto native_it = s.native_id_to_binding.find(native_id);
    if (native_it != s.native_id_to_binding.end() && native_it->second == id)
        s.native_id_to_binding.erase(native_it);
    const auto binding_it = s.binding_to_native_id.find(id);
    if (binding_it != s.binding_to_native_id.end() && binding_it->second == native_id)
        s.binding_to_native_id.erase(binding_it);
}

std::vector<std::string> unregister_native_locked(State& s) {
    std::vector<std::pair<int, std::string>> entries;
    entries.reserve(s.native_id_to_binding.size());
    for (const auto& [native_id, id] : s.native_id_to_binding)
        entries.emplace_back(native_id, id);

    std::vector<std::string> errors;
    for (const auto& [native_id, id] : entries) {
        uint32_t error = 0;
        if (native_unregister(s, native_id, &error)) {
            erase_native_mapping(s, native_id, id);
        } else {
            errors.push_back(unregistration_error(id, error));
        }
    }
    return errors;
}

bool save_locked(State& s) {
    if (s.owner == nullptr)
        return false;
    nlohmann::ordered_json values = nlohmann::ordered_json::object();
    for (const auto& binding : s.bindings) {
        values[binding.id] = nlohmann::ordered_json{
            {"description", binding.description},
            {"vk", binding.vk},
            {"mods", binding.modifiers},
        };
    }
    return s.owner->set_value_and_save("hotkeys", values) == SAO_STATUS_OK;
}

std::vector<std::string> conflicts_locked(const State& s, const std::string* changed_id,
                                          uint32_t vk, uint32_t modifiers) {
    std::vector<std::string> conflicts;
    for (const auto& binding : s.bindings) {
        if (changed_id != nullptr && binding.id == *changed_id)
            continue;
        if (binding.vk == vk && binding.modifiers == modifiers)
            conflicts.push_back(binding.id);
    }
    return conflicts;
}

} // namespace

extern "C" sao_status_t sao_launcher_hotkey_set_settings_owner(void* settings_owner_opaque) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.owner = reinterpret_cast<sao::launcher::settings_owner::SettingsOwner*>(
        settings_owner_opaque);
    return SAO_STATUS_OK;
}

void load_or_default(const std::vector<HotkeyBinding>& default_bindings) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    (void)unregister_native_locked(s);
    s.bindings = default_bindings;
    s.defaults = default_bindings;
    if (s.owner == nullptr)
        return;
    nlohmann::ordered_json section;
    if (s.owner->get_value("hotkeys", section) != SAO_STATUS_OK)
        return;

    auto apply_entry = [&](const std::string& id, const nlohmann::ordered_json& entry) {
        if (id.empty() || !entry.is_object())
            return;
        const uint32_t vk = entry.value("vk", 0u);
        const uint32_t modifiers = entry.contains("mods")
            ? entry.value("mods", 0u)
            : entry.value("modifiers", 0u);
        for (auto& binding : s.bindings) {
            if (binding.id == id) {
                binding.vk = vk;
                binding.modifiers = modifiers;
                break;
            }
        }
    };
    if (section.is_object()) {
        for (auto it = section.begin(); it != section.end(); ++it)
            apply_entry(it.key(), it.value());
    } else if (section.is_array()) {
        for (const auto& entry : section)
            apply_entry(entry.value("id", ""), entry);
    }
}

std::vector<std::string> register_all() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    std::vector<std::string> errors = unregister_native_locked(s);
    if (!errors.empty())
        return errors;
    std::unordered_map<uint64_t, std::string> seen;
    for (const auto& binding : s.bindings) {
        const uint64_t key = (uint64_t(binding.modifiers) << 32) | binding.vk;
        const auto found = seen.find(key);
        if (found != seen.end()) {
            errors.push_back(binding.id + " conflicts with " + found->second);
            continue;
        }
        seen.emplace(key, binding.id);
    }
    if (!errors.empty())
        return errors;

    std::vector<int> registered;
    for (const auto& binding : s.bindings) {
        const int native_id = ++s.next_native_id;
        uint32_t error = 0;
        if (!native_register(s, native_id, binding.modifiers, binding.vk, &error)) {
            errors.push_back(registration_error(binding.id, error));
            for (auto it = registered.rbegin(); it != registered.rend(); ++it) {
                const auto binding_it = s.native_id_to_binding.find(*it);
                if (binding_it == s.native_id_to_binding.end())
                    continue;
                const std::string registered_id = binding_it->second;
                uint32_t rollback_error = 0;
                if (native_unregister(s, *it, &rollback_error)) {
                    erase_native_mapping(s, *it, registered_id);
                } else {
                    errors.push_back(unregistration_error(registered_id, rollback_error));
                }
            }
            return errors;
        }
        registered.push_back(native_id);
        s.native_id_to_binding[native_id] = binding.id;
        s.binding_to_native_id[binding.id] = native_id;
    }
    return errors;
}

std::vector<std::string> unregister_all() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    return unregister_native_locked(s);
}

RebindResult rebind_live(const std::string& id, uint32_t new_vk, uint32_t new_modifiers,
                         std::string* out_reason) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    const auto binding_it = std::find_if(
        s.bindings.begin(), s.bindings.end(), [&](const HotkeyBinding& binding) {
            return binding.id == id;
        });
    if (binding_it == s.bindings.end()) {
        if (out_reason) *out_reason = "binding not found: " + id;
        return RebindResult::not_found;
    }

    const auto conflicts = conflicts_locked(s, &id, new_vk, new_modifiers);
    if (!conflicts.empty()) {
        if (out_reason) *out_reason = conflicts.front();
        return RebindResult::conflict;
    }

    const HotkeyBinding old_binding = *binding_it;
    const auto native_it = s.binding_to_native_id.find(id);
    const bool was_registered = native_it != s.binding_to_native_id.end();
    const int native_id = was_registered ? native_it->second : 0;
    if (was_registered) {
        uint32_t error = 0;
        if (!native_unregister(s, native_id, &error)) {
            if (out_reason) *out_reason = unregistration_error(id, error);
            return RebindResult::system_error;
        }
        erase_native_mapping(s, native_id, id);
    }

    uint32_t error = 0;
    if (was_registered && !native_register(s, native_id, new_modifiers, new_vk, &error)) {
        uint32_t rollback_error = 0;
        const bool rollback_ok = native_register(s, native_id, old_binding.modifiers,
                                                 old_binding.vk, &rollback_error);
        if (rollback_ok) {
            s.native_id_to_binding[native_id] = id;
            s.binding_to_native_id[id] = native_id;
        }
        if (out_reason) {
            *out_reason = registration_error(id, error);
            if (!rollback_ok)
                *out_reason += "; old binding rollback failed: " +
                               registration_error(id, rollback_error);
        }
        return rollback_ok ? RebindResult::system_error : RebindResult::rollback_error;
    }

    binding_it->vk = new_vk;
    binding_it->modifiers = new_modifiers;
    if (was_registered) {
        s.native_id_to_binding[native_id] = id;
        s.binding_to_native_id[id] = native_id;
    }

    if (save_locked(s))
        return RebindResult::success;

    if (!was_registered) {
        binding_it->vk = old_binding.vk;
        binding_it->modifiers = old_binding.modifiers;
        if (out_reason) *out_reason = "settings save failed; binding restored";
        return RebindResult::save_error;
    }

    uint32_t unregister_error = 0;
    if (!native_unregister(s, native_id, &unregister_error)) {
        if (out_reason)
            *out_reason = "settings save failed; new binding retained because unregister failed: " +
                          unregistration_error(id, unregister_error);
        return RebindResult::rollback_error;
    }
    erase_native_mapping(s, native_id, id);

    uint32_t rollback_error = 0;
    if (!native_register(s, native_id, old_binding.modifiers, old_binding.vk, &rollback_error)) {
        binding_it->vk = old_binding.vk;
        binding_it->modifiers = old_binding.modifiers;
        if (out_reason)
            *out_reason = "settings save failed; old binding restore failed: " +
                          registration_error(id, rollback_error);
        return RebindResult::rollback_error;
    }
    binding_it->vk = old_binding.vk;
    binding_it->modifiers = old_binding.modifiers;
    s.native_id_to_binding[native_id] = id;
    s.binding_to_native_id[id] = native_id;
    if (out_reason) *out_reason = "settings save failed; binding restored";
    return RebindResult::save_error;
}
RebindResult reset_to_default(const std::string& id, std::string* out_reason) {
    auto& s = state();
    std::optional<HotkeyBinding> default_binding;
    {
        std::lock_guard lock(s.mu);
        for (const auto& binding : s.defaults) {
            if (binding.id == id) {
                default_binding = binding;
                break;
            }
        }
    }
    if (!default_binding.has_value()) {
        if (out_reason) *out_reason = "default binding not found: " + id;
        return RebindResult::not_found;
    }
    return rebind_live(id, default_binding->vk, default_binding->modifiers, out_reason);
}

bool rebind(const std::string& id, uint32_t new_vk, uint32_t new_modifiers,
            std::string* out_conflict_reason) {
    std::string reason;
    const RebindResult result = rebind_live(id, new_vk, new_modifiers, &reason);
    if (result == RebindResult::conflict && out_conflict_reason)
        *out_conflict_reason = reason;
    return result == RebindResult::success;
}

void set_native_hooks_for_testing(NativeHooks hooks) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.native_hooks = std::move(hooks);
}

void clear_native_hooks_for_testing() {
    set_native_hooks_for_testing({});
}

std::optional<HotkeyBinding> query_binding(const std::string& id) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    for (const auto& binding : s.bindings) {
        if (binding.id == id)
            return binding;
    }
    return std::nullopt;
}

const HotkeyBinding* find_binding(const std::string& id) {
    thread_local std::optional<HotkeyBinding> copy;
    copy = query_binding(id);
    return copy.has_value() ? &copy.value() : nullptr;
}

void set_callback(const std::string& id, HotkeyCallback cb) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.callbacks[id] = std::move(cb);
}

void clear_callbacks() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.callbacks.clear();
}

bool dispatch_by_native_id(int native_hotkey_id) {
    auto& s = state();
    std::unique_lock lock(s.mu);
    const auto binding_it = s.native_id_to_binding.find(native_hotkey_id);
    if (binding_it == s.native_id_to_binding.end())
        return false;
    const auto callback_it = s.callbacks.find(binding_it->second);
    if (callback_it == s.callbacks.end())
        return false;
    HotkeyCallback callback = callback_it->second;
    lock.unlock();
    if (callback)
        callback();
    return true;
}

bool save() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    return save_locked(s);
}

std::vector<HotkeyBinding> snapshot() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    return s.bindings;
}

} // namespace sao::launcher::hotkey