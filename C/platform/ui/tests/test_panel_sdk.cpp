// SAO Auto — panel SDK descriptor registration and mutation tests.
//
// Verifies:
//   * register/unregister round-trip returns a handle and updates the
//     registry_count accurately.
//   * bring_to_front elevates the target's z_within_class above every
//     other panel in the same z_class.
//   * update_body's batched mutations are logged in submission order.
//
// The test rig drives panel_sdk in isolation — no live compositor, no
// D3D11.  panel_sdk stores the compositor argument for downstream
// integration but the registry itself is compositor-agnostic, so we
// pass nullptr here.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/panel_sdk.h"
#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_input.h"

// Test-only helpers exported by panel_sdk.cpp (introspection).
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_panel_body_mutation_count(sao_ui_panel_body_handle_t body);
extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_body_mutation_at(sao_ui_panel_body_handle_t body, size_t idx);
extern "C" SAO_UI_API int32_t SAO_UI_CALL sao_ui_panel_z_within_class(sao_ui_panel_handle_t panel);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_panel_is_visible(sao_ui_panel_handle_t panel);
extern "C" SAO_UI_API float SAO_UI_CALL sao_ui_panel_get_opacity_(sao_ui_panel_handle_t panel);
extern "C" SAO_UI_API int32_t SAO_UI_CALL sao_ui_panel_global_z_key_(sao_ui_panel_handle_t panel);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_pointer_button(sao_ui_panel_handle_t panel, int32_t x, int32_t y);
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_panel_retired_geometry_count_();
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_body_replace_failure_point(int32_t point);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_widget_test_props_state(
    sao_ui_widget_handle_t handle, const char* color_key, uint32_t* out_color, char* out_text,
    size_t out_text_capacity);

// Test APIs declared in the implementation only (the panel_sdk.h
// header remains unchanged).  Prototypes
// live here so the test rig links against the DLL exports.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_get_descriptor(sao_ui_panel_handle_t panel, SaoPanelDescriptor* descriptor_out);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_registry_count(size_t* count_out);
typedef void(SAO_UI_CALL* sao_ui_panel_registry_iterate_cb_t)(sao_ui_panel_handle_t panel,
                                                              const SaoPanelDescriptor* descriptor,
                                                              void* user_data);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_registry_iterate(sao_ui_panel_registry_iterate_cb_t callback, void* user_data);

namespace {

using namespace std::chrono_literals;

std::vector<uint8_t> compositor_snapshot(sao_ui_compositor_handle_t compositor,
                                         uint32_t* width = nullptr, uint32_t* height = nullptr) {
    uint32_t local_width = 0;
    uint32_t local_height = 0;
    size_t required = 0;
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0, &local_width, &local_height,
                                            &required) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(required);
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(), &local_width,
                                            &local_height, &required) == SAO_STATUS_OK);
    if (width != nullptr)
        *width = local_width;
    if (height != nullptr)
        *height = local_height;
    return pixels;
}

struct ActionCapture {
    std::string action;
    std::string args;
    size_t count{};
};

void SAO_UI_CALL capture_action(const char* action_id_utf8, const uint8_t* args_json_utf8,
                                size_t args_len, void* user_data) {
    auto* capture = static_cast<ActionCapture*>(user_data);
    capture->action = action_id_utf8 == nullptr ? "" : action_id_utf8;
    capture->args.assign(reinterpret_cast<const char*>(args_json_utf8), args_len);
    ++capture->count;
}

