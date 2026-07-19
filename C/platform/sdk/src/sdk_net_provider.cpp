#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sao_sdk_internal {

struct NetProviderSession {
    ContextState* owner = nullptr;
    SaoSdkNetProviderVTable provider{};
    void* session = nullptr;
    std::mutex mutex;
    std::mutex operation_mutex;
    std::condition_variable idle;
    size_t active_calls = 0;
    bool accepting = true;
    bool capturing = false;
    bool retained = false;
    sao_sdk_status_t cleanup_status = SAO_SDK_OK;
    sao_sdk_net_packet_callback_t packet_callback = nullptr;
    void* packet_user_data = nullptr;
};

thread_local ContextState* g_net_callback_owner = nullptr;
thread_local NetProviderSession* g_net_callback_session = nullptr;

bool net_callback_reentered(ContextState* state) noexcept {
    return state != nullptr && g_net_callback_owner == state;
}

namespace {

constexpr size_t kMaximumParsedResults = 4096;

class NetCallbackScope {
  public:
    NetCallbackScope(ContextState* owner, NetProviderSession* session) noexcept
        : previous_owner_(g_net_callback_owner), previous_session_(g_net_callback_session) {
        g_net_callback_owner = owner;
        g_net_callback_session = session;
    }

    ~NetCallbackScope() {
        g_net_callback_owner = previous_owner_;
        g_net_callback_session = previous_session_;
    }

    NetCallbackScope(const NetCallbackScope&) = delete;
    NetCallbackScope& operator=(const NetCallbackScope&) = delete;

  private:
    ContextState* previous_owner_ = nullptr;
    NetProviderSession* previous_session_ = nullptr;
};

class NetCallLease {
  public:
    explicit NetCallLease(ContextState* state) {
        if (state == nullptr)
            return;
        if (state->destroying.load(std::memory_order_acquire)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        if (net_callback_reentered(state)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mu);
            session_ = state->net_provider;
        }
        if (session_ == nullptr) {
            status_ = SAO_SDK_ERR_UNSUPPORTED;
            return;
        }
        std::lock_guard<std::mutex> lock(session_->mutex);
        if (!session_->accepting) {
            status_ = session_->cleanup_status == SAO_SDK_OK ? SAO_SDK_ERR_BUSY
                                                             : session_->cleanup_status;
            session_.reset();
            return;
        }
        ++session_->active_calls;
        active_ = true;
        status_ = SAO_SDK_OK;
    }

    ~NetCallLease() {
        if (!active_)
            return;
        std::lock_guard<std::mutex> lock(session_->mutex);
        --session_->active_calls;
        if (session_->active_calls == 0)
            session_->idle.notify_all();
    }

    NetCallLease(const NetCallLease&) = delete;
    NetCallLease& operator=(const NetCallLease&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }

    sao_sdk_status_t status() const noexcept {
        return status_;
    }

    NetProviderSession* operator->() const noexcept {
        return session_.get();
    }

  private:
    std::shared_ptr<NetProviderSession> session_;
    bool active_ = false;
    sao_sdk_status_t status_ = SAO_SDK_ERR_HANDLE_INVALID;
};

size_t minimum_packet_size(uint32_t link_type) noexcept {
    switch (link_type) {
    case SAO_SDK_NET_LINK_ETHERNET:
        return 14;
    case SAO_SDK_NET_LINK_RAW_IPV4:
        return 20;
    case SAO_SDK_NET_LINK_RAW_IPV6:
        return 40;
    case SAO_SDK_NET_LINK_UNKNOWN:
        return 1;
    default:
        return 0;
    }
}

sao_sdk_status_t validate_packet_view(const SaoSdkNetPacketView* packet,
                                      SaoSdkNetPacketView* out_normalized) noexcept {
    if (packet == nullptr || out_normalized == nullptr ||
        packet->struct_size < SAO_SDK_NET_PACKET_VIEW_REQUIRED_SIZE) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    SaoSdkNetPacketView normalized{};
    std::memcpy(&normalized, packet, sizeof(normalized));
    if ((normalized.abi_version >> 16) != SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    const size_t minimum_size = minimum_packet_size(normalized.link_type);
    if (minimum_size == 0 || normalized.data == nullptr || normalized.data_size < minimum_size ||
        normalized.data_size > SAO_SDK_NET_MAX_PACKET_SIZE) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    normalized.struct_size = sizeof(normalized);
    normalized.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    *out_normalized = normalized;
    return SAO_SDK_OK;
}

bool valid_result_range(size_t offset, size_t length, size_t packet_size) noexcept {
    return offset <= packet_size && length <= packet_size - offset;
}

void finish_async_callback(NetProviderSession* session) noexcept {
    try {
        std::lock_guard<std::mutex> lock(session->mutex);
        --session->active_calls;
        if (session->active_calls == 0)
            session->idle.notify_all();
    } catch (...) {
    }
}

void SAO_SDK_CALL net_packet_callback_bridge(const SaoSdkNetPacketView* packet,
                                             void* user_data) noexcept {
    auto* session = static_cast<NetProviderSession*>(user_data);
    if (session == nullptr)
        return;
    sao_sdk_net_packet_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
    try {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->accepting || session->packet_callback == nullptr)
                return;
            ++session->active_calls;
            callback = session->packet_callback;
            callback_user_data = session->packet_user_data;
        }
        SaoSdkNetPacketView normalized{};
        if (validate_packet_view(packet, &normalized) == SAO_SDK_OK) {
            PluginCallbackLease callback_lease(session->owner);
            if (callback_lease) {
                NetCallbackScope callback_scope(session->owner, session);
                (void)invoke_void_callback_barrier(
                    [&] { callback(&normalized, callback_user_data); });
            }
        }
        finish_async_callback(session);
    } catch (...) {
        if (callback != nullptr)
            finish_async_callback(session);
    }
}

