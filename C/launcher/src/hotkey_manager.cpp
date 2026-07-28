// hotkey_manager.cpp — Phase 12 (Python parity closure production).
//
// Persistence flows through the existing settings_owner "hotkeys" section.
// Conflict detection checks for duplicate (vk, modifiers) pairs across bindings.

#include "hotkey_manager.h"

#include "settings_owner_internal.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sao::launcher::hotkey {

namespace {
struct State {
    std::vector<HotkeyBinding> bindings;
    std::unordered_map<std::string, HotkeyCallback> callbacks;
    std::unordered_map<int, std::string> native_id_to_binding;
    int next_native_id = 0x5340;
    std::mutex mu;
    sao::launcher::settings_owner::SettingsOwner* owner = nullptr; // borrowed
};
State& state() {
    static State s;
    return s;
}
} // namespace

// Called by launcher init after the shared settings owner is constructed.
extern "C" void sao_launcher_hotkey_set_settings_owner(
    void* settings_owner_opaque) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.owner = reinterpret_cast<sao::launcher::settings_owner::SettingsOwner*>(
        settings_owner_opaque);
}

void load_or_default(const std::vector<HotkeyBinding>& default_bindings) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.bindings = default_bindings;
    if (s.owner == nullptr) return;
    nlohmann::ordered_json section;
    if (s.owner->get_value("hotkeys", section) != SAO_STATUS_OK) return;
    if (!section.is_array()) return;
    // Merge saved bindings onto defaults by id.
    for (const auto& entry : section) {
        if (!entry.is_object()) continue;
        std::string id = entry.value("id", "");
        if (id.empty()) continue;
        uint32_t vk = entry.value("vk", 0u);
        uint32_t mods = entry.value("mods", 0u);
        for (auto& b : s.bindings) {
            if (b.id == id) {
                b.vk = vk;
                b.modifiers = mods;
                break;
            }
        }
    }
}

std::vector<std::string> register_all() {
    std::vector<std::string> conflicts;
    auto& s = state();
    std::lock_guard lock(s.mu);
    // Conflict scan.
    std::unordered_map<uint64_t, std::string> seen;
    for (const auto& b : s.bindings) {
        uint64_t key = (uint64_t(b.modifiers) << 32) | b.vk;
        auto it = seen.find(key);
        if (it != seen.end()) {
            conflicts.push_back(b.id + " conflicts with " + it->second);
            continue;
        }
        seen[key] = b.id;
    }
    if (!conflicts.empty()) return conflicts;

#if defined(_WIN32)
    for (auto& b : s.bindings) {
        int native = ++s.next_native_id;
        if (RegisterHotKey(nullptr, native, b.modifiers, b.vk)) {
            s.native_id_to_binding[native] = b.id;
        }
    }
#endif
    return conflicts;
}

void unregister_all() {
    auto& s = state();
    std::lock_guard lock(s.mu);
#if defined(_WIN32)
    for (const auto& [native, _] : s.native_id_to_binding) {
        UnregisterHotKey(nullptr, native);
    }
#endif
    s.native_id_to_binding.clear();
}

bool rebind(const std::string& id, uint32_t new_vk, uint32_t new_modifiers,
            std::string* out_conflict_reason) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    for (const auto& b : s.bindings) {
        if (b.id == id) continue;
        if (b.vk == new_vk && b.modifiers == new_modifiers) {
            if (out_conflict_reason) *out_conflict_reason = b.id;
            return false;
        }
    }
    for (auto& b : s.bindings) {
        if (b.id == id) {
            b.vk = new_vk;
            b.modifiers = new_modifiers;
            return true;
        }
    }
    return false;
}

const HotkeyBinding* find_binding(const std::string& id) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    for (const auto& b : s.bindings) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

void set_callback(const std::string& id, HotkeyCallback cb) {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.callbacks[id] = std::move(cb);
}

void dispatch_by_native_id(int native_hotkey_id) {
    auto& s = state();
    std::unique_lock lock(s.mu);
    auto it = s.native_id_to_binding.find(native_hotkey_id);
    if (it == s.native_id_to_binding.end()) return;
    std::string id = it->second;
    auto cb_it = s.callbacks.find(id);
    if (cb_it == s.callbacks.end()) return;
    HotkeyCallback cb = cb_it->second;
    lock.unlock();
    if (cb) cb();
}

bool save() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    if (s.owner == nullptr) return false;
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const auto& b : s.bindings) {
        arr.push_back(nlohmann::ordered_json{
            {"id", b.id},
            {"description", b.description},
            {"vk", b.vk},
            {"mods", b.modifiers},
        });
    }
    return s.owner->set_value_and_save("hotkeys", arr) == SAO_STATUS_OK;
}

std::vector<HotkeyBinding> snapshot() {
    auto& s = state();
    std::lock_guard lock(s.mu);
    return s.bindings;
}

} // namespace sao::launcher::hotkey
