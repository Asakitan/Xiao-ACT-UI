// SAO Auto — SDK: memory read convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MAJOR 1u
#define SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MINOR 1u
#define SAO_SDK_MEMORY_PROVIDER_ABI_VERSION                                                        \
    ((SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MAJOR << 16) | SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MINOR)

#define SAO_SDK_MEMORY_NAME_CAPACITY 260u
#define SAO_SDK_MEMORY_MAX_MODULE_COUNT 4096u

struct SaoSdkMemoryTargetIdentity {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t process_id;
    uint32_t reserved;
    uint64_t process_start_time_100ns;
    char image_name_utf8[SAO_SDK_MEMORY_NAME_CAPACITY];
};

#define SAO_SDK_MEMORY_TARGET_IDENTITY_REQUIRED_SIZE                                               \
    (offsetof(struct SaoSdkMemoryTargetIdentity, process_id) +                                     \
     sizeof(((struct SaoSdkMemoryTargetIdentity*)0)->process_id))

struct SaoSdkMemoryModule {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t base_address;
    uint64_t image_size;
    char name_utf8[SAO_SDK_MEMORY_NAME_CAPACITY];
};

#define SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE sizeof(struct SaoSdkMemoryModule)

// Independent host-owned read-only memory surface. The SDK retains the owner
// while configured and obtains one provider session per SaoSdkContext. No
// process handle or RTIO proxy is created by the SDK.
struct SaoSdkMemoryProviderVTable {
    uint32_t abi_version;
    uint32_t struct_size;
    void* user_data;

    void(SAO_SDK_CALL* retain)(void* user_data);
    void(SAO_SDK_CALL* release)(void* user_data);

    sao_sdk_status_t(SAO_SDK_CALL* open_session)(void* user_data, const char* plugin_id_utf8,
                                                 void** out_session);
    sao_sdk_status_t(SAO_SDK_CALL* close_session)(void* user_data, void* session);
    sao_sdk_status_t(SAO_SDK_CALL* attach)(void* user_data, void* session,
                                           const struct SaoSdkMemoryTargetIdentity* identity);
    sao_sdk_status_t(SAO_SDK_CALL* detach)(void* user_data, void* session);
    sao_sdk_status_t(SAO_SDK_CALL* read)(void* user_data, void* session, uint64_t address,
                                         void* out_buffer, size_t buffer_len,
                                         size_t* out_bytes_read);
    sao_sdk_status_t(SAO_SDK_CALL* enumerate_modules)(void* user_data, void* session,
                                                      struct SaoSdkMemoryModule* out_modules,
                                                      size_t capacity, size_t element_stride,
                                                      size_t* out_count);
};

#define SAO_SDK_MEMORY_PROVIDER_REQUIRED_SIZE                                                      \
    (offsetof(struct SaoSdkMemoryProviderVTable, enumerate_modules) +                              \
     sizeof(((struct SaoSdkMemoryProviderVTable*)0)->enumerate_modules))

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_configure_memory_provider(
    struct SaoSdkContext* ctx, const struct SaoSdkMemoryProviderVTable* provider);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_memory_provider_status(const struct SaoSdkContext* ctx);

// Configures the process owner used to auto-open one memory session for each
// new context and for contexts bound to platform services. Passing null only
// affects future auto-bind operations; existing context sessions retain their
// own provider lease until cleared or destroyed.
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_memory_configure_provider(const struct SaoSdkMemoryProviderVTable* provider);

