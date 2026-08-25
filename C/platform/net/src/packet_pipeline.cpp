#include "sao/net/packet_pipeline.h"

#include "sao/net/tcp_reassembly.h"
#include "tcp_reassembly_internal.h"

#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

extern "C" bool sao_net_capture_retain_internal(
    sao_net_capture_handle_t handle);
extern "C" void sao_net_capture_release_internal(
    sao_net_capture_handle_t handle);

struct sao_net_pipeline_s {
    std::mutex mutex;
    sao_net_capture_handle_t capture = nullptr;
    sao_net_reassembler_handle_t reassembler = nullptr;
    SaoPipelineStats stats{};
    sao_net_frame_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
    uint32_t lifetime_refs = 1;
    uint32_t active_callbacks = 0;
    uint32_t active_frame_callbacks = 0;
    std::condition_variable frame_callback_cv;
    bool stopping = false;
    bool destroy_in_progress = false;
    bool owner_released = false;
    bool cleanup_done = false;
};

namespace {

thread_local sao_net_pipeline_s* g_active_pipeline_callback = nullptr;

class PipelineCallbackThreadScope {
public:
    explicit PipelineCallbackThreadScope(sao_net_pipeline_s* pipeline)
        : previous_(g_active_pipeline_callback) {
        g_active_pipeline_callback = pipeline;
    }

    ~PipelineCallbackThreadScope() {
        g_active_pipeline_callback = previous_;
    }

private:
    sao_net_pipeline_s* previous_;
};

bool endpoint_is_zero(const SaoTcpEndpoint& endpoint) noexcept {
    const uint8_t aggregate = static_cast<uint8_t>(
        endpoint.ipv4[0] | endpoint.ipv4[1] | endpoint.ipv4[2] | endpoint.ipv4[3]);
    return aggregate == 0 && endpoint.port == 0;
}

bool endpoint_matches(const SaoTcpEndpoint& endpoint,
                      const uint8_t ipv4[4], uint16_t port) noexcept {
    return endpoint.port == port && std::memcmp(endpoint.ipv4, ipv4, 4) == 0;
}

void release_pipeline_ref(sao_net_pipeline_s* pipeline);

bool retain_pipeline_ref(sao_net_pipeline_s* pipeline) {
    std::lock_guard<std::mutex> guard(pipeline->mutex);
    if (pipeline->lifetime_refs == 0) return false;
    ++pipeline->lifetime_refs;
    return true;
}

class PipelineOperationLease {
public:
    explicit PipelineOperationLease(sao_net_pipeline_s* pipeline)
        : pipeline_(pipeline != nullptr && retain_pipeline_ref(pipeline)
                        ? pipeline
                        : nullptr) {}
    ~PipelineOperationLease() { release_pipeline_ref(pipeline_); }
    explicit operator bool() const noexcept { return pipeline_ != nullptr; }
private:
    sao_net_pipeline_s* pipeline_ = nullptr;
};

void finalize_pipeline_if_ready(sao_net_pipeline_s* pipeline) {
    sao_net_reassembler_handle_t reassembler = nullptr;
    sao_net_capture_handle_t capture = nullptr;
    bool destroy = false;
    {
        std::lock_guard<std::mutex> guard(pipeline->mutex);
        if (!pipeline->owner_released || pipeline->active_callbacks != 0 ||
            pipeline->cleanup_done) {
            return;
        }
        pipeline->cleanup_done = true;
        reassembler = pipeline->reassembler;
        pipeline->reassembler = nullptr;
        capture = pipeline->capture;
        pipeline->capture = nullptr;
        destroy = pipeline->lifetime_refs == 0;
    }
    if (reassembler != nullptr) sao_net_reassembler_destroy(reassembler);
    if (capture != nullptr) sao_net_capture_release_internal(capture);
    if (destroy) delete pipeline;
}

void release_pipeline_ref(sao_net_pipeline_s* pipeline) {
    if (pipeline == nullptr) return;
    bool destroy = false;
    {
        std::lock_guard<std::mutex> guard(pipeline->mutex);
        if (pipeline->lifetime_refs == 0) return;
        --pipeline->lifetime_refs;
        destroy = pipeline->lifetime_refs == 0 && pipeline->owner_released &&
                  pipeline->cleanup_done;
    }
    if (destroy) {
        delete pipeline;
        return;
    }
    finalize_pipeline_if_ready(pipeline);
}

class PipelineCallbackLease {
public:
    explicit PipelineCallbackLease(sao_net_pipeline_s* pipeline)
        : pipeline_(pipeline) {
        if (pipeline_ == nullptr) return;
        std::lock_guard<std::mutex> guard(pipeline_->mutex);
        if (pipeline_->stopping || pipeline_->reassembler == nullptr) {
            pipeline_ = nullptr;
            return;
        }
        ++pipeline_->active_callbacks;
        ++pipeline_->lifetime_refs;
    }

    ~PipelineCallbackLease() {
        if (pipeline_ == nullptr) return;
        {
            std::lock_guard<std::mutex> guard(pipeline_->mutex);
            if (pipeline_->active_callbacks != 0) --pipeline_->active_callbacks;
        }
        release_pipeline_ref(pipeline_);
    }