struct GeometryCapture {
    std::mutex mutex;
    std::condition_variable cv;
    size_t count{};
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

void SAO_UI_CALL capture_geometry(const char*, int32_t x, int32_t y, int32_t width, int32_t height,
                                  void* user_data) {
    auto* capture = static_cast<GeometryCapture*>(user_data);
    {
        std::lock_guard lock(capture->mutex);
        ++capture->count;
        capture->x = x;
        capture->y = y;
        capture->width = width;
        capture->height = height;
    }
    capture->cv.notify_all();
}

struct BlockingCallback {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool release{};
};

void SAO_UI_CALL blocking_action(const char*, const uint8_t*, size_t, void* user_data) {
    auto* state = static_cast<BlockingCallback*>(user_data);
    std::unique_lock lock(state->mutex);
    state->entered = true;
    state->cv.notify_all();
    state->cv.wait(lock, [state] { return state->release; });
}

void SAO_UI_CALL blocking_geometry(const char*, int32_t, int32_t, int32_t, int32_t,
                                   void* user_data) {
    auto* state = static_cast<BlockingCallback*>(user_data);
    std::unique_lock lock(state->mutex);
    state->entered = true;
    state->cv.notify_all();
    state->cv.wait(lock, [state] { return state->release; });
}

struct SelfDestroyCapture {
    sao_ui_panel_handle_t panel{};
    size_t count{};
};

void SAO_UI_CALL destroy_from_action(const char*, const uint8_t*, size_t, void* user_data) {
    auto* capture = static_cast<SelfDestroyCapture*>(user_data);
    ++capture->count;
    sao_ui_panel_destroy(capture->panel);
}

struct RenderReentryCapture {
    sao_ui_panel_handle_t panel{};
    sao_status_t nested_status{SAO_STATUS_OK};
    size_t count{};
};

void SAO_UI_CALL render_reentry(void*, float, float, float, float, void* user_data) {
    auto* capture = static_cast<RenderReentryCapture*>(user_data);
    const char spec[] = R"({"version":1,"title":"","nodes":[]})";
    capture->nested_status = sao_ui_panel_set_spec(
        capture->panel, reinterpret_cast<const uint8_t*>(spec), std::strlen(spec));
    ++capture->count;
}

struct SelfUnregisterCapture {
    std::mutex mutex;
    std::condition_variable cv;
    sao_ui_panel_handle_t panel{};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
    bool done{};
};

void SAO_UI_CALL unregister_from_geometry(const char*, int32_t, int32_t, int32_t, int32_t,
                                          void* user_data) {
    auto* capture = static_cast<SelfUnregisterCapture*>(user_data);
    const sao_status_t status = sao_ui_panel_unregister(capture->panel);
    {
        std::lock_guard lock(capture->mutex);
        capture->status = status;
        capture->done = true;
    }
    capture->cv.notify_all();
}

struct SnapshotCapture {
    bool valid_after_unregister{};
    size_t count{};
};

void SAO_UI_CALL unregister_during_snapshot(sao_ui_panel_handle_t panel,
                                            const SaoPanelDescriptor* descriptor, void* user_data) {
    auto* capture = static_cast<SnapshotCapture*>(user_data);
    if (descriptor->panel_id_utf8 == nullptr ||
        std::strcmp(descriptor->panel_id_utf8, "panel_snapshot_owned") != 0) {
        return;
    }
    const std::string id = descriptor->panel_id_utf8;
    const auto* icon = static_cast<const uint8_t*>(descriptor->icon_bgra_pixels);
    const sao_status_t status = sao_ui_panel_unregister(panel);
    capture->valid_after_unregister = status == SAO_STATUS_OK &&
                                      std::strcmp(descriptor->panel_id_utf8, id.c_str()) == 0 &&
                                      icon != nullptr && icon[0] == 0x11U && icon[7] == 0x88U;
    ++capture->count;
}

SaoPanelDescriptor make_descriptor(const char* id, int32_t z_class, int32_t z_within) {
    SaoPanelDescriptor d{};
    d.panel_id_utf8 = id;
    d.title_utf8 = "Test Panel";
    d.anchor = SAO_UI_PANEL_ANCHOR_ABSOLUTE;
    d.default_x_px = 32;
    d.default_y_px = 64;
    d.default_width_px = 480;
    d.default_height_px = 320;
    d.min_width_px = 200;
    d.min_height_px = 120;
    d.movable = true;
    d.resizable = true;
    d.show_titlebar = true;
    d.show_close_button = true;
    d.visible = true;
    d.remember_geometry = false;
    d.z_class = z_class;
    d.z_within_class = z_within;
    d.initial_opacity = 0.93f;
    return d;
}

} // namespace

