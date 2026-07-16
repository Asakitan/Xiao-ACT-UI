#include "sao/net/packet_pipeline.h"

#include "sao/net/tcp_reassembly.h"
#include "tcp_reassembly_internal.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

struct sao_net_pipeline_s {
    sao_net_capture_handle_t capture = nullptr;
    sao_net_reassembler_handle_t reassembler = nullptr;
    std::mutex mutex;
    SaoPipelineStats stats{};
    sao_net_frame_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
    std::atomic<bool> stopping{false};
};

namespace {

bool endpoint_is_zero(const SaoTcpEndpoint& endpoint) noexcept {
    const uint8_t aggregate = static_cast<uint8_t>(
        endpoint.ipv4[0] | endpoint.ipv4[1] | endpoint.ipv4[2] | endpoint.ipv4[3]);
    return aggregate == 0 && endpoint.port == 0;
}

bool endpoint_matches(const SaoTcpEndpoint& endpoint,
                      const uint8_t ipv4[4], uint16_t port) noexcept {
    return endpoint.port == port && std::memcmp(endpoint.ipv4, ipv4, 4) == 0;
}

void SAO_NET_CALL pipeline_packet_callback(const uint8_t* bytes, size_t length,
                                           uint64_t ts_unix_ns,
                                           void* user_data) {
    auto* pipeline = static_cast<sao_net_pipeline_s*>(user_data);
    if (pipeline == nullptr || pipeline->stopping.load(std::memory_order_acquire)) return;

    uint8_t remote_ipv4[4]{};
    uint16_t remote_port = 0;
    if (!sao::net::internal::tcp_packet_remote_ipv4(
            bytes, length, remote_ipv4, &remote_port)) {
        std::lock_guard<std::mutex> guard(pipeline->mutex);
        ++pipeline->stats.drop_events;
        return;
    }
    {
        std::lock_guard<std::mutex> guard(pipeline->mutex);
        if (pipeline->stats.endpoint_locked &&
            !endpoint_matches(pipeline->stats.locked_remote,
                              remote_ipv4, remote_port)) {
            ++pipeline->stats.drop_events;
            return;
        }
    }

    auto status = sao_net_reassembler_ingest(
        pipeline->reassembler, bytes, length, ts_unix_ns / 1'000'000ull);
    if (status != SAO_STATUS_OK) {
        std::lock_guard<std::mutex> guard(pipeline->mutex);
        ++pipeline->stats.drop_events;
        return;
    }

    while (!pipeline->stopping.load(std::memory_order_acquire)) {
        sao_net_stream_id_t stream_id = 0;
        size_t required = 0;
        status = sao_net_reassembler_pop_stream(
            pipeline->reassembler, &stream_id, nullptr, 0, &required);
        if (status == SAO_STATUS_OK && required == 0) break;
        if (status != SAO_STATUS_ERR_BUFFER_TOO_SMALL || required == 0) break;
        try {
            std::vector<uint8_t> frame(required);
            size_t delivered = 0;
            status = sao_net_reassembler_pop_stream(
                pipeline->reassembler, &stream_id, frame.data(), frame.size(),
                &delivered);
            if (status != SAO_STATUS_OK) break;
            frame.resize(delivered);

            sao_net_frame_callback_t callback = nullptr;
            void* callback_user_data = nullptr;
            {
                std::lock_guard<std::mutex> guard(pipeline->mutex);
                ++pipeline->stats.frames_delivered;
                pipeline->stats.bytes_delivered += frame.size();
                callback = pipeline->callback;
                callback_user_data = pipeline->callback_user_data;
            }
            if (callback != nullptr) {
                callback(frame.data(), frame.size(), ts_unix_ns,
                         callback_user_data);
            }
        } catch (...) {
            std::lock_guard<std::mutex> guard(pipeline->mutex);
            ++pipeline->stats.drop_events;
            break;
        }
    }
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_create(
    sao_net_capture_handle_t capture, sao_net_pipeline_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (capture == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    try {
        auto* pipeline = new sao_net_pipeline_s();
        pipeline->capture = capture;
        SaoReassemblerConfig config{};
        auto status = sao_net_reassembler_create(&config, &pipeline->reassembler);
        if (status != SAO_STATUS_OK) {
            delete pipeline;
            return status;
        }
        status = sao_net_capture_start(capture, pipeline_packet_callback, pipeline);
        if (status != SAO_STATUS_OK) {
            sao_net_reassembler_destroy(pipeline->reassembler);
            delete pipeline;
            return status;
        }
        *out_handle = pipeline;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_NET_CALL sao_net_pipeline_destroy(
    sao_net_pipeline_handle_t handle) {
    if (handle == nullptr) return;
    handle->stopping.store(true, std::memory_order_release);
    if (handle->capture != nullptr) (void)sao_net_capture_stop(handle->capture);
    sao_net_reassembler_destroy(handle->reassembler);
    handle->reassembler = nullptr;
    delete handle;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_lock_endpoint(
    sao_net_pipeline_handle_t handle, const SaoTcpEndpoint* remote) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (remote == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(handle->mutex);
    if (endpoint_is_zero(*remote)) {
        std::memset(&handle->stats.locked_remote, 0,
                    sizeof(handle->stats.locked_remote));
        handle->stats.endpoint_locked = false;
        return SAO_STATUS_OK;
    }
    if (remote->port == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    handle->stats.locked_remote = *remote;
    handle->stats.endpoint_locked = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_set_frame_callback(
    sao_net_pipeline_handle_t handle, sao_net_frame_callback_t callback,
    void* user_data) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> guard(handle->mutex);
    handle->callback = callback;
    handle->callback_user_data = callback == nullptr ? nullptr : user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_snapshot_stats(
    sao_net_pipeline_handle_t handle, SaoPipelineStats* out_stats) {
    if (out_stats == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_stats, 0, sizeof(*out_stats));
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> guard(handle->mutex);
    *out_stats = handle->stats;
    return SAO_STATUS_OK;
}
