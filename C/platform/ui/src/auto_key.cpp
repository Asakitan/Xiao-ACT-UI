// SAO Auto — game-agnostic auto-key implementation.
//
// See `include/sao/ui/auto_key.h` for the ABI contract.  This slice is
// concerned only with the platform mechanics: SendInput batching, timed
// releases, thread-safe pending-hold tracker, and the arbitration gate.

#include "sao/ui/auto_key.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

// ─── Pending-hold ledger ──────────────────────────────────────────
//
// SendInput is atomic per call, but hold-then-release is two calls
// with a gap.  We queue the release on a lightweight worker thread so
// callers of `send_key` with `hold_ms > 0` don't block.  The ledger
// tracks outstanding holds so shutdown can drain them (no stuck keys).

struct HoldEntry {
    uint32_t                                    virtual_key;
    std::chrono::steady_clock::time_point       release_at;
    // 0 → single-key hold; otherwise a token linking this row to the
    // combo it belongs to (all releases in a combo happen atomically).
    uint64_t                                    combo_token;
};

class HoldWorker {
public:
    HoldWorker() = default;

    ~HoldWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        // Drain: release every outstanding key so we never leak a
        // stuck key on process exit.
        drain_all_holds();
    }

    // Schedule a single-key release.
    void schedule(uint32_t vk, uint32_t hold_ms) {
        auto release_at =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(hold_ms);
        std::lock_guard<std::mutex> lock(mutex_);
        holds_.push_back({vk, release_at, 0});
        ensure_worker_locked();
        cv_.notify_all();
    }

    // Schedule an atomic combo release: every vk in `vks` is released
    // in one SendInput batch at `release_at`.
    void schedule_combo(const std::vector<uint32_t>& vks, uint32_t hold_ms) {
        auto release_at =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(hold_ms);
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t token = ++next_combo_token_;
        for (uint32_t vk : vks) {
            holds_.push_back({vk, release_at, token});
        }
        ensure_worker_locked();
        cv_.notify_all();
    }

    size_t pending_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return holds_.size();
    }

private:
    void ensure_worker_locked() {
        if (worker_started_) return;
        worker_started_ = true;
        worker_ = std::thread([this] { worker_loop(); });
    }

    void worker_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!shutdown_) {
            if (holds_.empty()) {
                cv_.wait(lock, [this] {
                    return shutdown_ || !holds_.empty();
                });
                continue;
            }
            // Find next release time.
            auto next = holds_.front().release_at;
            for (auto& h : holds_) {
                if (h.release_at < next) next = h.release_at;
            }
            auto now = std::chrono::steady_clock::now();
            if (next > now) {
                cv_.wait_until(lock, next, [this, next] {
                    if (shutdown_) return true;
                    // Wake also if a new hold arrived that expires
                    // sooner than the current `next`.
                    for (auto& h : holds_) {
                        if (h.release_at < next) return true;
                    }
                    return false;
                });
                continue;
            }
            // Extract everything due, grouped by combo_token.
            std::unordered_map<uint64_t, std::vector<uint32_t>> due;
            std::vector<HoldEntry> remaining;
            remaining.reserve(holds_.size());
            for (auto& h : holds_) {
                if (h.release_at <= now) {
                    due[h.combo_token].push_back(h.virtual_key);
                } else {
                    remaining.push_back(h);
                }
            }
            holds_.swap(remaining);
            lock.unlock();
            // Fire outside the lock so a nested SendInput won't
            // deadlock against callers that hold the mutex.
            for (auto& kv : due) {
                emit_key_up_batch(kv.second);
            }
            lock.lock();
        }
    }

    void drain_all_holds() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t> vks;
        vks.reserve(holds_.size());
        for (auto& h : holds_) vks.push_back(h.virtual_key);
        holds_.clear();
        if (!vks.empty()) emit_key_up_batch(vks);
    }

    static void emit_key_up_batch(const std::vector<uint32_t>& vks) {
#if defined(_WIN32)
        if (vks.empty()) return;
        std::vector<INPUT> inputs(vks.size());
        std::memset(inputs.data(), 0, sizeof(INPUT) * inputs.size());
        for (size_t i = 0; i < vks.size(); ++i) {
            inputs[i].type = INPUT_KEYBOARD;
            inputs[i].ki.wVk = static_cast<WORD>(vks[i]);
            inputs[i].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
                  sizeof(INPUT));
#else
        (void)vks;
#endif
    }

    std::mutex                  mutex_;
    std::condition_variable     cv_;
    std::vector<HoldEntry>      holds_;
    std::thread                 worker_;
    bool                        worker_started_ = false;
    bool                        shutdown_       = false;
    uint64_t                    next_combo_token_ = 0;
};