TEST_CASE("panel_register_returns_handle", "[ui][panel_sdk][runtime]") {
    // Baseline registry state — some prior test could have leaked, so
    // read the count first and compare deltas.
    size_t baseline = 0;
    REQUIRE(sao_ui_panel_registry_count(&baseline) == SAO_STATUS_OK);

    const auto desc = make_descriptor("panel_sdk_test_register", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;

    REQUIRE(sao_ui_panel_register(nullptr, &desc, &panel, &body) == SAO_STATUS_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(body != nullptr);

    // Descriptor readback carries id + geometry.
    SaoPanelDescriptor readback{};
    REQUIRE(sao_ui_panel_get_descriptor(panel, &readback) == SAO_STATUS_OK);
    REQUIRE(readback.panel_id_utf8 != nullptr);
    REQUIRE(std::strcmp(readback.panel_id_utf8, "panel_sdk_test_register") == 0);
    REQUIRE(readback.default_width_px == 480);

    // Opacity honours the descriptor's initial value.
    REQUIRE(sao_ui_panel_get_opacity_(panel) > 0.9f);

    // Duplicate id → ERR_ALREADY_EXISTS.
    sao_ui_panel_handle_t dup = nullptr;
    sao_ui_panel_body_handle_t dup_body = nullptr;
    const auto dup_desc = make_descriptor("panel_sdk_test_register", SAO_UI_PANEL_Z_NORMAL, 0);
    REQUIRE(sao_ui_panel_register(nullptr, &dup_desc, &dup, &dup_body) ==
            SAO_STATUS_ERR_ALREADY_EXISTS);
    REQUIRE(dup == nullptr);

    // Cleanup.
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    size_t after = 0;
    REQUIRE(sao_ui_panel_registry_count(&after) == SAO_STATUS_OK);
    REQUIRE(after == baseline);
}

TEST_CASE("panel_registry_count_tracks_registrations", "[ui][panel_sdk][runtime]") {
    size_t base = 0;
    REQUIRE(sao_ui_panel_registry_count(&base) == SAO_STATUS_OK);

    std::vector<sao_ui_panel_handle_t> panels;
    for (int i = 0; i < 4; ++i) {
        const std::string id = "panel_count_test_" + std::to_string(i);
        auto d = make_descriptor(id.c_str(), SAO_UI_PANEL_Z_NORMAL, i);
        sao_ui_panel_handle_t p = nullptr;
        sao_ui_panel_body_handle_t b = nullptr;
        REQUIRE(sao_ui_panel_register(nullptr, &d, &p, &b) == SAO_STATUS_OK);
        panels.push_back(p);
    }
    size_t peak = 0;
    REQUIRE(sao_ui_panel_registry_count(&peak) == SAO_STATUS_OK);
    REQUIRE(peak == base + 4);

    for (auto p : panels) {
        REQUIRE(sao_ui_panel_unregister(p) == SAO_STATUS_OK);
    }
    size_t end = 0;
    REQUIRE(sao_ui_panel_registry_count(&end) == SAO_STATUS_OK);
    REQUIRE(end == base);
}

TEST_CASE("panel_unregister_removes_from_registry", "[ui][panel_sdk][runtime]") {
    const auto desc = make_descriptor("panel_sdk_unreg_target", SAO_UI_PANEL_Z_TOPMOST, 5);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &desc, &panel, &body) == SAO_STATUS_OK);

    // Baseline visibility must reflect the descriptor.
    REQUIRE(sao_ui_panel_is_visible(panel) == true);
    REQUIRE(sao_ui_panel_hide(panel) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_is_visible(panel) == false);
    REQUIRE(sao_ui_panel_show(panel) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_is_visible(panel) == true);

    // Now unregister and confirm subsequent operations return handle-
    // invalid rather than silently succeeding.
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_show(panel) == SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(sao_ui_panel_hide(panel) == SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_ERR_NOT_FOUND);
}

TEST_CASE("panel body handles never alias a later registration", "[ui][panel_sdk][lifetime]") {
    auto first_descriptor = make_descriptor("panel_body_stale_first", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t first_panel = nullptr;
    sao_ui_panel_body_handle_t stale_body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &first_descriptor, &first_panel, &stale_body) ==
        SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_unregister(first_panel) == SAO_STATUS_OK);

    auto second_descriptor = make_descriptor("panel_body_stale_second", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t second_panel = nullptr;
    sao_ui_panel_body_handle_t second_body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &second_descriptor, &second_panel, &second_body) ==
        SAO_STATUS_OK);
    CHECK(second_body != stale_body);
    sao_ui_layout_node_handle_t root = reinterpret_cast<sao_ui_layout_node_handle_t>(uintptr_t{1});
    CHECK(sao_ui_panel_body_get_root(stale_body, &root) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(root == nullptr);
    REQUIRE(sao_ui_panel_body_get_root(second_body, &root) == SAO_STATUS_OK);
    CHECK(root != nullptr);
    REQUIRE(sao_ui_panel_unregister(second_panel) == SAO_STATUS_OK);
}

TEST_CASE("panel body rejects widget families without generic backing",
      "[ui][panel_sdk][family]") {
    auto descriptor = make_descriptor("panel_body_family_gate", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);
    SaoUiLayoutSpec layout{};
    sao_ui_layout_spec_defaults(&layout);

    SaoUiButtonSpec button_spec{};
    button_spec.text_utf8 = "input";
    button_spec.kind = SAO_UI_BTN_NORMAL;
    sao_ui_widget_handle_t input = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &button_spec, &input) == SAO_STATUS_OK);
    SaoUiBodyMutation add{};
    add.kind = SAO_UI_BODY_ADD_WIDGET;
    add.widget = input;
    add.spec = &layout;
    CHECK(sao_ui_panel_update_body(body, &add, 1) == SAO_STATUS_ERR_NOT_IMPLEMENTED);

    SaoUiBarChartBar bar{"bar", 1.0, 0, 0, 0.0, 0};
    SaoUiBarChartSpec chart_spec{};
    chart_spec.bars = &bar;
    chart_spec.bar_count = 1;
    sao_ui_widget_handle_t chart = nullptr;
    REQUIRE(sao_ui_bar_chart_create(nullptr, &chart_spec, &chart) == SAO_STATUS_OK);
    add.widget = chart;
    CHECK(sao_ui_panel_update_body(body, &add, 1) == SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(sao_ui_panel_body_mutation_count(body) == 0);

    sao_ui_widget_destroy(chart);
    sao_ui_widget_destroy(input);
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
}

