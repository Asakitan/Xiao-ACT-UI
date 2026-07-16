// SAO Auto — SDK: text-to-speech convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// volume: 0..1  |  rate: -10..10 (SAPI convention)
static inline sao_sdk_status_t sao_sdk_tts_speak(
    const struct SaoSdkContext* ctx, const char* text_utf8,
    float volume, float rate) {
    return ctx->tts->speak(ctx->ctx_impl, text_utf8, volume, rate);
}

static inline sao_sdk_status_t sao_sdk_tts_stop(const struct SaoSdkContext* ctx) {
    return ctx->tts->stop(ctx->ctx_impl);
}

#ifdef __cplusplus
}  // extern "C"
#endif
