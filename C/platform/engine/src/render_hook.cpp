#include "sao/engine/render_hook.h"
#include "sao/engine/ui_spec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kInitialCallbackCapacity = 4096;

struct HookEntry {
    sao_engine_hook_token_t token = 0;
    std::string plugin_id;
    std::string surface_id;
    float priority = 0.0F;
    uint64_t insertion_sequence = 0;
    sao_engine_hook_callback_t callback = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> active{true};
};

struct ClockHookEntry {
    sao_engine_hook_token_t token = 0;
    std::string plugin_id;
    std::string surface_id;
    int32_t hook_point = SAO_ENGINE_RENDER_BEFORE_COMPOSITOR;
    float priority = 0.0F;
    uint64_t insertion_sequence = 0;
    sao_engine_render_clock_callback_t callback = nullptr;
    void* user_data = nullptr;
    sao_engine_render_clock_user_data_release_t release_user_data = nullptr;
    std::atomic<bool> active{true};

    ~ClockHookEntry() {
        if (release_user_data != nullptr) {
            release_user_data(user_data);
        }
    }
};

struct SurfaceClockState {
    uint32_t frame_index = 0;
    uint64_t seen_global_redraw_generation = 0;
    uint64_t seen_surface_redraw_generation = 0;
    bool redraw_requested = false;
};

struct OverlayKey {
    std::string plugin_id;
    std::string surface_id;

    bool operator==(const OverlayKey&) const = default;
};

struct OverlayKeyHash {
    size_t operator()(const OverlayKey& key) const noexcept {
        const size_t first = std::hash<std::string>{}(key.plugin_id);
        const size_t second = std::hash<std::string>{}(key.surface_id);
        return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
};

bool valid_string(const char* value) noexcept {
    return value != nullptr && value[0] != '\0';
}

} // namespace

struct sao_engine_render_hook_registry_s {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::vector<std::shared_ptr<HookEntry>>> hooks;
    std::unordered_map<sao_engine_hook_token_t, std::shared_ptr<HookEntry>> hooks_by_token;
    std::unordered_map<OverlayKey, std::vector<uint8_t>, OverlayKeyHash> overlays;
    std::unordered_map<std::string, std::vector<std::shared_ptr<ClockHookEntry>>> clock_hooks;
    std::unordered_map<sao_engine_hook_token_t, std::shared_ptr<ClockHookEntry>>
        clock_hooks_by_token;
    std::mutex clock_mutex;
    bool clock_initialized = false;
    uint64_t last_clock_time_ns = 0;
    uint32_t clock_frame_index = 0;
    uint32_t clock_frame_delta_us = 0;
    uint64_t global_redraw_generation = 0;
    std::unordered_map<std::string, uint64_t> redraw_generations;
    std::unordered_map<std::string, SurfaceClockState> surface_clocks;
    std::atomic<sao_engine_hook_token_t> next_token{1};
    std::atomic<uint64_t> next_insertion_sequence{1};
};

namespace {

bool valid_clock_point(int32_t hook_point) noexcept {
    return hook_point >= SAO_ENGINE_RENDER_BEFORE_COMPOSITOR &&
           hook_point <= SAO_ENGINE_RENDER_AFTER_PRESENT;
}

bool valid_dispatch_flags(uint32_t flags) noexcept {
    constexpr uint32_t kSources = SAO_ENGINE_RENDER_DISPATCH_LOGICAL_TICK |
                                  SAO_ENGINE_RENDER_DISPATCH_COMPOSITOR_PRESENT |
                                  SAO_ENGINE_RENDER_DISPATCH_GPU_PRESENT;
    const uint32_t source = flags & kSources;
    return (flags & ~kSources) == 0 && source != 0 && (source & (source - 1u)) == 0;
}

std::vector<std::shared_ptr<ClockHookEntry>>
snapshot_clock_hooks(sao_engine_render_hook_registry_handle_t handle, const std::string& surface_id,
                     int32_t hook_point) {
    std::vector<std::shared_ptr<ClockHookEntry>> result;
    std::shared_lock lock(handle->mutex);
    const auto append = [&](const std::string& key) {
        const auto found = handle->clock_hooks.find(key);
        if (found == handle->clock_hooks.end()) {
            return;
        }
        for (const auto& hook : found->second) {
            if (hook->hook_point == hook_point && hook->active.load(std::memory_order_acquire)) {
                result.push_back(hook);
            }
        }
    };
    append(surface_id);
    if (surface_id != SAO_ENGINE_ALL_SURFACES) {
        append(SAO_ENGINE_ALL_SURFACES);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs->priority != rhs->priority) {
            return lhs->priority > rhs->priority;
        }
        return lhs->insertion_sequence < rhs->insertion_sequence;
    });
    return result;
}