TEST_CASE("panel body restores complete props and reports rollback failure",
      "[ui][panel_sdk][rollback]") {
    auto descriptor = make_descriptor("panel_body_props_rollback", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) ==
        SAO_STATUS_OK);
    SaoUiLayoutSpec layout{};
    sao_ui_layout_spec_defaults(&layout);
    const char initial[] = R"({"text":"old","fill":"#112233"})";
    SaoUiBodyMutation initial_batch[2]{};
    initial_batch[0].kind = SAO_UI_BODY_ADD_WIDGET;
    initial_batch[0].widget = widget;
    initial_batch[0].spec = &layout;
    initial_batch[1].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    initial_batch[1].widget = widget;
    initial_batch[1].props_json_utf8 = reinterpret_cast<const uint8_t*>(initial);
    initial_batch[1].props_len = std::strlen(initial);
    REQUIRE(sao_ui_panel_update_body(body, initial_batch, 2) == SAO_STATUS_OK);

    const char changed[] = R"({"text":"new","fill":"#aabbcc","accent":"#ff0000"})";
    SaoUiBodyMutation update{};
    update.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    update.widget = widget;
    update.props_json_utf8 = reinterpret_cast<const uint8_t*>(changed);
    update.props_len = std::strlen(changed);
    sao_ui_panel_test_set_body_replace_failure_point(1);
    CHECK(sao_ui_panel_update_body(body, &update, 1) == SAO_STATUS_ERR_UNKNOWN);
    sao_ui_panel_test_set_body_replace_failure_point(0);
    uint32_t fill = 0;
    char text[32]{};
    REQUIRE(sao_ui_widget_test_props_state(widget, "fill", &fill, text, sizeof(text)));
    CHECK(fill == 0xff112233U);
    CHECK(std::string(text) == "old");
    uint32_t accent = 0;
    CHECK_FALSE(sao_ui_widget_test_props_state(widget, "accent", &accent, text, sizeof(text)));

    sao_ui_panel_test_set_body_replace_failure_point(2);
    CHECK(sao_ui_panel_update_body(body, &update, 1) ==
      SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED);
    sao_ui_panel_test_set_body_replace_failure_point(0);

    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    sao_ui_widget_destroy(widget);
}

TEST_CASE("panel_bring_to_front_updates_z", "[ui][panel_sdk][runtime]") {
    // Three panels in the same z-class.
    struct Trio {
        sao_ui_panel_handle_t handle;
        int32_t initial_z;
    };
    std::vector<Trio> trio;

    for (int i = 0; i < 3; ++i) {
        const std::string id = "panel_z_test_" + std::to_string(i);
        auto d = make_descriptor(id.c_str(), SAO_UI_PANEL_Z_NORMAL, i);
        sao_ui_panel_handle_t p = nullptr;
        sao_ui_panel_body_handle_t b = nullptr;
        REQUIRE(sao_ui_panel_register(nullptr, &d, &p, &b) == SAO_STATUS_OK);
        trio.push_back({p, i});
    }

    // Bring the first panel to the front — its z_within_class should
    // exceed both siblings'.
    REQUIRE(sao_ui_panel_bring_to_front(trio[0].handle) == SAO_STATUS_OK);
    const int32_t first_z = sao_ui_panel_z_within_class(trio[0].handle);
    const int32_t second_z = sao_ui_panel_z_within_class(trio[1].handle);
    const int32_t third_z = sao_ui_panel_z_within_class(trio[2].handle);
    REQUIRE(first_z > second_z);
    REQUIRE(first_z > third_z);

    // Send it back — z drops below every sibling.
    REQUIRE(sao_ui_panel_send_to_back(trio[0].handle) == SAO_STATUS_OK);
    const int32_t after_back = sao_ui_panel_z_within_class(trio[0].handle);
    REQUIRE(after_back <= second_z);
    REQUIRE(after_back <= third_z);

    for (auto& t : trio) {
        REQUIRE(sao_ui_panel_unregister(t.handle) == SAO_STATUS_OK);
    }
}

