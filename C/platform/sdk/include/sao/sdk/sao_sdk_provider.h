#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAO_SDK_PROVIDER_ABI_VERSION_MAJOR 1u
#define SAO_SDK_PROVIDER_ABI_VERSION_MINOR 2u
#define SAO_SDK_PROVIDER_ABI_VERSION \
    ((SAO_SDK_PROVIDER_ABI_VERSION_MAJOR << 16) | \
     SAO_SDK_PROVIDER_ABI_VERSION_MINOR)

typedef uint64_t sao_sdk_capability_token_t;
typedef sao_sdk_capability_token_t sao_sdk_timer_token_t;
typedef sao_sdk_capability_token_t sao_sdk_dialog_token_t;
typedef sao_sdk_capability_token_t sao_sdk_notify_token_t;
typedef sao_sdk_capability_token_t sao_sdk_overlay_token_t;

typedef void (SAO_SDK_CALL* sao_sdk_timer_callback_t)(
    sao_sdk_timer_token_t timer, void* user_data);

typedef void (SAO_SDK_CALL* sao_sdk_dialog_callback_t)(
    sao_sdk_dialog_token_t dialog,
    int32_t pressed_button,
    const char* input_text_utf8,
    size_t input_text_len,
    void* user_data);

enum sao_sdk_dialog_kind_e : int32_t {
    SAO_SDK_DIALOG_INFO = 0,
    SAO_SDK_DIALOG_WARNING = 1,
    SAO_SDK_DIALOG_ERROR = 2,
    SAO_SDK_DIALOG_ASK = 3,
    SAO_SDK_DIALOG_INPUT = 4,
};

enum sao_sdk_dialog_button_e : int32_t {
    SAO_SDK_DIALOG_BUTTON_OK = 0,
    SAO_SDK_DIALOG_BUTTON_CANCEL = 1,
    SAO_SDK_DIALOG_BUTTON_YES = 2,
    SAO_SDK_DIALOG_BUTTON_NO = 3,
    SAO_SDK_DIALOG_BUTTON_CUSTOM = 4,
    SAO_SDK_DIALOG_BUTTON_DISMISS = 5,
};

struct SaoSdkDialogSpec {
    int32_t kind;
    const char* title_utf8;
    const char* message_utf8;
    const char* input_prompt_utf8;
    const char* input_default_utf8;
    int32_t input_max_length;
    bool dismiss_on_focus_out;
    bool dismiss_on_esc;
    uint8_t reserved[6];
};

struct SaoSdkNotifySpec {
    const char* text_utf8;
    uint32_t duration_ms;
    uint32_t argb_color;
};

struct SaoSdkOverlaySpec {
    const char* surface_id_utf8;
    const uint8_t* spec_json_utf8;
    size_t spec_len;
};

struct SaoSdkGpuHuntRegion {
    uint64_t base;
    uint64_t size;
    uint32_t protect;
    uint32_t region_type;
};

// Host-owned capability provider. The SDK copies only `struct_size` bytes,
// so future hosts may append slots without changing SaoSdkContext layout.
// Registration slots return provider-local tokens; the SDK translates them
// to context-local tokens and owns their reverse-order teardown.
struct SaoSdkProviderVTable {
    uint32_t abi_version;
    uint32_t struct_size;
    void* user_data;

    void (SAO_SDK_CALL* retain)(void* user_data);
    void (SAO_SDK_CALL* release)(void* user_data);

    sao_sdk_status_t (SAO_SDK_CALL* tts_speak)(
        void* user_data, const char* text_utf8, float volume, float rate);
    sao_sdk_status_t (SAO_SDK_CALL* tts_stop)(void* user_data);

    sao_sdk_status_t (SAO_SDK_CALL* register_render_hook)(
        void* user_data,
        const char* plugin_id_utf8,
        int32_t hook_point,
        sao_sdk_render_hook_callback_t callback,
        void* callback_user_data,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* unregister_render_hook)(
        void* user_data, uint64_t provider_token);

    sao_sdk_status_t (SAO_SDK_CALL* register_timer)(
        void* user_data,
        uint32_t interval_ms,
        sao_sdk_timer_callback_t callback,
        void* callback_user_data,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* unregister_timer)(
        void* user_data, uint64_t provider_token);

    sao_sdk_status_t (SAO_SDK_CALL* register_hotkey)(
        void* user_data,
        const char* plugin_id_utf8,
        const char* binding_id_utf8,
        uint32_t virtual_key,
        uint32_t modifiers,
        sao_sdk_hotkey_callback_t callback,
        void* callback_user_data,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* unregister_hotkey)(
        void* user_data, uint64_t provider_token);

    sao_sdk_status_t (SAO_SDK_CALL* show_dialog)(
        void* user_data,
        const char* plugin_id_utf8,
        const struct SaoSdkDialogSpec* spec,
        sao_sdk_dialog_callback_t callback,
        void* callback_user_data,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* dismiss_dialog)(
        void* user_data, uint64_t provider_token);