sao_sdk_status_t shutdown_net_session(const std::shared_ptr<NetProviderSession>& session) {
    if (session == nullptr)
        return SAO_SDK_OK;
    if (g_net_callback_session == session.get() || net_callback_reentered(session->owner))
        return SAO_SDK_ERR_BUSY;

    {
        std::unique_lock<std::mutex> lock(session->mutex);
        session->accepting = false;
        session->idle.wait(lock, [&session] { return session->active_calls == 0; });
    }

    std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
    bool capturing = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        capturing = session->capturing;
    }
    if (capturing) {
        const auto status = invoke_callback_barrier([&] {
            NetCallbackScope callback_scope(session->owner, session.get());
            return normalize_provider_status(
                session->provider.capture_stop(session->provider.user_data, session->session));
        });
        if (status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cleanup_status = status;
            return status;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->capturing = false;
        session->packet_callback = nullptr;
        session->packet_user_data = nullptr;
    }

    if (session->session != nullptr) {
        const auto status = invoke_callback_barrier([&] {
            NetCallbackScope callback_scope(session->owner, session.get());
            return normalize_provider_status(
                session->provider.close_session(session->provider.user_data, session->session));
        });
        if (status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cleanup_status = status;
            return status;
        }
        session->session = nullptr;
    }

    if (session->retained) {
        const auto status = invoke_callback_barrier([&] {
            NetCallbackScope callback_scope(session->owner, session.get());
            session->provider.release(session->provider.user_data);
            return SAO_SDK_OK;
        });
        if (status == SAO_SDK_OK) {
            session->retained = false;
        } else {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cleanup_status = status;
            return status;
        }
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    session->cleanup_status = SAO_SDK_OK;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL net_set_frame(void*, SaoSdkNetTable::frame_callback_t,
                                            void*) noexcept {
    return SAO_SDK_ERR_UNSUPPORTED;
}

sao_sdk_status_t SAO_SDK_CALL net_table_capture_start(void* ctx_impl,
                                                      const SaoSdkNetCaptureConfig* config,
                                                      sao_sdk_net_packet_callback_t callback,
                                                      void* user_data) noexcept {
    try {
        return net_capture_start(cast_ctx(ctx_impl), config, callback, user_data);
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t SAO_SDK_CALL net_table_capture_stop(void* ctx_impl) noexcept {
    try {
        return net_capture_stop(cast_ctx(ctx_impl));
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t SAO_SDK_CALL net_table_parse_packet(void* ctx_impl,
                                                     const SaoSdkNetPacketView* packet,
                                                     SaoSdkNetParsedResult* out_results,
                                                     size_t capacity, size_t element_stride,
                                                     size_t* out_count) noexcept {
    try {
        return net_parse_packet(cast_ctx(ctx_impl), packet, out_results, capacity, element_stride,
                                out_count);
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_SDK_ERR_INTERNAL;
    }
}

const SaoSdkNetTable kNetTable = {
    net_set_frame,           SAO_SDK_NET_TABLE_ABI_VERSION, sizeof(SaoSdkNetTable),
    net_table_capture_start, net_table_capture_stop,        net_table_parse_packet,
};

} // namespace

const SaoSdkNetTable* make_net_table() {
    return &kNetTable;
}

sao_sdk_status_t configure_net_provider(ContextState* state,
                                        const SaoSdkNetProviderVTable* provider) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (state->destroying.load(std::memory_order_acquire) &&
        !context_destroy_on_current_thread(state))
        return SAO_SDK_ERR_BUSY;
    if (net_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;

    std::lock_guard<std::mutex> lifecycle_lock(state->net_lifecycle_mutex);

    std::vector<std::shared_ptr<NetProviderSession>> quarantined;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        quarantined = state->net_quarantine;
    }
    for (const auto& session : quarantined) {
        const auto status = shutdown_net_session(session);
        if (status != SAO_SDK_OK)
            return status;
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase(state->net_quarantine, session);
    }

    std::shared_ptr<NetProviderSession> previous;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        previous = state->net_provider;
    }
    if (provider == nullptr) {
        const auto cleanup_status = shutdown_net_session(previous);
        if (cleanup_status != SAO_SDK_OK)
            return cleanup_status;
        std::lock_guard<std::mutex> lock(state->mu);
        if (state->net_provider == previous)
            state->net_provider.reset();
        return SAO_SDK_OK;
    }

    if ((provider->abi_version >> 16) != SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < SAO_SDK_NET_PROVIDER_REQUIRED_SIZE) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }
    SaoSdkNetProviderVTable copy{};
    std::memcpy(&copy, provider, std::min<size_t>(provider->struct_size, sizeof(copy)));
    if (copy.retain == nullptr || copy.release == nullptr || copy.open_session == nullptr ||
        copy.close_session == nullptr || copy.capture_start == nullptr ||
        copy.capture_stop == nullptr || copy.parse_packet == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    std::shared_ptr<NetProviderSession> candidate;
    try {
        candidate = std::make_shared<NetProviderSession>();
        candidate->owner = state;
        candidate->provider = copy;
        const auto retain_status = invoke_callback_barrier([&] {
            NetCallbackScope callback_scope(state, candidate.get());
            copy.retain(copy.user_data);
            return SAO_SDK_OK;
        });
        if (retain_status != SAO_SDK_OK)
            return retain_status;
        candidate->retained = true;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }

    const auto open_status = invoke_callback_barrier([&] {
        NetCallbackScope callback_scope(state, candidate.get());
        return normalize_provider_status(
            copy.open_session(copy.user_data, state->plugin_id.c_str(), &candidate->session));
    });
    if (open_status != SAO_SDK_OK || candidate->session == nullptr) {
        candidate->accepting = false;
        const auto cleanup_status = shutdown_net_session(candidate);
        if (cleanup_status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->net_quarantine.push_back(candidate);
            return cleanup_status;
        }
        return open_status == SAO_SDK_OK ? SAO_SDK_ERR_HANDLE_INVALID : open_status;
    }

    const auto cleanup_status = shutdown_net_session(previous);
    if (cleanup_status != SAO_SDK_OK) {
        candidate->accepting = false;
        const auto candidate_cleanup_status = shutdown_net_session(candidate);
        if (candidate_cleanup_status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->net_quarantine.push_back(candidate);
        }
        return cleanup_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->net_provider = std::move(candidate);
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t net_provider_status(const ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    std::shared_ptr<NetProviderSession> session;
    {
        std::lock_guard<std::mutex> lock(const_cast<ContextState*>(state)->mu);
        session = state->net_provider;
        if (session == nullptr && !state->net_quarantine.empty())
            session = state->net_quarantine.front();
    }
    if (session == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->cleanup_status != SAO_SDK_OK)
        return session->cleanup_status;
    return session->accepting ? SAO_SDK_OK : SAO_SDK_ERR_BUSY;
}

sao_sdk_status_t net_provider_cleanup(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    try {
        return configure_net_provider(state, nullptr);
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t net_capture_start(ContextState* state, const SaoSdkNetCaptureConfig* config,
                                   sao_sdk_net_packet_callback_t callback, void* user_data) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (config == nullptr || callback == nullptr ||
        config->struct_size < SAO_SDK_NET_CAPTURE_CONFIG_REQUIRED_SIZE) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    SaoSdkNetCaptureConfig normalized{};
    std::memcpy(&normalized, config, sizeof(normalized));
    if ((normalized.abi_version >> 16) != SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if (normalized.snap_length == 0 || normalized.snap_length > SAO_SDK_NET_MAX_PACKET_SIZE ||
        normalized.source_id_len == 0 ||
        normalized.source_id_len > SAO_SDK_NET_MAX_SOURCE_ID_SIZE ||
        normalized.source_id_utf8 == nullptr ||
        normalized.filter_len > SAO_SDK_NET_MAX_FILTER_SIZE ||
        (normalized.filter_len != 0 && normalized.filter_utf8 == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    normalized.struct_size = sizeof(normalized);
    normalized.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;

    NetCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    {
        std::lock_guard<std::mutex> lock(lease->mutex);
        if (lease->capturing)
            return SAO_SDK_ERR_BUSY;
        lease->packet_callback = callback;
        lease->packet_user_data = user_data;
    }
    const auto status = invoke_callback_barrier([&] {
        NetCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(
            lease->provider.capture_start(lease->provider.user_data, lease->session, &normalized,
                                          net_packet_callback_bridge, lease.operator->()));
    });
    {
        std::unique_lock<std::mutex> lock(lease->mutex);
        if (status == SAO_SDK_OK) {
            lease->capturing = true;
        } else {
            lease->packet_callback = nullptr;
            lease->packet_user_data = nullptr;
            lease->idle.wait(lock, [&lease] { return lease->active_calls == 1; });
        }
    }
    return status;
}

sao_sdk_status_t net_capture_stop(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    NetCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    {
        std::lock_guard<std::mutex> lock(lease->mutex);
        if (!lease->capturing)
            return SAO_SDK_OK;
    }
    const auto status = invoke_callback_barrier([&] {
        NetCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(
            lease->provider.capture_stop(lease->provider.user_data, lease->session));
    });
    if (status == SAO_SDK_OK) {
        std::unique_lock<std::mutex> lock(lease->mutex);
        lease->capturing = false;
        lease->packet_callback = nullptr;
        lease->packet_user_data = nullptr;
        lease->idle.wait(lock, [&lease] { return lease->active_calls == 1; });
    }
    return status;
}

sao_sdk_status_t net_parse_packet(ContextState* state, const SaoSdkNetPacketView* packet,
                                  SaoSdkNetParsedResult* out_results, size_t capacity,
                                  size_t element_stride, size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (out_count == nullptr || (capacity != 0 && out_results == nullptr) ||
        element_stride != SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE ||
        capacity > kMaximumParsedResults) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    SaoSdkNetPacketView normalized{};
    const auto packet_status = validate_packet_view(packet, &normalized);
    if (packet_status != SAO_SDK_OK)
        return packet_status;
    if (out_results != nullptr && capacity != 0)
        std::memset(out_results, 0, capacity * element_stride);

    NetCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    const auto status = invoke_callback_barrier([&] {
        NetCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(
            lease->provider.parse_packet(lease->provider.user_data, lease->session, &normalized,
                                         out_results, capacity, element_stride, out_count));
    });
    if (status == SAO_SDK_ERR_INTERNAL) {
        if (out_results != nullptr && capacity != 0)
            std::memset(out_results, 0, capacity * element_stride);
        *out_count = 0;
        return status;
    }
    if (*out_count > kMaximumParsedResults) {
        if (out_results != nullptr && capacity != 0)
            std::memset(out_results, 0, capacity * element_stride);
        *out_count = 0;
        return SAO_SDK_ERR_INTERNAL;
    }
    if (status == SAO_SDK_ERR_BUFFER_TOO_SMALL) {
        if (out_results != nullptr && capacity != 0)
            std::memset(out_results, 0, capacity * element_stride);
        return status;
    }
    if (status != SAO_SDK_OK) {
        if (out_results != nullptr && capacity != 0)
            std::memset(out_results, 0, capacity * element_stride);
        *out_count = 0;
        return status;
    }
    if (*out_count > capacity) {
        if (out_results != nullptr && capacity != 0)
            std::memset(out_results, 0, capacity * element_stride);
        *out_count = 0;
        return SAO_SDK_ERR_INTERNAL;
    }
    for (size_t index = 0; index < *out_count; ++index) {
        const auto* result = reinterpret_cast<const SaoSdkNetParsedResult*>(
            reinterpret_cast<const uint8_t*>(out_results) + index * element_stride);
        if (result->struct_size != SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE ||
            (result->abi_version >> 16) != SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR) {
            std::memset(out_results, 0, capacity * element_stride);
            *out_count = 0;
            return SAO_SDK_ERR_ABI_MISMATCH;
        }
        if (!valid_result_range(result->header_offset, result->header_size, normalized.data_size) ||
            !valid_result_range(result->payload_offset, result->payload_size,
                                normalized.data_size)) {
            std::memset(out_results, 0, capacity * element_stride);
            *out_count = 0;
            return SAO_SDK_ERR_INTERNAL;
        }
    }
    return SAO_SDK_OK;
}

} // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_configure_net_provider(
    SaoSdkContext* ctx, const SaoSdkNetProviderVTable* provider) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    try {
        return sao_sdk_internal::configure_net_provider(sao_sdk_internal::cast_ctx(ctx->ctx_impl),
                                                        provider);
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_net_provider_status(const SaoSdkContext* ctx) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    try {
        return sao_sdk_internal::net_provider_status(sao_sdk_internal::cast_ctx(ctx->ctx_impl));
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}
