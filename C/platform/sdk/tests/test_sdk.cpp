#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/input_router.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"
#include "sao/ui/widget_kit.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_panel_state_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_panel_unregister(sao_sdk_status_t status);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_widget_count(const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_cleanup_pending_count(const SaoSdkContext* ctx);
extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_runtime_state(
    void** out_compositor, void** out_input_router, bool* out_owns_compositor,
    bool* out_compositor_bound, size_t* out_active_contexts);
extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_reset_runtime(void);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_test_fail_next_compositor_destroy_after_preflight(void);
extern "C" SAO_SDK_API sao_ui_panel_body_handle_t SAO_SDK_CALL
sao_sdk_test_panel_body(const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_panel_body_mutation_count(sao_ui_panel_body_handle_t body);
extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_body_mutation_at(sao_ui_panel_body_handle_t body, size_t idx);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_body_replace_failure_point(int32_t point);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_widget_test_props_state(sao_ui_widget_handle_t handle, const char* color_key,
                                                                      uint32_t* out_color,
                                                                      char* out_text,
                                                                      size_t out_text_capacity);
extern "C" SAO_SDK_API sao_ui_widget_handle_t SAO_SDK_CALL
sao_sdk_test_widget_native_handle(const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel,
                                   sao_sdk_ui_widget_t widget);

namespace {

SaoSdkPanelDescriptor test_panel_descriptor(const char* panel_id) {
    SaoSdkPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = panel_id;
    descriptor.title_utf8 = "SDK UI transaction test";
    descriptor.default_width_px = 320;
    descriptor.default_height_px = 240;
    descriptor.min_width_px = 100;
    descriptor.min_height_px = 80;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = true;
    descriptor.initial_opacity = 1.0F;
    return descriptor;
}

sao_sdk_status_t SAO_SDK_CALL throwing_register_panel(void*, const SaoSdkPanelDescriptor*,
                                                       sao_sdk_ui_panel_t*) {
    throw std::runtime_error("UI callback fixture");
}

size_t compositor_layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    return sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK
               ? count
               : (std::numeric_limits<size_t>::max)();
}

struct RuntimeBindProbe {
    sao_ui_compositor_handle_t replacement = nullptr;
    sao_sdk_status_t status = SAO_SDK_OK;
};

struct RouterLeaseProbe {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

struct RouterRegistrationProbe {
    uint32_t calls = 0;
};

void SAO_UI_CALL bind_during_render(void*, float, void* user_data) {
    auto& probe = *static_cast<RuntimeBindProbe*>(user_data);
    probe.status = sao_sdk_platform_bind_ui_compositor(probe.replacement);
}

void SAO_UI_CALL hold_router_lease(sao_ui_widget_handle_t, sao_ui_widget_handle_t,
                                   void* user_data) {
    auto& probe = *static_cast<RouterLeaseProbe*>(user_data);
    std::unique_lock lock(probe.mutex);
    probe.entered = true;
    probe.changed.notify_all();
    probe.changed.wait(lock, [&probe] { return probe.release; });
}

void SAO_UI_CALL record_router_hotkey(const char*, const SaoUiInputEvent*, void* user_data) {
    ++static_cast<RouterRegistrationProbe*>(user_data)->calls;
}

sao_ui_widget_handle_t create_router_probe_widget() {
    SaoUiButtonSpec spec{};
    spec.text_utf8 = "router-lease";
    spec.kind = SAO_UI_BTN_NORMAL;
    spec.radius_px = 4;
    spec.pad_x_px = 8;
    spec.pad_y_px = 4;
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &spec, &widget) == SAO_STATUS_OK);
    return widget;
}