HoldWorker& hold_worker() {
    // Meyers singleton — constructed lazily on first use.
    static HoldWorker instance;
    return instance;
}

// ─── SendInput helpers ────────────────────────────────────────────

// Standard modifier VK codes.
constexpr uint32_t kModCtrl  = 0x11;  // VK_CONTROL
constexpr uint32_t kModShift = 0x10;  // VK_SHIFT
constexpr uint32_t kModAlt   = 0x12;  // VK_MENU
constexpr uint32_t kModWin   = 0x5B;  // VK_LWIN

// Bit flags (must mirror sao_ui_hotkey_modifier_e semantics).
constexpr uint32_t kBitCtrl  = 1u << 0;
constexpr uint32_t kBitAlt   = 1u << 1;
constexpr uint32_t kBitShift = 1u << 2;
constexpr uint32_t kBitWin   = 1u << 3;
constexpr uint32_t kModifierMask = kBitCtrl | kBitAlt | kBitShift | kBitWin;

#if defined(_WIN32)
bool send_input_key_batch(const std::vector<uint32_t>& vks,
                          const std::vector<bool>&    is_up) {
    if (vks.empty()) return true;
    std::vector<INPUT> inputs(vks.size());
    std::memset(inputs.data(), 0, sizeof(INPUT) * inputs.size());
    for (size_t i = 0; i < vks.size(); ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = static_cast<WORD>(vks[i]);
        inputs[i].ki.dwFlags = is_up[i] ? KEYEVENTF_KEYUP : 0;
    }
    UINT sent = SendInput(static_cast<UINT>(inputs.size()),
                          inputs.data(), sizeof(INPUT));
    return sent == inputs.size();
}
#endif

}  // namespace

