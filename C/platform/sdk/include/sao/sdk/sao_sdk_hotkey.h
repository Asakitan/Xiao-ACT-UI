// SAO Auto — SDK: hotkey convenience wrappers.
//
// hotkey_name_utf8 is a plugin-scoped identifier that appears in the
// user's hotkey settings UI.  Plugins should use a prefix ("mp_toggle",
// "raid_edit") so users see the source at a glance.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline sao_sdk_status_t sao_sdk_hotkey_register(
    const struct SaoSdkContext* ctx,
    const char* hotkey_name_utf8,
    uint32_t virtual_key,
    uint32_t modifier_mask,
    sao_sdk_hotkey_callback_t callback,
    void* user_data,
    sao_sdk_hotkey_id_t* out_id) {
    return ctx->hotkey->register_hotkey(ctx->ctx_impl, hotkey_name_utf8,
                                         virtual_key, modifier_mask,
                                         callback, user_data, out_id);
}

static inline sao_sdk_status_t sao_sdk_hotkey_unregister(
    const struct SaoSdkContext* ctx, sao_sdk_hotkey_id_t id) {
    return ctx->hotkey->unregister_hotkey(ctx->ctx_impl, id);
}

#ifdef __cplusplus
}  // extern "C"
#endif
