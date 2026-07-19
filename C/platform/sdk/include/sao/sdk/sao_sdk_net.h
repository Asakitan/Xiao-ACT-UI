// SAO Auto — SDK: net capability convenience wrappers.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR 1u
#define SAO_SDK_NET_PROVIDER_ABI_VERSION_MINOR 0u
#define SAO_SDK_NET_PROVIDER_ABI_VERSION                                                           \
    ((SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR << 16) | SAO_SDK_NET_PROVIDER_ABI_VERSION_MINOR)

#define SAO_SDK_NET_MAX_PACKET_SIZE (16u * 1024u * 1024u)
#define SAO_SDK_NET_MAX_SOURCE_ID_SIZE 1024u
#define SAO_SDK_NET_MAX_FILTER_SIZE (64u * 1024u)

enum sao_sdk_net_link_type_e : uint32_t {
    SAO_SDK_NET_LINK_UNKNOWN = 0,
    SAO_SDK_NET_LINK_ETHERNET = 1,
    SAO_SDK_NET_LINK_RAW_IPV4 = 2,
    SAO_SDK_NET_LINK_RAW_IPV6 = 3,
};

struct SaoSdkNetCaptureConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t snap_length;
    uint32_t flags;
    const char* source_id_utf8;
    size_t source_id_len;
    const char* filter_utf8;
    size_t filter_len;
};

// `source_id_utf8` and `filter_utf8` are borrowed only for the duration of
// capture_start. A provider that needs them after return must copy them.

#define SAO_SDK_NET_CAPTURE_CONFIG_REQUIRED_SIZE sizeof(struct SaoSdkNetCaptureConfig)

// `data` is borrowed and valid only for the duration of the provider call or
// packet callback that supplies this view. Consumers copy bytes they retain.
struct SaoSdkNetPacketView {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t link_type;
    uint32_t flags;
    const uint8_t* data;
    size_t data_size;
    uint64_t timestamp_unix_ns;
};

#define SAO_SDK_NET_PACKET_VIEW_REQUIRED_SIZE sizeof(struct SaoSdkNetPacketView)

// Results are caller-allocated POD values. All offsets and lengths refer to
// the input packet; providers never return pointers or provider-owned buffers.
struct SaoSdkNetParsedResult {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t layer;
    uint32_t protocol;
    size_t header_offset;
    size_t header_size;
    size_t payload_offset;
    size_t payload_size;
    uint64_t flow_id;
};

#define SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE sizeof(struct SaoSdkNetParsedResult)

// Independent host-owned capture/parse surface. The SDK retains the owner
// while configured and opens one session per SaoSdkContext. The provider owns
// capture implementation details; configuring this table never starts Npcap.
struct SaoSdkNetProviderVTable {
    uint32_t abi_version;
    uint32_t struct_size;
    void* user_data;

    void(SAO_SDK_CALL* retain)(void* user_data);
    void(SAO_SDK_CALL* release)(void* user_data);

    sao_sdk_status_t(SAO_SDK_CALL* open_session)(void* user_data, const char* plugin_id_utf8,
                                                 void** out_session);
    sao_sdk_status_t(SAO_SDK_CALL* close_session)(void* user_data, void* session);
    sao_sdk_status_t(SAO_SDK_CALL* capture_start)(void* user_data, void* session,
                                                  const struct SaoSdkNetCaptureConfig* config,
                                                  sao_sdk_net_packet_callback_t callback,
                                                  void* callback_user_data);
    sao_sdk_status_t(SAO_SDK_CALL* capture_stop)(void* user_data, void* session);
    sao_sdk_status_t(SAO_SDK_CALL* parse_packet)(void* user_data, void* session,
                                                 const struct SaoSdkNetPacketView* packet,
                                                 struct SaoSdkNetParsedResult* out_results,
                                                 size_t capacity, size_t element_stride,
                                                 size_t* out_count);
};