    explicit operator bool() const { return pipeline_ != nullptr; }

private:
    sao_net_pipeline_s* pipeline_ = nullptr;
};

void SAO_NET_CALL pipeline_packet_callback(const uint8_t* bytes, size_t length,
                                           uint64_t ts_unix_ns,
                                           void* user_data) {
    auto* pipeline = static_cast<sao_net_pipeline_s*>(user_data);
    PipelineCallbackLease lease(pipeline);
    if (!lease) return;
    PipelineCallbackThreadScope callback_scope(pipeline);

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

    while (true) {
        {
            std::lock_guard<std::mutex> guard(pipeline->mutex);
            if (pipeline->stopping) return;
        }
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
                if (pipeline->stopping) return;
                ++pipeline->stats.frames_delivered;
                pipeline->stats.bytes_delivered += frame.size();
                callback = pipeline->callback;
                callback_user_data = pipeline->callback_user_data;
                if (callback != nullptr) {
                    ++pipeline->active_frame_callbacks;
                }
            }
            if (callback != nullptr) {
                bool callback_failed = false;
                try {
                    callback(frame.data(), frame.size(), ts_unix_ns,
                             callback_user_data);
                } catch (...) {
                    callback_failed = true;
                }
                {
                    std::lock_guard<std::mutex> guard(pipeline->mutex);
                    if (pipeline->active_frame_callbacks != 0u) {
                        --pipeline->active_frame_callbacks;
                    }
                    if (callback_failed) ++pipeline->stats.drop_events;
                }
                pipeline->frame_callback_cv.notify_all();
                if (callback_failed) return;
            }
        } catch (...) {
            std::lock_guard<std::mutex> guard(pipeline->mutex);
            ++pipeline->stats.drop_events;
            return;
        }
    }
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_create(
    sao_net_capture_handle_t capture, sao_net_pipeline_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (capture == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!sao_net_capture_retain_internal(capture)) {
        return SAO_STATUS_ERR_CANCELLED;
    }
    try {
        auto* pipeline = new sao_net_pipeline_s();
        pipeline->capture = capture;
        SaoReassemblerConfig config{};
        auto status = sao_net_reassembler_create(&config, &pipeline->reassembler);
        if (status != SAO_STATUS_OK) {
            sao_net_capture_release_internal(capture);
            delete pipeline;
            return status;
        }
        status = sao_net_capture_start(capture, pipeline_packet_callback, pipeline);
        if (status != SAO_STATUS_OK) {
            sao_net_reassembler_destroy(pipeline->reassembler);
            sao_net_capture_release_internal(capture);
            delete pipeline;
            return status;
        }
        *out_handle = pipeline;
        return SAO_STATUS_OK;
    } catch (...) {
        sao_net_capture_release_internal(capture);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_try_destroy(
    sao_net_pipeline_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_OK;
    if (!retain_pipeline_ref(handle)) return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_net_capture_handle_t capture = nullptr;
    bool already_busy = false;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->owner_released || handle->destroy_in_progress) {
            already_busy = true;
        } else {
        handle->destroy_in_progress = true;
        handle->stopping = true;
        handle->callback = nullptr;
        handle->callback_user_data = nullptr;
            capture = handle->capture;
        }
    }
    if (already_busy) {
        release_pipeline_ref(handle);
        return SAO_NET_STATUS_BUSY;
    }
    const auto capture_status = capture == nullptr
                                    ? SAO_STATUS_OK
                                    : sao_net_capture_stop(capture);
    const bool busy = capture_status == SAO_NET_STATUS_BUSY;
    const bool caller_is_pipeline_callback =
        g_active_pipeline_callback == handle;
    if (busy && !caller_is_pipeline_callback) {
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            handle->destroy_in_progress = false;
        }
        release_pipeline_ref(handle);
        return SAO_NET_STATUS_BUSY;
    }
    bool release_owner = false;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        handle->destroy_in_progress = false;
        if (!handle->owner_released) {
            handle->owner_released = true;
            release_owner = true;
        }
    }
    if (release_owner) release_pipeline_ref(handle);
    if (!busy) finalize_pipeline_if_ready(handle);
    release_pipeline_ref(handle);
    return busy ? SAO_NET_STATUS_BUSY : capture_status;
}

extern "C" void SAO_NET_CALL sao_net_pipeline_destroy(
    sao_net_pipeline_handle_t handle) {
    (void)sao_net_pipeline_try_destroy(handle);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_lock_endpoint(
    sao_net_pipeline_handle_t handle, const SaoTcpEndpoint* remote) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (remote == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PipelineOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> guard(handle->mutex);
    if (handle->stopping) return SAO_NET_STATUS_BUSY;
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
    PipelineOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> guard(handle->mutex);
    if (handle->stopping) return SAO_NET_STATUS_BUSY;
    if (handle->active_frame_callbacks != 0u) return SAO_NET_STATUS_BUSY;
    handle->callback = callback;
    handle->callback_user_data = callback == nullptr ? nullptr : user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_pipeline_snapshot_stats(
    sao_net_pipeline_handle_t handle, SaoPipelineStats* out_stats) {
    if (out_stats == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_stats, 0, sizeof(*out_stats));
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    PipelineOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> guard(handle->mutex);
    *out_stats = handle->stats;
    return SAO_STATUS_OK;
}