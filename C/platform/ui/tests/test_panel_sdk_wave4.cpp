// SAO Auto — Wave 4 panel SDK descriptor registration tests (G3.10 gate).
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

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/panel_sdk.h"

// Test-only helpers exported by panel_sdk.cpp (introspection).
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_panel_body_mutation_count(sao_ui_panel_body_handle_t body);
extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_body_mutation_at(sao_ui_panel_body_handle_t body, size_t idx);
extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_z_within_class(sao_ui_panel_handle_t panel);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_panel_is_visible(sao_ui_panel_handle_t panel);
extern "C" SAO_UI_API float SAO_UI_CALL
sao_ui_panel_get_opacity_(sao_ui_panel_handle_t panel);

// Wave 4 slice APIs declared in the implementation only (the panel_sdk.h
// header is frozen for this slice — see task constraints).  Prototypes
// live here so the test rig links against the DLL exports.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_get_descriptor(
    sao_ui_panel_handle_t panel,
    SaoPanelDescriptor* descriptor_out);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_registry_count(size_t* count_out);
typedef void (SAO_UI_CALL* sao_ui_panel_registry_iterate_cb_t)(
    sao_ui_panel_handle_t panel,
    const SaoPanelDescriptor* descriptor,
    void* user_data);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_registry_iterate(
    sao_ui_panel_registry_iterate_cb_t callback, void* user_data);

namespace {

SaoPanelDescriptor make_descriptor(const char* id, int32_t z_class,
                                   int32_t z_within) {
    SaoPanelDescriptor d{};
    d.panel_id_utf8      = id;
    d.title_utf8         = "Test Panel";
    d.anchor             = SAO_UI_PANEL_ANCHOR_ABSOLUTE;
    d.default_x_px       = 32;
    d.default_y_px       = 64;
    d.default_width_px   = 480;
    d.default_height_px  = 320;
    d.min_width_px       = 200;
    d.min_height_px      = 120;
    d.movable            = true;
    d.resizable          = true;
    d.show_titlebar      = true;
    d.show_close_button  = true;
    d.visible            = true;
    d.remember_geometry  = false;
    d.z_class            = z_class;
    d.z_within_class     = z_within;
    d.initial_opacity    = 0.93f;
    return d;
}

}  // namespace

TEST_CASE("panel_register_returns_handle", "[ui][panel_sdk][wave4]") {
    // Baseline registry state — some prior test could have leaked, so
    // read the count first and compare deltas.
    size_t baseline = 0;
    REQUIRE(sao_ui_panel_registry_count(&baseline) == SAO_STATUS_OK);

    const auto desc = make_descriptor(
        "panel_sdk_test_register", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;

    REQUIRE(sao_ui_panel_register(nullptr, &desc, &panel, &body)
            == SAO_STATUS_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(body  != nullptr);

    // Descriptor readback carries id + geometry.
    SaoPanelDescriptor readback{};
    REQUIRE(sao_ui_panel_get_descriptor(panel, &readback) == SAO_STATUS_OK);
    REQUIRE(readback.panel_id_utf8 != nullptr);
    REQUIRE(std::strcmp(readback.panel_id_utf8,
                       "panel_sdk_test_register") == 0);
    REQUIRE(readback.default_width_px == 480);

    // Opacity honours the descriptor's initial value.
    REQUIRE(sao_ui_panel_get_opacity_(panel) > 0.9f);

    // Duplicate id → ERR_ALREADY_EXISTS.
    sao_ui_panel_handle_t dup = nullptr;
    sao_ui_panel_body_handle_t dup_body = nullptr;
    const auto dup_desc = make_descriptor(
        "panel_sdk_test_register", SAO_UI_PANEL_Z_NORMAL, 0);
    REQUIRE(sao_ui_panel_register(nullptr, &dup_desc, &dup, &dup_body)
            == SAO_STATUS_ERR_ALREADY_EXISTS);
    REQUIRE(dup == nullptr);

    // Cleanup.
    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
    size_t after = 0;
    REQUIRE(sao_ui_panel_registry_count(&after) == SAO_STATUS_OK);
    REQUIRE(after == baseline);
}

TEST_CASE("panel_registry_count_tracks_registrations",
          "[ui][panel_sdk][wave4]") {
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

TEST_CASE("panel_unregister_removes_from_registry",
          "[ui][panel_sdk][wave4]") {
    const auto desc = make_descriptor(
        "panel_sdk_unreg_target", SAO_UI_PANEL_Z_TOPMOST, 5);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &desc, &panel, &body)
            == SAO_STATUS_OK);

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

TEST_CASE("panel_bring_to_front_updates_z", "[ui][panel_sdk][wave4]") {
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

TEST_CASE("panel_update_body_batches_mutations", "[ui][panel_sdk][wave4]") {
    const auto desc = make_descriptor(
        "panel_body_batch_target", SAO_UI_PANEL_Z_NORMAL, 0);
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_panel_body_handle_t body = nullptr;
    REQUIRE(sao_ui_panel_register(nullptr, &desc, &panel, &body)
            == SAO_STATUS_OK);

    SaoUiBodyMutation batch[3] = {};
    batch[0].kind = SAO_UI_BODY_ADD_CONTAINER;
    batch[1].kind = SAO_UI_BODY_ADD_WIDGET;
    batch[2].kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;

    REQUIRE(sao_ui_panel_update_body(body, batch, 3) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_body_mutation_count(body) == 3);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 0)
            == SAO_UI_BODY_ADD_CONTAINER);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 1)
            == SAO_UI_BODY_ADD_WIDGET);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 2)
            == SAO_UI_BODY_UPDATE_WIDGET_PROPS);

    // Second batch appends to the same log.
    SaoUiBodyMutation batch2[1] = {};
    batch2[0].kind = SAO_UI_BODY_REORDER_NODE;
    REQUIRE(sao_ui_panel_update_body(body, batch2, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_body_mutation_count(body) == 4);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 3)
            == SAO_UI_BODY_REORDER_NODE);

    // set_spec records a synthetic UPDATE_SPEC event.
    const char kSpec[] = "{}";
    REQUIRE(sao_ui_panel_body_set_spec(
                body, reinterpret_cast<const uint8_t*>(kSpec), 2)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_panel_body_mutation_count(body) == 5);
    REQUIRE(sao_ui_panel_body_mutation_at(body, 4)
            == SAO_UI_BODY_UPDATE_SPEC);

    REQUIRE(sao_ui_panel_unregister(panel) == SAO_STATUS_OK);
}
