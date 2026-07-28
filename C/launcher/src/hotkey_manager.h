// hotkey_manager.h — user-facing hotkey table with rebinding + conflict
// detection + persistence.
//
// Phase 12 (Python parity closure) — port of gui_modules/sao_hotkey_manager.py.
// Wraps RegisterHotKey; state persists to settings.json under "hotkeys" section.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sao::launcher::hotkey {

struct HotkeyBinding {
    std::string id;          // 稳定 ID, e.g. "menu.home"
    std::string description; // 人可读
    uint32_t vk = 0;         // Win32 virtual-key code
    uint32_t modifiers = 0;  // MOD_* bitmask
};

using HotkeyCallback = std::function<void()>;

// 从 settings.json 加载已有配置; 未设置的用默认 default_bindings。
void load_or_default(const std::vector<HotkeyBinding>& default_bindings);

// 注册所有 binding 到 Win32 RegisterHotKey。冲突时返回冲突列表。
std::vector<std::string> register_all();

// Unregister all — shutdown path。
void unregister_all();

// 更改某 binding, 回落到冲突检测。
bool rebind(const std::string& id, uint32_t new_vk, uint32_t new_modifiers,
            std::string* out_conflict_reason);

// 查询当前 binding。
const HotkeyBinding* find_binding(const std::string& id);

// 附着回调; 系统收到 WM_HOTKEY 后, launcher window proc 派发到这里。
void set_callback(const std::string& id, HotkeyCallback cb);
void dispatch_by_native_id(int native_hotkey_id);

// Save current table back to settings.json.
bool save();

// 列表 (config panel enum 用)。
std::vector<HotkeyBinding> snapshot();

} // namespace sao::launcher::hotkey
