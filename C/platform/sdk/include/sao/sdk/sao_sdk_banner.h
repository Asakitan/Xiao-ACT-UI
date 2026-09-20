// SAO Auto — SDK: banner alert convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// argb_color: 0xAARRGGBB — 0 selects the theme's default warn color.
static inline sao_sdk_status_t sao_sdk_banner_show(
    const struct SaoSdkContext* ctx, const char* text_utf8,
    uint32_t duration_ms, uint32_t argb_color) {
    if (ctx == NULL || ctx->banner == NULL || ctx->banner->show == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->banner->show(ctx->ctx_impl, text_utf8, duration_ms, argb_color);
}

#ifdef __cplusplus
}  // extern "C"
#endif
