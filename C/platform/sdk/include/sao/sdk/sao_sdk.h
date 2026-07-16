// SAO Auto — plugin SDK single-include header.
//
// Include this and only this from a plugin.  Every capability sub-header
// is pulled in transitively so the plugin sees the full ABI surface.
//
// The plugin exports two symbols:
//     SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_plugin_init(
//         const struct SaoSdkContext* ctx);
//     SAO_SDK_API void SAO_SDK_CALL sao_plugin_shutdown(
//         const struct SaoSdkContext* ctx);
//
// These live in the plugin binary; the platform LoadLibrarys the plugin,
// GetProcAddresses the two symbols, calls init at load and shutdown at
// unload.  Both are called on the main thread.

#pragma once

#include "sao/sdk/sao_sdk_version.h"
#include "sao/sdk/sao_sdk_context.h"
#include "sao/sdk/sao_sdk_ui.h"
#include "sao/sdk/sao_sdk_event.h"
#include "sao/sdk/sao_sdk_mem.h"
#include "sao/sdk/sao_sdk_net.h"
#include "sao/sdk/sao_sdk_config.h"
#include "sao/sdk/sao_sdk_hotkey.h"
#include "sao/sdk/sao_sdk_tts.h"
#include "sao/sdk/sao_sdk_banner.h"
#include "sao/sdk/sao_sdk_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runtime version probe — plugins should call this before doing anything
// else and refuse to init on major-version mismatch.
SAO_SDK_API uint32_t SAO_SDK_CALL sao_sdk_abi_version(void);

// Status code semantics — thin subset of sao_status_t (see status.h).
enum : sao_sdk_status_t {
    SAO_SDK_OK                       =    0,
    SAO_SDK_ERR_INVALID_ARGUMENT     =   -1,
    SAO_SDK_ERR_NOT_INITIALIZED      =   -2,
    SAO_SDK_ERR_HANDLE_INVALID       =   -3,
    SAO_SDK_ERR_BUFFER_TOO_SMALL     =   -4,
    SAO_SDK_ERR_NOT_IMPLEMENTED      =   -5,
    SAO_SDK_ERR_ABI_MISMATCH         =   -9,
    SAO_SDK_ERR_UNSUPPORTED          =  -10,
    SAO_SDK_ERR_NOT_FOUND            =  -22,
    SAO_SDK_ERR_ALREADY_EXISTS       =  -23,
    SAO_SDK_ERR_READ_FAULT           =  -41,
};

// Signature the plugin exports.  The platform GetProcAddresses this by
// name and refuses to load plugins that don't provide it.
typedef sao_sdk_status_t (SAO_SDK_CALL* sao_plugin_init_fn_t)(
    const struct SaoSdkContext* ctx);
typedef void (SAO_SDK_CALL* sao_plugin_shutdown_fn_t)(
    const struct SaoSdkContext* ctx);

// ─── Context lifecycle (Wave 7) ─────────────────────────────────────
//
// The platform side calls these to hand a fully-wired context to a
// plugin.  The context owns per-plugin state including:
//   * base_dir_utf8 — canonical path plugins should stage files under
//   * plugin_id_utf8 — used as config scope + event owner tag +
//     hotkey plugin id
// Every SaoSdkContext returned is safe to hand across the plugin ABI:
// its vtables live in the DLL, and its ctx_impl points at an
// allocation freed by sao_sdk_context_destroy.

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_create(
    const char* base_dir_utf8,
    const char* plugin_id_utf8,
    struct SaoSdkContext** out_ctx);

SAO_SDK_API void SAO_SDK_CALL sao_sdk_context_destroy(
    struct SaoSdkContext* ctx);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_get_plugin_id(
    const struct SaoSdkContext* ctx,
    const char** out_plugin_id_utf8);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_get_base_dir(
    const struct SaoSdkContext* ctx,
    const char** out_base_dir_utf8);

// Unified loader entry.  It initializes a caller-owned public context with
// the same per-plugin state and vtables used by sao_sdk_context_create().
// Pass the returned stack/embedded context to sao_sdk_context_destroy() when
// the plugin is unloaded.
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_bind_context(
    const char* plugin_id_utf8,
    const char* plugin_version_utf8,
    struct SaoSdkContext* out_ctx);

// ─── UI panel + widget + render hook wire (Wave 7) ──────────────────
//
// Free-function wrappers around the ctx->ui-> pointers so plugins never
// dereference the vtable manually.  These forward directly to the
// platform's sao_ui_panel_register / widget_kit / render_hook manager.

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_ui_panel(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_unregister_ui_panel(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_add_widget(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    const struct SaoSdkWidgetSpec* widget_spec,
    sao_sdk_ui_widget_t* out_widget);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_update_widget(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    sao_sdk_ui_widget_t widget,
    const struct SaoSdkWidgetSpec* widget_spec);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_remove_widget(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    sao_sdk_ui_widget_t widget);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook(
    const struct SaoSdkContext* ctx,
    int32_t hook_point,
    sao_sdk_render_hook_callback_t callback,
    void* user_data,
    sao_sdk_hook_token_t* out_hook_handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook_ex(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback,
    void* user_data,
    sao_sdk_hook_token_t* out_hook_handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_unregister_render_hook(
    const struct SaoSdkContext* ctx,
    sao_sdk_hook_token_t hook_handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_request_redraw(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_request_redraw_surface(
    const struct SaoSdkContext* ctx,
    const char* surface_id_utf8);

// ─── Event bus wire (Wave 7) ────────────────────────────────────────

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_subscribe_event(
    const struct SaoSdkContext* ctx,
    const char* event_type_utf8,
    sao_sdk_event_callback_t callback,
    void* user_data,
    sao_sdk_subscription_t* out_handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_unsubscribe_event(
    const struct SaoSdkContext* ctx,
    sao_sdk_subscription_t handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_publish_event(
    const struct SaoSdkContext* ctx,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t size);

// ─── Hotkey wire (Wave 7) ───────────────────────────────────────────

// Hotkey descriptor — trimmed subset of `SaoUiHotkeyBindingSpec`.
struct SaoSdkHotkeySpec {
    const char* binding_id_utf8;
    uint32_t    virtual_key;
    uint32_t    modifiers;
    bool        enforce_ctrl_prefix;
    bool        prevent_default;
    bool        allow_repeat;
    uint8_t     _pad[5];
};

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_hotkey(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkHotkeySpec* spec,
    sao_sdk_hotkey_callback_t callback,
    void* user_data,
    sao_sdk_hotkey_id_t* out_handle);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_unregister_hotkey(
    const struct SaoSdkContext* ctx,
    sao_sdk_hotkey_id_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
