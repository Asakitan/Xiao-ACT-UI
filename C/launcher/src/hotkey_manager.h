#pragma once

#include "sao/core/status.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

extern "C" sao_status_t sao_launcher_hotkey_set_settings_owner(void* settings_owner_opaque) noexcept;

namespace sao::launcher::hotkey {

struct HotkeyBinding {
    std::string id;
    std::string description;
    uint32_t vk = 0;
    uint32_t modifiers = 0;
};

using HotkeyCallback = std::function<void()>;

enum class RebindResult {
    success,
    not_found,
    conflict,
    system_error,
    save_error,
    rollback_error,
};

struct NativeHooks {
    std::function<bool(int, uint32_t, uint32_t)> register_hotkey;
    std::function<bool(int)> unregister_hotkey;
    std::function<uint32_t()> last_error;
};

void load_or_default(const std::vector<HotkeyBinding>& default_bindings);
std::vector<std::string> register_all();
std::vector<std::string> unregister_all();

bool rebind(const std::string& id, uint32_t new_vk, uint32_t new_modifiers,
            std::string* out_conflict_reason);
RebindResult rebind_live(const std::string& id, uint32_t new_vk, uint32_t new_modifiers,
                         std::string* out_reason);
RebindResult reset_to_default(const std::string& id, std::string* out_reason);

void set_native_hooks_for_testing(NativeHooks hooks);
void clear_native_hooks_for_testing();

std::optional<HotkeyBinding> query_binding(const std::string& id);
[[deprecated("use query_binding()")]] const HotkeyBinding* find_binding(const std::string& id);

void set_callback(const std::string& id, HotkeyCallback cb);
void clear_callbacks();
bool dispatch_by_native_id(int native_hotkey_id);

bool save();
std::vector<HotkeyBinding> snapshot();

} // namespace sao::launcher::hotkey