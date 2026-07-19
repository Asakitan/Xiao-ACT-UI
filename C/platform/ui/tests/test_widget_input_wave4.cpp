// Wave 4 tests for the Button widget (G3.8 first slice).
//
// Coverage (5 CASE):
//   * button_create_stores_spec_and_text
//   * button_preferred_size_matches_text_and_padding
//   * button_hit_test_inside_and_outside
//   * button_dispatch_click_fires_callback_with_action_id
//   * button_disabled_button_swallows_click
//
// All tests are pure state-machine — no D3D device.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"

// ── Wave 4 helper API prototypes (not in widget_input.h) ───────────

extern "C" {

struct SaoUiPointF {
    float x;
    float y;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_preferred_size(
    sao_ui_widget_handle_t handle, int32_t* out_width, int32_t* out_height);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_hit_test(sao_ui_widget_handle_t handle,
                                                                  SaoUiPointF point, bool* out_hit);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_dispatch_event(
    sao_ui_widget_handle_t handle, int32_t event_type, int32_t* out_action_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_is_active(sao_ui_widget_handle_t handle,
                                                                   bool* out_active);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle);

} // extern "C"

namespace {

// Match widget_input.cpp private enum.
constexpr int32_t kBtnEvtMouseDown = 100;
constexpr int32_t kBtnEvtMouseUp = 101;

SaoUiButtonSpec make_button_spec(const char* text, int32_t kind = SAO_UI_BTN_NORMAL,
                                 bool active = false, bool disabled = false, int32_t pad_x = 10,
                                 int32_t pad_y = 6) {
    SaoUiButtonSpec spec{};
    spec.text_utf8 = text;
    spec.kind = kind;
    spec.radius_px = 4;
    spec.pad_x_px = pad_x;
    spec.pad_y_px = pad_y;
    spec.active = active;
    spec.disabled = disabled;
    return spec;
}

struct ClickSink {
    std::atomic<int> hits{0};
};

struct InputSink {
    int clicks{0};
    int picks{0};
    int toggles{0};
    int radio_picks{0};
    int slider_changes{0};
    int generic_events{0};
    int32_t last_item{-1};
    int32_t last_group{-1};
    int32_t last_value{-1};
    bool last_checked{false};
    float last_slider{0.0F};
    std::string last_payload;
};

struct DestroySink {
    sao_ui_widget_handle_t handle{nullptr};
    int callbacks{0};
};

extern "C" void SAO_UI_CALL sink_click_cb(void* user_data) {
    if (user_data != nullptr) {
        static_cast<ClickSink*>(user_data)->hits.fetch_add(1);
    }
}

extern "C" void SAO_UI_CALL input_click_cb(void* user_data) {
    static_cast<InputSink*>(user_data)->clicks++;
}

extern "C" void SAO_UI_CALL input_pick_cb(int32_t item_id, void* user_data) {
    auto* sink = static_cast<InputSink*>(user_data);
    sink->picks++;
    sink->last_item = item_id;
}

extern "C" void SAO_UI_CALL input_toggle_cb(bool checked, void* user_data) {
    auto* sink = static_cast<InputSink*>(user_data);
    sink->toggles++;
    sink->last_checked = checked;
}

extern "C" void SAO_UI_CALL input_radio_cb(int32_t group_id, int32_t value_id, void* user_data) {
    auto* sink = static_cast<InputSink*>(user_data);
    sink->radio_picks++;
    sink->last_group = group_id;
    sink->last_value = value_id;
}

extern "C" void SAO_UI_CALL input_slider_cb(float value, void* user_data) {
    auto* sink = static_cast<InputSink*>(user_data);
    sink->slider_changes++;
    sink->last_slider = value;
}

extern "C" void SAO_UI_CALL input_generic_cb(int32_t, const uint8_t* payload, size_t payload_len,
                                             void* user_data) {
    auto* sink = static_cast<InputSink*>(user_data);
    sink->generic_events++;
    sink->last_payload.assign(payload == nullptr ? "" : reinterpret_cast<const char*>(payload),
                              payload_len);
}

extern "C" void SAO_UI_CALL destroy_peer_generic_cb(int32_t, const uint8_t*, size_t,
                                                    void* user_data) {
    auto* sink = static_cast<DestroySink*>(user_data);
    sink->callbacks++;
    sao_ui_widget_destroy(sink->handle);
}

void SAO_UI_CALL throwing_click_cb(void*) {
    throw std::runtime_error("input callback failure");
}

} // namespace

TEST_CASE("button_create_stores_spec_and_text", "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("OK");
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(h != nullptr);
    bool active = true;
    REQUIRE(sao_ui_widget_button_is_active(h, &active) == SAO_STATUS_OK);
    REQUIRE(active == false);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_preferred_size_matches_text_and_padding", "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("hello", SAO_UI_BTN_NORMAL, false, false, 8, 4);
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_widget_button_preferred_size(h, &w, &hpx) == SAO_STATUS_OK);
    // "hello" = 5 glyphs × 8 = 40 px, + 2×pad_x 8 = 56 px total width.
    REQUIRE(w == 5 * 8 + 2 * 8);
    // Glyph height 16 + 2×pad_y 4 = 24.
    REQUIRE(hpx == 16 + 2 * 4);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_hit_test_inside_and_outside", "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("go", SAO_UI_BTN_NORMAL, false, false, 4, 2);
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_widget_button_preferred_size(h, &w, &hpx) == SAO_STATUS_OK);
    bool hit = false;
    // Origin inside.
    REQUIRE(sao_ui_widget_button_hit_test(h, {0.5f, 0.5f}, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == true);
    // Right beyond the right edge.
    REQUIRE(sao_ui_widget_button_hit_test(h, {static_cast<float>(w) + 1.0f, 1.0f}, &hit) ==
            SAO_STATUS_OK);
    REQUIRE(hit == false);
    // Negative.
    REQUIRE(sao_ui_widget_button_hit_test(h, {-1.0f, 0.0f}, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == false);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_dispatch_click_fires_callback_with_action_id", "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("primary", SAO_UI_BTN_GOLD);
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    ClickSink sink;
    REQUIRE(sao_ui_button_set_click_handler(h, sink_click_cb, &sink) == SAO_STATUS_OK);
    int32_t action_id = -1;
    // Full down/up cycle → click.
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseDown, &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 0); // no click on down alone
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseUp, &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 1);
    REQUIRE(action_id == SAO_UI_BTN_GOLD);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_disabled_button_swallows_click", "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("no");
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    ClickSink sink;
    REQUIRE(sao_ui_button_set_click_handler(h, sink_click_cb, &sink) == SAO_STATUS_OK);
    REQUIRE(sao_ui_button_set_disabled(h, true) == SAO_STATUS_OK);
    int32_t action_id = -1;
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseDown, &action_id) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseUp, &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 0);
    REQUIRE(action_id == -1);
    // Re-enable → clicks fire again.
    REQUIRE(sao_ui_button_set_disabled(h, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseDown, &action_id) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseUp, &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 1);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("portable_icon_button_owns_pixels_and_dispatches_callbacks",
          "[ui][widget][input][portable]") {
    uint8_t pixels[16]{1, 2, 3, 4};
    SaoUiIconButtonSpec spec{};
    spec.icon_bgra_pixels = pixels;
    spec.icon_width = 2;
    spec.icon_height = 2;
    spec.icon_stride = 8;
    spec.tooltip_utf8 = "icon";
    spec.kind = SAO_UI_BTN_CYAN;

    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_icon_button_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);
    InputSink sink;
    uint64_t token = 0;
    REQUIRE(sao_ui_icon_button_set_click_handler(handle, input_click_cb, &sink) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(handle, SAO_UI_EVT_CLICK, input_generic_cb, &sink,
                                            &token) == SAO_STATUS_OK);
    pixels[0] = 99;
    REQUIRE(sao_ui_icon_button_invoke(handle) == SAO_STATUS_OK);
    REQUIRE(sink.clicks == 1);
    REQUIRE(sink.generic_events == 1);
    REQUIRE(sink.last_payload == "{\"source\":\"input\"}");
    sao_ui_widget_destroy(handle);

    spec.icon_bgra_pixels = nullptr;
    handle = nullptr;
    REQUIRE(sao_ui_icon_button_create(nullptr, &spec, &handle) == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("portable_dropdown_rebuild_validates_and_selects_enabled_entries",
          "[ui][widget][input][portable]") {
    SaoUiDropdownEntry entries[] = {
        {"One", 1, true, false, {}},
        {nullptr, SAO_UI_DROPDOWN_SEPARATOR, false, false, {}},
        {"Two", 2, false, false, {}},
    };
    SaoUiDropdownButtonSpec spec{};
    spec.text_utf8 = "Pick";
    spec.kind = SAO_UI_BTN_NORMAL;
    spec.entries = entries;
    spec.entry_count = 3;

    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_dropdown_button_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    InputSink sink;
    uint64_t token = 0;
    REQUIRE(sao_ui_dropdown_button_set_pick_handler(handle, input_pick_cb, &sink) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(handle, SAO_UI_EVT_SELECTION_CHANGED, input_generic_cb,
                                            &sink, &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dropdown_button_select(handle, 1) == SAO_STATUS_OK);
    REQUIRE(sink.picks == 1);
    REQUIRE(sink.last_item == 1);
    REQUIRE(sink.last_payload == "{\"item_id\":1}");
    REQUIRE(sao_ui_dropdown_button_select(handle, 2) == SAO_STATUS_ERR_ACCESS_DENIED);
    REQUIRE(sao_ui_dropdown_button_select(handle, 99) == SAO_STATUS_ERR_NOT_FOUND);

    SaoUiDropdownEntry duplicate[] = {
        {"A", 4, true, false, {}},
        {"B", 4, true, false, {}},
    };
    REQUIRE(sao_ui_dropdown_button_set_entries(handle, duplicate, 2) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_dropdown_button_select(handle, 1) == SAO_STATUS_OK);
    REQUIRE(sink.picks == 2);
    REQUIRE(sao_ui_dropdown_button_set_entries(handle, nullptr, 1) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_widget_destroy(handle);
}

TEST_CASE("portable_checkbox_toggle_state_callback_and_disabled_gate",
          "[ui][widget][input][portable]") {
    SaoUiCheckboxSpec spec{};
    spec.label_utf8 = "Enabled";
    spec.box_size_px = 16;
    spec.font_size_px = 14;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_checkbox_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    InputSink sink;
    uint64_t token = 0;
    REQUIRE(sao_ui_checkbox_set_toggle_handler(handle, input_toggle_cb, &sink) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(handle, SAO_UI_EVT_VALUE_CHANGED, input_generic_cb,
                                            &sink, &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_checkbox_toggle(handle) == SAO_STATUS_OK);
    bool checked = false;
    REQUIRE(sao_ui_checkbox_get_checked(handle, &checked) == SAO_STATUS_OK);
    REQUIRE(checked);
    REQUIRE(sink.toggles == 1);
    REQUIRE(sink.last_checked);
    REQUIRE(sink.generic_events == 1);
    REQUIRE(sao_ui_checkbox_set_checked(handle, true) == SAO_STATUS_OK);
    REQUIRE(sink.toggles == 1);
    sao_ui_widget_destroy(handle);

    spec.disabled = true;
    REQUIRE(sao_ui_checkbox_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_checkbox_toggle(handle) == SAO_STATUS_ERR_ACCESS_DENIED);
    bool focusable = true;
    REQUIRE(sao_ui_widget_input_is_focusable(handle, &focusable) == SAO_STATUS_OK);
    REQUIRE_FALSE(focusable);
    sao_ui_widget_destroy(handle);
}

TEST_CASE("portable_radio_group_is_exclusive_and_reports_pick", "[ui][widget][input][portable]") {
    SaoUiRadioSpec first_spec{};
    first_spec.label_utf8 = "A";
    first_spec.group_id = 7;
    first_spec.value_id = 10;
    first_spec.selected = true;
    first_spec.font_size_px = 14;
    first_spec.ring_size_px = 16;
    SaoUiRadioSpec second_spec = first_spec;
    second_spec.label_utf8 = "B";
    second_spec.value_id = 20;
    second_spec.selected = false;

    sao_ui_widget_handle_t first = nullptr;
    sao_ui_widget_handle_t second = nullptr;
    REQUIRE(sao_ui_radio_create(nullptr, &first_spec, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_create(nullptr, &second_spec, &second) == SAO_STATUS_OK);
    InputSink sink;
    REQUIRE(sao_ui_radio_set_group_pick_handler(second, input_radio_cb, &sink) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_set_selected(second, true) == SAO_STATUS_OK);
    bool selected = true;
    REQUIRE(sao_ui_radio_get_selected(first, &selected) == SAO_STATUS_OK);
    REQUIRE_FALSE(selected);
    REQUIRE(sao_ui_radio_get_selected(second, &selected) == SAO_STATUS_OK);
    REQUIRE(selected);
    REQUIRE(sink.radio_picks == 1);
    REQUIRE(sink.last_group == 7);
    REQUIRE(sink.last_value == 20);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(first);
}

TEST_CASE("portable radio group concurrent selection has no ABBA",
          "[ui][widget][input][radio][threading]") {
    SaoUiRadioSpec first_spec{};
    first_spec.label_utf8 = "first";
    first_spec.group_id = 71;
    first_spec.value_id = 1;
    first_spec.selected = true;
    first_spec.font_size_px = 14;
    first_spec.ring_size_px = 16;
    SaoUiRadioSpec second_spec = first_spec;
    second_spec.label_utf8 = "second";
    second_spec.value_id = 2;
    second_spec.selected = false;
    sao_ui_widget_handle_t first = nullptr;
    sao_ui_widget_handle_t second = nullptr;
    REQUIRE(sao_ui_radio_create(nullptr, &first_spec, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_create(nullptr, &second_spec, &second) == SAO_STATUS_OK);

    std::atomic_bool start{false};
    std::atomic_int failures{0};
    const auto select = [&](sao_ui_widget_handle_t radio) {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int iteration = 0; iteration < 500; ++iteration) {
            if (sao_ui_radio_set_selected(radio, true) != SAO_STATUS_OK)
                failures.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first_worker(select, first);
    std::thread second_worker(select, second);
    start.store(true, std::memory_order_release);
    first_worker.join();
    second_worker.join();
    CHECK(failures.load(std::memory_order_relaxed) == 0);

    bool first_selected = false;
    bool second_selected = false;
    REQUIRE(sao_ui_radio_get_selected(first, &first_selected) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_get_selected(second, &second_selected) == SAO_STATUS_OK);
    CHECK(first_selected != second_selected);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(first);
}

TEST_CASE("portable_slider_clamps_snaps_and_emits_only_on_change",
          "[ui][widget][input][portable]") {
    SaoUiSliderSpec spec{};
    spec.value = 0.26F;
    spec.min_value = 0.0F;
    spec.max_value = 1.0F;
    spec.step = 0.25F;
    spec.track_thickness_px = 4;
    spec.thumb_size_px = 12;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_slider_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    float value = -1.0F;
    REQUIRE(sao_ui_slider_get_value(handle, &value) == SAO_STATUS_OK);
    REQUIRE(std::fabs(value - 0.25F) < 1e-6F);
    InputSink sink;
    uint64_t token = 0;
    REQUIRE(sao_ui_slider_set_change_handler(handle, input_slider_cb, &sink) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(handle, SAO_UI_EVT_VALUE_CHANGED, input_generic_cb,
                                            &sink, &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_slider_set_value(handle, 0.62F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_slider_get_value(handle, &value) == SAO_STATUS_OK);
    REQUIRE(std::fabs(value - 0.5F) < 1e-6F);
    REQUIRE(sink.slider_changes == 1);
    REQUIRE(std::fabs(sink.last_slider - 0.5F) < 1e-6F);
    REQUIRE(sink.generic_events == 1);
    REQUIRE(sao_ui_slider_set_value(handle, 0.51F) == SAO_STATUS_OK);
    REQUIRE(sink.slider_changes == 1);
    REQUIRE(sao_ui_slider_set_value(handle, std::numeric_limits<float>::quiet_NaN()) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_widget_destroy(handle);

    spec.min_value = 1.0F;
    spec.max_value = 1.0F;
    REQUIRE(sao_ui_slider_create(nullptr, &spec, &handle) == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("portable_input_create_validation_rejects_invalid_specs",
          "[ui][widget][input][portable]") {
    sao_ui_widget_handle_t handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    SaoUiButtonSpec button = make_button_spec("bad", 99);
    REQUIRE(sao_ui_button_create(nullptr, &button, &handle) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    SaoUiCheckboxSpec checkbox{};
    checkbox.box_size_px = -1;
    REQUIRE(sao_ui_checkbox_create(nullptr, &checkbox, &handle) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    SaoUiRadioSpec radio{};
    radio.ring_size_px = -1;
    REQUIRE(sao_ui_radio_create(nullptr, &radio, &handle) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    SaoUiDropdownButtonSpec dropdown{};
    dropdown.kind = SAO_UI_BTN_NORMAL;
    dropdown.entry_count = 1;
    REQUIRE(sao_ui_dropdown_button_create(nullptr, &dropdown, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
}

TEST_CASE("portable_input_handles_retire_without_reuse_and_destroy_is_idempotent",
          "[ui][widget][input][hardening]") {
    const SaoUiButtonSpec spec = make_button_spec("stable");
    sao_ui_widget_handle_t first = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &spec, &first) == SAO_STATUS_OK);
    uint64_t first_generation = 0;
    REQUIRE(sao_ui_widget_input_get_generation(first, &first_generation) == SAO_STATUS_OK);
    REQUIRE(first_generation != 0);
    sao_ui_widget_destroy(first);
    sao_ui_widget_destroy(first);
    bool active = false;
    REQUIRE(sao_ui_widget_button_is_active(first, &active) == SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(sao_ui_widget_input_get_generation(first, nullptr) == SAO_STATUS_ERR_HANDLE_INVALID);

    sao_ui_widget_handle_t second = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &spec, &second) == SAO_STATUS_OK);
    uint64_t second_generation = 0;
    REQUIRE(sao_ui_widget_input_get_generation(second, &second_generation) == SAO_STATUS_OK);
    REQUIRE(second != first);
    REQUIRE(second_generation > first_generation);
    sao_ui_widget_destroy(second);
}

TEST_CASE("portable_checkbox_toggle_is_linearizable_under_contention",
          "[ui][widget][input][hardening]") {
    SaoUiCheckboxSpec spec{};
    spec.label_utf8 = "linear";
    spec.box_size_px = 16;
    spec.font_size_px = 14;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_checkbox_create(nullptr, &spec, &handle) == SAO_STATUS_OK);

    constexpr int kToggleCount = 64;
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kToggleCount);
    for (int index = 0; index < kToggleCount; ++index) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (sao_ui_checkbox_toggle(handle) != SAO_STATUS_OK)
                failures.fetch_add(1, std::memory_order_relaxed);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers)
        worker.join();
    bool checked = true;
    REQUIRE(failures.load(std::memory_order_relaxed) == 0);
    REQUIRE(sao_ui_checkbox_get_checked(handle, &checked) == SAO_STATUS_OK);
    REQUIRE_FALSE(checked);
    sao_ui_widget_destroy(handle);
}

TEST_CASE("portable_input_self_destroy_returns_busy_and_stops_typed_dispatch",
          "[ui][widget][input][hardening]") {
    SaoUiIconButtonSpec spec{};
    uint8_t pixels[4]{};
    spec.icon_bgra_pixels = pixels;
    spec.icon_width = 1;
    spec.icon_height = 1;
    spec.icon_stride = 4;
    spec.kind = SAO_UI_BTN_NORMAL;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_icon_button_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    DestroySink destroy{handle};
    InputSink typed;
    uint64_t token = 0;
    REQUIRE(sao_ui_widget_add_event_handler(handle, SAO_UI_EVT_CLICK, destroy_peer_generic_cb,
                                            &destroy, &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_icon_button_set_click_handler(handle, input_click_cb, &typed) == SAO_STATUS_OK);
    REQUIRE(sao_ui_icon_button_invoke(handle) == SAO_UI_STATUS_ERR_BUSY);
    REQUIRE(destroy.callbacks == 1);
    REQUIRE(typed.clicks == 0);
    REQUIRE(sao_ui_icon_button_invoke(handle) == SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_widget_destroy(handle);
}

TEST_CASE("portable_radio_peer_destroy_stops_remaining_dispatch",
          "[ui][widget][input][hardening]") {
    SaoUiRadioSpec first_spec{};
    first_spec.label_utf8 = "first";
    first_spec.group_id = 9;
    first_spec.value_id = 1;
    first_spec.selected = true;
    first_spec.font_size_px = 14;
    first_spec.ring_size_px = 16;
    SaoUiRadioSpec second_spec = first_spec;
    second_spec.label_utf8 = "second";
    second_spec.value_id = 2;
    second_spec.selected = false;
    sao_ui_widget_handle_t first = nullptr;
    sao_ui_widget_handle_t second = nullptr;
    REQUIRE(sao_ui_radio_create(nullptr, &first_spec, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_create(nullptr, &second_spec, &second) == SAO_STATUS_OK);
    DestroySink destroy{second};
    InputSink typed;
    uint64_t token = 0;
    REQUIRE(sao_ui_widget_add_event_handler(first, SAO_UI_EVT_VALUE_CHANGED,
                                            destroy_peer_generic_cb, &destroy,
                                            &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_set_group_pick_handler(second, input_radio_cb, &typed) == SAO_STATUS_OK);
    REQUIRE(sao_ui_radio_set_selected(second, true) == SAO_UI_STATUS_ERR_BUSY);
    REQUIRE(destroy.callbacks == 1);
    REQUIRE(typed.radio_picks == 0);
    bool selected = false;
    REQUIRE(sao_ui_radio_get_selected(second, &selected) == SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_widget_destroy(second);
    sao_ui_widget_destroy(first);
}

TEST_CASE("portable_input_callback_exceptions_do_not_cross_c_abi",
          "[ui][widget][input][hardening]") {
    const SaoUiButtonSpec spec = make_button_spec("throw");
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_button_set_click_handler(handle, throwing_click_cb, nullptr) == SAO_STATUS_OK);
    int32_t action = -1;
    REQUIRE(sao_ui_widget_button_dispatch_event(handle, kBtnEvtMouseDown, &action) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(handle, kBtnEvtMouseUp, &action) ==
            SAO_STATUS_ERR_UNKNOWN);
    sao_ui_widget_destroy(handle);
}

TEST_CASE("portable_input_api_races_destroy_without_uaf", "[ui][widget][input][hardening]") {
    const SaoUiButtonSpec spec = make_button_spec("race");
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    std::atomic<bool> started{false};
    std::atomic<int> unexpected{0};
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        for (int index = 0; index < 10000; ++index) {
            const sao_status_t status = sao_ui_button_set_active(handle, (index % 2) != 0);
            if (status == SAO_STATUS_ERR_HANDLE_INVALID)
                break;
            if (status != SAO_STATUS_OK) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    sao_ui_widget_destroy(handle);
    worker.join();
    REQUIRE(unexpected.load(std::memory_order_relaxed) == 0);
    bool active = false;
    REQUIRE(sao_ui_widget_button_is_active(handle, &active) == SAO_STATUS_ERR_HANDLE_INVALID);
}