static inline sao_sdk_status_t sao_sdk_mem_read(const struct SaoSdkContext* ctx, uint64_t address,
                                                void* out_buffer, size_t buffer_len,
                                                size_t* out_bytes_read) {
    if (out_bytes_read != NULL)
        *out_bytes_read = 0;
    if (ctx == NULL || ctx->mem == NULL || ctx->mem->read == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->mem->read(ctx->ctx_impl, address, out_buffer, buffer_len, out_bytes_read);
}

static inline sao_sdk_status_t sao_sdk_mem_read_u32(const struct SaoSdkContext* ctx,
                                                    uint64_t address, uint32_t* out_value) {
    if (out_value != NULL)
        *out_value = 0;
    if (ctx == NULL || ctx->mem == NULL || ctx->mem->read_u32 == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->mem->read_u32(ctx->ctx_impl, address, out_value);
}

static inline sao_sdk_status_t sao_sdk_mem_read_u64(const struct SaoSdkContext* ctx,
                                                    uint64_t address, uint64_t* out_value) {
    if (out_value != NULL)
        *out_value = 0;
    if (ctx == NULL || ctx->mem == NULL || ctx->mem->read_u64 == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->mem->read_u64(ctx->ctx_impl, address, out_value);
}

static inline sao_sdk_status_t sao_sdk_mem_read_ptr_chain(const struct SaoSdkContext* ctx,
                                                          uint64_t base_address,
                                                          const int32_t* offsets,
                                                          size_t offset_count,
                                                          uint64_t* out_final_address) {
    if (out_final_address != NULL)
        *out_final_address = 0;
    if (ctx == NULL || ctx->mem == NULL || ctx->mem->read_ptr_chain == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->mem->read_ptr_chain(ctx->ctx_impl, base_address, offsets, offset_count,
                                    out_final_address);
}

static inline sao_sdk_status_t sao_sdk_mem_module_base(const struct SaoSdkContext* ctx,
                                                       const char* module_name_utf8,
                                                       uint64_t* out_base) {
    if (out_base != NULL)
        *out_base = 0;
    if (ctx == NULL || ctx->mem == NULL || ctx->mem->module_base == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->mem->module_base(ctx->ctx_impl, module_name_utf8, out_base);
}

static inline sao_sdk_status_t sao_sdk_mem_v1_5_status(const struct SaoSdkContext* ctx) {
    if (ctx == NULL || ctx->mem == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const uint32_t context_major = ctx->abi_version >> 16;
    const uint32_t context_minor = ctx->abi_version & 0xffffu;
    if (context_major != SAO_SDK_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if (context_minor < 5u)
        return SAO_SDK_ERR_UNSUPPORTED;
    if ((ctx->mem->abi_version >> 16) != SAO_SDK_MEM_TABLE_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if ((ctx->mem->abi_version & 0xffffu) < 5u)
        return SAO_SDK_ERR_UNSUPPORTED;
    if (ctx->mem->struct_size < SAO_SDK_MEM_TABLE_V1_5_REQUIRED_SIZE)
        return SAO_SDK_ERR_UNSUPPORTED;
    return SAO_SDK_OK;
}

static inline sao_sdk_status_t
sao_sdk_mem_attach(const struct SaoSdkContext* ctx,
                   const struct SaoSdkMemoryTargetIdentity* identity) {
    const sao_sdk_status_t table_status = sao_sdk_mem_v1_5_status(ctx);
    if (table_status != SAO_SDK_OK)
        return table_status;
    if (ctx->mem->attach == NULL)
        return SAO_SDK_ERR_UNSUPPORTED;
    return ctx->mem->attach(ctx->ctx_impl, identity);
}

static inline sao_sdk_status_t sao_sdk_mem_detach(const struct SaoSdkContext* ctx) {
    const sao_sdk_status_t table_status = sao_sdk_mem_v1_5_status(ctx);
    if (table_status != SAO_SDK_OK)
        return table_status;
    if (ctx->mem->detach == NULL)
        return SAO_SDK_ERR_UNSUPPORTED;
    return ctx->mem->detach(ctx->ctx_impl);
}

static inline sao_sdk_status_t sao_sdk_mem_enumerate_modules(const struct SaoSdkContext* ctx,
                                                             struct SaoSdkMemoryModule* out_modules,
                                                             size_t capacity, size_t element_stride,
                                                             size_t* out_count) {
    if (out_count != NULL)
        *out_count = 0;
    const sao_sdk_status_t table_status = sao_sdk_mem_v1_5_status(ctx);
    if (table_status != SAO_SDK_OK)
        return table_status;
    if (ctx->mem->enumerate_modules == NULL)
        return SAO_SDK_ERR_UNSUPPORTED;
    return ctx->mem->enumerate_modules(ctx->ctx_impl, out_modules, capacity, element_stride,
                                       out_count);
}

#ifdef __cplusplus
} // extern "C"
#endif