// ─── Public ABI ───────────────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_send_key(
    uint32_t virtual_key,
    uint32_t modifiers_mask,
    uint32_t hold_ms) {
#if defined(_WIN32)
    try {
    if (virtual_key == 0 || virtual_key > 0xFF || (modifiers_mask & ~kModifierMask) != 0U) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // Build the down batch: modifiers first, then main VK.
    std::vector<uint32_t> down_vks;
    std::vector<bool>     down_up;
    if (modifiers_mask & kBitCtrl)  { down_vks.push_back(kModCtrl);  down_up.push_back(false); }
    if (modifiers_mask & kBitShift) { down_vks.push_back(kModShift); down_up.push_back(false); }
    if (modifiers_mask & kBitAlt)   { down_vks.push_back(kModAlt);   down_up.push_back(false); }
    if (modifiers_mask & kBitWin)   { down_vks.push_back(kModWin);   down_up.push_back(false); }
    down_vks.push_back(virtual_key);
    down_up.push_back(false);
    if (!send_input_key_batch(down_vks, down_up)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (hold_ms == 0) {
        // Immediate release — reverse order so modifiers go up last.
        std::vector<uint32_t> up_vks;
        std::vector<bool>     up_up;
        up_vks.push_back(virtual_key);
        up_up.push_back(true);
        if (modifiers_mask & kBitWin)   { up_vks.push_back(kModWin);   up_up.push_back(true); }
        if (modifiers_mask & kBitAlt)   { up_vks.push_back(kModAlt);   up_up.push_back(true); }
        if (modifiers_mask & kBitShift) { up_vks.push_back(kModShift); up_up.push_back(true); }
        if (modifiers_mask & kBitCtrl)  { up_vks.push_back(kModCtrl);  up_up.push_back(true); }
        if (!send_input_key_batch(up_vks, up_up)) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        return SAO_STATUS_OK;
    }
    // Deferred release path — for holds we schedule the main VK plus
    // any modifiers as a combo so they all lift atomically.
    std::vector<uint32_t> pending;
    pending.push_back(virtual_key);
    if (modifiers_mask & kBitWin)   pending.push_back(kModWin);
    if (modifiers_mask & kBitAlt)   pending.push_back(kModAlt);
    if (modifiers_mask & kBitShift) pending.push_back(kModShift);
    if (modifiers_mask & kBitCtrl)  pending.push_back(kModCtrl);
    hold_worker().schedule_combo(pending, hold_ms);
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)virtual_key; (void)modifiers_mask; (void)hold_ms;
    // CAPABILITY GATE: requires SendInput (Windows user32.dll).
    // The Windows branch above is the real implementation.  See PLAN.md §1.5.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_auto_key_send_key_combo(
    const uint32_t* vk_array,
    size_t          count,
    uint32_t        hold_ms) {
#if defined(_WIN32)
    try {
    if (count == 0 || count > static_cast<size_t>(std::numeric_limits<UINT>::max()) ||
        count > std::numeric_limits<size_t>::max() / sizeof(INPUT))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (vk_array == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        if (vk_array[i] == 0 || vk_array[i] > 0xFF) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }
    std::vector<uint32_t> down_vks(vk_array, vk_array + count);
    std::vector<bool>     down_up(count, false);
    if (!send_input_key_batch(down_vks, down_up)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (hold_ms == 0) {
        // Immediate atomic release.
        std::vector<uint32_t> up_vks;
        up_vks.reserve(count);
        for (size_t i = count; i-- > 0;) up_vks.push_back(vk_array[i]);
        std::vector<bool> up_up(count, true);
        if (!send_input_key_batch(up_vks, up_up)) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        return SAO_STATUS_OK;
    }
    hold_worker().schedule_combo(down_vks, hold_ms);
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)vk_array; (void)count; (void)hold_ms;
    // CAPABILITY GATE: SendInput combo (Windows user32.dll).
    // See PLAN.md §1.5 for gate classification.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_auto_key_send_text(
    const uint16_t* text_utf16,
    size_t          count) {
#if defined(_WIN32)
    try {
    if (text_utf16 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    size_t n = count;
    if (n == 0) {
        while (text_utf16[n] != 0) ++n;
    }
    if (n == 0) return SAO_STATUS_OK;
    if (n > static_cast<size_t>(std::numeric_limits<UINT>::max()) / 2U ||
        n > std::numeric_limits<size_t>::max() / (2U * sizeof(INPUT)))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Each code unit emits two INPUTs — down+up — as
    // KEYEVENTF_UNICODE.
    std::vector<INPUT> inputs(n * 2);
    std::memset(inputs.data(), 0, sizeof(INPUT) * inputs.size());
    for (size_t i = 0; i < n; ++i) {
        WORD wch = static_cast<WORD>(text_utf16[i]);
        INPUT& down = inputs[i * 2];
        INPUT& up   = inputs[i * 2 + 1];
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0;
        down.ki.wScan = wch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        up.type = INPUT_KEYBOARD;
        up.ki.wVk = 0;
        up.ki.wScan = wch;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    UINT sent = SendInput(static_cast<UINT>(inputs.size()),
                          inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)text_utf16; (void)count;
    // CAPABILITY GATE: SendInput KEYEVENTF_UNICODE (Windows user32.dll).
    // See PLAN.md §1.5 for gate classification.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_auto_key_send_mouse_click(
    int32_t x_screen_px,
    int32_t y_screen_px,
    int32_t button) {
#if defined(_WIN32)
    try {
    DWORD down_flag = 0;
    DWORD up_flag   = 0;
    DWORD mouse_data = 0;
    switch (button) {
        case SAO_UI_AUTO_KEY_MOUSE_LEFT:
            down_flag = MOUSEEVENTF_LEFTDOWN;
            up_flag   = MOUSEEVENTF_LEFTUP;
            break;
        case SAO_UI_AUTO_KEY_MOUSE_RIGHT:
            down_flag = MOUSEEVENTF_RIGHTDOWN;
            up_flag   = MOUSEEVENTF_RIGHTUP;
            break;
        case SAO_UI_AUTO_KEY_MOUSE_MIDDLE:
            down_flag = MOUSEEVENTF_MIDDLEDOWN;
            up_flag   = MOUSEEVENTF_MIDDLEUP;
            break;
        case SAO_UI_AUTO_KEY_MOUSE_X1:
            down_flag = MOUSEEVENTF_XDOWN;
            up_flag   = MOUSEEVENTF_XUP;
            mouse_data = XBUTTON1;
            break;
        case SAO_UI_AUTO_KEY_MOUSE_X2:
            down_flag = MOUSEEVENTF_XDOWN;
            up_flag   = MOUSEEVENTF_XUP;
            mouse_data = XBUTTON2;
            break;
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // Move first if the sentinel isn't set (INT32_MIN, INT32_MIN).
    const bool move = !(x_screen_px == INT32_MIN &&
                        y_screen_px == INT32_MIN);
    std::vector<INPUT> inputs;
    inputs.reserve(move ? 3 : 2);
    if (move) {
        INPUT mv;
        std::memset(&mv, 0, sizeof(mv));
        mv.type = INPUT_MOUSE;
        // SendInput absolute coords use a normalized [0, 65535] range
        // over the primary monitor. GetSystemMetrics is authoritative.
        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0) screen_w = 1;
        if (screen_h <= 0) screen_h = 1;
        // Clamp to a well-defined space so the arithmetic never
        // overflows even if the caller passes an off-screen coord.
        long long x_norm = (static_cast<long long>(x_screen_px) * 65535)
                           / screen_w;
        long long y_norm = (static_cast<long long>(y_screen_px) * 65535)
                           / screen_h;
        mv.mi.dx = static_cast<LONG>(x_norm);
        mv.mi.dy = static_cast<LONG>(y_norm);
        mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inputs.push_back(mv);
    }
    INPUT down;
    std::memset(&down, 0, sizeof(down));
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = down_flag;
    down.mi.mouseData = mouse_data;
    inputs.push_back(down);
    INPUT up;
    std::memset(&up, 0, sizeof(up));
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = up_flag;
    up.mi.mouseData = mouse_data;
    inputs.push_back(up);
    UINT sent = SendInput(static_cast<UINT>(inputs.size()),
                          inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)x_screen_px; (void)y_screen_px; (void)button;
    // CAPABILITY GATE: SendInput MOUSEEVENTF_* + GetSystemMetrics
    // (Windows user32.dll).  See PLAN.md §1.5 for gate classification.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_auto_key_get_key_state(
    uint32_t virtual_key,
    bool*    is_down_out) {
    if (is_down_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (virtual_key == 0 || virtual_key > 0xFF) {
        *is_down_out = false;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    SHORT state = GetAsyncKeyState(static_cast<int>(virtual_key));
    *is_down_out = (state & 0x8000) != 0;
    return SAO_STATUS_OK;
#else
    (void)virtual_key;
    *is_down_out = false;
    // CAPABILITY GATE: GetAsyncKeyState (Windows user32.dll).
    // See PLAN.md §1.5 for gate classification.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_auto_key_arbitrate(
    int32_t   policy,
    uint32_t  virtual_key,
    bool*     allowed_out) {
    if (allowed_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (virtual_key == 0 || virtual_key > 0xFF) {
        *allowed_out = false;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    switch (policy) {
        case SAO_UI_AUTO_KEY_POLICY_SHARED:
        case SAO_UI_AUTO_KEY_POLICY_PLUGIN_ONLY:
            *allowed_out = true;
            return SAO_STATUS_OK;
        case SAO_UI_AUTO_KEY_POLICY_GAME_ONLY:
        case SAO_UI_AUTO_KEY_POLICY_BLOCKED:
            *allowed_out = false;
            return SAO_STATUS_OK;
        default:
            *allowed_out = false;
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

// ─── Test-only introspection ─────────────────────────────────────

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_auto_key_pending_holds(void) {
    return hold_worker().pending_count();
}
