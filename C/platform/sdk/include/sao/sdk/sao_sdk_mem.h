// SAO Auto — SDK: memory read convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline sao_sdk_status_t sao_sdk_mem_read(
    const struct SaoSdkContext* ctx,
    uint64_t address, void* out_buffer, size_t buffer_len,
    size_t* out_bytes_read) {
    return ctx->mem->read(ctx->ctx_impl, address, out_buffer, buffer_len,
                           out_bytes_read);
}

static inline sao_sdk_status_t sao_sdk_mem_read_u32(
    const struct SaoSdkContext* ctx, uint64_t address, uint32_t* out_value) {
    return ctx->mem->read_u32(ctx->ctx_impl, address, out_value);
}

static inline sao_sdk_status_t sao_sdk_mem_read_u64(
    const struct SaoSdkContext* ctx, uint64_t address, uint64_t* out_value) {
    return ctx->mem->read_u64(ctx->ctx_impl, address, out_value);
}

static inline sao_sdk_status_t sao_sdk_mem_read_ptr_chain(
    const struct SaoSdkContext* ctx, uint64_t base_address,
    const int32_t* offsets, size_t offset_count, uint64_t* out_final_address) {
    return ctx->mem->read_ptr_chain(ctx->ctx_impl, base_address, offsets,
                                     offset_count, out_final_address);
}

static inline sao_sdk_status_t sao_sdk_mem_module_base(
    const struct SaoSdkContext* ctx, const char* module_name_utf8,
    uint64_t* out_base) {
    return ctx->mem->module_base(ctx->ctx_impl, module_name_utf8, out_base);
}

#ifdef __cplusplus
}  // extern "C"
#endif
