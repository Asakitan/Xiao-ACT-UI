// SAO Auto — portable input widget state and callback dispatch.

#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_BTN_NORMAL == 0, "button kind enum drifted");
static_assert(SAO_UI_BTN_DANGER == 4, "button kind enum drifted");
static_assert(SAO_UI_DROPDOWN_SEPARATOR == -1, "dropdown separator sentinel drifted");

namespace {

constexpr int32_t kButtonTag = SAO_UI_WIDGET_BUTTON;
constexpr int32_t kIconButtonTag = SAO_UI_WIDGET_ICON_BUTTON;
constexpr int32_t kDropdownButtonTag = SAO_UI_WIDGET_DROPDOWN_BUTTON_EXT;
constexpr int32_t kCheckboxTag = SAO_UI_WIDGET_CHECKBOX_EXT;
constexpr int32_t kRadioTag = SAO_UI_WIDGET_RADIO_EXT;
constexpr int32_t kSliderTag = SAO_UI_WIDGET_SLIDER_EXT;
constexpr size_t kMaxDropdownEntries = 4096;
constexpr size_t kMaxIconBytes = 64U * 1024U * 1024U;

struct ButtonState {
    int32_t tag{kButtonTag};
    SaoUiButtonSpec spec{};
    std::string text; // owns text bytes
    // Layout cache — cheap to recompute but paid at every hit test.
    int32_t cached_width{0};
    int32_t cached_height{0};
    // Callback wiring.
    sao_ui_click_cb_t click_cb{nullptr};
    void* click_user_data{nullptr};
    // Interaction state — driven by dispatch_event.
    bool pressed{false}; // mouse-down within bounds
    bool hovered{false}; // last cursor was inside
    mutable std::mutex mtx;
};

struct IconButtonState {
    int32_t tag{kIconButtonTag};
    SaoUiIconButtonSpec spec{};
    std::vector<uint8_t> icon_pixels;
    std::string tooltip;
    sao_ui_click_cb_t click_cb{nullptr};
    void* click_user_data{nullptr};
    mutable std::mutex mtx;
};

struct OwnedDropdownEntry {
    std::string label;
    int32_t item_id{0};
    bool enabled{false};
    bool checked{false};
};

struct DropdownButtonState {
    int32_t tag{kDropdownButtonTag};
    SaoUiDropdownButtonSpec spec{};
    std::string text;
    std::vector<OwnedDropdownEntry> entries;
    sao_ui_dropdown_pick_cb_t pick_cb{nullptr};
    void* pick_user_data{nullptr};
    mutable std::mutex mtx;
};

struct CheckboxState {
    int32_t tag{kCheckboxTag};
    SaoUiCheckboxSpec spec{};
    std::string label;
    sao_ui_toggle_cb_t toggle_cb{nullptr};
    void* toggle_user_data{nullptr};
    mutable std::mutex mtx;
};

struct RadioState {
    int32_t tag{kRadioTag};
    sao_ui_widget_handle_t handle{nullptr};
    SaoUiRadioSpec spec{};
    std::string label;
    sao_ui_radio_pick_cb_t pick_cb{nullptr};
    void* pick_user_data{nullptr};
    mutable std::mutex mtx;
};

struct SliderState {
    int32_t tag{kSliderTag};
    SaoUiSliderSpec spec{};
    sao_ui_slider_change_cb_t change_cb{nullptr};
    void* change_user_data{nullptr};
    mutable std::mutex mtx;
};

struct RadioRegistry {
    std::mutex mtx;
    std::unordered_map<int32_t, std::vector<std::weak_ptr<RadioState>>> groups;
};

RadioRegistry& radio_registry() {
    static RadioRegistry registry;
    return registry;
}

struct InputHandleShell {
    int32_t tag{-1};
    uint32_t reserved{0};
    uint64_t generation{0};
};

struct InputHandleRecord {
    std::mutex lifecycle_mtx;
    std::condition_variable lifecycle_cv;
    std::recursive_mutex operation_mtx;
    sao_ui_widget_handle_t handle{nullptr};
    int32_t tag{-1};
    uint64_t generation{0};
    uint64_t transition_generation{1};
    std::shared_ptr<void> state;
    size_t in_flight{0};
    bool accepting{true};
    bool retired{false};
    bool finalization_started{false};
    bool finalized{false};
};

struct InputHandleRegistry {
    std::mutex mtx;
    std::unordered_map<sao_ui_widget_handle_t, std::shared_ptr<InputHandleRecord>> active;
    std::unordered_map<sao_ui_widget_handle_t, std::pair<int32_t, uint64_t>> known;
    std::vector<std::unique_ptr<InputHandleShell>> shells;
    std::atomic<uint64_t> next_generation{1};
};

InputHandleRegistry& input_handle_registry() {
    static InputHandleRegistry registry;
    return registry;
}

thread_local std::vector<sao_ui_widget_handle_t> current_callback_handles;

class CallbackHandleScope {
  public:
    explicit CallbackHandleScope(sao_ui_widget_handle_t handle) : handle_(handle) {
        current_callback_handles.push_back(handle);
    }