TEST_CASE("panel_update_body_batches_mutations", "[ui][panel_sdk][runtime]") {
    const auto desc = make_descriptor("panel_body_batch_target", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &desc, &panel, &body) == SAO_STATUS_OK);

    sao_ui_widget_handle_t first_widget = nullptr;
    sao_ui_widget_handle_t second_widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &first_widget) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &second_widget) ==
            SAO_STATUS_OK);
    SaoUiLayoutSpec widget_spec{};
    sao_ui_layout_spec_defaults(&widget_spec);
    widget_spec.fixed_height_px = 36;
    sao_ui_layout_node_handle_t first_node = nullptr;
    sao_ui_layout_node_handle_t second_node = nullptr;
    const char kProps[] = R"({"text":"Updated","fill":"#336699"})";

    SaoUiBodyMutation batch[3] = {};
    batch[0].kind = SAO_UI_BODY_ADD_WIDGET;
    batch[0].widget = first_widget;
    batch[0].spec = &widget_spec;
    batch[0].out_new_node = &first_node;
    batch[1].kind = SAO_UI_BODY_ADD_WIDGET;
    batch[1].widget = second_widget;
    batch[1].spec = &widget_spec;
    batch[1].out_new_node = &second_node;
    batch[2].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    batch[2].widget = first_widget;
    batch[2].props_json_utf8 = reinterpret_cast<const uint8_t*>(kProps);
    batch[2].props_len = std::strlen(kProps);

    REQUIRE(sao_ui_panel_update_body(body, batch, 3) == SAO_STATUS_OK);
    REQUIRE(first_node != nullptr);
    REQUIRE(second_node != nullptr);
    REQUIRE(sao_ui_panel_body_mutation_count(body) == 3);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 0) == SAO_UI_BODY_ADD_WIDGET);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 1) == SAO_UI_BODY_ADD_WIDGET);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 2) == SAO_UI_BODY_UPDATE_WIDGET_PROPS);

    SaoUiLayoutSpec resized_spec = widget_spec;
    resized_spec.fixed_height_px = 48;
    SaoUiBodyMutation batch2[2] = {};
    batch2[0].kind = SAO_UI_BODY_REORDER_NODE;
    batch2[0].target = second_node;
    batch2[0].new_index = 0;
    batch2[1].kind = SAO_UI_BODY_UPDATE_SPEC;
    batch2[1].target = first_node;
    batch2[1].spec = &resized_spec;
    REQUIRE(sao_ui_panel_update_body(body, batch2, 2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_body_mutation_count(body) == 5);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 3) == SAO_UI_BODY_REORDER_NODE);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 4) == SAO_UI_BODY_UPDATE_SPEC);

    sao_ui_layout_node_handle_t unpublished = reinterpret_cast<sao_ui_layout_node_handle_t>(1);
    SaoUiBodyMutation rollback_batch[2] = {};
    rollback_batch[0].kind = SAO_UI_BODY_ADD_WIDGET;
    rollback_batch[0].widget = first_widget;
    rollback_batch[0].spec = &widget_spec;
    rollback_batch[0].out_new_node = &unpublished;
    rollback_batch[1].kind = SAO_UI_BODY_UPDATE_SPEC;
    rollback_batch[1].target = first_node;
    REQUIRE(sao_ui_panel_update_body(body, rollback_batch, 2) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(unpublished == reinterpret_cast<sao_ui_layout_node_handle_t>(1));
    CHECK(sao_ui_panel_body_mutation_count(body) == 5);

    sao_ui_layout_node_handle_t previous_root = nullptr;
    REQUIRE(sao_ui_panel_body_get_root(body, &previous_root) == SAO_STATUS_OK);
    const char kSpec[] =
        R"({"version":1,"title":"","nodes":[{"type":"slider","id":"sdk_slider","value":0.25,"height":32}]})";
    REQUIRE(sao_ui_panel_body_set_spec(body, reinterpret_cast<const uint8_t*>(kSpec),
                                       std::strlen(kSpec)) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_body_mutation_count(body) == 6);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 5) == SAO_UI_BODY_UPDATE_SPEC);
    sao_ui_layout_node_handle_t replacement_root = nullptr;
    REQUIRE(sao_ui_panel_body_get_root(body, &replacement_root) == SAO_STATUS_OK);
    CHECK(replacement_root != previous_root);
    const char kUpdate[] = R"({"value":0.75})";
    CHECK(sao_ui_panel_update_widget(panel, "sdk_slider", reinterpret_cast<const uint8_t*>(kUpdate),
                                     std::strlen(kUpdate)) == SAO_STATUS_OK);

    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    sao_ui_widget_destroy(first_widget);
    sao_ui_widget_destroy(second_widget);
}

