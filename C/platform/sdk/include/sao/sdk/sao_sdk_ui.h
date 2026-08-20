// SAO Auto — SDK: UI capability convenience header.
//
// Everything here is a re-declaration of SaoSdkUiTable's function
// pointers wrapped in inline `sao_sdk_ui_*` free functions so plugins
// don't have to dereference the vtable manually.  Requires a valid
// `SaoSdkContext* ctx` in scope.

#pragma once

#include <cstring>

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif
static inline sao_sdk_status_t sao_sdk_ui_table_status(const struct SaoSdkContext* ctx) {
    if (ctx == NULL || ctx->ui == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if ((ctx->abi_version >> 16) != SAO_SDK_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    return SAO_SDK_OK;
}

static inline sao_sdk_status_t sao_sdk_ui_table_slot_status(
    const struct SaoSdkContext* ctx, size_t slot_offset, size_t slot_size,
    uint32_t required_context_minor) {
    const sao_sdk_status_t base_status = sao_sdk_ui_table_status(ctx);
    if (base_status != SAO_SDK_OK)
        return base_status;
    if (slot_offset > SIZE_MAX - slot_size ||
        slot_offset + slot_size > sizeof(struct SaoSdkUiTable))
        return SAO_SDK_ERR_UNSUPPORTED;
    const uint32_t context_minor = ctx->abi_version & 0xffffu;
    if (required_context_minor == 0u) {
        if (slot_offset + slot_size > SAO_SDK_UI_TABLE_LEGACY_SIZE)
            return SAO_SDK_ERR_UNSUPPORTED;
        return SAO_SDK_OK;
    }
    if (context_minor < required_context_minor)
        return SAO_SDK_ERR_UNSUPPORTED;

    const auto* table_bytes = reinterpret_cast<const unsigned char*>(ctx->ui);
    uint32_t table_abi_version = 0;
    uint32_t declared_size = 0;
    std::memcpy(&table_abi_version,
                table_bytes + offsetof(struct SaoSdkUiTable, abi_version),
                sizeof(table_abi_version));
    std::memcpy(&declared_size,
                table_bytes + offsetof(struct SaoSdkUiTable, struct_size),
                sizeof(declared_size));
    if ((table_abi_version >> 16) != SAO_SDK_UI_TABLE_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if ((table_abi_version & 0xffffu) < required_context_minor ||
        declared_size < SAO_SDK_UI_TABLE_ABI_PREFIX_SIZE)
        return SAO_SDK_ERR_UNSUPPORTED;
    const size_t table_size = declared_size > sizeof(struct SaoSdkUiTable)
                                  ? sizeof(struct SaoSdkUiTable)
                                  : declared_size;
    if (slot_offset + slot_size > table_size)
        return SAO_SDK_ERR_UNSUPPORTED;
    return SAO_SDK_OK;
}

static inline sao_sdk_status_t sao_sdk_ui_table_authorize_slot(
    const struct SaoSdkContext* ctx, size_t slot_offset, size_t slot_size,
    uint32_t required_context_minor) {
    return sao_sdk_ui_table_slot_status(ctx, slot_offset, slot_size, required_context_minor);
}

static inline sao_sdk_status_t sao_sdk_ui_table_copy_slot(
    const struct SaoSdkContext* ctx, size_t slot_offset, size_t slot_size,
    uint32_t required_context_minor, void* out_slot) {
    if (out_slot == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const sao_sdk_status_t status =
        sao_sdk_ui_table_authorize_slot(ctx, slot_offset, slot_size, required_context_minor);
    if (status != SAO_SDK_OK)
        return status;
    std::memcpy(out_slot, reinterpret_cast<const unsigned char*>(ctx->ui) + slot_offset,
                slot_size);
    return SAO_SDK_OK;
}

#define SAO_SDK_UI_COPY_SLOT(ctx, slot, out_slot) \
    sao_sdk_ui_table_copy_slot( \
        (ctx), offsetof(struct SaoSdkUiTable, slot), sizeof(out_slot), 0u, &(out_slot))

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
    decltype(((struct SaoSdkUiTable*)0)->register_panel) slot = NULL;
    const sao_sdk_status_t status = SAO_SDK_UI_COPY_SLOT(ctx, register_panel, slot);
    if (status != SAO_SDK_OK)
        return status;
    return slot(ctx->ctx_impl, panel_id_utf8, title_utf8, initial_spec_json_utf8, spec_len,
                action_cb, action_user_data, out_panel);
}

static inline sao_sdk_status_t sao_sdk_ui_set_panel_spec(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    const uint8_t* spec_json_utf8,
    size_t spec_len) {
    decltype(((struct SaoSdkUiTable*)0)->set_panel_spec) slot = NULL;
    const sao_sdk_status_t status = SAO_SDK_UI_COPY_SLOT(ctx, set_panel_spec, slot);
    if (status != SAO_SDK_OK)
        return status;
    return slot(ctx->ctx_impl, panel, spec_json_utf8, spec_len);
}

static inline sao_sdk_status_t sao_sdk_ui_set_overlay(
    const struct SaoSdkContext* ctx,
    const char* surface_id_utf8,
    const uint8_t* spec_json_utf8,
    size_t spec_len) {
    decltype(((struct SaoSdkUiTable*)0)->set_overlay) slot = NULL;
    const sao_sdk_status_t status = SAO_SDK_UI_COPY_SLOT(ctx, set_overlay, slot);
    if (status != SAO_SDK_OK)
        return status;
    return slot(ctx->ctx_impl, surface_id_utf8, spec_json_utf8, spec_len);
}

static inline sao_sdk_status_t sao_sdk_ui_request_redraw(
    const struct SaoSdkContext* ctx, const char* surface_id_utf8) {
    decltype(((struct SaoSdkUiTable*)0)->request_redraw) slot = NULL;
    const sao_sdk_status_t status = SAO_SDK_UI_COPY_SLOT(ctx, request_redraw, slot);
    if (status != SAO_SDK_OK)
        return status;
    return slot(ctx->ctx_impl, surface_id_utf8);
}

#ifdef __cplusplus
}  // extern "C"
#endif