#define SAO_SDK_NET_PROVIDER_REQUIRED_SIZE                                                         \
    (offsetof(struct SaoSdkNetProviderVTable, parse_packet) +                                      \
     sizeof(((struct SaoSdkNetProviderVTable*)0)->parse_packet))

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_configure_net_provider(
    struct SaoSdkContext* ctx, const struct SaoSdkNetProviderVTable* provider);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_net_provider_status(const struct SaoSdkContext* ctx);

// Configures the process owner used to auto-open one net session for each new
// context and for contexts bound to platform services. Existing sessions keep
// their own provider lease when the process owner is replaced or cleared.
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_net_configure_provider(const struct SaoSdkNetProviderVTable* provider);

static inline sao_sdk_status_t sao_sdk_net_set_frame_callback(
    const struct SaoSdkContext* ctx,
    void(SAO_SDK_CALL* frame_callback)(const uint8_t*, size_t, uint64_t, void*), void* user_data) {
    if (ctx == NULL || ctx->net == NULL || ctx->net->set_frame_callback == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->net->set_frame_callback(ctx->ctx_impl, frame_callback, user_data);
}

static inline sao_sdk_status_t sao_sdk_net_v1_5_status(const struct SaoSdkContext* ctx) {
    if (ctx == NULL || ctx->net == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const uint32_t context_major = ctx->abi_version >> 16;
    const uint32_t context_minor = ctx->abi_version & 0xffffu;
    if (context_major != SAO_SDK_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if (context_minor < 5u)
        return SAO_SDK_ERR_UNSUPPORTED;
    if ((ctx->net->abi_version >> 16) != SAO_SDK_NET_TABLE_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if ((ctx->net->abi_version & 0xffffu) < 5u)
        return SAO_SDK_ERR_UNSUPPORTED;
    if (ctx->net->struct_size < SAO_SDK_NET_TABLE_V1_5_REQUIRED_SIZE)
        return SAO_SDK_ERR_UNSUPPORTED;
    return SAO_SDK_OK;
}

static inline sao_sdk_status_t
sao_sdk_net_capture_start(const struct SaoSdkContext* ctx,
                          const struct SaoSdkNetCaptureConfig* config,
                          sao_sdk_net_packet_callback_t callback, void* user_data) {
    const sao_sdk_status_t table_status = sao_sdk_net_v1_5_status(ctx);
    if (table_status != SAO_SDK_OK)
        return table_status;
    if (ctx->net->capture_start == NULL)
        return SAO_SDK_ERR_UNSUPPORTED;
    return ctx->net->capture_start(ctx->ctx_impl, config, callback, user_data);
}

static inline sao_sdk_status_t sao_sdk_net_capture_stop(const struct SaoSdkContext* ctx) {
    const sao_sdk_status_t table_status = sao_sdk_net_v1_5_status(ctx);
    if (table_status != SAO_SDK_OK)
        return table_status;
    if (ctx->net->capture_stop == NULL)
        return SAO_SDK_ERR_UNSUPPORTED;
    return ctx->net->capture_stop(ctx->ctx_impl);
}

static inline sao_sdk_status_t sao_sdk_net_parse_packet(const struct SaoSdkContext* ctx,
                                                        const struct SaoSdkNetPacketView* packet,
                                                        struct SaoSdkNetParsedResult* out_results,
                                                        size_t capacity, size_t element_stride,
                                                        size_t* out_count) {
    if (out_count != NULL)
        *out_count = 0;
    const sao_sdk_status_t table_status = sao_sdk_net_v1_5_status(ctx);
    if (table_status != SAO_SDK_OK)
        return table_status;
    if (ctx->net->parse_packet == NULL)
        return SAO_SDK_ERR_UNSUPPORTED;
    return ctx->net->parse_packet(ctx->ctx_impl, packet, out_results, capacity, element_stride,
                                  out_count);
}

#ifdef __cplusplus
} // extern "C"
#endif
