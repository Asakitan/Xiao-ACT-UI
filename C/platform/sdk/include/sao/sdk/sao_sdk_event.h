// SAO Auto — SDK: event bus convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline sao_sdk_status_t sao_sdk_event_subscribe(
    const struct SaoSdkContext* ctx,
    const char* topic_utf8,
    sao_sdk_event_callback_t callback,
    void* user_data,
    sao_sdk_subscription_t* out_subscription) {
    return ctx->event->subscribe(ctx->ctx_impl, topic_utf8, callback,
                                  user_data, out_subscription);
}

static inline sao_sdk_status_t sao_sdk_event_unsubscribe(
    const struct SaoSdkContext* ctx, sao_sdk_subscription_t subscription) {
    return ctx->event->unsubscribe(ctx->ctx_impl, subscription);
}

static inline sao_sdk_status_t sao_sdk_event_publish(
    const struct SaoSdkContext* ctx,
    const char* topic_utf8,
    const uint8_t* json_payload_utf8,
    size_t payload_len) {
    return ctx->event->publish(ctx->ctx_impl, topic_utf8,
                                json_payload_utf8, payload_len);
}

#ifdef __cplusplus
}  // extern "C"
#endif
