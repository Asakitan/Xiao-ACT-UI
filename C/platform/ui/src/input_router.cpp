// SAO Auto — typed input router, focus stack, and hotkey matching.
//
// Implements the widget-facing side of input routing that the coarse
// legacy `input.h` doesn't cover: typed event dispatch, focus stack,
// modal barrier, and the hotkey subset matcher from memory
// [快捷键架构].
//
// Subset matcher contract (memory [快捷键架构]):
//   * A binding's `modifiers` bitmask matches an observed modifier
//     bitmask when every set bit in the binding is also set in the
//     observed mask (subset containment).
//   * Bindings with SAO_UI_MOD_ANY_* wildcard bits ignore the parallel
//     hard bit in the observed mask.
//   * When multiple bindings match a single event, "most specific
//     wins" — largest set-bit count in the effective modifier mask.
//     Ties broken by registration order (older wins) so the settings
//     UI can visualise conflicts deterministically.
//   * Modifier reality: modifiers on the event come from
//     GetAsyncKeyState (the raw-Win32 feed) so LL-hook key-up drops
//     are immune.
//
// Python source alignment:
//   * sao_gui_hotkey.py::HotkeyBinding.matches
//   * sao_gui_hotkeys_mixin.py::_active_hotkeys (subset selection)
//   * gui_modules/plugin_hotkey_registry.py (enforce_ctrl_prefix)
//
// The raw-Win32 path translates mouse, keyboard, character, and focus
// messages into the same typed route/dispatch path used by synthetic events.

#include "sao/ui/input_router.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"

#include "input_router_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#endif