TEST_CASE("classic_panel_rebuilds_widgets_and_updates_real_layer",
          "[ui][panel_sdk][panel][runtime]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);

    SaoPanelConfig config{};
    config.panel_id_utf8 = "classic_real_layer";
    config.title_utf8 = "Classic";
    config.default_width = 180;
    config.default_height = 100;
    config.show_titlebar = true;
    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_create(compositor, &config, &panel) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_layer(panel) != nullptr);

    const char kSpec[] =
        R"({"type":"panel","title":"Classic","children":[{"type":"button","id":"launch","label":"Go","action":"launch_action","payload":{"value":7}}]})";
    REQUIRE(sao_ui_panel_set_spec(panel, reinterpret_cast<const uint8_t*>(kSpec),
                                  std::strlen(kSpec)) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_set_visible(panel, true) == SAO_STATUS_OK);

    uint32_t before_width = 0;
    uint32_t before_height = 0;
    const auto before = compositor_snapshot(compositor, &before_width, &before_height);
    CHECK(before_width == 180u);
    CHECK(before_height == 100u);

    const char kProps[] = R"({"text":"Changed"})";
    REQUIRE(sao_ui_panel_update_widget(panel, "launch", reinterpret_cast<const uint8_t*>(kProps),
                                       std::strlen(kProps)) == SAO_STATUS_OK);
    const auto after = compositor_snapshot(compositor);
    CHECK(after != before);
    CHECK(sao_ui_panel_update_widget(panel, "missing", reinterpret_cast<const uint8_t*>(kProps),
                                     std::strlen(kProps)) == SAO_STATUS_ERR_NOT_FOUND);

    ActionCapture action;
    REQUIRE(sao_ui_panel_set_action_handler(panel, &capture_action, &action) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_test_pointer_button(panel, 10, 30) == SAO_STATUS_OK);
    CHECK(action.count == 1u);
    CHECK(action.action == "launch_action");
    CHECK(action.args.find("\"value\":7") != std::string::npos);

    sao_ui_panel_destroy(panel);
    size_t layer_count = 1;
    REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &layer_count) == SAO_STATUS_OK);
    CHECK(layer_count == 0u);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("panel_geometry_persistence_debounces_and_stops_on_teardown",
          "[ui][panel_sdk][debounce][runtime]") {
    auto descriptor = make_descriptor("panel_geometry_debounce", SAO_UI_PANEL_Z_NORMAL, 0);
    descriptor.remember_geometry = true;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);

    GeometryCapture capture;
    REQUIRE(sao_ui_panel_set_geometry_persist_handler(panel, &capture_geometry, &capture) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_set_geometry(panel, 10, 20, 300, 200) == SAO_STATUS_OK);
    std::this_thread::sleep_for(50ms);
    REQUIRE(sao_ui_panel_set_geometry(panel, 30, 40, 320, 220) == SAO_STATUS_OK);
    std::this_thread::sleep_for(50ms);
    const auto last_change = std::chrono::steady_clock::now();
    REQUIRE(sao_ui_panel_set_geometry(panel, 50, 60, 340, 240) == SAO_STATUS_OK);

    {
        std::unique_lock lock(capture.mutex);
        REQUIRE(capture.cv.wait_for(lock, 1500ms, [&capture] { return capture.count == 1u; }));
        CHECK(std::chrono::steady_clock::now() - last_change >= 400ms);
        CHECK(capture.x == 50);
        CHECK(capture.y == 60);
        CHECK(capture.width == 340);
        CHECK(capture.height == 240);
    }

    REQUIRE(sao_ui_panel_set_geometry(panel, 70, 80, 360, 260) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    std::this_thread::sleep_for(650ms);
    {
        std::lock_guard lock(capture.mutex);
        CHECK(capture.count == 1u);
    }
}

