// SAO Auto — SDK: UI capability convenience header.
//
// Everything here is a re-declaration of SaoSdkUiTable's function
// pointers wrapped in inline `sao_sdk_ui_*` free functions so plugins
// don't have to dereference the vtable manually.  Requires a valid
// `SaoSdkContext* ctx` in scope.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// Convenience wrappers — inline, no ABI cost.
static inline sao_sdk_status_t sao_sdk_ui_register_panel(
    const struct SaoSdkContext* ctx,
    const char* panel_id_utf8,
    const char* title_utf8,
    const uint8_t* initial_spec_json_utf8,
    size_t spec_len,
    sao_sdk_panel_action_callback_t action_cb,
    void* action_user_data,
    sao_sdk_ui_panel_t* out_panel) {
    return ctx->ui->register_panel(ctx->ctx_impl, panel_id_utf8, title_utf8,
                                    initial_spec_json_utf8, spec_len,
                                    action_cb, action_user_data, out_panel);
}

static inline sao_sdk_status_t sao_sdk_ui_set_panel_spec(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    const uint8_t* spec_json_utf8,
    size_t spec_len) {
    return ctx->ui->set_panel_spec(ctx->ctx_impl, panel, spec_json_utf8, spec_len);
}

static inline sao_sdk_status_t sao_sdk_ui_set_overlay(
    const struct SaoSdkContext* ctx,
    const char* surface_id_utf8,
    const uint8_t* spec_json_utf8,
    size_t spec_len) {
    return ctx->ui->set_overlay(ctx->ctx_impl, surface_id_utf8,
                                 spec_json_utf8, spec_len);
}

static inline sao_sdk_status_t sao_sdk_ui_request_redraw(
    const struct SaoSdkContext* ctx, const char* surface_id_utf8) {
    return ctx->ui->request_redraw(ctx->ctx_impl, surface_id_utf8);
}

#ifdef __cplusplus
}  // extern "C"
#endif