    ~CallbackHandleScope() {
        const auto found =
            std::find(current_callback_handles.rbegin(), current_callback_handles.rend(), handle_);
        if (found != current_callback_handles.rend())
            current_callback_handles.erase(std::next(found).base());
    }

    CallbackHandleScope(const CallbackHandleScope&) = delete;
    CallbackHandleScope& operator=(const CallbackHandleScope&) = delete;

  private:
    sao_ui_widget_handle_t handle_;
};

bool callback_owns_handle(sao_ui_widget_handle_t handle) {
    return std::find(current_callback_handles.begin(), current_callback_handles.end(), handle) !=
           current_callback_handles.end();
}

void unregister_radio(const std::shared_ptr<RadioState>& state) noexcept;

void finalize_input_record(const std::shared_ptr<InputHandleRecord>& record) noexcept {
    try {
        std::shared_ptr<void> state;
        {
            std::lock_guard<std::mutex> lock(record->lifecycle_mtx);
            state = record->state;
        }
        if (record->tag == kRadioTag && state != nullptr)
            unregister_radio(std::static_pointer_cast<RadioState>(state));
        {
            std::lock_guard<std::mutex> lock(record->lifecycle_mtx);
            record->state.reset();
            record->finalized = true;
        }
        record->lifecycle_cv.notify_all();
    } catch (...) {
        std::lock_guard<std::mutex> lock(record->lifecycle_mtx);
        record->state.reset();
        record->finalized = true;
        record->lifecycle_cv.notify_all();
    }
}

void release_input_lease(const std::shared_ptr<InputHandleRecord>& record) noexcept {
    bool finalize = false;
    {
        std::lock_guard<std::mutex> lock(record->lifecycle_mtx);
        if (record->in_flight != 0)
            --record->in_flight;
        if (record->retired && record->in_flight == 0 && !record->finalization_started) {
            record->finalization_started = true;
            finalize = true;
        }
    }
    record->lifecycle_cv.notify_all();
    if (finalize)
        finalize_input_record(record);
}

template <typename State> class InputLease {
  public:
    InputLease() = default;

    InputLease(std::shared_ptr<InputHandleRecord> record, std::shared_ptr<State> state,
               std::unique_lock<std::recursive_mutex> operation_lock)
        : record_(std::move(record)), state_(std::move(state)),
          operation_lock_(std::move(operation_lock)) {}

    ~InputLease() {
        reset();
    }

    InputLease(const InputLease&) = delete;
    InputLease& operator=(const InputLease&) = delete;
    InputLease(InputLease&&) noexcept = default;
    InputLease& operator=(InputLease&&) noexcept = default;

    explicit operator bool() const noexcept {
        return state_ != nullptr;
    }

    State* operator->() const noexcept {
        return state_.get();
    }

    State& operator*() const noexcept {
        return *state_;
    }

    sao_ui_widget_handle_t handle() const noexcept {
        return record_ == nullptr ? nullptr : record_->handle;
    }

    uint64_t transition_generation() const noexcept {
        if (record_ == nullptr)
            return 0;
        std::lock_guard<std::mutex> lock(record_->lifecycle_mtx);
        return record_->transition_generation;
    }

    bool transition_is_current(uint64_t expected) const noexcept {
        if (record_ == nullptr)
            return false;
        std::lock_guard<std::mutex> lock(record_->lifecycle_mtx);
        return record_->accepting && record_->transition_generation == expected;
    }

  private:
    void reset() noexcept {
        if (record_ == nullptr)
            return;
        if (operation_lock_.owns_lock())
            operation_lock_.unlock();
        release_input_lease(record_);
        state_.reset();
        record_.reset();
    }

    std::shared_ptr<InputHandleRecord> record_;
    std::shared_ptr<State> state_;
    std::unique_lock<std::recursive_mutex> operation_lock_;
};

template <typename State>
InputLease<State> acquire_input_lease(sao_ui_widget_handle_t handle, int32_t expected_tag) {
    if (handle == nullptr)
        return {};
    std::shared_ptr<InputHandleRecord> record;
    {
        auto& registry = input_handle_registry();
        std::lock_guard<std::mutex> registry_lock(registry.mtx);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end())
            return {};
        record = found->second;
        std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
        if (!record->accepting || record->retired || record->tag != expected_tag)
            return {};
        ++record->in_flight;
    }
    try {
        std::unique_lock<std::recursive_mutex> operation_lock(record->operation_mtx);
        std::shared_ptr<State> state;
        {
            std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
            state = std::static_pointer_cast<State>(record->state);
        }
        if (state == nullptr) {
            if (operation_lock.owns_lock())
                operation_lock.unlock();
            release_input_lease(record);
            return {};
        }
        return InputLease<State>(std::move(record), std::move(state), std::move(operation_lock));
    } catch (...) {
        release_input_lease(record);
        throw;
    }
}