TEST_CASE("panel_body_mutation_rebuilds_and_uploads_real_frame",
          "[ui][panel_sdk][mutation][layer]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    auto descriptor = make_descriptor("panel_body_real_upload", SAO_UI_PANEL_Z_NORMAL, 0);
    descriptor.show_titlebar = false;
    descriptor.default_width_px = 120;
    descriptor.default_height_px = 80;
    descriptor.min_width_px = 1;
    descriptor.min_height_px = 1;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(compositor, &descriptor, &panel, &body) == SAO_STATUS_OK);
    const auto before = compositor_snapshot(compositor);

    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);
    SaoUiLayoutSpec spec{};
    sao_ui_layout_spec_defaults(&spec);
    spec.fixed_width_px = 100;
    spec.fixed_height_px = 40;
    const char props[] = R"({"text":"Real","fill":"#ff0000"})";
    SaoUiBodyMutation mutations[2]{};
    mutations[0].kind = SAO_UI_BODY_ADD_WIDGET;
    mutations[0].widget = widget;
    mutations[0].spec = &spec;
    mutations[1].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mutations[1].widget = widget;
    mutations[1].props_json_utf8 = reinterpret_cast<const uint8_t*>(props);
    mutations[1].props_len = std::strlen(props);
    REQUIRE(sao_ui_panel_update_body(body, mutations, 2) == SAO_STATUS_OK);
    const auto after = compositor_snapshot(compositor);
    CHECK(after != before);

    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    sao_ui_widget_destroy(widget);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("panel_z_classes_map_to_nonoverlapping_global_keys", "[ui][panel_sdk][z]") {
    const auto bottom_descriptor =
        make_descriptor("panel_z_bottom_class", SAO_UI_PANEL_Z_BOTTOM, 200'000'000);
    const auto normal_descriptor =
        make_descriptor("panel_z_normal_class", SAO_UI_PANEL_Z_NORMAL, -200'000'000);
    const auto top_descriptor =
        make_descriptor("panel_z_top_class", SAO_UI_PANEL_Z_TOPMOST, -200'000'000);
    sao_ui_panel_handle_t bottom = nullptr;
    sao_ui_panel_handle_t normal = nullptr;
    sao_ui_panel_handle_t top = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &bottom_descriptor, &bottom, &body) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_register(nullptr, &normal_descriptor, &normal, &body) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_register(nullptr, &top_descriptor, &top, &body) == SAO_STATUS_OK);
    CHECK(sao_ui_panel_global_z_key_(bottom) < sao_ui_panel_global_z_key_(normal));
    CHECK(sao_ui_panel_global_z_key_(normal) < sao_ui_panel_global_z_key_(top));
    REQUIRE(sao_ui_panel_unregister(bottom) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_unregister(normal) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_unregister(top) == SAO_STATUS_OK);
}