namespace sao::ui::input_router_detail {

namespace {

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kLeftButtonDoubleClick = 0x0203;
constexpr uint32_t kRightButtonDown = 0x0204;
constexpr uint32_t kRightButtonUp = 0x0205;
constexpr uint32_t kRightButtonDoubleClick = 0x0206;
constexpr uint32_t kMiddleButtonDown = 0x0207;
constexpr uint32_t kMiddleButtonUp = 0x0208;
constexpr uint32_t kMiddleButtonDoubleClick = 0x0209;
constexpr uint32_t kMouseWheel = 0x020A;
constexpr uint32_t kCaptureChanged = 0x0215;
constexpr uint32_t kMouseLeave = 0x02A3;
constexpr uint32_t kCancelMode = 0x001F;
constexpr float kWheelDelta = 120.0F;

bool valid_layer_message(uint32_t message) noexcept {
    return message == kMouseMove || message == kMouseLeave || message == kMouseWheel ||
           message == kCaptureChanged || message == kCancelMode ||
           message == kLeftButtonDown || message == kLeftButtonUp ||
           message == kLeftButtonDoubleClick || message == kRightButtonDown ||
           message == kRightButtonUp || message == kRightButtonDoubleClick ||
           message == kMiddleButtonDown || message == kMiddleButtonUp ||
           message == kMiddleButtonDoubleClick;
}

bool append_action(std::array<LayerInputAction, 3>* actions, size_t* count,
                   const LayerInputAction& action) noexcept {
    if (actions == nullptr || count == nullptr || *count >= actions->size())
        return false;
    (*actions)[(*count)++] = action;
    return true;
}

uint64_t next_writer_revision(uint64_t current) noexcept {
    ++current;
    return current == 0 ? 1 : current;
}

} // namespace

struct LayerInputState {
    void* hovered_layer{};
    void* captured_layer{};
    int32_t captured_button{-1};
    int32_t suppressed_button_up{-1};
    int32_t last_pointer_x{};
    int32_t last_pointer_y{};
    bool has_pointer{};
    uint64_t writer_revision{};
};

LayerInputState* create_layer_input_state() noexcept {
    return new (std::nothrow) LayerInputState();
}

void destroy_layer_input_state(LayerInputState* state) noexcept {
    delete state;
}

bool layer_event_uses_coordinates(uint32_t message) noexcept {
    return message != kMouseLeave && message != kCaptureChanged && message != kCancelMode;
}

sao_status_t route_layer_input(LayerInputState* state, uint32_t message, int32_t host_x,
                               int32_t host_y, int32_t button, int32_t wheel_delta,
                               void* hit_layer, float hit_x, float hit_y,
                               LayerInputAction* out_actions, size_t action_capacity,
                               size_t* out_action_count) noexcept {
    if (out_action_count == nullptr || state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_action_count = 0;
    if (out_actions == nullptr && action_capacity != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!valid_layer_message(message))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    LayerInputState next = *state;
    std::array<LayerInputAction, 3> actions{};
    size_t action_count = 0;

    const auto emit_leave = [&](void* layer) {
        return layer == nullptr ||
               append_action(&actions, &action_count,
                             {LayerInputActionKind::leave, layer});
    };
    const auto release_capture = [&](bool suppress_button_up) {
        if (suppress_button_up && next.captured_button >= 0)
            next.suppressed_button_up = next.captured_button;
        next.captured_layer = nullptr;
        next.captured_button = -1;
    };

    if (message == kCaptureChanged || message == kCancelMode) {
        void* leave_target = next.captured_layer != nullptr ? next.captured_layer
                                                            : next.hovered_layer;
        if (!emit_leave(leave_target))
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        release_capture(true);
        next.hovered_layer = nullptr;
        next.has_pointer = false;
    } else if (message == kMouseLeave) {
        if (next.captured_layer == nullptr) {
            next.has_pointer = false;
            if (!emit_leave(next.hovered_layer))
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            next.hovered_layer = nullptr;
        }
    } else {
        next.last_pointer_x = host_x;
        next.last_pointer_y = host_y;
        next.has_pointer = true;
        if (message == kMouseMove) {
            if (next.captured_layer == nullptr && next.hovered_layer != hit_layer) {
                if (!emit_leave(next.hovered_layer))
                    return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
                next.hovered_layer = hit_layer;
            }
            void* route_target = next.captured_layer != nullptr ? next.captured_layer : hit_layer;
            if (route_target != nullptr &&
                !append_action(&actions, &action_count,
                               {LayerInputActionKind::cursor, route_target, hit_x, hit_y, 0.0F,
                                0.0F, -1, 0, route_target != hit_layer})) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
        } else if (message == kMouseWheel) {
            if (hit_layer != nullptr &&
                !append_action(&actions, &action_count,
                               {LayerInputActionKind::scroll, hit_layer, hit_x, hit_y, 0.0F,
                                static_cast<float>(wheel_delta) / kWheelDelta})) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
        } else {
            const bool double_click = message == kLeftButtonDoubleClick ||
                                      message == kRightButtonDoubleClick ||
                                      message == kMiddleButtonDoubleClick;
            const bool released = message == kLeftButtonUp || message == kRightButtonUp ||
                                  message == kMiddleButtonUp;
            if (double_click) {
                next.suppressed_button_up = button;
                release_capture(false);
            } else if (released && next.suppressed_button_up == button) {
                next.suppressed_button_up = -1;
                release_capture(false);
            } else {
                void* route_target = released && next.captured_layer != nullptr
                                         ? next.captured_layer
                                         : hit_layer;
                if (!released && route_target != nullptr) {
                    next.captured_layer = route_target;
                    next.captured_button = button;
                }
                if (route_target != nullptr &&
                    !append_action(&actions, &action_count,
                                   {LayerInputActionKind::button, route_target, hit_x, hit_y, 0.0F,
                                    0.0F, button, released ? 0 : 1,
                                    route_target != hit_layer})) {
                    return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
                }
                if (released) {
                    release_capture(false);
                    if (next.hovered_layer != hit_layer) {
                        if (!emit_leave(next.hovered_layer))
                            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
                        next.hovered_layer = hit_layer;
                    }
                }
            }
        }
    }

    if (action_count > action_capacity)
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    for (size_t index = 0; index < action_count; ++index)
        out_actions[index] = actions[index];
    next.writer_revision = next_writer_revision(state->writer_revision);
    *state = next;
    *out_action_count = action_count;
    return SAO_STATUS_OK;
}

bool layer_input_references(const LayerInputState* state, const void* layer) noexcept {
    return state != nullptr && layer != nullptr &&
           (state->hovered_layer == layer || state->captured_layer == layer);
}

bool layer_input_last_pointer(const LayerInputState* state, int32_t* out_x,
                              int32_t* out_y) noexcept {
    if (state == nullptr || out_x == nullptr || out_y == nullptr || !state->has_pointer)
        return false;
    *out_x = state->last_pointer_x;
    *out_y = state->last_pointer_y;
    return true;
}

sao_status_t invalidate_layer_input(LayerInputState* state, void* layer,
                                    bool still_accepts_at_pointer,
                                    LayerInputAction* out_actions, size_t action_capacity,
                                    size_t* out_action_count) noexcept {
    if (state == nullptr || layer == nullptr || out_action_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_action_count = 0;
    if (out_actions == nullptr && action_capacity != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!layer_input_references(state, layer) || still_accepts_at_pointer)
        return SAO_STATUS_OK;
    if (action_capacity < 1)
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;

    LayerInputState next = *state;
    if (next.hovered_layer == layer)
        next.hovered_layer = nullptr;
    if (next.captured_layer == layer) {
        if (next.captured_button >= 0)
            next.suppressed_button_up = next.captured_button;
        next.captured_layer = nullptr;
        next.captured_button = -1;
    }
    out_actions[0] = {LayerInputActionKind::leave, layer};
    *out_action_count = 1;
    next.writer_revision = next_writer_revision(state->writer_revision);
    *state = next;
    return SAO_STATUS_OK;
}

void reset_layer_input(LayerInputState* state) noexcept {
    if (state == nullptr)
        return;
    const uint64_t revision = next_writer_revision(state->writer_revision);
    *state = {};
    state->captured_button = -1;
    state->suppressed_button_up = -1;
    state->writer_revision = revision;
}

uint64_t layer_input_writer_revision(const LayerInputState* state) noexcept {
    return state == nullptr ? 0 : state->writer_revision;
}

sao_status_t apply_host_input_regions(sao_ui_overlay_host_handle_t host,
                                      const SaoOverlayHostInputRect* rects,
                                      size_t rect_count) noexcept {
    return sao_ui_overlay_host_set_input_region(host, rects, rect_count);
}

sao_status_t apply_host_input_passthrough(sao_ui_overlay_host_handle_t host,
                                          bool passthrough) noexcept {
    return sao_ui_overlay_host_set_input_passthrough(host, passthrough);
}

sao_status_t reset_host_input(sao_ui_overlay_host_handle_t host) noexcept {
    const sao_status_t region_status = apply_host_input_regions(host, nullptr, 0);
    if (region_status != SAO_STATUS_OK)
        return region_status;
    return apply_host_input_passthrough(host, true);
}

} // namespace sao::ui::input_router_detail

namespace {

// ─── Focus stack entry ─────────────────────────────────────────────
struct FocusEntry {
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_widget_handle_t widget = nullptr;
    uint64_t widget_generation = 0;
    // true → modal barrier; events don't leak past this entry.
    bool modal = false;
    uint64_t registration_order = 0;
};

// ─── Hotkey record ─────────────────────────────────────────────────
struct HotkeyRecord {
    sao_ui_hotkey_binding_t handle = 0;
    std::string plugin_id;
    std::string binding_id;
    uint32_t virtual_key = 0;
    uint32_t modifiers = 0; // full mask (raw + wildcards)
    int32_t scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    std::string scope_panel_id;
    bool enforce_ctrl_prefix = false;
    bool prevent_default = false;
    bool allow_repeat = false;
    bool require_release = false;
    sao_ui_hotkey_cb_t callback = nullptr;
    void* user_data = nullptr;
    // Insertion counter for stable tie-break in subset matcher.
    uint64_t insert_order = 0;
    std::mutex callback_mutex;
    std::condition_variable callback_idle;
    size_t active_callbacks = 0;
    bool active = true;
};

// Modifier bit constants for the observed side (hard bits only).
constexpr uint32_t kHardBits = SAO_UI_MOD_CTRL_BIT | SAO_UI_MOD_ALT_BIT | SAO_UI_MOD_SHIFT_BIT |
                               SAO_UI_MOD_WIN_BIT | SAO_UI_MOD_CAPS_BIT | SAO_UI_MOD_NUM_BIT;

// popcount for 32-bit — used as "specificity" of a binding.
inline int popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return static_cast<int>((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}

// Check whether a binding's modifier spec matches an observed mask.
// SAO_UI_MOD_ANY_CTRL sets a wildcard for CTRL — the observed value
// of CTRL is ignored, but the binding's own hard CTRL bit is treated
// as "don't require".  Formally:
//   For each modifier pair (hard_bit, any_bit):
//     if binding.any_bit  set                       → always pass
//     else if binding.hard_bit set                  → require observed hard_bit
//     else                                          → require observed hard_bit == 0
// This gives strict-match semantics (memory [快捷键架构] "modifiers must
// match exactly — a plain-F5 binding does not fire for CTRL+F5").
bool binding_matches(uint32_t bind_mods, uint32_t observed_mods) {
    struct Pair {
        uint32_t hard;
        uint32_t any;
    };
    constexpr Pair kPairs[] = {
        {SAO_UI_MOD_CTRL_BIT, SAO_UI_MOD_ANY_CTRL},
        {SAO_UI_MOD_ALT_BIT, SAO_UI_MOD_ANY_ALT},
        {SAO_UI_MOD_SHIFT_BIT, SAO_UI_MOD_ANY_SHIFT},
        {SAO_UI_MOD_WIN_BIT, SAO_UI_MOD_ANY_WIN},
    };
    for (const auto& p : kPairs) {
        const bool wildcard = (bind_mods & p.any) != 0;
        if (wildcard)
            continue;
        const bool bind_hard = (bind_mods & p.hard) != 0;
        if (bind_hard && (observed_mods & p.hard) == 0)
            return false;
    }
    constexpr uint32_t kLockBits = SAO_UI_MOD_CAPS_BIT | SAO_UI_MOD_NUM_BIT;
    return (bind_mods & observed_mods & kLockBits) == (bind_mods & kLockBits);
}

// Effective specificity — count of hard modifier bits in the binding.
// Wildcard bits do NOT add to specificity (they are more permissive).
int binding_specificity(uint32_t bind_mods) {
    return popcount32(bind_mods & kHardBits);
}

} // namespace

// ─── Router record ──────────────────────────────────────────────────
struct sao_ui_input_router_deep_s {
    uint64_t generation = 0;
    std::mutex lifecycle_mu;
    std::condition_variable lifecycle_cv;
    size_t in_flight = 0;
    bool accepting = true;
    bool retired = false;
    bool finalized = false;
    bool finalization_started = false;

    std::mutex mu;
    sao_ui_compositor_handle_t compositor = nullptr;
    uint64_t transition_generation = 0;

    // Focus stack — top of vector is the current focus target.
    std::vector<FocusEntry> focus_stack;
    uint64_t next_focus_registration_order = 0;

    // Hotkey list.  Insertion order preserved so ties broken oldest-first.
    std::vector<std::shared_ptr<HotkeyRecord>> hotkeys;
    uint64_t next_hotkey_handle = 1;
    uint64_t next_insert_order = 0;

    // Hover tracking.
    sao_ui_widget_handle_t hover_widget = nullptr;
    uint64_t hover_widget_generation = 0;
    sao_ui_hover_change_cb_t hover_cb = nullptr;
    void* hover_user_data = nullptr;

    // Mouse capture.
    sao_ui_widget_handle_t captured_widget = nullptr;
    uint64_t captured_widget_generation = 0;

    // Modal barrier: index into focus_stack of the topmost modal entry
    // (SIZE_MAX → none).  Recomputed on push/pop.
    size_t modal_top = SIZE_MAX;

    // Per-panel custom hit regions.
    struct RegionCopy {
        int32_t shape;
        int32_t x_or_cx;
        int32_t y_or_cy;
        int32_t w_or_r;
        int32_t h_or_pt_count;
        std::vector<int32_t> poly_verts;
        uint8_t alpha_threshold;
    };
    std::unordered_map<sao_ui_panel_handle_t, RegionCopy> regions;

    // Last routed event target — used to synthesize enter/leave events
    // when the hovered widget changes.  We store the widget only; the
    // panel is implicit through the focus stack.
    sao_ui_widget_handle_t last_route_target = nullptr;
    uint64_t last_route_target_generation = 0;
};

namespace {

struct RouterHandleRegistry {
    std::mutex mu;
    uint64_t next_generation = 1;
    std::unordered_set<sao_ui_input_router_deep_handle_t> active;
    std::vector<std::unique_ptr<sao_ui_input_router_deep_s>> shells;
};

RouterHandleRegistry& router_handle_registry() {
    static RouterHandleRegistry* registry = new RouterHandleRegistry();
    return *registry;
}

std::atomic<int32_t> g_router_create_failure_point{0};

thread_local std::vector<sao_ui_input_router_deep_handle_t> g_router_callback_stack;
thread_local std::vector<const HotkeyRecord*> g_hotkey_callback_stack;

bool router_callback_owns(sao_ui_input_router_deep_handle_t handle) {
    return std::find(g_router_callback_stack.begin(), g_router_callback_stack.end(), handle) !=
           g_router_callback_stack.end();
}

class RouterCallbackScope final {
  public:
    explicit RouterCallbackScope(sao_ui_input_router_deep_handle_t handle) : handle_(handle) {
        g_router_callback_stack.push_back(handle_);
    }

    ~RouterCallbackScope() {
        g_router_callback_stack.pop_back();
    }

    RouterCallbackScope(const RouterCallbackScope&) = delete;
    RouterCallbackScope& operator=(const RouterCallbackScope&) = delete;

  private:
    sao_ui_input_router_deep_handle_t handle_;
};

size_t hotkey_callback_owned_count(const HotkeyRecord* record) {
    return static_cast<size_t>(
        std::count(g_hotkey_callback_stack.begin(), g_hotkey_callback_stack.end(), record));
}

class HotkeyCallbackScope final {
  public:
    explicit HotkeyCallbackScope(const HotkeyRecord* record) {
        g_hotkey_callback_stack.push_back(record);
    }

    ~HotkeyCallbackScope() {
        g_hotkey_callback_stack.pop_back();
    }

    HotkeyCallbackScope(const HotkeyCallbackScope&) = delete;
    HotkeyCallbackScope& operator=(const HotkeyCallbackScope&) = delete;
};

class HotkeyDispatchLease final {
  public:
    explicit HotkeyDispatchLease(std::shared_ptr<HotkeyRecord> record)
        : record_(std::move(record)) {
        if (record_ == nullptr)
            return;
        std::lock_guard<std::mutex> lock(record_->callback_mutex);
        if (!record_->active)
            return;
        ++record_->active_callbacks;
        active_ = true;
    }

    ~HotkeyDispatchLease() {
        if (!active_)
            return;
        {
            std::lock_guard<std::mutex> lock(record_->callback_mutex);
            --record_->active_callbacks;
        }
        record_->callback_idle.notify_all();
    }

    HotkeyDispatchLease(const HotkeyDispatchLease&) = delete;
    HotkeyDispatchLease& operator=(const HotkeyDispatchLease&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }

  private:
    std::shared_ptr<HotkeyRecord> record_;
    bool active_ = false;
};

void retire_hotkey(const std::shared_ptr<HotkeyRecord>& record) {
    if (record == nullptr)
        return;
    const size_t owned_callbacks = hotkey_callback_owned_count(record.get());
    std::unique_lock<std::mutex> lock(record->callback_mutex);
    record->active = false;
    record->callback_idle.wait(
        lock, [&record, owned_callbacks] { return record->active_callbacks <= owned_callbacks; });
}

void finalize_router(sao_ui_input_router_deep_handle_t handle) noexcept {
    try {
        std::lock_guard<std::mutex> guard(handle->mu);
        handle->focus_stack.clear();
        handle->hotkeys.clear();
        handle->regions.clear();
        handle->hover_widget = nullptr;
        handle->hover_widget_generation = 0;
        handle->hover_cb = nullptr;
        handle->hover_user_data = nullptr;
        handle->captured_widget = nullptr;
        handle->captured_widget_generation = 0;
        handle->last_route_target = nullptr;
        handle->last_route_target_generation = 0;
        handle->modal_top = SIZE_MAX;
        handle->compositor = nullptr;
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lifecycle_guard(handle->lifecycle_mu);
        handle->finalized = true;
    }
    handle->lifecycle_cv.notify_all();
}

class RouterLease final {
  public:
    RouterLease() = default;
    explicit RouterLease(sao_ui_input_router_deep_handle_t handle) : handle_(handle) {}

    RouterLease(RouterLease&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    RouterLease& operator=(RouterLease&& other) noexcept {
        if (this != &other) {
            release();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~RouterLease() {
        release();
    }

    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    sao_ui_input_router_deep_s* operator->() const noexcept {
        return handle_;
    }

    sao_ui_input_router_deep_handle_t get() const noexcept {
        return handle_;
    }

    bool transition_is_current(uint64_t generation) const noexcept {
        if (handle_ == nullptr)
            return false;
        std::lock_guard<std::mutex> lifecycle_guard(handle_->lifecycle_mu);
        if (!handle_->accepting || handle_->retired)
            return false;
        std::lock_guard<std::mutex> guard(handle_->mu);
        return handle_->transition_generation == generation;
    }

  private:
    void release() noexcept {
        if (handle_ == nullptr)
            return;
        bool finalize = false;
        {
            std::lock_guard<std::mutex> lifecycle_guard(handle_->lifecycle_mu);
            if (handle_->in_flight > 0)
                --handle_->in_flight;
            if (handle_->retired && handle_->in_flight == 0 && !handle_->finalized &&
                !handle_->finalization_started) {
                handle_->finalization_started = true;
                finalize = true;
            }
        }
        handle_->lifecycle_cv.notify_all();
        if (finalize)
            finalize_router(handle_);
        handle_ = nullptr;
    }

    sao_ui_input_router_deep_handle_t handle_ = nullptr;
};

RouterLease acquire_router(sao_ui_input_router_deep_handle_t handle) {
    if (handle == nullptr)
        return {};
    auto& registry = router_handle_registry();
    std::lock_guard<std::mutex> registry_guard(registry.mu);
    if (!registry.active.contains(handle))
        return {};
    std::lock_guard<std::mutex> lifecycle_guard(handle->lifecycle_mu);
    if (!handle->accepting || handle->retired)
        return {};
    ++handle->in_flight;
    return RouterLease(handle);
}

bool hotkey_dispatch_completed(const RouterLease& lease, uint64_t transition,
                               sao_ui_hotkey_binding_t binding) {
    if (!lease)
        return false;
    std::lock_guard<std::mutex> lifecycle_guard(lease->lifecycle_mu);
    if (!lease->accepting || lease->retired)
        return false;
    std::lock_guard<std::mutex> guard(lease->mu);
    if (lease->transition_generation == transition)
        return true;
    return std::ranges::none_of(
        lease->hotkeys, [binding](const auto& record) { return record->handle == binding; });
}

bool input_handle_is_active(sao_ui_widget_handle_t handle, uint64_t expected_generation = 0) {
    if (handle == nullptr)
        return false;
    sao::ui::detail::WidgetHandleMetadata metadata{};
    if (!sao::ui::detail::inspect_widget_handle(handle, &metadata))
        return false;
    return expected_generation == 0 || expected_generation == metadata.generation;
}

void recompute_modal_top_locked(sao_ui_input_router_deep_s* router) {
    router->modal_top = SIZE_MAX;
    for (size_t index = router->focus_stack.size(); index-- > 0;) {
        if (router->focus_stack[index].modal) {
            router->modal_top = index;
            break;
        }
    }
}

void prune_retired_widgets_locked(sao_ui_input_router_deep_s* router) {
    router->focus_stack.erase(std::remove_if(router->focus_stack.begin(), router->focus_stack.end(),
                                             [](const FocusEntry& entry) {
                                                 return entry.widget != nullptr &&
                                                        !input_handle_is_active(
                                                            entry.widget, entry.widget_generation);
                                             }),
                              router->focus_stack.end());
    recompute_modal_top_locked(router);
    if (!input_handle_is_active(router->hover_widget, router->hover_widget_generation)) {
        router->hover_widget = nullptr;
        router->hover_widget_generation = 0;
    }
    if (!input_handle_is_active(router->captured_widget, router->captured_widget_generation)) {
        router->captured_widget = nullptr;
        router->captured_widget_generation = 0;
    }
    if (!input_handle_is_active(router->last_route_target, router->last_route_target_generation)) {
        router->last_route_target = nullptr;
        router->last_route_target_generation = 0;
    }
}

uint64_t bump_transition_locked(sao_ui_input_router_deep_s* router) {
    return ++router->transition_generation;
}

struct HotkeyMatch {
    std::shared_ptr<HotkeyRecord> record;
    sao_ui_hotkey_binding_t binding = 0;
    sao_ui_hotkey_cb_t callback = nullptr;
    void* user_data = nullptr;
    std::string binding_id;
    bool prevent_default = false;
};

bool hotkey_scope_matches(const sao_ui_input_router_deep_s* router, const HotkeyRecord& record) {
    if (record.scope == SAO_UI_HOTKEY_SCOPE_GLOBAL)
        return true;
    if (record.scope == SAO_UI_HOTKEY_SCOPE_HOST_FOCUS) {
#if defined(_WIN32)
        void* hwnd = sao_ui_compositor_host_hwnd(router->compositor);
        return hwnd != nullptr && ::GetForegroundWindow() == reinterpret_cast<HWND>(hwnd);
#else
        return false;
#endif
    }
    if (record.scope != SAO_UI_HOTKEY_SCOPE_PANEL || record.scope_panel_id.empty() ||
        router->focus_stack.empty()) {
        return false;
    }
    sao_ui_panel_handle_t expected = nullptr;
    return sao_ui_panel_find_by_id(router->compositor, record.scope_panel_id.c_str(), &expected) ==
               SAO_STATUS_OK &&
           expected == router->focus_stack.back().panel;
}

HotkeyMatch select_hotkey_locked(const sao_ui_input_router_deep_s* router,
                                 const SaoUiInputEvent& event) {
    std::shared_ptr<HotkeyRecord> best;
    int best_specificity = -1;
    uint64_t best_insert = UINT64_MAX;
    for (const auto& record : router->hotkeys) {
        if (record->virtual_key != event.virtual_key)
            continue;
        if (event.kind == SAO_UI_INPUT_KEY_UP && !record->require_release)
            continue;
        if (event.kind == SAO_UI_INPUT_KEY_DOWN && record->require_release)
            continue;
        if (event.kind != SAO_UI_INPUT_KEY_DOWN && event.kind != SAO_UI_INPUT_KEY_UP)
            continue;
        if (event.key_repeat && !record->allow_repeat)
            continue;
        if (!binding_matches(record->modifiers, event.modifiers))
            continue;
        if (!hotkey_scope_matches(router, *record))
            continue;
        const int specificity = binding_specificity(record->modifiers);
        if (specificity > best_specificity ||
            (specificity == best_specificity && record->insert_order < best_insert)) {
            best = record;
            best_specificity = specificity;
            best_insert = record->insert_order;
        }
    }
    if (best == nullptr)
        return {};
    return {best,
            best->handle,
            best->callback,
            best->user_data,
            best->binding_id,
            best->prevent_default};
}

bool valid_event_kind(int32_t kind) {
    return kind >= SAO_UI_INPUT_MOUSE_MOVE && kind <= SAO_UI_INPUT_TOUCH_END;
}

bool region_contains(const sao_ui_input_router_deep_s::RegionCopy& region, int32_t x, int32_t y) {
    if (region.shape == SAO_UI_HIT_RECT || region.shape == SAO_UI_HIT_ALPHA) {
        const int64_t right = static_cast<int64_t>(region.x_or_cx) + region.w_or_r;
        const int64_t bottom = static_cast<int64_t>(region.y_or_cy) + region.h_or_pt_count;
        return x >= region.x_or_cx && y >= region.y_or_cy && x < right && y < bottom;
    }
    if (region.shape == SAO_UI_HIT_CIRCLE) {
        const int64_t dx = static_cast<int64_t>(x) - region.x_or_cx;
        const int64_t dy = static_cast<int64_t>(y) - region.y_or_cy;
        const int64_t radius = region.w_or_r;
        return dx * dx + dy * dy <= radius * radius;
    }
    bool inside = false;
    const size_t count = region.poly_verts.size() / 2;
    for (size_t i = 0, previous = count - 1; i < count; previous = i++) {
        const int32_t xi = region.poly_verts[i * 2];
        const int32_t yi = region.poly_verts[i * 2 + 1];
        const int32_t xj = region.poly_verts[previous * 2];
        const int32_t yj = region.poly_verts[previous * 2 + 1];
        const bool crosses = (yi > y) != (yj > y) &&
                             static_cast<double>(x) < (static_cast<double>(xj) - xi) *
                                                              (static_cast<double>(y) - yi) /
                                                              (static_cast<double>(yj) - yi) +
                                                          xi;
        if (crosses)
            inside = !inside;
    }
    return inside;
}

} // namespace

// ─── Manager lifecycle ──────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_deep_create(
    sao_ui_compositor_handle_t compositor, sao_ui_input_router_deep_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        auto router = std::make_unique<sao_ui_input_router_deep_s>();
        router->compositor = compositor;
        auto& registry = router_handle_registry();
        std::lock_guard<std::mutex> registry_guard(registry.mu);
        router->generation = registry.next_generation++;
        if (router->generation == 0)
            router->generation = registry.next_generation++;
        auto* published = router.get();
        registry.active.insert(published);
        try {
            if (g_router_create_failure_point.load(std::memory_order_acquire) == 1)
                throw std::bad_alloc();
            registry.shells.push_back(std::move(router));
        } catch (...) {
            registry.active.erase(published);
            throw;
        }
        *out_handle = published;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL
sao_ui_input_router_deep_destroy(sao_ui_input_router_deep_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        auto& registry = router_handle_registry();
        {
            std::lock_guard<std::mutex> registry_guard(registry.mu);
            if (!registry.active.contains(handle))
                return;
            {
                std::lock_guard<std::mutex> lifecycle_guard(handle->lifecycle_mu);
                handle->accepting = false;
                handle->retired = true;
            }
            registry.active.erase(handle);
        }
#if defined(_WIN32)
        if (::GetCapture() != nullptr)
            ::ReleaseCapture();
#endif
        if (router_callback_owns(handle))
            return;

        bool finalize = false;
        std::unique_lock<std::mutex> lifecycle_guard(handle->lifecycle_mu);
        while (!handle->finalized) {
            if (handle->in_flight == 0 && !handle->finalization_started) {
                handle->finalization_started = true;
                finalize = true;
                break;
            }
            handle->lifecycle_cv.wait(lifecycle_guard);
        }
        lifecycle_guard.unlock();
        if (finalize)
            finalize_router(handle);
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_input_router_deep_try_destroy(sao_ui_input_router_deep_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_OK;
    try {
        auto& registry = router_handle_registry();
        {
            std::lock_guard<std::mutex> registry_guard(registry.mu);
            if (!registry.active.contains(handle))
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lifecycle_guard(handle->lifecycle_mu);
            if (!handle->accepting || handle->retired)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            if (handle->in_flight != 0)
                return SAO_UI_STATUS_ERR_BUSY;
            handle->accepting = false;
            handle->retired = true;
            handle->finalization_started = true;
            registry.active.erase(handle);
        }
#if defined(_WIN32)
        if (::GetCapture() != nullptr)
            ::ReleaseCapture();
#endif
        finalize_router(handle);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_deep_rebind_compositor(
    sao_ui_input_router_deep_handle_t handle, sao_ui_compositor_handle_t expected_compositor,
    sao_ui_compositor_handle_t replacement_compositor) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        auto& registry = router_handle_registry();
        std::lock_guard<std::mutex> registry_guard(registry.mu);
        if (!registry.active.contains(handle))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lifecycle_guard(handle->lifecycle_mu);
        if (!handle->accepting || handle->retired || handle->finalized ||
            handle->finalization_started) {
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        if (handle->in_flight != 0)
            return SAO_UI_STATUS_ERR_BUSY;
        std::lock_guard<std::mutex> state_guard(handle->mu);
        if (handle->compositor != expected_compositor)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        handle->compositor = replacement_compositor;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_input_router_feed_raw_win32(sao_ui_input_router_deep_handle_t handle, uint32_t msg,
                                   uint64_t wparam, int64_t lparam, bool* out_consumed) {
    if (out_consumed != nullptr)
        *out_consumed = false;
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(_WIN32)
        (void)msg;
        (void)wparam;
        (void)lparam;
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#else
        SaoUiInputEvent event{};
        POINT point{};
        ::GetCursorPos(&point);
        event.screen_x_px = point.x;
        event.screen_y_px = point.y;

        const auto modifier_down = [](int virtual_key) {
            return (::GetAsyncKeyState(virtual_key) & 0x8000) != 0;
        };
        if (modifier_down(VK_CONTROL))
            event.modifiers |= SAO_UI_MOD_CTRL_BIT;
        if (modifier_down(VK_MENU))
            event.modifiers |= SAO_UI_MOD_ALT_BIT;
        if (modifier_down(VK_SHIFT))
            event.modifiers |= SAO_UI_MOD_SHIFT_BIT;
        if (modifier_down(VK_LWIN) || modifier_down(VK_RWIN))
            event.modifiers |= SAO_UI_MOD_WIN_BIT;
        if ((::GetKeyState(VK_CAPITAL) & 1) != 0)
            event.modifiers |= SAO_UI_MOD_CAPS_BIT;
        if ((::GetKeyState(VK_NUMLOCK) & 1) != 0)
            event.modifiers |= SAO_UI_MOD_NUM_BIT;

        switch (msg) {
        case WM_MOUSEMOVE:
            event.kind = SAO_UI_INPUT_MOUSE_MOVE;
            break;
        case WM_MOUSELEAVE:
            event.kind = SAO_UI_INPUT_MOUSE_LEAVE;
            break;
        case WM_MOUSEWHEEL:
            event.kind = SAO_UI_INPUT_MOUSE_WHEEL;
            event.screen_x_px = GET_X_LPARAM(lparam);
            event.screen_y_px = GET_Y_LPARAM(lparam);
            event.wheel_delta = GET_WHEEL_DELTA_WPARAM(wparam);
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDBLCLK:
            event.kind = SAO_UI_INPUT_MOUSE_DOWN;
            event.button = msg == WM_LBUTTONDOWN                              ? SAO_UI_MOUSE_LEFT
                           : msg == WM_LBUTTONDBLCLK                          ? SAO_UI_MOUSE_LEFT
                           : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ? SAO_UI_MOUSE_RIGHT
                           : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK ? SAO_UI_MOUSE_MIDDLE
                           : GET_XBUTTON_WPARAM(wparam) == XBUTTON1           ? SAO_UI_MOUSE_X1
                                                                              : SAO_UI_MOUSE_X2;
            event.click_count = msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                                        msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK
                                    ? 2
                                    : 1;
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            event.kind = SAO_UI_INPUT_MOUSE_UP;
            event.button = msg == WM_LBUTTONUP                      ? SAO_UI_MOUSE_LEFT
                           : msg == WM_RBUTTONUP                    ? SAO_UI_MOUSE_RIGHT
                           : msg == WM_MBUTTONUP                    ? SAO_UI_MOUSE_MIDDLE
                           : GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? SAO_UI_MOUSE_X1
                                                                    : SAO_UI_MOUSE_X2;
            event.click_count = 1;
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            event.kind = SAO_UI_INPUT_KEY_DOWN;
            event.virtual_key = static_cast<uint32_t>(wparam);
            event.scan_code = static_cast<uint32_t>((lparam >> 16) & 0xFF);
            event.key_repeat = (lparam & (1LL << 30)) != 0;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            event.kind = SAO_UI_INPUT_KEY_UP;
            event.virtual_key = static_cast<uint32_t>(wparam);
            event.scan_code = static_cast<uint32_t>((lparam >> 16) & 0xFF);
            break;
        case WM_CHAR:
            event.kind = SAO_UI_INPUT_KEY_CHAR;
            event.unicode_codepoint = static_cast<uint32_t>(wparam);
            break;
        case WM_SETFOCUS:
            event.kind = SAO_UI_INPUT_FOCUS_GAIN;
            break;
        case WM_KILLFOCUS:
            event.kind = SAO_UI_INPUT_FOCUS_LOSE;
            break;
        default:
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        return sao_ui_input_router_route_event(handle, &event, out_consumed);
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Explicit dispatch (test rig + synthetic events) ────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_route_event(
    sao_ui_input_router_deep_handle_t handle, const SaoUiInputEvent* event, bool* out_consumed) {

    if (out_consumed != nullptr)
        *out_consumed = false;
    if (event == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!valid_event_kind(event->kind))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::unique_lock<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());

        // Resolve the routing target — mouse capture wins.  Custom panel regions
        // then select the top-most focused widget whose panel contains the point;
        // without custom regions the focus-stack top remains the default target.
        sao_ui_widget_handle_t target = lease->captured_widget;
        const bool mouse_event =
            event->kind >= SAO_UI_INPUT_MOUSE_MOVE && event->kind <= SAO_UI_INPUT_MOUSE_LEAVE;
        if (target == nullptr) {
            if (mouse_event) {
                if (event->kind != SAO_UI_INPUT_MOUSE_LEAVE) {
                    if (lease->regions.empty()) {
                        if (!lease->focus_stack.empty())
                            target = lease->focus_stack.back().widget;
                    } else {
                        for (size_t index = lease->focus_stack.size(); index-- > 0;) {
                            const auto found = lease->regions.find(lease->focus_stack[index].panel);
                            if (found != lease->regions.end() &&
                                region_contains(found->second, event->screen_x_px,
                                                event->screen_y_px)) {
                                target = lease->focus_stack[index].widget;
                                break;
                            }
                        }
                    }
                }
            } else if (!lease->focus_stack.empty()) {
                target = lease->focus_stack.back().widget;
            }
        }

        uint64_t target_generation = 0;
        if (target != nullptr) {
            sao::ui::detail::WidgetHandleMetadata metadata{};
            if (!sao::ui::detail::inspect_widget_handle(target, &metadata)) {
                target = nullptr;
            } else {
                target_generation = metadata.generation;
            }
        }

        // Modal barrier semantics — if the event bears a panel target
        // (via the future overlay hit-test), the barrier would filter
        // here.  In the test-focused slice we approximate by treating
        // barrier-present as "target must be inside the modal panel".
        // The router only knows the focus stack, so barrier == outermost
        // modal entry — any focus-target inside the stack after
        // `modal_top` is legal, everything else is dropped.
        //
        // Special case: a fresh push_modal leaves the top entry with
        // widget == nullptr, so `target` inherits nullptr from the top of
        // the stack.  We must still absorb the event (the modal panel is
        // present even if no widget under it has claimed focus yet); the
        // dropped-through no-target path below would otherwise report
        // `consumed = false` which the "router_modal_barrier_blocks_others"
        // contract explicitly forbids.
        if (lease->modal_top != SIZE_MAX) {
            bool inside_modal = false;
            if (target != nullptr) {
                // Confirm the target belongs to a focus-stack entry above
                // the modal barrier (i.e. on the modal panel or a child).
                for (size_t i = lease->modal_top; i < lease->focus_stack.size(); ++i) {
                    if (lease->focus_stack[i].widget == target) {
                        inside_modal = true;
                        break;
                    }
                }
            }
            if (!inside_modal) {
                // Event blocked by modal barrier.
                if (out_consumed != nullptr)
                    *out_consumed = true;
                return SAO_STATUS_OK;
            }
        }

        lease->last_route_target = target;
        lease->last_route_target_generation = target_generation;
        sao_ui_hover_change_cb_t hover_callback = nullptr;
        void* hover_user_data = nullptr;
        sao_ui_widget_handle_t previous_hover = lease->hover_widget;
        sao_ui_widget_handle_t next_hover = previous_hover;
        if (event->kind == SAO_UI_INPUT_MOUSE_MOVE)
            next_hover = target;
        if (event->kind == SAO_UI_INPUT_MOUSE_LEAVE)
            next_hover = nullptr;
        if (next_hover != previous_hover) {
            lease->hover_widget = next_hover;
            lease->hover_widget_generation = next_hover == nullptr ? 0 : target_generation;
            hover_callback = lease->hover_cb;
            hover_user_data = lease->hover_user_data;
            bump_transition_locked(lease.get());
        }
        const HotkeyMatch hotkey = select_hotkey_locked(lease.get(), *event);
        const uint64_t transition = lease->transition_generation;
        int32_t nudge_direction = 0;
        if (event->kind == SAO_UI_INPUT_KEY_DOWN && hotkey.binding == 0) {
            if (event->virtual_key == 0x25U || event->virtual_key == 0x28U)
                nudge_direction = -1;
            else if (event->virtual_key == 0x26U || event->virtual_key == 0x27U)
                nudge_direction = 1;
        }
        if (out_consumed != nullptr) {
            *out_consumed = target != nullptr || hotkey.prevent_default;
        }
        guard.unlock();
        const auto apply_state = [](sao_ui_widget_handle_t widget, auto setter,
                                    bool value) -> sao_status_t {
            if (widget == nullptr)
                return SAO_STATUS_OK;
            const sao_status_t status = setter(widget, value);
            return status == SAO_STATUS_ERR_NOT_IMPLEMENTED ? SAO_STATUS_OK : status;
        };
        if (next_hover != previous_hover) {
            sao_status_t state_status =
                apply_state(previous_hover, sao_ui_widget_set_hovered, false);
            if (state_status != SAO_STATUS_OK && state_status != SAO_STATUS_ERR_HANDLE_INVALID)
                return state_status;
            state_status = apply_state(next_hover, sao_ui_widget_set_hovered, true);
            if (state_status != SAO_STATUS_OK)
                return state_status == SAO_STATUS_ERR_HANDLE_INVALID ? SAO_UI_STATUS_ERR_BUSY
                                                                     : state_status;
        }
        if (event->kind == SAO_UI_INPUT_MOUSE_DOWN) {
            const sao_status_t state_status = apply_state(target, sao_ui_widget_set_pressed, true);
            if (state_status != SAO_STATUS_OK)
                return state_status == SAO_STATUS_ERR_HANDLE_INVALID ? SAO_UI_STATUS_ERR_BUSY
                                                                     : state_status;
        } else if (event->kind == SAO_UI_INPUT_MOUSE_UP ||
                   event->kind == SAO_UI_INPUT_MOUSE_LEAVE) {
            const sao_ui_widget_handle_t pressed_target =
                target != nullptr ? target : previous_hover;
            const sao_status_t state_status =
                apply_state(pressed_target, sao_ui_widget_set_pressed, false);
            if (state_status != SAO_STATUS_OK && state_status != SAO_STATUS_ERR_HANDLE_INVALID)
                return state_status;
        }
        if (event->kind == SAO_UI_INPUT_FOCUS_GAIN || event->kind == SAO_UI_INPUT_FOCUS_LOSE) {
            const sao_status_t state_status = apply_state(target, sao_ui_widget_set_focused,
                                                          event->kind == SAO_UI_INPUT_FOCUS_GAIN);
            if (state_status != SAO_STATUS_OK)
                return state_status == SAO_STATUS_ERR_HANDLE_INVALID ? SAO_UI_STATUS_ERR_BUSY
                                                                     : state_status;
        }
        if (hover_callback != nullptr) {
            RouterCallbackScope callback_scope(handle);
            hover_callback(previous_hover, next_hover, hover_user_data);
            if (!lease.transition_is_current(transition) ||
                (target != nullptr && !input_handle_is_active(target, target_generation))) {
                return SAO_UI_STATUS_ERR_BUSY;
            }
        }
        if (target != nullptr && nudge_direction != 0) {
            const sao_status_t nudge_status = sao_ui_widget_nudge_value(target, nudge_direction);
            if (nudge_status == SAO_STATUS_OK) {
                if (out_consumed != nullptr)
                    *out_consumed = true;
                if (!input_handle_is_active(target, target_generation))
                    return SAO_UI_STATUS_ERR_BUSY;
            } else if (nudge_status != SAO_STATUS_ERR_NOT_IMPLEMENTED &&
                       nudge_status != SAO_STATUS_ERR_ACCESS_DENIED) {
                return nudge_status;
            }
        }
        if (hotkey.callback != nullptr) {
            HotkeyDispatchLease hotkey_lease(hotkey.record);
            if (hotkey_lease) {
                RouterCallbackScope callback_scope(handle);
                HotkeyCallbackScope hotkey_scope(hotkey.record.get());
                hotkey.callback(hotkey.binding_id.c_str(), event, hotkey.user_data);
            }
            if (!hotkey_dispatch_completed(lease, transition, hotkey.binding))
                return SAO_UI_STATUS_ERR_BUSY;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Hit-region declarations ────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL
sao_ui_input_router_set_panel_region(sao_ui_input_router_deep_handle_t handle,
                                     sao_ui_panel_handle_t panel, const SaoUiHitRegion* region) {
    if (panel == nullptr || region == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (region->shape < SAO_UI_HIT_RECT || region->shape > SAO_UI_HIT_ALPHA) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (region->shape == SAO_UI_HIT_RECT && (region->w_or_r <= 0 || region->h_or_pt_count <= 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (region->shape == SAO_UI_HIT_CIRCLE && region->w_or_r <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (region->shape == SAO_UI_HIT_ALPHA && (region->w_or_r <= 0 || region->h_or_pt_count <= 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (region->shape == SAO_UI_HIT_POLYGON &&
        (region->h_or_pt_count < 3 || region->poly_verts_xy_pairs == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_input_router_deep_s::RegionCopy copy{};
        copy.shape = region->shape;
        copy.x_or_cx = region->x_or_cx;
        copy.y_or_cy = region->y_or_cy;
        copy.w_or_r = region->w_or_r;
        copy.h_or_pt_count = region->h_or_pt_count;
        copy.alpha_threshold = region->alpha_threshold;
        if (region->shape == SAO_UI_HIT_POLYGON) {
            copy.poly_verts.assign(region->poly_verts_xy_pairs,
                                   region->poly_verts_xy_pairs +
                                       static_cast<size_t>(region->h_or_pt_count) * 2);
        }
        std::lock_guard<std::mutex> guard(lease->mu);
        lease->regions[panel] = std::move(copy);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_clear_panel_region(
    sao_ui_input_router_deep_handle_t handle, sao_ui_panel_handle_t panel) {

    if (panel == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        lease->regions.erase(panel);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Focus stack ────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_set_focus_widget(
    sao_ui_input_router_deep_handle_t handle, sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        sao::ui::detail::WidgetHandleMetadata metadata{};
        if (!sao::ui::detail::inspect_widget_handle(widget, &metadata))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const uint64_t widget_generation = metadata.generation;
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_widget_handle_t previous = nullptr;
        {
            std::lock_guard<std::mutex> guard(lease->mu);
            prune_retired_widgets_locked(lease.get());
            previous = lease->focus_stack.empty() ? nullptr : lease->focus_stack.back().widget;
            FocusEntry entry{};
            entry.widget = widget;
            entry.widget_generation = widget_generation;
            entry.registration_order = lease->next_focus_registration_order++;
            if (!lease->focus_stack.empty())
                entry.panel = lease->focus_stack.back().panel;
            lease->focus_stack.push_back(entry);
            bump_transition_locked(lease.get());
        }
        if (previous != nullptr && previous != widget)
            (void)sao_ui_widget_set_focused(previous, false);
        const sao_status_t focus_status = sao_ui_widget_set_focused(widget, true);
        if (focus_status != SAO_STATUS_OK && focus_status != SAO_STATUS_ERR_NOT_IMPLEMENTED)
            return focus_status;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_input_router_focus_next(sao_ui_input_router_deep_handle_t handle, bool reverse) {

    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_widget_handle_t previous = nullptr;
        sao_ui_widget_handle_t next = nullptr;
        uint64_t previous_generation = 0;
        uint64_t next_generation = 0;
        uint64_t transition = 0;
        {
            std::lock_guard<std::mutex> guard(lease->mu);
            prune_retired_widgets_locked(lease.get());
            const size_t begin = lease->modal_top == SIZE_MAX ? 0 : lease->modal_top + 1;
            std::vector<size_t> focusable_indices;
            focusable_indices.reserve(lease->focus_stack.size() - begin);
            for (size_t index = begin; index < lease->focus_stack.size(); ++index) {
                const sao_ui_widget_handle_t widget = lease->focus_stack[index].widget;
                if (widget == nullptr)
                    continue;
                bool focusable = false;
                if (sao_ui_widget_is_focusable(widget, &focusable) == SAO_STATUS_OK && focusable) {
                    focusable_indices.push_back(index);
                }
            }
            if (focusable_indices.empty())
                return SAO_STATUS_ERR_NOT_FOUND;

            std::sort(focusable_indices.begin(), focusable_indices.end(),
                      [&lease](size_t left, size_t right) {
                          return lease->focus_stack[left].registration_order <
                                 lease->focus_stack[right].registration_order;
                      });

            previous = lease->focus_stack.empty() ? nullptr : lease->focus_stack.back().widget;
            previous_generation =
                lease->focus_stack.empty() ? 0 : lease->focus_stack.back().widget_generation;
            size_t target_position = reverse ? focusable_indices.size() - 1 : 0;
            const auto current =
                std::find_if(focusable_indices.begin(), focusable_indices.end(),
                             [&lease, previous](size_t index) {
                                 return lease->focus_stack[index].widget == previous;
                             });
            if (current != focusable_indices.end()) {
                const size_t position =
                    static_cast<size_t>(std::distance(focusable_indices.begin(), current));
                target_position =
                    reverse ? (position + focusable_indices.size() - 1) % focusable_indices.size()
                            : (position + 1) % focusable_indices.size();
            }

            const size_t target_index = focusable_indices[target_position];
            FocusEntry selected = lease->focus_stack[target_index];
            next = selected.widget;
            next_generation = selected.widget_generation;
            if (target_index + 1 != lease->focus_stack.size()) {
                lease->focus_stack.erase(lease->focus_stack.begin() +
                                         static_cast<std::ptrdiff_t>(target_index));
                lease->focus_stack.push_back(selected);
            }
            transition = bump_transition_locked(lease.get());
        }

        if (previous != nullptr && previous != next) {
            if (!input_handle_is_active(previous, previous_generation))
                return SAO_UI_STATUS_ERR_BUSY;
            (void)sao_ui_widget_set_focused(previous, false);
            RouterCallbackScope callback_scope(handle);
            const sao_status_t status =
                sao_ui_widget_dispatch_event(previous, SAO_UI_EVT_FOCUS_LOST, nullptr, 0);
            if (status != SAO_STATUS_OK)
                return status;
            if (!lease.transition_is_current(transition) ||
                !input_handle_is_active(previous, previous_generation) ||
                !input_handle_is_active(next, next_generation)) {
                return SAO_UI_STATUS_ERR_BUSY;
            }
        }
        if (next != nullptr && previous != next) {
            const sao_status_t focus_status = sao_ui_widget_set_focused(next, true);
            if (focus_status != SAO_STATUS_OK && focus_status != SAO_STATUS_ERR_NOT_IMPLEMENTED)
                return focus_status;
            RouterCallbackScope callback_scope(handle);
            const sao_status_t status =
                sao_ui_widget_dispatch_event(next, SAO_UI_EVT_FOCUS_GAINED, nullptr, 0);
            if (status != SAO_STATUS_OK)
                return status;
            if (!lease.transition_is_current(transition) ||
                !input_handle_is_active(next, next_generation)) {
                return SAO_UI_STATUS_ERR_BUSY;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_get_focus(
    sao_ui_input_router_deep_handle_t handle, sao_ui_widget_handle_t* out_widget,
    sao_ui_panel_handle_t* out_panel) {

    if (out_widget != nullptr)
        *out_widget = nullptr;
    if (out_panel != nullptr)
        *out_panel = nullptr;
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());
        if (out_widget != nullptr)
            *out_widget = lease->focus_stack.empty() ? nullptr : lease->focus_stack.back().widget;
        if (out_panel != nullptr)
            *out_panel = lease->focus_stack.empty() ? nullptr : lease->focus_stack.back().panel;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Push a modal barrier at the current top of stack.
extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_push_modal(
    sao_ui_input_router_deep_handle_t handle, sao_ui_panel_handle_t panel) {

    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());
        FocusEntry entry{};
        entry.panel = panel;
        entry.modal = true;
        lease->focus_stack.push_back(entry);
        lease->modal_top = lease->focus_stack.size() - 1;
        bump_transition_locked(lease.get());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_input_router_pop_modal(sao_ui_input_router_deep_handle_t handle) {

    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());
        if (lease->modal_top == SIZE_MAX)
            return SAO_STATUS_OK;
        while (!lease->focus_stack.empty()) {
            const bool was_modal = lease->focus_stack.back().modal;
            lease->focus_stack.pop_back();
            if (was_modal)
                break;
        }
        recompute_modal_top_locked(lease.get());
        bump_transition_locked(lease.get());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Hover ──────────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_set_hover_change_handler(
    sao_ui_input_router_deep_handle_t handle, sao_ui_hover_change_cb_t callback, void* user_data) {

    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        lease->hover_cb = callback;
        lease->hover_user_data = user_data;
        bump_transition_locked(lease.get());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Hotkey registration ────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_register_hotkey(
    sao_ui_input_router_deep_handle_t handle, const char* plugin_id_utf8,
    const SaoUiHotkeyBindingSpec* spec, sao_ui_hotkey_cb_t callback, void* user_data,
    sao_ui_hotkey_binding_t* out_binding) {
    if (spec == nullptr || spec->virtual_key == 0 || spec->scope < SAO_UI_HOTKEY_SCOPE_GLOBAL ||
        spec->scope > SAO_UI_HOTKEY_SCOPE_PANEL ||
        (spec->scope == SAO_UI_HOTKEY_SCOPE_PANEL &&
         (spec->scope_panel_id_utf8 == nullptr || spec->scope_panel_id_utf8[0] == '\0'))) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_binding != nullptr)
        *out_binding = 0;

    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);

        // enforce_ctrl_prefix guardrail — plugin hotkeys must carry a
        // hard CTRL bit or wildcard.  Main UI (F5-F12 bare) is registered
        // with enforce_ctrl_prefix=false.
        uint32_t effective_mods = spec->modifiers;
        if (spec->enforce_ctrl_prefix) {
            if (((effective_mods & SAO_UI_MOD_CTRL_BIT) == 0) &&
                ((effective_mods & SAO_UI_MOD_ANY_CTRL) == 0)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }

        auto rec = std::make_shared<HotkeyRecord>();
        rec->handle = lease->next_hotkey_handle++;
        rec->insert_order = lease->next_insert_order++;
        rec->plugin_id = (plugin_id_utf8 == nullptr ? "" : plugin_id_utf8);
        rec->binding_id = (spec->binding_id_utf8 == nullptr ? "" : spec->binding_id_utf8);
        rec->virtual_key = spec->virtual_key;
        rec->modifiers = effective_mods;
        rec->scope = spec->scope;
        rec->scope_panel_id =
            (spec->scope_panel_id_utf8 == nullptr ? "" : spec->scope_panel_id_utf8);
        rec->enforce_ctrl_prefix = spec->enforce_ctrl_prefix;
        rec->prevent_default = spec->prevent_default;
        rec->allow_repeat = spec->allow_repeat;
        rec->require_release = spec->require_release;
        rec->callback = callback;
        rec->user_data = user_data;

        lease->hotkeys.push_back(rec);
        if (out_binding != nullptr)
            *out_binding = rec->handle;
        bump_transition_locked(lease.get());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_unregister_hotkey(
    sao_ui_input_router_deep_handle_t handle, sao_ui_hotkey_binding_t binding) {

    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::shared_ptr<HotkeyRecord> removed;
        {
            std::lock_guard<std::mutex> guard(lease->mu);
            auto& hotkeys = lease->hotkeys;
            const auto found =
                std::find_if(hotkeys.begin(), hotkeys.end(),
                             [binding](const auto& record) { return record->handle == binding; });
            if (found == hotkeys.end())
                return SAO_STATUS_ERR_NOT_FOUND;
            removed = *found;
            hotkeys.erase(found);
            bump_transition_locked(lease.get());
        }
        retire_hotkey(removed);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_unregister_plugin_hotkeys(
    sao_ui_input_router_deep_handle_t handle, const char* plugin_id_utf8) {

    if (plugin_id_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const std::string needle(plugin_id_utf8);
        std::vector<std::shared_ptr<HotkeyRecord>> removed;
        {
            std::lock_guard<std::mutex> guard(lease->mu);
            auto& hotkeys = lease->hotkeys;
            for (const auto& record : hotkeys) {
                if (record->plugin_id == needle)
                    removed.push_back(record);
            }
            hotkeys.erase(std::remove_if(hotkeys.begin(), hotkeys.end(),
                                         [&needle](const auto& record) {
                                             return record->plugin_id == needle;
                                         }),
                          hotkeys.end());
            if (!removed.empty())
                bump_transition_locked(lease.get());
        }
        for (const auto& record : removed)
            retire_hotkey(record);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_find_conflicts(
    sao_ui_input_router_deep_handle_t handle, uint32_t virtual_key, uint32_t modifiers,
    sao_ui_hotkey_binding_t* out_bindings, size_t capacity, size_t* out_written) {

    if (out_written != nullptr)
        *out_written = 0;
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);

        size_t written = 0;
        for (const auto& record : lease->hotkeys) {
            if (record->virtual_key != virtual_key)
                continue;
            if (!binding_matches(record->modifiers, modifiers))
                continue;
            if (out_bindings != nullptr && written < capacity)
                out_bindings[written] = record->handle;
            ++written;
        }
        if (out_written != nullptr)
            *out_written = written;
        if (out_bindings != nullptr && written > capacity)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Subset matcher ─────────────────────────────────────────────────
//
// Called from the raw-Win32 feed path (or a test rig).  Given the
// key + modifiers pair from a KEY_DOWN event, walk the hotkey list
// picking every binding whose modifier spec is a subset of the
// observed modifier mask.  Return the most specific winner (largest
// hard-bit count) with oldest-first tie-break.  Fires its callback
// unless out_binding is nullptr — the settings UI passes nullptr to
// merely query.

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_match_hotkey(
    sao_ui_input_router_deep_handle_t handle, const SaoUiInputEvent* event,
    sao_ui_hotkey_binding_t* out_binding) {

    if (out_binding != nullptr)
        *out_binding = 0;
    if (event == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (event->kind != SAO_UI_INPUT_KEY_DOWN && event->kind != SAO_UI_INPUT_KEY_UP) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        HotkeyMatch match{};
        uint64_t transition = 0;
        {
            std::lock_guard<std::mutex> guard(lease->mu);
            match = select_hotkey_locked(lease.get(), *event);
            transition = lease->transition_generation;
        }
        if (match.binding == 0)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (out_binding != nullptr)
            *out_binding = match.binding;
        if (match.callback != nullptr) {
            HotkeyDispatchLease hotkey_lease(match.record);
            if (hotkey_lease) {
                RouterCallbackScope callback_scope(handle);
                HotkeyCallbackScope hotkey_scope(match.record.get());
                match.callback(match.binding_id.c_str(), event, match.user_data);
            }
            if (!hotkey_dispatch_completed(lease, transition, match.binding))
                return SAO_UI_STATUS_ERR_BUSY;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Mouse capture ──────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_capture_mouse(
    sao_ui_input_router_deep_handle_t handle, sao_ui_widget_handle_t captured_widget) {

    if (captured_widget == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        sao::ui::detail::WidgetHandleMetadata metadata{};
        if (!sao::ui::detail::inspect_widget_handle(captured_widget, &metadata)) {
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        const uint64_t widget_generation = metadata.generation;
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        lease->captured_widget = captured_widget;
        lease->captured_widget_generation = widget_generation;
        bump_transition_locked(lease.get());
#if defined(_WIN32)
        void* hwnd = sao_ui_compositor_host_hwnd(lease->compositor);
        if (hwnd != nullptr)
            ::SetCapture(reinterpret_cast<HWND>(hwnd));
#endif
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_input_router_release_mouse(sao_ui_input_router_deep_handle_t handle) {

    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mu);
        lease->captured_widget = nullptr;
        lease->captured_widget_generation = 0;
        bump_transition_locked(lease.get());
#if defined(_WIN32)
        if (::GetCapture() != nullptr)
            ::ReleaseCapture();
#endif
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Test-only helpers (introspection) ──────────────────────────────

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_input_router_hotkey_count(sao_ui_input_router_deep_handle_t handle) {
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return 0;
        std::lock_guard<std::mutex> guard(lease->mu);
        return lease->hotkeys.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_input_router_focus_depth(sao_ui_input_router_deep_handle_t handle) {
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return 0;
        std::lock_guard<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());
        return lease->focus_stack.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_UI_API sao_ui_widget_handle_t SAO_UI_CALL
sao_ui_input_router_last_route_target(sao_ui_input_router_deep_handle_t handle) {
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return nullptr;
        std::lock_guard<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());
        return lease->last_route_target;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_input_router_has_modal_barrier(sao_ui_input_router_deep_handle_t handle) {
    try {
        auto lease = acquire_router(handle);
        if (!lease)
            return false;
        std::lock_guard<std::mutex> guard(lease->mu);
        prune_retired_widgets_locked(lease.get());
        return lease->modal_top != SIZE_MAX;
    } catch (...) {
        return false;
    }
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_input_router_test_set_create_failure_point(int32_t point) {
    g_router_create_failure_point.store(point, std::memory_order_release);
}

extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_input_router_test_active_count() {
    try {
        auto& registry = router_handle_registry();
        std::lock_guard lock(registry.mu);
        return registry.active.size();
    } catch (...) {
        return 0;
    }
}