template <typename State>
sao_ui_widget_handle_t register_input_state(int32_t tag, const std::shared_ptr<State>& state) {
    auto& registry = input_handle_registry();
    auto shell = std::make_unique<InputHandleShell>();
    auto record = std::make_shared<InputHandleRecord>();
    uint64_t generation = registry.next_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0)
        generation = registry.next_generation.fetch_add(1, std::memory_order_relaxed);
    shell->tag = tag;
    shell->generation = generation;
    auto* raw_shell = shell.get();
    const auto handle = reinterpret_cast<sao_ui_widget_handle_t>(raw_shell);
    record->handle = handle;
    record->tag = tag;
    record->generation = generation;
    record->state = state;

    std::lock_guard<std::mutex> lock(registry.mtx);
    registry.active.emplace(handle, record);
    struct RegistrationRollback {
        InputHandleRegistry& registry;
        sao_ui_widget_handle_t handle;
        bool committed{false};
        ~RegistrationRollback() {
            if (!committed) {
                registry.active.erase(handle);
                registry.known.erase(handle);
            }
        }
    } rollback{registry, handle};
    registry.known.emplace(handle, std::make_pair(tag, generation));
    registry.shells.push_back(std::move(shell));
    rollback.committed = true;
    return handle;
}

int32_t known_input_tag(sao_ui_widget_handle_t handle) noexcept {
    if (handle == nullptr)
        return -1;
    try {
        auto& registry = input_handle_registry();
        std::lock_guard<std::mutex> lock(registry.mtx);
        const auto found = registry.known.find(handle);
        return found == registry.known.end() ? -1 : found->second.first;
    } catch (...) {
        return -1;
    }
}

bool valid_button_kind(int32_t kind) {
    return kind >= SAO_UI_BTN_NORMAL && kind <= SAO_UI_BTN_DANGER;
}

bool valid_button_spec(const SaoUiButtonSpec& spec) {
    return valid_button_kind(spec.kind) && spec.radius_px >= 0 && spec.pad_x_px >= 0 &&
           spec.pad_y_px >= 0;
}

bool valid_icon_spec(const SaoUiIconButtonSpec& spec) {
    if (!valid_button_kind(spec.kind) || spec.radius_px < 0 || spec.pad_px < 0 ||
        spec.icon_bgra_pixels == nullptr || spec.icon_width == 0 || spec.icon_height == 0 ||
        spec.icon_width > std::numeric_limits<uint32_t>::max() / 4U ||
        spec.icon_stride < spec.icon_width * 4U) {
        return false;
    }
    return static_cast<uint64_t>(spec.icon_stride) * spec.icon_height <=
           static_cast<uint64_t>(kMaxIconBytes);
}

bool build_dropdown_entries(const SaoUiDropdownEntry* entries, size_t count,
                            std::vector<OwnedDropdownEntry>& out) {
    if ((entries == nullptr && count != 0) || count > kMaxDropdownEntries) {
        return false;
    }
    std::unordered_set<int32_t> ids;
    out.clear();
    out.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const SaoUiDropdownEntry& entry = entries[index];
        if (entry.item_id != SAO_UI_DROPDOWN_SEPARATOR) {
            if (entry.label_utf8 == nullptr || !ids.insert(entry.item_id).second) {
                return false;
            }
        }
        out.push_back({entry.label_utf8 == nullptr ? "" : entry.label_utf8, entry.item_id,
                       entry.enabled, entry.checked});
    }
    return true;
}

bool valid_checkbox_spec(const SaoUiCheckboxSpec& spec) {
    return spec.font_size_px >= 0 && spec.box_size_px >= 0;
}

bool valid_radio_spec(const SaoUiRadioSpec& spec) {
    return spec.font_size_px >= 0 && spec.ring_size_px >= 0;
}

bool valid_slider_spec(const SaoUiSliderSpec& spec) {
    return std::isfinite(spec.value) && std::isfinite(spec.min_value) &&
           std::isfinite(spec.max_value) && std::isfinite(spec.step) &&
           spec.min_value < spec.max_value && spec.step >= 0.0F && spec.track_thickness_px >= 0 &&
           spec.thumb_size_px >= 0;
}

float normalize_slider_value(const SaoUiSliderSpec& spec, float value) {
    float normalized = std::clamp(value, spec.min_value, spec.max_value);
    if (spec.step > 0.0F) {
        const double steps =
            std::round((static_cast<double>(normalized) - spec.min_value) / spec.step);
        normalized = static_cast<float>(spec.min_value + steps * spec.step);
        normalized = std::clamp(normalized, spec.min_value, spec.max_value);
    }
    return normalized;
}

bool slider_values_equal(float left, float right) {
    const float scale = std::max({1.0F, std::fabs(left), std::fabs(right)});
    return std::fabs(left - right) <= std::numeric_limits<float>::epsilon() * 4.0F * scale;
}