TEST_CASE("panel_opacity_accepts_zero_and_rejects_nonfinite", "[ui][panel_sdk][opacity]") {
    auto descriptor = make_descriptor("panel_zero_opacity", SAO_UI_PANEL_Z_NORMAL, 0);
    descriptor.initial_opacity = 0.0F;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);
    CHECK(sao_ui_panel_get_opacity_(panel) == 0.0F);
    CHECK(sao_ui_panel_set_opacity(panel, std::numeric_limits<float>::quiet_NaN()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_panel_get_opacity_(panel) == 0.0F);
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);

    descriptor.panel_id_utf8 = "panel_nan_opacity";
    descriptor.initial_opacity = std::numeric_limits<float>::infinity();
    CHECK(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("panel_restored_ids_truncate_by_utf8_codepoint", "[ui][panel][utf8]") {
    SaoPanelConfig config{};
    config.panel_id_utf8 = "panel_utf8_id";
    config.default_width = 120;
    config.default_height = 80;
    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_create(nullptr, &config, &panel) == SAO_STATUS_OK);
    std::string full_id;
    for (size_t index = 0; index < 81; ++index)
        full_id += "界";
    const std::string expected = full_id.substr(0, full_id.size() - std::strlen("界"));
    const std::string spec =
        std::string(R"({"type":"text","id":")") + full_id + R"(","text":"value"})";
    REQUIRE(sao_ui_panel_set_spec(panel, reinterpret_cast<const uint8_t*>(spec.data()),
                                  spec.size()) == SAO_STATUS_OK);
    const char props[] = R"({"text":"changed"})";
    CHECK(sao_ui_panel_update_widget(panel, expected.c_str(),
                                     reinterpret_cast<const uint8_t*>(props),
                                     std::strlen(props)) == SAO_STATUS_OK);
    CHECK(sao_ui_panel_update_widget(panel, full_id.c_str(),
                                     reinterpret_cast<const uint8_t*>(props),
                                     std::strlen(props)) == SAO_STATUS_ERR_NOT_FOUND);
    sao_ui_panel_destroy(panel);
}

TEST_CASE("panel_action_replacement_waits_for_old_callback", "[ui][panel][callback]") {
    SaoPanelConfig config{};
    config.panel_id_utf8 = "panel_callback_rundown";
    config.default_width = 120;
    config.default_height = 80;
    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_create(nullptr, &config, &panel) == SAO_STATUS_OK);
    const char spec[] = R"({"type":"button","id":"run","label":"Run","action":"run"})";
    REQUIRE(sao_ui_panel_set_spec(panel, reinterpret_cast<const uint8_t*>(spec),
                                  std::strlen(spec)) == SAO_STATUS_OK);
    BlockingCallback state;
    REQUIRE(sao_ui_panel_set_action_handler(panel, &blocking_action, &state) == SAO_STATUS_OK);
    std::atomic<sao_status_t> caller_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread caller([&] { caller_status.store(sao_ui_panel_test_pointer_button(panel, 4, 4)); });
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
    }
    std::atomic_bool replaced{false};
    std::atomic<sao_status_t> replacement_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread replacement([&] {
        replacement_status.store(sao_ui_panel_set_action_handler(panel, nullptr, nullptr));
        replaced.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(replaced.load());
    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
    caller.join();
    replacement.join();
    CHECK(caller_status.load() == SAO_STATUS_OK);
    CHECK(replacement_status.load() == SAO_STATUS_OK);
    CHECK(replaced.load());
    sao_ui_panel_destroy(panel);
}

TEST_CASE("panel_render_reentry_returns_busy_without_recursion", "[ui][panel][render]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    SaoPanelConfig config{};
    config.panel_id_utf8 = "panel_render_busy";
    config.default_width = 80;
    config.default_height = 60;
    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_create(compositor, &config, &panel) == SAO_STATUS_OK);
    RenderReentryCapture capture{panel};
    REQUIRE(sao_ui_panel_set_render_fn(panel, &render_reentry, &capture) == SAO_STATUS_OK);
    CHECK(capture.count == 1u);
    CHECK(capture.nested_status == SAO_UI_PANEL_STATUS_ERR_BUSY);
    sao_ui_panel_destroy(panel);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("panel_action_callback_can_destroy_its_panel", "[ui][panel][callback][destroy]") {
    SaoPanelConfig config{};
    config.panel_id_utf8 = "panel_callback_self_destroy";
    config.default_width = 120;
    config.default_height = 80;
    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_create(nullptr, &config, &panel) == SAO_STATUS_OK);
    const char spec[] = R"({"type":"button","id":"close","label":"Close","action":"close"})";
    REQUIRE(sao_ui_panel_set_spec(panel, reinterpret_cast<const uint8_t*>(spec),
                                  std::strlen(spec)) == SAO_STATUS_OK);
    SelfDestroyCapture capture{panel};
    REQUIRE(sao_ui_panel_set_action_handler(panel, &destroy_from_action, &capture) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_test_pointer_button(panel, 4, 4) == SAO_STATUS_OK);
    CHECK(capture.count == 1u);
    SaoPanelState state{};
    CHECK(sao_ui_panel_get_state(panel, &state) == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("panel_registry_snapshot_owns_strings_and_icons", "[ui][panel_sdk][snapshot]") {
    const uint8_t icon[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    auto descriptor = make_descriptor("panel_snapshot_owned", SAO_UI_PANEL_Z_NORMAL, 0);
    descriptor.icon_bgra_pixels = icon;
    descriptor.icon_width = 1;
    descriptor.icon_height = 2;
    descriptor.icon_stride = 4;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);
    SnapshotCapture capture;
    REQUIRE(sao_ui_panel_registry_iterate(&unregister_during_snapshot, &capture) == SAO_STATUS_OK);
    CHECK(capture.count >= 1u);
    CHECK(capture.valid_after_unregister);
    CHECK(sao_ui_panel_show(panel) == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("panel_geometry_callback_can_unregister_itself", "[ui][panel_sdk][geometry]") {
    const size_t retired_baseline = sao_ui_panel_retired_geometry_count_();
    auto descriptor = make_descriptor("panel_geometry_self_unregister", SAO_UI_PANEL_Z_NORMAL, 0);
    descriptor.remember_geometry = true;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);
    SelfUnregisterCapture capture;
    capture.panel = panel;
    REQUIRE(sao_ui_panel_set_geometry_persist_handler(panel, &unregister_from_geometry, &capture) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_set_geometry(panel, 1, 2, 300, 200) == SAO_STATUS_OK);
    {
        std::unique_lock lock(capture.mutex);
        REQUIRE(capture.cv.wait_for(lock, 2s, [&] { return capture.done; }));
    }
    CHECK(capture.status == SAO_STATUS_OK);
    CHECK(sao_ui_panel_show(panel) == SAO_STATUS_ERR_HANDLE_INVALID);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (sao_ui_panel_retired_geometry_count_() != retired_baseline &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(sao_ui_panel_retired_geometry_count_() == retired_baseline);
}

TEST_CASE("panel_geometry_handler_replacement_waits_for_old_generation",
          "[ui][panel_sdk][geometry][callback]") {
    auto descriptor = make_descriptor("panel_geometry_replace", SAO_UI_PANEL_Z_NORMAL, 0);
    descriptor.remember_geometry = true;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &descriptor, &panel, &body) == SAO_STATUS_OK);
    BlockingCallback state;
    REQUIRE(sao_ui_panel_set_geometry_persist_handler(panel, &blocking_geometry, &state) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_set_geometry(panel, 1, 2, 300, 200) == SAO_STATUS_OK);
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.cv.wait_for(lock, 2s, [&] { return state.entered; }));
    }
    std::atomic_bool replaced{false};
    std::atomic<sao_status_t> replacement_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread replacement([&] {
        replacement_status.store(
            sao_ui_panel_set_geometry_persist_handler(panel, nullptr, nullptr));
        replaced.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(replaced.load());
    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
    replacement.join();
    CHECK(replaced.load());
    CHECK(replacement_status.load() == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
}
