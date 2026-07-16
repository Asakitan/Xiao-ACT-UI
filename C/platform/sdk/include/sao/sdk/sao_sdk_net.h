// SAO Auto — SDK: net capability convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline sao_sdk_status_t sao_sdk_net_set_frame_callback(
    const struct SaoSdkContext* ctx,
    void (SAO_SDK_CALL* frame_callback)(
        const uint8_t*, size_t, uint64_t, void*),
    void* user_data) {
    return ctx->net->set_frame_callback(ctx->ctx_impl, frame_callback, user_data);
}

#ifdef __cplusplus
}  // extern "C"
#endif