std::vector<std::shared_ptr<HookEntry>>
snapshot_hooks(sao_engine_render_hook_registry_handle_t handle, const std::string& surface_id) {
    std::vector<std::shared_ptr<HookEntry>> result;
    std::shared_lock lock(handle->mutex);
    const auto append = [&](const std::string& key) {
        const auto found = handle->hooks.find(key);
        if (found == handle->hooks.end()) {
            return;
        }
        for (const auto& hook : found->second) {
            if (hook->active.load(std::memory_order_acquire)) {
                result.push_back(hook);
            }
        }
    };
    append(surface_id);
    if (surface_id != SAO_ENGINE_ALL_SURFACES) {
        append(SAO_ENGINE_ALL_SURFACES);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs->priority != rhs->priority) {
            return lhs->priority > rhs->priority;
        }
        return lhs->insertion_sequence < rhs->insertion_sequence;
    });
    return result;
}

sao_status_t invoke_hook(const HookEntry& hook, const char* surface_id,
                         const std::vector<uint8_t>& input, std::vector<uint8_t>* output) {
    size_t capacity = std::max(kInitialCallbackCapacity, input.size() + 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<uint8_t> candidate(capacity);
        size_t written = 0;
        sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
        try {
            status = hook.callback(surface_id, input.data(), input.size(), candidate.data(),
                                   candidate.size(), &written, hook.user_data);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL && written > capacity) {
            capacity = written;
            continue;
        }
        if (status != SAO_STATUS_OK) {
            return status;
        }
        if (written > candidate.size()) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        if (written != 0) {
            candidate.resize(written);
            *output = std::move(candidate);
        }
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
}

} // namespace

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_render_hook_registry_create(sao_engine_render_hook_registry_handle_t* out_handle) {
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        *out_handle = new sao_engine_render_hook_registry_s();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_ENGINE_CALL
sao_engine_render_hook_registry_destroy(sao_engine_render_hook_registry_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_render_hook_provider_status(sao_engine_render_hook_registry_handle_t handle) {
    return handle != nullptr ? SAO_STATUS_ERR_CAPABILITY_MISSING : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_register(
    sao_engine_render_hook_registry_handle_t handle, const char* plugin_id, const char* surface_id,
    float priority, sao_engine_hook_callback_t callback, void* user_data,
    sao_engine_hook_token_t* out_token) {
    if (out_token != nullptr) {
        *out_token = 0;
    }
    if (handle == nullptr || !valid_string(plugin_id) || !valid_string(surface_id) ||
        !std::isfinite(priority) || callback == nullptr || out_token == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto hook = std::make_shared<HookEntry>();
        hook->token = handle->next_token.fetch_add(1, std::memory_order_relaxed);
        hook->plugin_id = plugin_id;
        hook->surface_id = surface_id;
        hook->priority = priority;
        hook->insertion_sequence =
            handle->next_insertion_sequence.fetch_add(1, std::memory_order_relaxed);
        hook->callback = callback;
        hook->user_data = user_data;
        {
            std::unique_lock lock(handle->mutex);
            handle->hooks[hook->surface_id].push_back(hook);
            handle->hooks_by_token.emplace(hook->token, hook);
        }
        *out_token = hook->token;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_unregister(
    sao_engine_render_hook_registry_handle_t handle, sao_engine_hook_token_t token) {
    if (handle == nullptr || token == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_lock lock(handle->mutex);
        const auto found = handle->hooks_by_token.find(token);
        if (found == handle->hooks_by_token.end()) {
            return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
        }
        const auto hook = found->second;
        hook->active.store(false, std::memory_order_release);
        handle->hooks_by_token.erase(found);
        const auto bucket = handle->hooks.find(hook->surface_id);
        if (bucket != handle->hooks.end()) {
            auto& hooks = bucket->second;
            hooks.erase(std::remove_if(
                            hooks.begin(), hooks.end(),
                            [token](const auto& candidate) { return candidate->token == token; }),
                        hooks.end());
            if (hooks.empty()) {
                handle->hooks.erase(bucket);
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_clock_register(
    sao_engine_render_hook_registry_handle_t handle, const char* plugin_id, const char* surface_id,
    int32_t hook_point, float priority, sao_engine_render_clock_callback_t callback,
    void* user_data, sao_engine_render_clock_user_data_release_t release_user_data,
    sao_engine_hook_token_t* out_token) {
    if (out_token != nullptr) {
        *out_token = 0;
    }
    if (handle == nullptr || !valid_string(plugin_id) || !valid_string(surface_id) ||
        !valid_clock_point(hook_point) || !std::isfinite(priority) || callback == nullptr ||
        out_token == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto hook = std::make_shared<ClockHookEntry>();
        hook->token = handle->next_token.fetch_add(1, std::memory_order_relaxed);
        hook->plugin_id = plugin_id;
        hook->surface_id = surface_id;
        hook->hook_point = hook_point;
        hook->priority = priority;
        hook->insertion_sequence =
            handle->next_insertion_sequence.fetch_add(1, std::memory_order_relaxed);
        hook->callback = callback;
        hook->user_data = user_data;
        {
            std::unique_lock lock(handle->mutex);
            auto [bucket, bucket_inserted] = handle->clock_hooks.try_emplace(hook->surface_id);
            try {
                bucket->second.push_back(hook);
                const auto [unused, token_inserted] =
                    handle->clock_hooks_by_token.emplace(hook->token, hook);
                (void)unused;
                if (!token_inserted) {
                    bucket->second.pop_back();
                    if (bucket_inserted) {
                        handle->clock_hooks.erase(bucket);
                    }
                    return SAO_STATUS_ERR_ALREADY_EXISTS;
                }
            } catch (...) {
                if (!bucket->second.empty() && bucket->second.back() == hook) {
                    bucket->second.pop_back();
                }
                if (bucket_inserted && bucket->second.empty()) {
                    handle->clock_hooks.erase(bucket);
                }
                throw;
            }
        }
        hook->release_user_data = release_user_data;
        *out_token = hook->token;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_clock_unregister(
    sao_engine_render_hook_registry_handle_t handle, sao_engine_hook_token_t token) {
    if (handle == nullptr || token == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_lock lock(handle->mutex);
        const auto found = handle->clock_hooks_by_token.find(token);
        if (found == handle->clock_hooks_by_token.end()) {
            return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
        }
        const auto hook = found->second;
        hook->active.store(false, std::memory_order_release);
        handle->clock_hooks_by_token.erase(found);
        const auto bucket = handle->clock_hooks.find(hook->surface_id);
        if (bucket != handle->clock_hooks.end()) {
            auto& hooks = bucket->second;
            std::erase_if(hooks,
                          [token](const auto& candidate) { return candidate->token == token; });
            if (hooks.empty()) {
                handle->clock_hooks.erase(bucket);
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_clock_request_redraw(
    sao_engine_render_hook_registry_handle_t handle, const char* surface_id) {
    if (handle == nullptr || !valid_string(surface_id)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(handle->clock_mutex);
        if (std::strcmp(surface_id, SAO_ENGINE_ALL_SURFACES) == 0) {
            ++handle->global_redraw_generation;
        } else {
            ++handle->redraw_generations[surface_id];
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_clock_dispatch(
    sao_engine_render_hook_registry_handle_t handle, const char* surface_id, int32_t hook_point,
    uint64_t monotonic_time_ns, int32_t viewport_x_px, int32_t viewport_y_px,
    int32_t viewport_width_px, int32_t viewport_height_px, uint32_t dispatch_flags) {
    if (handle == nullptr || !valid_string(surface_id) || !valid_clock_point(hook_point) ||
        !valid_dispatch_flags(dispatch_flags) || viewport_width_px < 0 || viewport_height_px < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if ((dispatch_flags & SAO_ENGINE_RENDER_DISPATCH_GPU_PRESENT) != 0) {
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }
    SaoEngineRenderClockPayload payload{};
    try {
        {
            std::lock_guard lock(handle->clock_mutex);
            if (handle->clock_initialized && monotonic_time_ns < handle->last_clock_time_ns) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if (!handle->clock_initialized || monotonic_time_ns > handle->last_clock_time_ns) {
                const uint64_t delta_ns =
                    handle->clock_initialized ? monotonic_time_ns - handle->last_clock_time_ns : 0;
                handle->clock_initialized = true;
                handle->last_clock_time_ns = monotonic_time_ns;
                ++handle->clock_frame_index;
                if (handle->clock_frame_index == 0) {
                    handle->clock_frame_index = 1;
                }
                handle->clock_frame_delta_us = static_cast<uint32_t>(
                    std::min<uint64_t>(delta_ns / 1000u, std::numeric_limits<uint32_t>::max()));
            }

            auto& surface_clock = handle->surface_clocks[surface_id];
            const uint64_t surface_generation = handle->redraw_generations[surface_id];
            const bool redraw_changed =
                surface_clock.seen_global_redraw_generation != handle->global_redraw_generation ||
                surface_clock.seen_surface_redraw_generation != surface_generation;
            if (surface_clock.frame_index != handle->clock_frame_index) {
                surface_clock.redraw_requested = redraw_changed;
                surface_clock.frame_index = handle->clock_frame_index;
            } else if (redraw_changed) {
                surface_clock.redraw_requested = true;
            }
            surface_clock.seen_global_redraw_generation = handle->global_redraw_generation;
            surface_clock.seen_surface_redraw_generation = surface_generation;

            payload.frame_time_us = static_cast<int64_t>(
                std::min<uint64_t>(monotonic_time_ns / 1000u,
                                   static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
            payload.frame_index = handle->clock_frame_index;
            payload.frame_delta_us = handle->clock_frame_delta_us;
            payload.viewport_x_px = viewport_x_px;
            payload.viewport_y_px = viewport_y_px;
            payload.viewport_width_px = viewport_width_px;
            payload.viewport_height_px = viewport_height_px;
            payload.flags = dispatch_flags;
            if (surface_clock.redraw_requested) {
                payload.flags |= SAO_ENGINE_RENDER_DISPATCH_REDRAW_REQUESTED;
            }
        }

        const auto hooks = snapshot_clock_hooks(handle, surface_id, hook_point);
        sao_status_t first_error = SAO_STATUS_OK;
        for (const auto& hook : hooks) {
            if (!hook->active.load(std::memory_order_acquire)) {
                continue;
            }
            sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
            try {
                status = hook->callback(hook_point, &payload, hook->user_data);
            } catch (...) {
                status = SAO_STATUS_ERR_UNKNOWN;
            }
            if (first_error == SAO_STATUS_OK && status != SAO_STATUS_OK) {
                first_error = status;
            }
        }
        return first_error;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_set_overlay(
    sao_engine_render_hook_registry_handle_t handle, const char* plugin_id, const char* surface_id,
    const uint8_t* spec, size_t spec_len) {
    if (handle == nullptr || !valid_string(plugin_id) || !valid_string(surface_id) ||
        (spec_len != 0 && spec == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        size_t normalized_size = 0;
        sao_status_t status =
            sao_engine_ui_spec_normalize(spec, spec_len, nullptr, 0, &normalized_size);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        std::vector<uint8_t> owned_spec(normalized_size);
        status = sao_engine_ui_spec_normalize(spec, spec_len, owned_spec.data(), owned_spec.size(),
                                              &normalized_size);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        std::unique_lock lock(handle->mutex);
        handle->overlays[OverlayKey{plugin_id, surface_id}] = std::move(owned_spec);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_render_hook_clear_overlay(sao_engine_render_hook_registry_handle_t handle,
                                     const char* plugin_id, const char* surface_id) {
    if (handle == nullptr || !valid_string(plugin_id)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_lock lock(handle->mutex);
        if (surface_id != nullptr) {
            if (surface_id[0] == '\0') {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            handle->overlays.erase(OverlayKey{plugin_id, surface_id});
            return SAO_STATUS_OK;
        }
        size_t removed = 0;
        for (auto entry = handle->overlays.begin(); entry != handle->overlays.end();) {
            if (entry->first.plugin_id == plugin_id) {
                entry = handle->overlays.erase(entry);
                ++removed;
            } else {
                ++entry;
            }
        }
        (void)removed;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_render_hook_dispatch(
    sao_engine_render_hook_registry_handle_t handle, const char* surface_id, const uint8_t* input,
    size_t input_len, uint8_t* out_json, size_t out_capacity, size_t* out_written) {
    if (out_written != nullptr) {
        *out_written = 0;
    }
    if (handle == nullptr || !valid_string(surface_id) || (input_len != 0 && input == nullptr) ||
        out_written == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<uint8_t> current;
        if (input_len != 0) {
            current.assign(input, input + input_len);
        }
        auto hooks = snapshot_hooks(handle, surface_id);
        for (const auto& hook : hooks) {
            if (!hook->active.load(std::memory_order_acquire)) {
                continue;
            }
            std::vector<uint8_t> replacement;
            const sao_status_t status = invoke_hook(*hook, surface_id, current, &replacement);
            if (status == SAO_STATUS_OK && !replacement.empty()) {
                current = std::move(replacement);
            }
        }
        *out_written = current.size();
        if (current.size() > out_capacity || (!current.empty() && out_json == nullptr)) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        if (!current.empty()) {
            std::memcpy(out_json, current.data(), current.size());
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