    sao_sdk_status_t (SAO_SDK_CALL* show_notify)(
        void* user_data,
        const char* plugin_id_utf8,
        const struct SaoSdkNotifySpec* spec,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* dismiss_notify)(
        void* user_data, uint64_t provider_token);

    sao_sdk_status_t (SAO_SDK_CALL* set_overlay)(
        void* user_data,
        const char* plugin_id_utf8,
        const struct SaoSdkOverlaySpec* spec,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* clear_overlay)(
        void* user_data, uint64_t provider_token);

    sao_sdk_status_t (SAO_SDK_CALL* register_render_hook_ex)(
        void* user_data,
        const char* plugin_id_utf8,
        const struct SaoSdkRenderHookSpec* spec,
        sao_sdk_render_hook_callback_t callback,
        void* callback_user_data,
        uint64_t* out_provider_token);
    sao_sdk_status_t (SAO_SDK_CALL* request_redraw)(
        void* user_data, const char* surface_id_utf8);

    // GPU hunt I/O is session-scoped: every tracker opens an independent
    // provider session and holds an extra retain/release lease until the
    // tracker is destroyed.  A host backed by a process-global rt_io proxy
    // may reject a second session explicitly instead of sharing mutable
    // attach state between trackers.
    sao_sdk_status_t (SAO_SDK_CALL* gpu_hunt_open_session)(
        void* user_data,
        const char* plugin_id_utf8,
        void** out_session);
    sao_sdk_status_t (SAO_SDK_CALL* gpu_hunt_close_session)(
        void* user_data, void* session);
    sao_sdk_status_t (SAO_SDK_CALL* gpu_hunt_attach)(
        void* user_data, void* session, uint32_t pid);
    sao_sdk_status_t (SAO_SDK_CALL* gpu_hunt_detach)(
        void* user_data, void* session);
    sao_sdk_status_t (SAO_SDK_CALL* gpu_hunt_enum_regions)(
        void* user_data,
        void* session,
        struct SaoSdkGpuHuntRegion* out_regions,
        size_t capacity,
        size_t* out_count);
    sao_sdk_status_t (SAO_SDK_CALL* gpu_hunt_read)(
        void* user_data,
        void* session,
        uint64_t address,
        uint8_t* out_buffer,
        size_t buffer_size,
        size_t* out_bytes_read);
};

enum sao_sdk_render_dispatch_flag_e : uint32_t {
    SAO_SDK_RENDER_DISPATCH_LOGICAL_TICK = 1u << 0u,
    SAO_SDK_RENDER_DISPATCH_COMPOSITOR_PRESENT = 1u << 1u,
    SAO_SDK_RENDER_DISPATCH_GPU_PRESENT = 1u << 2u,
    SAO_SDK_RENDER_DISPATCH_REDRAW_REQUESTED = 1u << 3u,
};

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_bind_provider(
    struct SaoSdkContext* ctx, const struct SaoSdkProviderVTable* provider);

// Binds the SDK adapter backed by the currently linked platform services.
// Capability slots whose platform service is not production-bound remain
// null and report SAO_SDK_ERR_UNSUPPORTED.
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_platform_services(struct SaoSdkContext* ctx);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_register_render_hook_ex(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback,
    void* user_data,
    sao_sdk_hook_token_t* out_hook_handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw_surface(
    const struct SaoSdkContext* ctx,
    const char* surface_id_utf8);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_render_dispatch(
    const char* surface_id_utf8,
    int32_t hook_point,
    uint64_t monotonic_time_ns,
    int32_t viewport_x_px,
    int32_t viewport_y_px,
    int32_t viewport_width_px,
    int32_t viewport_height_px,
    uint32_t dispatch_flags);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_render_gpu_provider_status(void);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_provider_status(
    const struct SaoSdkContext* ctx);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_timer_register(
    const struct SaoSdkContext* ctx,
    uint32_t interval_ms,
    sao_sdk_timer_callback_t callback,
    void* user_data,
    sao_sdk_timer_token_t* out_timer);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_timer_unregister(
    const struct SaoSdkContext* ctx, sao_sdk_timer_token_t timer);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_dialog_show(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkDialogSpec* spec,
    sao_sdk_dialog_callback_t callback,
    void* user_data,
    sao_sdk_dialog_token_t* out_dialog);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_dialog_dismiss(
    const struct SaoSdkContext* ctx, sao_sdk_dialog_token_t dialog);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_notify_show(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkNotifySpec* spec,
    sao_sdk_notify_token_t* out_notify);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_notify_dismiss(
    const struct SaoSdkContext* ctx, sao_sdk_notify_token_t notify);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_overlay_set(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkOverlaySpec* spec,
    sao_sdk_overlay_token_t* out_overlay);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_overlay_clear(
    const struct SaoSdkContext* ctx, sao_sdk_overlay_token_t overlay);

#ifdef __cplusplus
}  // extern "C"
#endif
