// SAO Auto — SDK: config read/write convenience wrappers.
//
// Every plugin's config lives under a scoped key prefix
// ("plugins.<plugin_id>.<key>") automatically applied by the platform;
// plugins pass in the *tail* portion only.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline sao_sdk_status_t sao_sdk_config_get_bool(
    const struct SaoSdkContext* ctx, const char* key_utf8, bool* out_value) {
    return ctx->config->get_bool(ctx->ctx_impl, key_utf8, out_value);
}

static inline sao_sdk_status_t sao_sdk_config_get_int(
    const struct SaoSdkContext* ctx, const char* key_utf8, int64_t* out_value) {
    return ctx->config->get_int(ctx->ctx_impl, key_utf8, out_value);
}

static inline sao_sdk_status_t sao_sdk_config_get_double(
    const struct SaoSdkContext* ctx, const char* key_utf8, double* out_value) {
    return ctx->config->get_double(ctx->ctx_impl, key_utf8, out_value);
}

static inline sao_sdk_status_t sao_sdk_config_get_string(
    const struct SaoSdkContext* ctx, const char* key_utf8,
    char* out_buffer, size_t buffer_len, size_t* out_bytes_needed) {
    return ctx->config->get_string(ctx->ctx_impl, key_utf8, out_buffer,
                                    buffer_len, out_bytes_needed);
}

static inline sao_sdk_status_t sao_sdk_config_set_bool(
    const struct SaoSdkContext* ctx, const char* key_utf8, bool value) {
    return ctx->config->set_bool(ctx->ctx_impl, key_utf8, value);
}

static inline sao_sdk_status_t sao_sdk_config_set_int(
    const struct SaoSdkContext* ctx, const char* key_utf8, int64_t value) {
    return ctx->config->set_int(ctx->ctx_impl, key_utf8, value);
}

static inline sao_sdk_status_t sao_sdk_config_set_double(
    const struct SaoSdkContext* ctx, const char* key_utf8, double value) {
    return ctx->config->set_double(ctx->ctx_impl, key_utf8, value);
}

static inline sao_sdk_status_t sao_sdk_config_set_string(
    const struct SaoSdkContext* ctx, const char* key_utf8, const char* value_utf8) {
    return ctx->config->set_string(ctx->ctx_impl, key_utf8, value_utf8);
}

#ifdef __cplusplus
}  // extern "C"
#endif