sao_status_t dispatch_generic_event(sao_ui_widget_handle_t handle, int32_t event_type,
                                    const char* payload) noexcept {
    try {
        const auto* bytes = reinterpret_cast<const uint8_t*>(payload);
        return sao_ui_widget_dispatch_event(handle, event_type, bytes,
                                            payload == nullptr ? 0 : std::strlen(payload));
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

template <typename Callback, typename... Args>
sao_status_t invoke_typed_callback(Callback callback, Args... args) noexcept {
    if (callback == nullptr)
        return SAO_STATUS_OK;
    try {
        callback(args...);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t combine_callback_status(sao_status_t generic_status, sao_status_t typed_status) {
    return generic_status != SAO_STATUS_OK ? generic_status : typed_status;
}

template <typename State, typename Callback, typename... Args>
sao_status_t dispatch_callbacks(InputLease<State>& lease, int32_t event_type, const char* payload,
                                Callback callback, Args... args) noexcept {
    const uint64_t transition = lease.transition_generation();
    const CallbackHandleScope callback_scope(lease.handle());
    const sao_status_t generic_status = dispatch_generic_event(lease.handle(), event_type, payload);
    if (generic_status != SAO_STATUS_OK)
        return generic_status;
    if (!lease.transition_is_current(transition))
        return SAO_UI_STATUS_ERR_BUSY;
    const sao_status_t typed_status = invoke_typed_callback(callback, args...);
    if (typed_status != SAO_STATUS_OK)
        return typed_status;
    return lease.transition_is_current(transition) ? SAO_STATUS_OK : SAO_UI_STATUS_ERR_BUSY;
}

// Fixed-width glyph model matches widget_text.cpp — Wave4 first slice
// uses an 8-px advance so measurements are stable in unit tests.
constexpr int32_t kAsciiGlyphAdvancePx = 8;

size_t utf8_glyph_count(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80)
            ++n;
    }
    return n;
}

// Compute preferred size from text + padding, mirroring the Python
// action_button intrinsic sizing (label glyph count × advance + 2 × pad).
void recompute_size_locked(ButtonState& s) {
    const int32_t pad_x = s.spec.pad_x_px > 0 ? s.spec.pad_x_px : 10;
    const int32_t pad_y = s.spec.pad_y_px > 0 ? s.spec.pad_y_px : 6;
    const int32_t glyph_h = 16; // fixed line height for now
    const int32_t text_w = static_cast<int32_t>(utf8_glyph_count(s.text)) * kAsciiGlyphAdvancePx;
    s.cached_width = text_w + 2 * pad_x;
    s.cached_height = glyph_h + 2 * pad_y;
}

void apply_button_spec_no_lock(ButtonState& s, const SaoUiButtonSpec* spec) {
    s.spec = *spec;
    s.text = spec->text_utf8 ? spec->text_utf8 : "";
    s.spec.text_utf8 = nullptr;
    recompute_size_locked(s);
}

void unregister_radio(const std::shared_ptr<RadioState>& state) noexcept {
    auto& registry = radio_registry();
    std::lock_guard<std::mutex> guard(registry.mtx);
    const auto found = registry.groups.find(state->spec.group_id);
    if (found == registry.groups.end())
        return;
    auto& members = found->second;
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&state](const auto& weak) {
                                     const auto member = weak.lock();
                                     return member == nullptr || member.get() == state.get();
                                 }),
                  members.end());
    if (members.empty())
        registry.groups.erase(found);
}

} // namespace