sao_sdk_status_t SAO_SDK_CALL motion_predicted_view_stub(
    void*, sao_sdk_gpu_tracker_t, uint32_t, float out_matrix[16], float out_cam[3],
    float* out_confidence) {
    if (out_matrix != nullptr)
        out_matrix[0] = 1.0F;
    if (out_cam != nullptr)
        out_cam[0] = 2.0F;
    if (out_confidence != nullptr)
        *out_confidence = 0.75F;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL motion_confidence_stub(
    void*, sao_sdk_gpu_tracker_t, float* out_confidence) {
    if (out_confidence != nullptr)
        *out_confidence = 0.5F;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL motion_sample_count_stub(
    void*, sao_sdk_gpu_tracker_t, uint32_t* out_count) {
    if (out_count != nullptr)
        *out_count = 4;
    return SAO_SDK_OK;
}

} // namespace

TEST_CASE("sdk ABI version is queryable", "[sdk][abi]") {
    REQUIRE(sao_sdk_abi_version() == SAO_SDK_ABI_VERSION);
    REQUIRE(SAO_SDK_ABI_VERSION_MAJOR == 1u);
    REQUIRE(SAO_SDK_ABI_VERSION_MINOR == 11u);
    REQUIRE(SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MAJOR == SAO_SDK_ABI_VERSION_MAJOR);
    REQUIRE(SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MINOR == SAO_SDK_ABI_VERSION_MINOR);
    REQUIRE(SAO_SDK_GPU_HUNT_TABLE_LEGACY_SIZE ==
            offsetof(SaoSdkGpuHuntTable, get_split_lock_info) +
                sizeof(SaoSdkGpuHuntTable::get_split_lock_info));
    REQUIRE(offsetof(SaoSdkGpuHuntTable, abi_version) == 224u);
    REQUIRE(offsetof(SaoSdkGpuHuntTable, struct_size) == 228u);
    REQUIRE(SAO_SDK_GPU_HUNT_TABLE_REQUIRED_SIZE == 232u);
    REQUIRE(offsetof(SaoSdkGpuHuntTable, get_predicted_view) == 232u);
    REQUIRE(offsetof(SaoSdkGpuHuntTable, get_motion_prediction_confidence) == 240u);
    REQUIRE(offsetof(SaoSdkGpuHuntTable, get_motion_prediction_sample_count) == 248u);
    REQUIRE(SAO_SDK_GPU_HUNT_TABLE_MOTION_REQUIRED_SIZE == 256u);
    REQUIRE(sizeof(SaoSdkGpuHuntTable) == 256u);
}

TEST_CASE("GPU table metadata gates current slots without changing legacy offsets",
          "[sdk][abi][gpu_hunt][table]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("gpu.table.abi", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt != nullptr);
    CHECK(ctx.gpu_hunt->abi_version == SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION);
    CHECK(ctx.gpu_hunt->struct_size == sizeof(SaoSdkGpuHuntTable));
    CHECK(ctx.gpu_hunt->get_predicted_view != nullptr);
    CHECK(ctx.gpu_hunt->get_motion_prediction_confidence != nullptr);
    CHECK(ctx.gpu_hunt->get_motion_prediction_sample_count != nullptr);
    CHECK(sao_sdk_gpu_hunt_table_status(&ctx) == SAO_SDK_OK);

    SaoSdkGpuHuntTable table = *ctx.gpu_hunt;
    SaoSdkContext probe = ctx;
    probe.gpu_hunt = &table;

    probe.abi_version = (SAO_SDK_ABI_VERSION_MAJOR << 16) | 10u;
    CHECK(sao_sdk_gpu_hunt_table_status(&probe) == SAO_SDK_ERR_UNSUPPORTED);
    probe.abi_version = SAO_SDK_ABI_VERSION;

    table.abi_version = (2u << 16) | SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MINOR;
    CHECK(sao_sdk_gpu_hunt_table_status(&probe) == SAO_SDK_ERR_ABI_MISMATCH);
    table.abi_version = (SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MAJOR << 16) | 10u;
    CHECK(sao_sdk_gpu_hunt_table_status(&probe) == SAO_SDK_ERR_UNSUPPORTED);
    table.abi_version = SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION;
    table.struct_size = SAO_SDK_GPU_HUNT_TABLE_LEGACY_SIZE;
    CHECK(sao_sdk_gpu_hunt_table_status(&probe) == SAO_SDK_ERR_UNSUPPORTED);
    table.struct_size = SAO_SDK_GPU_HUNT_TABLE_REQUIRED_SIZE;
    CHECK(sao_sdk_gpu_hunt_table_status(&probe) == SAO_SDK_OK);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("GPU motion tail authorizes each slot independently by struct size",
          "[sdk][abi][gpu_hunt][table][motion][tail]") {
    SaoSdkContext context{};
    REQUIRE(sao_sdk_bind_context("gpu.table.motion.tail", "1.0", &context) == SAO_SDK_OK);
    SaoSdkGpuHuntTable table = *context.gpu_hunt;
    table.get_predicted_view = &motion_predicted_view_stub;
    table.get_motion_prediction_confidence = &motion_confidence_stub;
    table.get_motion_prediction_sample_count = &motion_sample_count_stub;
    SaoSdkContext probe = context;
    probe.gpu_hunt = &table;

    float matrix[16]{};
    float camera[3]{};
    float confidence = 0.0F;
    uint32_t sample_count = 0;
    table.struct_size = SAO_SDK_GPU_HUNT_TABLE_REQUIRED_SIZE;
    CHECK(sao_sdk_gpu_hunt_get_predicted_view(&probe, 0, 1, matrix, camera, &confidence) ==
          SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_gpu_hunt_get_motion_prediction_confidence(&probe, 0, &confidence) ==
          SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_gpu_hunt_get_motion_prediction_sample_count(&probe, 0, &sample_count) ==
          SAO_SDK_ERR_UNSUPPORTED);

    table.struct_size = offsetof(SaoSdkGpuHuntTable, get_motion_prediction_confidence);
    REQUIRE(sao_sdk_gpu_hunt_get_predicted_view(&probe, 0, 1, matrix, camera, &confidence) ==
            SAO_SDK_OK);
    CHECK(matrix[0] == 1.0F);
    CHECK(camera[0] == 2.0F);
    CHECK(confidence == 0.75F);
    CHECK(sao_sdk_gpu_hunt_get_motion_prediction_confidence(&probe, 0, &confidence) ==
          SAO_SDK_ERR_UNSUPPORTED);

    table.struct_size = offsetof(SaoSdkGpuHuntTable, get_motion_prediction_sample_count);
    REQUIRE(sao_sdk_gpu_hunt_get_motion_prediction_confidence(&probe, 0, &confidence) ==
            SAO_SDK_OK);
    CHECK(confidence == 0.5F);
    CHECK(sao_sdk_gpu_hunt_get_motion_prediction_sample_count(&probe, 0, &sample_count) ==
          SAO_SDK_ERR_UNSUPPORTED);

    table.struct_size = SAO_SDK_GPU_HUNT_TABLE_MOTION_REQUIRED_SIZE;
    REQUIRE(sao_sdk_gpu_hunt_get_motion_prediction_sample_count(&probe, 0, &sample_count) ==
            SAO_SDK_OK);
    CHECK(sample_count == 4);
    REQUIRE(sao_sdk_context_try_destroy(&context) == SAO_SDK_OK);
}

TEST_CASE("SDK first compositor bind does not manufacture a headless runtime",
          "[sdk][ui][compositor][binding][transaction]") {
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
    void* runtime_compositor = reinterpret_cast<void*>(uintptr_t{1});
    void* runtime_router = reinterpret_cast<void*>(uintptr_t{1});
    bool owns_compositor = true;
    bool compositor_bound = true;
    size_t active_contexts = 1;
    REQUIRE(sao_sdk_test_runtime_state(&runtime_compositor, &runtime_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(runtime_compositor == nullptr);
    CHECK(runtime_router == nullptr);
    CHECK_FALSE(owns_compositor);
    CHECK_FALSE(compositor_bound);
    CHECK(active_contexts == 0);

    sao_ui_compositor_handle_t replacement = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &replacement) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_platform_bind_ui_compositor(replacement) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_runtime_state(&runtime_compositor, &runtime_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(runtime_compositor == replacement);
    CHECK(runtime_router != nullptr);
    CHECK_FALSE(owns_compositor);
    CHECK(compositor_bound);
    CHECK(active_contexts == 0);

    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(replacement) == SAO_STATUS_OK);
}

TEST_CASE("SDK rejects arbitrary and stale compositor handles without changing runtime",
        "[sdk][ui][compositor][binding][handle][hardening]") {
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
    CHECK(sao_sdk_platform_bind_ui_compositor(reinterpret_cast<void*>(uintptr_t{0x1234})) ==
        SAO_SDK_ERR_HANDLE_INVALID);

    sao_ui_compositor_handle_t stale = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &stale) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(stale) == SAO_STATUS_OK);
    CHECK(sao_sdk_platform_bind_ui_compositor(stale) == SAO_SDK_ERR_HANDLE_INVALID);

    void* runtime_compositor = reinterpret_cast<void*>(uintptr_t{1});
    void* runtime_router = reinterpret_cast<void*>(uintptr_t{1});
    bool owns_compositor = true;
    bool compositor_bound = true;
    size_t active_contexts = 1;
    REQUIRE(sao_sdk_test_runtime_state(&runtime_compositor, &runtime_router, &owns_compositor,
                           &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(runtime_compositor == nullptr);
    CHECK(runtime_router == nullptr);
    CHECK_FALSE(owns_compositor);
    CHECK_FALSE(compositor_bound);
    CHECK(active_contexts == 0);
}

TEST_CASE("SDK bind failure preserves the old headless compositor and router for retry",
          "[sdk][ui][compositor][binding][transaction][retry]") {
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("runtime.bind.retry", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);

    void* old_compositor_raw = nullptr;
    void* old_router = nullptr;
    bool owns_compositor = false;
    bool compositor_bound = true;
    size_t active_contexts = 1;
    REQUIRE(sao_sdk_test_runtime_state(&old_compositor_raw, &old_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    auto* old_compositor = static_cast<sao_ui_compositor_handle_t>(old_compositor_raw);
    REQUIRE(old_compositor != nullptr);
    REQUIRE(old_router != nullptr);
    CHECK(owns_compositor);
    CHECK_FALSE(compositor_bound);
    CHECK(active_contexts == 0);

    sao_ui_compositor_handle_t replacement = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &replacement) == SAO_STATUS_OK);
    RuntimeBindProbe probe{replacement};
    SaoLayerConfig layer_config{};
    layer_config.name_utf8 = "sdk.bind.retry.callback";
    layer_config.width = 2;
    layer_config.height = 2;
    layer_config.click_through = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(old_compositor, &layer_config, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_render_fn(layer, &bind_during_render, &probe) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(old_compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(probe.status == SAO_SDK_ERR_BUSY);

    void* retained_compositor = nullptr;
    void* retained_router = nullptr;
    REQUIRE(sao_sdk_test_runtime_state(&retained_compositor, &retained_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(retained_compositor == old_compositor_raw);
    CHECK(retained_router == old_router);
    CHECK(owns_compositor);
    CHECK_FALSE(compositor_bound);

    sao_ui_layer_destroy(layer);
    REQUIRE(sao_sdk_platform_bind_ui_compositor(replacement) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_runtime_state(&retained_compositor, &retained_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(retained_compositor == replacement);
    CHECK(retained_router == old_router);
    CHECK_FALSE(owns_compositor);
    CHECK(compositor_bound);
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(replacement) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
}

TEST_CASE("SDK bind retries after an in-flight headless router lease",
          "[sdk][ui][compositor][binding][router][transaction][retry]") {
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
    SaoSdkContext context{};
    REQUIRE(sao_sdk_bind_context("runtime.router.bind.retry", "1.0", &context) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&context) == SAO_SDK_OK);

    void* old_compositor = nullptr;
    void* old_router_raw = nullptr;
    bool owns_compositor = false;
    bool compositor_bound = true;
    size_t active_contexts = 1;
    REQUIRE(sao_sdk_test_runtime_state(&old_compositor, &old_router_raw, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    auto* old_router = static_cast<sao_ui_input_router_deep_handle_t>(old_router_raw);
    REQUIRE(old_compositor != nullptr);
    REQUIRE(old_router != nullptr);
    CHECK(owns_compositor);
    CHECK_FALSE(compositor_bound);

    const auto widget = create_router_probe_widget();
    REQUIRE(sao_ui_input_router_set_focus_widget(old_router, widget) == SAO_STATUS_OK);
    RouterLeaseProbe probe;
    REQUIRE(sao_ui_input_router_set_hover_change_handler(old_router, &hold_router_lease, &probe) ==
            SAO_STATUS_OK);
    SaoUiInputEvent move{};
    move.kind = SAO_UI_INPUT_MOUSE_MOVE;
    move.screen_x_px = 1;
    move.screen_y_px = 1;
    bool consumed = false;
    sao_status_t route_status = SAO_STATUS_ERR_UNKNOWN;
    std::thread route_thread([&] {
        route_status = sao_ui_input_router_route_event(old_router, &move, &consumed);
    });
    {
        std::unique_lock lock(probe.mutex);
        REQUIRE(probe.changed.wait_for(lock, std::chrono::seconds(5),
                                       [&probe] { return probe.entered; }));
    }

    sao_ui_compositor_handle_t replacement = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &replacement) == SAO_STATUS_OK);
    CHECK(sao_sdk_platform_bind_ui_compositor(replacement) == SAO_SDK_ERR_BUSY);
    void* retained_compositor = nullptr;
    void* retained_router = nullptr;
    REQUIRE(sao_sdk_test_runtime_state(&retained_compositor, &retained_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(retained_compositor == old_compositor);
    CHECK(retained_router == old_router_raw);
    CHECK(owns_compositor);
    CHECK_FALSE(compositor_bound);

    {
        std::lock_guard lock(probe.mutex);
        probe.release = true;
    }
    probe.changed.notify_all();
    route_thread.join();
    CHECK(route_status == SAO_STATUS_OK);
    CHECK(consumed);

    REQUIRE(sao_sdk_platform_bind_ui_compositor(replacement) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_runtime_state(&retained_compositor, &retained_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(retained_compositor == replacement);
    CHECK(retained_router == old_router_raw);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(old_router, &focused, nullptr) == SAO_STATUS_OK);
    CHECK(focused == widget);
    sao_ui_widget_destroy(widget);
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(replacement) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
}

TEST_CASE("SDK bind rolls back a post-preflight destroy failure with the same router",
          "[sdk][ui][compositor][binding][router][transaction][rollback]") {
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
    SaoSdkContext context{};
    REQUIRE(sao_sdk_bind_context("runtime.router.rollback", "1.0", &context) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&context) == SAO_SDK_OK);

    void* old_compositor_raw = nullptr;
    void* old_router_raw = nullptr;
    bool owns_compositor = false;
    bool compositor_bound = true;
    size_t active_contexts = 1;
    REQUIRE(sao_sdk_test_runtime_state(&old_compositor_raw, &old_router_raw, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    auto* old_router = static_cast<sao_ui_input_router_deep_handle_t>(old_router_raw);
    REQUIRE(old_compositor_raw != nullptr);
    REQUIRE(old_router != nullptr);
    CHECK(owns_compositor);
    CHECK_FALSE(compositor_bound);

    const auto widget = create_router_probe_widget();
    REQUIRE(sao_ui_input_router_set_focus_widget(old_router, widget) == SAO_STATUS_OK);
    RouterRegistrationProbe hotkey_probe;
    SaoUiHotkeyBindingSpec hotkey_spec{};
    hotkey_spec.binding_id_utf8 = "sdk-router-rollback";
    hotkey_spec.virtual_key = 0x79;
    hotkey_spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    sao_ui_hotkey_binding_t hotkey = 0;
    REQUIRE(sao_ui_input_router_register_hotkey(old_router, "sdk", &hotkey_spec,
                                                 &record_router_hotkey, &hotkey_probe,
                                                 &hotkey) == SAO_STATUS_OK);
    size_t hotkey_conflicts = 0;
    REQUIRE(sao_ui_input_router_find_conflicts(old_router, hotkey_spec.virtual_key, 0, nullptr, 0,
                                                &hotkey_conflicts) == SAO_STATUS_OK);
    CHECK(hotkey_conflicts == 1);
    sao_ui_compositor_handle_t replacement = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &replacement) == SAO_STATUS_OK);
    sao_ui_test_fail_next_compositor_destroy_after_preflight();
    CHECK(sao_sdk_platform_bind_ui_compositor(replacement) == SAO_SDK_ERR_INTERNAL);

    void* retained_compositor = nullptr;
    void* retained_router = nullptr;
    REQUIRE(sao_sdk_test_runtime_state(&retained_compositor, &retained_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(retained_compositor == old_compositor_raw);
    CHECK(retained_router == old_router_raw);
    CHECK(owns_compositor);
    CHECK_FALSE(compositor_bound);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_input_router_get_focus(old_router, &focused, nullptr) == SAO_STATUS_OK);
    CHECK(focused == widget);
    hotkey_conflicts = 0;
    REQUIRE(sao_ui_input_router_find_conflicts(old_router, hotkey_spec.virtual_key, 0, nullptr, 0,
                                                &hotkey_conflicts) == SAO_STATUS_OK);
    CHECK(hotkey_conflicts == 1);
    SaoUiInputEvent hotkey_event{};
    hotkey_event.kind = SAO_UI_INPUT_KEY_DOWN;
    hotkey_event.virtual_key = hotkey_spec.virtual_key;
    sao_ui_hotkey_binding_t matched = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(old_router, &hotkey_event, &matched) ==
            SAO_STATUS_OK);
    CHECK(matched == hotkey);
    CHECK(hotkey_probe.calls == 1);

    REQUIRE(sao_sdk_platform_bind_ui_compositor(replacement) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_runtime_state(&retained_compositor, &retained_router, &owns_compositor,
                                       &compositor_bound, &active_contexts) == SAO_SDK_OK);
    CHECK(retained_compositor == replacement);
    CHECK(retained_router == old_router_raw);
    CHECK_FALSE(owns_compositor);
    CHECK(compositor_bound);
    REQUIRE(sao_ui_input_router_get_focus(old_router, &focused, nullptr) == SAO_STATUS_OK);
    CHECK(focused == widget);
    hotkey_conflicts = 0;
    REQUIRE(sao_ui_input_router_find_conflicts(old_router, hotkey_spec.virtual_key, 0, nullptr, 0,
                                                &hotkey_conflicts) == SAO_STATUS_OK);
    CHECK(hotkey_conflicts == 1);
    matched = 0;
    REQUIRE(sao_ui_input_router_match_hotkey(old_router, &hotkey_event, &matched) ==
            SAO_STATUS_OK);
    CHECK(matched == hotkey);
    CHECK(hotkey_probe.calls == 2);

    REQUIRE(sao_ui_input_router_unregister_hotkey(old_router, hotkey) == SAO_STATUS_OK);
    sao_ui_widget_destroy(widget);
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(replacement) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
}

TEST_CASE("sdk_bind_context creates the unified owned context", "[sdk][bind]") {
    SaoSdkContext ctx;
    for (auto& b : reinterpret_cast<uint8_t (&)[sizeof(ctx)]>(ctx))
        b = 0xEE;
    const auto rc = sao_sdk_bind_context("test", "0.0.0", &ctx);
    REQUIRE(rc == SAO_SDK_OK);
    REQUIRE(ctx.abi_version == SAO_SDK_ABI_VERSION);
    REQUIRE(ctx.ctx_impl != nullptr);
    REQUIRE(ctx.ui != nullptr);
    REQUIRE(ctx.event != nullptr);
    REQUIRE(ctx.mem != nullptr);
    REQUIRE(ctx.net != nullptr);
    REQUIRE(ctx.config != nullptr);
    REQUIRE(ctx.hotkey != nullptr);
    REQUIRE(ctx.tts != nullptr);
    REQUIRE(ctx.banner != nullptr);
    REQUIRE(ctx.gpu_hunt != nullptr);

    bool enabled = false;
    REQUIRE(sao_sdk_config_set_bool(&ctx, "enabled", true) == SAO_SDK_OK);
    REQUIRE(sao_sdk_config_get_bool(&ctx, "enabled", &enabled) == SAO_SDK_OK);
    REQUIRE(enabled);

    uint32_t value = 0;
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0, &value) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(sao_sdk_net_set_frame_callback(&ctx, nullptr, nullptr) == SAO_SDK_ERR_UNSUPPORTED);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    sao_sdk_context_destroy(&ctx);
    REQUIRE(ctx.ctx_impl == nullptr);
    REQUIRE(ctx.ui == nullptr);
}

TEST_CASE("sdk_bind_context rejects null out_ctx", "[sdk][bind]") {
    REQUIRE(sao_sdk_bind_context("test", "0.0.0", nullptr) == SAO_SDK_ERR_INVALID_ARGUMENT);
}

TEST_CASE("launcher binding makes SDK panels borrow the central compositor",
          "[sdk][ui][compositor][binding]") {
    REQUIRE(sao_sdk_test_reset_runtime() == SAO_SDK_OK);
    SaoCompositorConfig config{};
    config.target_hz = 60;
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    void* bound = reinterpret_cast<void*>(uintptr_t{1});
    CHECK(sao_sdk_platform_get_ui_compositor(&bound) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(bound == nullptr);
    REQUIRE(sao_sdk_platform_bind_ui_compositor(compositor) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_get_ui_compositor(&bound) == SAO_SDK_OK);
    CHECK(bound == compositor);
    CHECK(sao_sdk_platform_bind_ui_compositor(compositor) == SAO_SDK_ERR_ALREADY_EXISTS);

    sao_sdk_status_t wrong_thread_bind = SAO_SDK_OK;
    sao_sdk_status_t wrong_thread_unbind = SAO_SDK_OK;
    std::thread wrong_thread([&] {
        wrong_thread_bind = sao_sdk_platform_bind_ui_compositor(compositor);
        wrong_thread_unbind = sao_sdk_platform_unbind_ui_compositor();
    });
    wrong_thread.join();
    CHECK(wrong_thread_bind == SAO_SDK_ERR_ACCESS_DENIED);
    CHECK(wrong_thread_unbind == SAO_SDK_ERR_ACCESS_DENIED);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.central.binding", "1.0", &ctx) == SAO_SDK_OK);
    CHECK(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_ERR_BUSY);
    CHECK(sao_sdk_platform_bind_ui_compositor(compositor) == SAO_SDK_ERR_BUSY);
    CHECK(compositor_layer_count(compositor) == 0);

    const auto descriptor = test_panel_descriptor("sdk.ui.central.binding");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);
    CHECK(compositor_layer_count(compositor) == 1);
    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    CHECK(compositor_layer_count(compositor) == 0);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    bound = reinterpret_cast<void*>(uintptr_t{1});
    CHECK(sao_sdk_platform_get_ui_compositor(&bound) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(bound == nullptr);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("legacy JSON UI path is context-owned", "[sdk][legacy_ui]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("legacy.test", "1.0.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    constexpr char kSpec[] = R"({"kind":"panel","children":[{"kind":"canvas"}]})";
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_ui_register_panel(&ctx, "legacy.panel", "Legacy Panel",
                                      reinterpret_cast<const uint8_t*>(kSpec), sizeof(kSpec) - 1,
                                      nullptr, nullptr, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_sdk_ui_set_panel_spec(&ctx, panel, reinterpret_cast<const uint8_t*>(kSpec),
                                      sizeof(kSpec) - 1) == SAO_SDK_OK);
    REQUIRE(sao_sdk_ui_set_overlay(&ctx, "legacy.surface", reinterpret_cast<const uint8_t*>(kSpec),
                                   sizeof(kSpec) - 1) == SAO_SDK_OK);

    sao_sdk_hook_token_t token = 0;
    REQUIRE(ctx.ui->register_render_hook(ctx.ctx_impl, "legacy.surface", 1.0f,
                                         reinterpret_cast<void*>(uintptr_t(1)), nullptr,
                                         &token) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(token == 0);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    sao_sdk_context_destroy(&ctx);
}

TEST_CASE("panel unregister failure preserves SDK ownership for retry",
          "[sdk][ui][panel][unregister][retry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.unregister.retry", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.unregister.retry");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);

    SaoSdkWidgetSpec widget{};
    widget.kind = SAO_SDK_UI_WIDGET_LABEL;
    widget.widget_id_utf8 = "retained-widget";
    widget.text_utf8 = "retained";
    sao_sdk_ui_widget_t widget_handle = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(&ctx, panel, &widget, &widget_handle) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_panel_widget_count(&ctx, panel) == 1);

    sao_sdk_test_fail_next_panel_unregister(SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_test_panel_widget_count(&ctx, panel) == 1);
    sao_sdk_ui_panel_t duplicate = nullptr;
    CHECK(sao_sdk_register_ui_panel(&ctx, &descriptor, &duplicate) ==
          SAO_SDK_ERR_ALREADY_EXISTS);
    CHECK(duplicate == nullptr);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    CHECK(sao_sdk_test_panel_widget_count(&ctx, panel) == 0);
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("panel registration rolls native ownership back when SDK insertion fails",
          "[sdk][ui][panel][register][rollback]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.register.rollback", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.register.rollback");

    sao_sdk_test_fail_next_panel_state_insertion();
    sao_sdk_ui_panel_t panel = reinterpret_cast<sao_sdk_ui_panel_t>(uintptr_t{1});
    CHECK(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_ERR_INTERNAL);
    CHECK(panel == nullptr);

    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("panel registration quarantines native ownership when insertion and rollback fail",
          "[sdk][ui][panel][register][rollback][cleanup-pending]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.register.double_failure", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.register.double_failure");

    sao_sdk_test_fail_next_panel_state_insertion();
    sao_sdk_test_fail_next_panel_unregister(SAO_SDK_ERR_INTERNAL);
    sao_sdk_ui_panel_t panel = reinterpret_cast<sao_sdk_ui_panel_t>(uintptr_t{1});
    CHECK(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_ERR_INTERNAL);
    CHECK(panel == nullptr);
    CHECK(sao_sdk_test_panel_cleanup_pending_count(&ctx) == 1);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);

    SaoSdkContext retry{};
    REQUIRE(sao_sdk_bind_context("ui.register.double_failure.retry", "1.0", &retry) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_register_ui_panel(&retry, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_ui_panel(&retry, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&retry) == SAO_SDK_OK);
}

TEST_CASE("unknown widget kind is rejected without panel side effects",
          "[sdk][ui][widget][validation]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.widget.validation", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.widget.validation");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);

    SaoSdkWidgetSpec widget{};
    widget.kind = 777;
    widget.widget_id_utf8 = "unknown-widget";
    sao_sdk_ui_widget_t widget_handle = reinterpret_cast<sao_sdk_ui_widget_t>(uintptr_t{1});
    CHECK(sao_sdk_panel_add_widget(&ctx, panel, &widget, &widget_handle) ==
          SAO_SDK_ERR_INVALID_ARGUMENT);
    CHECK(widget_handle == nullptr);
    CHECK(sao_sdk_test_panel_widget_count(&ctx, panel) == 0);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("public UI C ABI contains callback exceptions", "[sdk][ui][abi][exception]") {
    SaoSdkUiTable table{};
    table.register_ui_panel = throwing_register_panel;
    table.abi_version = SAO_SDK_UI_TABLE_ABI_VERSION;
    table.struct_size = sizeof(SaoSdkUiTable);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.throwing.callback", "1.0", &ctx) == SAO_SDK_OK);
    ctx.ui = &table;
    const auto descriptor = test_panel_descriptor("sdk.ui.throwing.callback");
    sao_sdk_ui_panel_t panel = nullptr;
    sao_sdk_status_t status = SAO_SDK_OK;

    CHECK_NOTHROW(status = sao_sdk_register_ui_panel(&ctx, &descriptor, &panel));
    CHECK(status == SAO_SDK_ERR_INTERNAL);
    CHECK(panel == nullptr);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

namespace {
sao_sdk_status_t SAO_SDK_CALL legacy_prefix_register(void*, const char*, const char*,
                                                      const uint8_t*, size_t,
                                                      sao_sdk_panel_action_callback_t, void*,
                                                      sao_sdk_ui_panel_t*) {
    return SAO_SDK_ERR_UNSUPPORTED;
}
}

TEST_CASE("seven-slot old context rejects typed slots without reading table metadata",
          "[sdk][ui][abi][table][old-context][guard-page]") {
    SaoSdkContext context{};
    REQUIRE(sao_sdk_bind_context("sdk.ui.short.table", "1.0", &context) == SAO_SDK_OK);
    const auto* original_ui = context.ui;
    struct LegacyStorage {
        alignas(SaoSdkUiTable) std::array<std::byte, SAO_SDK_UI_TABLE_LEGACY_SIZE> bytes{};
        uint64_t canary = 0x1122334455667788ULL;
    } storage;
    const auto legacy_register = &legacy_prefix_register;
    std::memcpy(storage.bytes.data(), &legacy_register, sizeof(legacy_register));
    context.abi_version = (SAO_SDK_ABI_VERSION_MAJOR << 16) | 10u;
    context.ui = reinterpret_cast<const SaoSdkUiTable*>(storage.bytes.data());

    const auto descriptor = test_panel_descriptor("sdk.ui.short.table.panel");
    SaoSdkWidgetSpec widget{};
    widget.kind = SAO_SDK_UI_WIDGET_LABEL;
    widget.widget_id_utf8 = "short";
    widget.text_utf8 = "short";
    sao_sdk_ui_panel_t panel = reinterpret_cast<sao_sdk_ui_panel_t>(uintptr_t{1});
    sao_sdk_ui_widget_t handle = reinterpret_cast<sao_sdk_ui_widget_t>(uintptr_t{2});
    CHECK(sao_sdk_register_ui_panel(&context, &descriptor, &panel) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_panel_add_widget(&context, panel, &widget, &handle) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_panel_update_widget(&context, panel, handle, &widget) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_panel_remove_widget(&context, panel, handle) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(storage.canary == 0x1122334455667788ULL);

    context.ui = original_ui;
    context.abi_version = SAO_SDK_ABI_VERSION;
    REQUIRE(sao_sdk_context_try_destroy(&context) == SAO_SDK_OK);
}

TEST_CASE("legacy set_panel_spec clears typed widgets and blocks mixed CRUD",
          "[sdk][ui][typed][legacy][stale]") {
    SaoSdkContext context{};
    REQUIRE(sao_sdk_bind_context("sdk.typed.legacy.mode", "1.0", &context) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.typed.legacy.mode");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&context, &descriptor, &panel) == SAO_SDK_OK);
    SaoSdkWidgetSpec widget{};
    widget.kind = SAO_SDK_UI_WIDGET_LABEL;
    widget.widget_id_utf8 = "typed-label";
    widget.text_utf8 = "typed";
    sao_sdk_ui_widget_t handle = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(&context, panel, &widget, &handle) == SAO_SDK_OK);
    CHECK(sao_sdk_test_panel_widget_count(&context, panel) == 1);
    const auto body = sao_sdk_test_panel_body(&context, panel);
    REQUIRE(body != nullptr);
    const size_t mutations_before = sao_ui_panel_body_mutation_count(body);
    constexpr char legacy_spec[] = R"({"kind":"panel","children":[]})";
    REQUIRE(sao_sdk_ui_set_panel_spec(&context, panel,
                                      reinterpret_cast<const uint8_t*>(legacy_spec),
                                      sizeof(legacy_spec) - 1) == SAO_SDK_OK);
    CHECK(sao_sdk_test_panel_widget_count(&context, panel) == 0);
    bool removed_native_node = false;
    const size_t mutations_after = sao_ui_panel_body_mutation_count(body);
    for (size_t index = mutations_before; index < mutations_after; ++index)
        removed_native_node = removed_native_node ||
                              sao_ui_panel_body_mutation_at(body, index) == SAO_UI_BODY_REMOVE_NODE;
    CHECK(removed_native_node);
    sao_sdk_ui_widget_t blocked = nullptr;
    CHECK(sao_sdk_panel_add_widget(&context, panel, &widget, &blocked) == SAO_SDK_ERR_BUSY);
    REQUIRE(sao_sdk_unregister_ui_panel(&context, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&context) == SAO_SDK_OK);
}

TEST_CASE("typed widget update quarantines native rollback uncertainty and retries cleanup",
          "[sdk][ui][typed][rollback][quarantine][retry][canary]") {
    SaoSdkContext context{};
    REQUIRE(sao_sdk_bind_context("sdk.typed.native.rollback.uncertain", "1.0", &context) ==
            SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.typed.native.rollback.uncertain.panel");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&context, &descriptor, &panel) == SAO_SDK_OK);

    constexpr char initial_props[] = R"({"fill":"#112233"})";
    struct WidgetSpecFixture {
        SaoSdkWidgetSpec value{};
        uint64_t canary = 0x13579BDF2468ACE0ULL;
    } initial;
    initial.value.kind = SAO_SDK_UI_WIDGET_LABEL;
    initial.value.widget_id_utf8 = "uncertain-widget";
    initial.value.text_utf8 = "old";
    initial.value.props_json_utf8 = reinterpret_cast<const uint8_t*>(initial_props);
    initial.value.props_len = sizeof(initial_props) - 1;
    sao_sdk_ui_widget_t widget = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(&context, panel, &initial.value, &widget) == SAO_SDK_OK);
    auto* native = sao_sdk_test_widget_native_handle(&context, panel, widget);
    REQUIRE(native != nullptr);

    constexpr char updated_props[] = R"({"fill":"#AABBCC"})";
    WidgetSpecFixture updated = initial;
    updated.value.text_utf8 = "new";
    updated.value.props_json_utf8 = reinterpret_cast<const uint8_t*>(updated_props);
    updated.value.props_len = sizeof(updated_props) - 1;

    sao_ui_panel_test_set_body_replace_failure_point(2);
    CHECK(sao_sdk_panel_update_widget(&context, panel, widget, &updated.value) ==
          static_cast<sao_sdk_status_t>(SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED));
    sao_ui_panel_test_set_body_replace_failure_point(0);

    CHECK(initial.canary == 0x13579BDF2468ACE0ULL);
    CHECK(updated.canary == 0x13579BDF2468ACE0ULL);
    CHECK(sao_sdk_test_panel_widget_count(&context, panel) == 1);
    CHECK(sao_sdk_test_widget_native_handle(&context, panel, widget) == native);
    uint32_t fill = 0;
    char text[32]{};
    REQUIRE(sao_ui_widget_test_props_state(native, "fill", &fill, text, sizeof(text)));
    CHECK(fill == 0xFFAABBCCU);

    SaoSdkWidgetSpec later = updated.value;
    CHECK(sao_sdk_panel_update_widget(&context, panel, widget, &later) == SAO_SDK_ERR_BUSY);
    sao_sdk_ui_widget_t replacement = nullptr;
    CHECK(sao_sdk_panel_add_widget(&context, panel, &later, &replacement) == SAO_SDK_ERR_BUSY);
    CHECK(replacement == nullptr);
    CHECK(sao_sdk_panel_remove_widget(&context, panel, widget) == SAO_SDK_ERR_BUSY);
    CHECK(sao_sdk_unregister_ui_panel(&context, panel) == SAO_SDK_ERR_BUSY);

    sao_sdk_test_fail_next_panel_unregister(SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_context_try_destroy(&context) == SAO_SDK_ERR_INTERNAL);
    CHECK(context.ctx_impl != nullptr);
    CHECK(sao_sdk_test_panel_cleanup_pending_count(&context) == 1);
    REQUIRE(sao_sdk_context_try_destroy(&context) == SAO_SDK_OK);
    CHECK(context.ctx_impl == nullptr);
}