// ---------------------------------------------------------------------------
// Button ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_create(void* /*d3d_device_ptr*/,
                                                         const SaoUiButtonSpec* spec,
                                                         sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || !valid_button_spec(*spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<ButtonState>();
        apply_button_spec_no_lock(*state, spec);
        *out_handle = register_input_state(kButtonTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_update(sao_ui_widget_handle_t handle,
                                                         const SaoUiButtonSpec* spec) {
    if (spec == nullptr || !valid_button_spec(*spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        apply_button_spec_no_lock(*lease, spec);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_text(sao_ui_widget_handle_t handle,
                                                           const char* text_utf8) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        lease->text = text_utf8 ? text_utf8 : "";
        recompute_size_locked(*lease);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_active(sao_ui_widget_handle_t handle,
                                                             bool active) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        lease->spec.active = active;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_disabled(sao_ui_widget_handle_t handle,
                                                               bool disabled) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        lease->spec.disabled = disabled;
        lease->pressed = false;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_button_set_click_handler(sao_ui_widget_handle_t handle,
                                                                    sao_ui_click_cb_t callback,
                                                                    void* user_data) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        lease->click_cb = callback;
        lease->click_user_data = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Extended input widget ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_icon_button_create(void* /*d3d_device_ptr*/,
                                                              const SaoUiIconButtonSpec* spec,
                                                              sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || !valid_icon_spec(*spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<IconButtonState>();
        state->spec = *spec;
        state->tooltip = spec->tooltip_utf8 == nullptr ? "" : spec->tooltip_utf8;
        state->spec.icon_bgra_pixels = nullptr;
        state->spec.tooltip_utf8 = nullptr;
        const size_t byte_count = static_cast<size_t>(spec->icon_stride) * spec->icon_height;
        const auto* pixels = static_cast<const uint8_t*>(spec->icon_bgra_pixels);
        state->icon_pixels.assign(pixels, pixels + byte_count);
        *out_handle = register_input_state(kIconButtonTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_icon_button_set_click_handler(
    sao_ui_widget_handle_t handle, sao_ui_click_cb_t callback, void* user_data) {
    try {
        auto lease = acquire_input_lease<IconButtonState>(handle, kIconButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        lease->click_cb = callback;
        lease->click_user_data = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_icon_button_invoke(sao_ui_widget_handle_t handle) {
    try {
        auto lease = acquire_input_lease<IconButtonState>(handle, kIconButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_click_cb_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> guard(lease->mtx);
            callback = lease->click_cb;
            user_data = lease->click_user_data;
        }
        return dispatch_callbacks(lease, SAO_UI_EVT_CLICK, "{\"source\":\"input\"}", callback,
                                  user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_create(void* /*d3d_device_ptr*/, const SaoUiDropdownButtonSpec* spec,
                              sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || !valid_button_kind(spec->kind)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<OwnedDropdownEntry> entries;
        if (!build_dropdown_entries(spec->entries, spec->entry_count, entries)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto state = std::make_shared<DropdownButtonState>();
        state->spec = *spec;
        state->text = spec->text_utf8 == nullptr ? "" : spec->text_utf8;
        state->entries = std::move(entries);
        state->spec.text_utf8 = nullptr;
        state->spec.entries = nullptr;
        state->spec.entry_count = 0;
        *out_handle = register_input_state(kDropdownButtonTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dropdown_button_set_entries(
    sao_ui_widget_handle_t handle, const SaoUiDropdownEntry* entries, size_t entry_count) {
    try {
        std::vector<OwnedDropdownEntry> candidate;
        if (!build_dropdown_entries(entries, entry_count, candidate)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto lease = acquire_input_lease<DropdownButtonState>(handle, kDropdownButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        lease->entries = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dropdown_button_set_pick_handler(
    sao_ui_widget_handle_t handle, sao_ui_dropdown_pick_cb_t callback, void* user_data) {
    try {
        auto lease = acquire_input_lease<DropdownButtonState>(handle, kDropdownButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        lease->pick_cb = callback;
        lease->pick_user_data = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dropdown_button_select(sao_ui_widget_handle_t handle,
                                                                  int32_t item_id) {
    try {
        auto lease = acquire_input_lease<DropdownButtonState>(handle, kDropdownButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_dropdown_pick_cb_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> guard(lease->mtx);
            const auto found = std::find_if(
                lease->entries.begin(), lease->entries.end(),
                [item_id](const OwnedDropdownEntry& entry) { return entry.item_id == item_id; });
            if (found == lease->entries.end() || found->item_id == SAO_UI_DROPDOWN_SEPARATOR)
                return SAO_STATUS_ERR_NOT_FOUND;
            if (!found->enabled)
                return SAO_STATUS_ERR_ACCESS_DENIED;
            callback = lease->pick_cb;
            user_data = lease->pick_user_data;
        }
        char payload[64]{};
        std::snprintf(payload, sizeof(payload), "{\"item_id\":%d}", item_id);
        return dispatch_callbacks(lease, SAO_UI_EVT_SELECTION_CHANGED, payload, callback, item_id,
                                  user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_checkbox_create(void* /*d3d_device_ptr*/,
                                                           const SaoUiCheckboxSpec* spec,
                                                           sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || !valid_checkbox_spec(*spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<CheckboxState>();
        state->spec = *spec;
        state->label = spec->label_utf8 == nullptr ? "" : spec->label_utf8;
        state->spec.label_utf8 = nullptr;
        *out_handle = register_input_state(kCheckboxTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_checkbox_set_checked(sao_ui_widget_handle_t handle,
                                                                bool checked) {
    try {
        auto lease = acquire_input_lease<CheckboxState>(handle, kCheckboxTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_toggle_cb_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> guard(lease->mtx);
            if (lease->spec.checked == checked)
                return SAO_STATUS_OK;
            lease->spec.checked = checked;
            callback = lease->toggle_cb;
            user_data = lease->toggle_user_data;
        }
        const char* payload = checked ? "{\"checked\":true}" : "{\"checked\":false}";
        return dispatch_callbacks(lease, SAO_UI_EVT_VALUE_CHANGED, payload, callback, checked,
                                  user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_checkbox_set_toggle_handler(
    sao_ui_widget_handle_t handle, sao_ui_toggle_cb_t callback, void* user_data) {
    try {
        auto lease = acquire_input_lease<CheckboxState>(handle, kCheckboxTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        lease->toggle_cb = callback;
        lease->toggle_user_data = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_checkbox_toggle(sao_ui_widget_handle_t handle) {
    try {
        auto lease = acquire_input_lease<CheckboxState>(handle, kCheckboxTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        bool next = false;
        sao_ui_toggle_cb_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> guard(lease->mtx);
            if (lease->spec.disabled)
                return SAO_STATUS_ERR_ACCESS_DENIED;
            next = !lease->spec.checked;
            lease->spec.checked = next;
            callback = lease->toggle_cb;
            user_data = lease->toggle_user_data;
        }
        const char* payload = next ? "{\"checked\":true}" : "{\"checked\":false}";
        return dispatch_callbacks(lease, SAO_UI_EVT_VALUE_CHANGED, payload, callback, next,
                                  user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_checkbox_get_checked(sao_ui_widget_handle_t handle,
                                                                bool* out_checked) {
    if (out_checked == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_input_lease<CheckboxState>(handle, kCheckboxTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        *out_checked = lease->spec.checked;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_radio_create(void* /*d3d_device_ptr*/,
                                                        const SaoUiRadioSpec* spec,
                                                        sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || !valid_radio_spec(*spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<RadioState>();
        state->spec = *spec;
        state->label = spec->label_utf8 == nullptr ? "" : spec->label_utf8;
        state->spec.label_utf8 = nullptr;
        auto& registry = radio_registry();
        std::vector<std::shared_ptr<RadioState>> deselected;
        {
            std::lock_guard<std::mutex> group_guard(registry.mtx);
            const bool group_existed = registry.groups.contains(spec->group_id);
            auto& group = registry.groups[spec->group_id];
            try {
                group.erase(std::remove_if(group.begin(), group.end(),
                                           [](const auto& weak) { return weak.expired(); }),
                            group.end());
                if (spec->selected) {
                    deselected.reserve(group.size());
                    for (const auto& weak : group) {
                        const auto member = weak.lock();
                        if (member == nullptr)
                            continue;
                        std::lock_guard<std::mutex> member_guard(member->mtx);
                        if (member->spec.selected) {
                            member->spec.selected = false;
                            deselected.push_back(member);
                        }
                    }
                }
                group.push_back(state);
            } catch (...) {
                for (const auto& member : deselected) {
                    std::lock_guard<std::mutex> member_guard(member->mtx);
                    member->spec.selected = true;
                }
                if (!group_existed && group.empty())
                    registry.groups.erase(spec->group_id);
                throw;
            }
        }
        try {
            *out_handle = register_input_state(kRadioTag, state);
            state->handle = *out_handle;
        } catch (...) {
            unregister_radio(state);
            for (const auto& member : deselected) {
                std::lock_guard<std::mutex> member_guard(member->mtx);
                member->spec.selected = true;
            }
            throw;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_radio_set_group_pick_handler(
    sao_ui_widget_handle_t handle, sao_ui_radio_pick_cb_t callback, void* user_data) {
    try {
        auto lease = acquire_input_lease<RadioState>(handle, kRadioTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        lease->pick_cb = callback;
        lease->pick_user_data = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_radio_set_selected(sao_ui_widget_handle_t handle,
                                                              bool selected) {
    try {
        auto lease = acquire_input_lease<RadioState>(handle, kRadioTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::vector<sao_ui_widget_handle_t> deselected;
        sao_ui_radio_pick_cb_t callback = nullptr;
        void* user_data = nullptr;
        int32_t group_id = 0;
        int32_t value_id = 0;
        auto& registry = radio_registry();
        {
            std::lock_guard<std::mutex> group_guard(registry.mtx);
            std::lock_guard<std::mutex> state_guard(lease->mtx);
            if (selected && lease->spec.disabled)
                return SAO_STATUS_ERR_ACCESS_DENIED;
            if (lease->spec.selected == selected)
                return SAO_STATUS_OK;
            group_id = lease->spec.group_id;
            value_id = lease->spec.value_id;
            if (selected) {
                const auto group = registry.groups.find(group_id);
                if (group != registry.groups.end()) {
                    deselected.reserve(group->second.size());
                    for (const auto& weak : group->second) {
                        const auto member = weak.lock();
                        if (member == nullptr || member.get() == &*lease)
                            continue;
                        std::lock_guard<std::mutex> member_guard(member->mtx);
                        if (member->spec.selected) {
                            member->spec.selected = false;
                            deselected.push_back(member->handle);
                        }
                    }
                }
            }
            lease->spec.selected = selected;
            callback = lease->pick_cb;
            user_data = lease->pick_user_data;
        }
        const CallbackHandleScope outer_scope(handle);
        const uint64_t transition = lease.transition_generation();
        for (const sao_ui_widget_handle_t member : deselected) {
            const CallbackHandleScope member_scope(member);
            const sao_status_t status = dispatch_generic_event(
                member, SAO_UI_EVT_VALUE_CHANGED, "{\"selected\":false}");
            if (status != SAO_STATUS_OK)
                return status;
            if (!lease.transition_is_current(transition)) {
                return SAO_UI_STATUS_ERR_BUSY;
            }
        }
        sao_status_t status =
            dispatch_generic_event(handle, SAO_UI_EVT_VALUE_CHANGED,
                                   selected ? "{\"selected\":true}" : "{\"selected\":false}");
        if (status != SAO_STATUS_OK)
            return status;
        if (!lease.transition_is_current(transition))
            return SAO_UI_STATUS_ERR_BUSY;
        if (!selected)
            return SAO_STATUS_OK;
        char payload[96]{};
        std::snprintf(payload, sizeof(payload), "{\"group_id\":%d,\"value_id\":%d}", group_id,
                      value_id);
        status = dispatch_generic_event(handle, SAO_UI_EVT_SELECTION_CHANGED, payload);
        if (status != SAO_STATUS_OK)
            return status;
        if (!lease.transition_is_current(transition))
            return SAO_UI_STATUS_ERR_BUSY;
        status = invoke_typed_callback(callback, group_id, value_id, user_data);
        if (status != SAO_STATUS_OK)
            return status;
        return lease.transition_is_current(transition) ? SAO_STATUS_OK : SAO_UI_STATUS_ERR_BUSY;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_radio_get_selected(sao_ui_widget_handle_t handle,
                                                              bool* out_selected) {
    if (out_selected == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_input_lease<RadioState>(handle, kRadioTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        *out_selected = lease->spec.selected;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_slider_create(void* /*d3d_device_ptr*/,
                                                         const SaoUiSliderSpec* spec,
                                                         sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || !valid_slider_spec(*spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<SliderState>();
        state->spec = *spec;
        state->spec.value = normalize_slider_value(*spec, spec->value);
        *out_handle = register_input_state(kSliderTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_slider_set_value(sao_ui_widget_handle_t handle,
                                                            float value) {
    if (!std::isfinite(value))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_input_lease<SliderState>(handle, kSliderTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        float normalized = 0.0F;
        sao_ui_slider_change_cb_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> guard(lease->mtx);
            normalized = normalize_slider_value(lease->spec, value);
            if (slider_values_equal(lease->spec.value, normalized))
                return SAO_STATUS_OK;
            lease->spec.value = normalized;
            callback = lease->change_cb;
            user_data = lease->change_user_data;
        }
        char payload[64]{};
        std::snprintf(payload, sizeof(payload), "{\"value\":%.9g}",
                      static_cast<double>(normalized));
        return dispatch_callbacks(lease, SAO_UI_EVT_VALUE_CHANGED, payload, callback, normalized,
                                  user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_slider_set_change_handler(
    sao_ui_widget_handle_t handle, sao_ui_slider_change_cb_t callback, void* user_data) {
    try {
        auto lease = acquire_input_lease<SliderState>(handle, kSliderTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        lease->change_cb = callback;
        lease->change_user_data = user_data;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_slider_get_value(sao_ui_widget_handle_t handle,
                                                            float* out_value) {
    if (out_value == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_input_lease<SliderState>(handle, kSliderTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> guard(lease->mtx);
        *out_value = lease->spec.value;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_widget_input_is_focusable(sao_ui_widget_handle_t handle,
                                                                     bool* out_focusable) {
    if (out_focusable == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_focusable = false;
    try {
        switch (known_input_tag(handle)) {
        case kButtonTag: {
            auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> guard(lease->mtx);
            *out_focusable = !lease->spec.disabled;
            return SAO_STATUS_OK;
        }
        case kIconButtonTag:
        case kDropdownButtonTag:
            *out_focusable = true;
            return sao_ui_widget_input_get_generation(handle, nullptr) == SAO_STATUS_OK
                       ? SAO_STATUS_OK
                       : SAO_STATUS_ERR_HANDLE_INVALID;
        case kCheckboxTag: {
            auto lease = acquire_input_lease<CheckboxState>(handle, kCheckboxTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> guard(lease->mtx);
            *out_focusable = !lease->spec.disabled;
            return SAO_STATUS_OK;
        }
        case kRadioTag: {
            auto lease = acquire_input_lease<RadioState>(handle, kRadioTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> guard(lease->mtx);
            *out_focusable = !lease->spec.disabled;
            return SAO_STATUS_OK;
        }
        case kSliderTag: {
            auto lease = acquire_input_lease<SliderState>(handle, kSliderTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> guard(lease->mtx);
            *out_focusable = !lease->spec.disabled;
            return SAO_STATUS_OK;
        }
        default:
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_widget_input_get_generation(sao_ui_widget_handle_t handle, uint64_t* out_generation) {
    if (out_generation != nullptr)
        *out_generation = 0;
    try {
        auto& registry = input_handle_registry();
        std::lock_guard<std::mutex> registry_lock(registry.mtx);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end())
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const auto& record = found->second;
        std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
        if (!record->accepting || record->retired)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (out_generation != nullptr)
            *out_generation = record->generation;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Wave 4 helper API — hit test + event dispatch + preferred-size query.
// Not part of widget_input.h (yet); exposed for the test binary to
// exercise the state machine without dragging in the compositor.
// ---------------------------------------------------------------------------

struct SaoUiPointF {
    float x;
    float y;
};

// Preferred size in pixels (matches action_button intrinsic sizing).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_preferred_size(
    sao_ui_widget_handle_t handle, int32_t* out_width, int32_t* out_height) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        if (out_width)
            *out_width = lease->cached_width;
        if (out_height)
            *out_height = lease->cached_height;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Hit test relative to the button's local coord (0..width, 0..height).
// Disabled buttons still report inside/outside but the caller can
// gate downstream dispatch on that separately.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_button_hit_test(sao_ui_widget_handle_t handle, SaoUiPointF point, bool* out_hit) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        const bool hit = point.x >= 0.0F && point.y >= 0.0F &&
                         point.x < static_cast<float>(lease->cached_width) &&
                         point.y < static_cast<float>(lease->cached_height);
        if (out_hit)
            *out_hit = hit;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Wave 4 event codes — locally aligned with widget_kit.h
// SAO_UI_EVT_* range but redeclared to keep this TU independent.
enum ButtonEvent : int32_t {
    kBtnEvtHoverEnter = 3,
    kBtnEvtHoverLeave = 4,
    kBtnEvtMouseDown = 100,
    kBtnEvtMouseUp = 101,
    kBtnEvtClick = 0, // aligns with SAO_UI_EVT_CLICK
};

// Dispatch a mouse event.  mouse_down → sets pressed; mouse_up → if
// still pressed AND still hovered, fires click.  Disabled buttons
// consume the event silently and never fire the callback.  Returns
// out_action_id = kind on click (caller-visible identifier), -1 otherwise.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_dispatch_event(
    sao_ui_widget_handle_t handle, int32_t event_type, int32_t* out_action_id) {
    if (out_action_id)
        *out_action_id = -1;
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_click_cb_t callback = nullptr;
        void* user_data = nullptr;
        bool fire = false;
        {
            std::lock_guard<std::mutex> lk(lease->mtx);
            switch (event_type) {
            case kBtnEvtHoverEnter:
                lease->hovered = true;
                break;
            case kBtnEvtHoverLeave:
                lease->hovered = false;
                lease->pressed = false;
                break;
            case kBtnEvtMouseDown:
                if (!lease->spec.disabled)
                    lease->pressed = true;
                break;
            case kBtnEvtMouseUp:
                if (!lease->spec.disabled && lease->pressed) {
                    fire = true;
                    if (out_action_id)
                        *out_action_id = lease->spec.kind;
                    callback = lease->click_cb;
                    user_data = lease->click_user_data;
                }
                lease->pressed = false;
                break;
            case kBtnEvtClick:
                if (!lease->spec.disabled) {
                    fire = true;
                    if (out_action_id)
                        *out_action_id = lease->spec.kind;
                    callback = lease->click_cb;
                    user_data = lease->click_user_data;
                }
                break;
            default:
                break;
            }
        }
        if (!fire)
            return SAO_STATUS_OK;
        return dispatch_callbacks(lease, SAO_UI_EVT_CLICK, "{\"source\":\"input\"}", callback,
                                  user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Query whether the button is in its "active" style (matches
// action_button.set_active).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_button_is_active(sao_ui_widget_handle_t handle, bool* out_active) {
    try {
        auto lease = acquire_input_lease<ButtonState>(handle, kButtonTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        if (out_active)
            *out_active = lease->spec.active;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Shared destroy helper.  Follows the same tag-dispatch convention as
// widget_text.cpp.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        std::shared_ptr<InputHandleRecord> record;
        auto& registry = input_handle_registry();
        {
            std::lock_guard<std::mutex> registry_lock(registry.mtx);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end())
                return;
            record = found->second;
            {
                std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
                if (!record->retired) {
                    record->accepting = false;
                    record->retired = true;
                    ++record->transition_generation;
                }
            }
            registry.active.erase(found);
        }

        uint32_t removed = 0;
        (void)sao_ui_widget_release_event_handlers(handle, &removed);
        if (callback_owns_handle(handle))
            return;

        bool finalize = false;
        std::unique_lock<std::mutex> lifecycle_lock(record->lifecycle_mtx);
        while (!record->finalized) {
            if (record->in_flight == 0 && !record->finalization_started) {
                record->finalization_started = true;
                finalize = true;
                break;
            }
            record->lifecycle_cv.wait(lifecycle_lock);
        }
        lifecycle_lock.unlock();
        if (finalize)
            finalize_input_record(record);
    } catch (...) {
    }
}
