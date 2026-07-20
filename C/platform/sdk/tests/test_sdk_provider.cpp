#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

extern "C" SAO_SDK_API void* SAO_SDK_CALL sao_sdk_test_event_snapshot_user_data(
    const SaoSdkContext* ctx, sao_sdk_subscription_t subscription);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_invoke_event_snapshot(void* snapshot_user_data);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_release_event_snapshot(void* snapshot_user_data);
extern "C" SAO_SDK_API void* SAO_SDK_CALL sao_sdk_test_platform_hotkey_snapshot_user_data(
    const SaoSdkContext* ctx, sao_sdk_hotkey_id_t hotkey);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_invoke_platform_hotkey_snapshot(void* snapshot_user_data);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_release_platform_hotkey_snapshot(void* snapshot_user_data);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_arm_context_api_pause(uint32_t point);
extern "C" SAO_SDK_API bool SAO_SDK_CALL sao_sdk_test_wait_for_context_api_pause(uint32_t point);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_resume_context_api_pause(uint32_t point);
extern "C" SAO_SDK_API bool SAO_SDK_CALL
sao_sdk_test_wait_for_context_shutdown(const SaoSdkContext* ctx);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_render_state_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_hotkey_state_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_notify_state_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_overlay_state_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_timer_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_hotkey_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_dialog_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_overlay_insertion(void);

namespace {

constexpr uint32_t kEventSubscribeRegistered = 1;
constexpr uint32_t kEventUnsubscribeUnlocked = 2;
constexpr uint32_t kPanelRegistered = 3;
constexpr uint32_t kPanelOperationUnlocked = 4;
constexpr uint32_t kGpuRuntimeTogglesLeased = 5;

struct CallbackProbe {
    sao_sdk_timer_token_t timer = 0;
    sao_sdk_hotkey_id_t hotkey = 0;
    sao_sdk_dialog_token_t dialog = 0;
    int32_t dialog_button = -1;
    std::string dialog_input;
    int render_calls = 0;
};

struct ProviderFixture {
    uint64_t next_token = 101;
    std::vector<std::string> events;
    std::string last_plugin_id;
    sao_sdk_timer_callback_t timer_callback = nullptr;
    void* timer_user_data = nullptr;
    sao_sdk_hotkey_callback_t hotkey_callback = nullptr;
    void* hotkey_user_data = nullptr;
    sao_sdk_dialog_callback_t dialog_callback = nullptr;
    void* dialog_user_data = nullptr;
    sao_sdk_render_hook_callback_t render_callback = nullptr;
    void* render_user_data = nullptr;
    size_t live_gpu_sessions = 0;
    bool fail_timer_unregister = false;
    bool fire_timer_synchronously = false;
    sao_sdk_status_t gpu_close_status = SAO_SDK_OK;
    sao_sdk_status_t gpu_detach_status = SAO_SDK_OK;
    bool throw_gpu_enum = false;
    bool throw_retain = false;
    bool throw_release = false;
    bool throw_tts = false;
    bool block_release = false;
    bool release_entered = false;
    bool allow_release = false;
    std::mutex release_mutex;
    std::condition_variable release_condition;
    SaoSdkContext* configure_reentry_context = nullptr;
    const SaoSdkMemoryProviderVTable* configure_memory_replacement = nullptr;
    const SaoSdkNetProviderVTable* configure_net_replacement = nullptr;
    sao_sdk_status_t configure_memory_status = SAO_SDK_OK;
    sao_sdk_status_t configure_net_status = SAO_SDK_OK;
    SaoSdkContext* gpu_reentry_context = nullptr;
    sao_sdk_gpu_tracker_t gpu_reentry_tracker = 0;
    sao_sdk_status_t gpu_destroy_status = SAO_SDK_OK;
    std::array<sao_sdk_status_t, 24> gpu_tracker_reentry_statuses{};
    size_t gpu_tracker_reentry_call_count = 0;

    uint64_t allocate(const char* kind) {
        const uint64_t token = next_token++;
        events.push_back(std::string("register:") + kind + ":" + std::to_string(token));
        return token;
    }

    void unregister(const char* kind, uint64_t token) {
        events.push_back(std::string("unregister:") + kind + ":" + std::to_string(token));
    }
};

struct GpuSessionFixture {
    ProviderFixture* owner = nullptr;
    uint32_t pid = 0;
};

void record_gpu_tracker_reentry(GpuSessionFixture* session) {
    auto* state = session->owner;
    auto* context = state->gpu_reentry_context;
    const auto tracker = state->gpu_reentry_tracker;
    if (context == nullptr || tracker == 0)
        return;

    const auto* gpu = context->gpu_hunt;
    auto* ctx_impl = context->ctx_impl;
    sao_sdk_gpu_tracker_t nested_tracker = 0;
    float matrix[16]{};
    float position[3]{};
    float screen_x = 0.0F;
    float screen_y = 0.0F;
    uint8_t flag = 0;
    size_t count = 0;
    uint64_t heap_base = 0;
    uint64_t heap_size = 0;
    uint64_t toggle_mask = 0;
    uint32_t offset = 0;
    uint32_t profile = 0;

    state->gpu_tracker_reentry_statuses = {
        gpu->create_tracker(ctx_impl, &nested_tracker),
        gpu->destroy_tracker(ctx_impl, tracker),
        gpu->tick(ctx_impl, tracker),
        gpu->get_view_proj(ctx_impl, tracker, matrix, &flag),
        gpu->get_camera_pos(ctx_impl, tracker, position),
        gpu->get_skeleton_positions(ctx_impl, tracker, nullptr, 0, &count),
        gpu->world_to_screen(ctx_impl, tracker, position, 640, 480, &screen_x, &screen_y, &flag),
        gpu->invalidate(ctx_impl, tracker),
        gpu->attach_tracker(ctx_impl, tracker, 6062),
        gpu->detach_tracker(ctx_impl, tracker),
        gpu->get_matrix_lock_info(ctx_impl, tracker, &heap_base, &offset, &heap_size, &flag),
        gpu->set_prior_lock_hint(ctx_impl, tracker, 4096, 64),
        gpu->get_skeleton_fingerprint(ctx_impl, tracker, nullptr, 0, &count),
        gpu->set_skeleton_fingerprint_hint(ctx_impl, tracker, nullptr, 0),
        gpu->get_bone_cluster_count(ctx_impl, tracker, &count),
        gpu->get_bone_cluster_positions(ctx_impl, tracker, 0, nullptr, 0, &count),
        gpu->get_prior_hot_heaps(ctx_impl, tracker, nullptr, 0, &count),
        gpu->set_prior_hot_heaps(ctx_impl, tracker, nullptr, 0),
        gpu->set_pinned_heap(ctx_impl, tracker, 0x1000, 0x2000, 1),
        gpu->get_pinned_heap(ctx_impl, tracker, &heap_base, &heap_size, &flag, &flag),
        gpu->get_runtime_toggles(ctx_impl, tracker, &toggle_mask),
        gpu->set_runtime_toggles(ctx_impl, tracker, 0, UINT64_MAX),
        gpu->get_locator_profile(ctx_impl, tracker, &profile),
        gpu->set_locator_profile(ctx_impl, tracker, SAO_SDK_GPU_HUNT_LOCATOR_PROFILE_D3D12_UPLOAD),
    };
    state->gpu_tracker_reentry_call_count = state->gpu_tracker_reentry_statuses.size();
}

struct PlatformGpuHuntProviderReset {
    ~PlatformGpuHuntProviderReset() {
        (void)sao_sdk_platform_gpu_hunt_configure_provider(nullptr);
    }
};

ProviderFixture* fixture(void* user_data) {
    return static_cast<ProviderFixture*>(user_data);
}

void SAO_SDK_CALL provider_retain(void* user_data) {
    auto* state = fixture(user_data);
    if (state->throw_retain)
        throw std::runtime_error("provider retain fixture");
    state->events.emplace_back("retain");
}

void SAO_SDK_CALL provider_release(void* user_data) {
    auto* state = fixture(user_data);
    state->events.emplace_back("release");
    {
        std::unique_lock<std::mutex> lock(state->release_mutex);
        if (state->block_release) {
            state->release_entered = true;
            state->release_condition.notify_all();
            state->release_condition.wait(lock, [state] { return state->allow_release; });
        }
    }
    if (state->configure_reentry_context != nullptr) {
        state->configure_memory_status = sao_sdk_context_configure_memory_provider(
            state->configure_reentry_context, state->configure_memory_replacement);
        state->configure_net_status = sao_sdk_context_configure_net_provider(
            state->configure_reentry_context, state->configure_net_replacement);
    }
    if (state->throw_release)
        throw std::runtime_error("provider release fixture");
}

sao_sdk_status_t SAO_SDK_CALL provider_tts_speak(void* user_data, const char* text_utf8, float,
                                                 float) {
    auto* state = fixture(user_data);
    if (state->throw_tts)
        throw std::runtime_error("provider TTS fixture");
    state->events.push_back(std::string("tts:speak:") + (text_utf8 == nullptr ? "" : text_utf8));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_tts_stop(void* user_data) {
    fixture(user_data)->events.emplace_back("tts:stop");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_register_render(void* user_data, const char* plugin_id_utf8,
                                                       int32_t,
                                                       sao_sdk_render_hook_callback_t callback,
                                                       void* callback_user_data,
                                                       uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    state->render_callback = callback;
    state->render_user_data = callback_user_data;
    *out_provider_token = state->allocate("render");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_unregister_render(void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("render", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_register_timer(void* user_data, uint32_t,
                                                      sao_sdk_timer_callback_t callback,
                                                      void* callback_user_data,
                                                      uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->timer_callback = callback;
    state->timer_user_data = callback_user_data;
    *out_provider_token = state->allocate("timer");
    if (state->fire_timer_synchronously)
        callback(*out_provider_token, callback_user_data);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_unregister_timer(void* user_data, uint64_t provider_token) {
    auto* state = fixture(user_data);
    state->unregister("timer", provider_token);
    return state->fail_timer_unregister ? SAO_SDK_ERR_INTERNAL : SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_register_hotkey(void* user_data, const char* plugin_id_utf8,
                                                       const char*, uint32_t, uint32_t,
                                                       sao_sdk_hotkey_callback_t callback,
                                                       void* callback_user_data,
                                                       uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    state->hotkey_callback = callback;
    state->hotkey_user_data = callback_user_data;
    *out_provider_token = state->allocate("hotkey");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_unregister_hotkey(void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("hotkey", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_show_dialog(void* user_data, const char* plugin_id_utf8,
                                                   const SaoSdkDialogSpec*,
                                                   sao_sdk_dialog_callback_t callback,
                                                   void* callback_user_data,
                                                   uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    state->dialog_callback = callback;
    state->dialog_user_data = callback_user_data;
    *out_provider_token = state->allocate("dialog");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_dismiss_dialog(void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("dialog", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_show_notify(void* user_data, const char* plugin_id_utf8,
                                                   const SaoSdkNotifySpec*,
                                                   uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    *out_provider_token = state->allocate("notify");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_dismiss_notify(void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("notify", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_set_overlay(void* user_data, const char* plugin_id_utf8,
                                                   const SaoSdkOverlaySpec*,
                                                   uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    *out_provider_token = state->allocate("overlay");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_clear_overlay(void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("overlay", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_open(void* user_data, const char* plugin_id_utf8,
                                                void** out_session) {
    if (out_session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    auto* state = fixture(user_data);
    auto* session = new GpuSessionFixture{state, 0};
    ++state->live_gpu_sessions;
    state->events.push_back(std::string("gpu:open:") + plugin_id_utf8);
    *out_session = session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_close(void*, void* session_value) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->owner->events.emplace_back("gpu:close");
    if (session->owner->gpu_close_status != SAO_SDK_OK)
        return session->owner->gpu_close_status;
    --session->owner->live_gpu_sessions;
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_open_failed_with_session(void* user_data,
                                                                    const char* plugin_id_utf8,
                                                                    void** out_session) {
    const auto status = provider_gpu_open(user_data, plugin_id_utf8, out_session);
    return status == SAO_SDK_OK ? SAO_SDK_ERR_NOT_INITIALIZED : status;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_close_then_throw(void* user_data, void* session_value) {
    (void)provider_gpu_close(user_data, session_value);
    throw std::runtime_error("GPU close fixture failure");
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_attach(void*, void* session_value, uint32_t pid) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr || pid == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->pid = pid;
    session->owner->events.push_back("gpu:attach:" + std::to_string(pid));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_detach(void*, void* session_value) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->owner->events.emplace_back("gpu:detach");
    if (session->owner->gpu_detach_status != SAO_SDK_OK)
        return session->owner->gpu_detach_status;
    session->pid = 0;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_enum_regions(void*, void* session_value,
                                                        SaoSdkGpuHuntRegion* out_regions,
                                                        size_t capacity, size_t* out_count) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr || out_count == nullptr || session->pid == 0) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    if (session->owner->throw_gpu_enum)
        throw std::runtime_error("GPU enumerate fixture failure");
    if (session->owner->gpu_reentry_context != nullptr) {
        session->owner->gpu_destroy_status =
            sao_sdk_context_try_destroy(session->owner->gpu_reentry_context);
        record_gpu_tracker_reentry(session);
    }
    session->owner->events.emplace_back(out_regions == nullptr ? "gpu:enum:size" : "gpu:enum:fill");
    *out_count = 0;
    (void)capacity;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_read(void*, void* session_value, uint64_t,
                                                uint8_t* out_buffer, size_t buffer_size,
                                                size_t* out_bytes_read) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr || out_bytes_read == nullptr || session->pid == 0 ||
        (buffer_size != 0 && out_buffer == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    session->owner->events.emplace_back("gpu:read");
    for (size_t index = 0; index < buffer_size; ++index) {
        out_buffer[index] = 0;
    }
    *out_bytes_read = buffer_size;
    return SAO_SDK_OK;
}

SaoSdkProviderVTable make_provider(ProviderFixture* fixture_state) {
    SaoSdkProviderVTable provider{};
    provider.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = fixture_state;
    provider.retain = provider_retain;
    provider.release = provider_release;
    provider.tts_speak = provider_tts_speak;
    provider.tts_stop = provider_tts_stop;
    provider.register_render_hook = provider_register_render;
    provider.unregister_render_hook = provider_unregister_render;
    provider.register_timer = provider_register_timer;
    provider.unregister_timer = provider_unregister_timer;
    provider.register_hotkey = provider_register_hotkey;
    provider.unregister_hotkey = provider_unregister_hotkey;
    provider.show_dialog = provider_show_dialog;
    provider.dismiss_dialog = provider_dismiss_dialog;
    provider.show_notify = provider_show_notify;
    provider.dismiss_notify = provider_dismiss_notify;
    provider.set_overlay = provider_set_overlay;
    provider.clear_overlay = provider_clear_overlay;
    provider.gpu_hunt_open_session = provider_gpu_open;
    provider.gpu_hunt_close_session = provider_gpu_close;
    provider.gpu_hunt_attach = provider_gpu_attach;
    provider.gpu_hunt_detach = provider_gpu_detach;
    provider.gpu_hunt_enum_regions = provider_gpu_enum_regions;
    provider.gpu_hunt_read = provider_gpu_read;
    return provider;
}

struct ProcessMemoryFixture;

struct ProcessMemorySession {
    ProcessMemoryFixture* owner = nullptr;
    bool attached = false;
};

struct ProcessMemoryFixture {
    size_t retain_count = 0;
    size_t release_count = 0;
    size_t open_count = 0;
    size_t close_count = 0;
    size_t live_sessions = 0;
    sao_sdk_status_t open_status = SAO_SDK_OK;
    sao_sdk_status_t close_status = SAO_SDK_OK;
    const SaoSdkMemoryProviderVTable* replacement_on_open = nullptr;
    sao_sdk_status_t replacement_status = SAO_SDK_OK;
};

void SAO_SDK_CALL process_memory_retain(void* user_data) {
    ++static_cast<ProcessMemoryFixture*>(user_data)->retain_count;
}

void SAO_SDK_CALL process_memory_release(void* user_data) {
    ++static_cast<ProcessMemoryFixture*>(user_data)->release_count;
}

sao_sdk_status_t SAO_SDK_CALL process_memory_open(void* user_data, const char*,
                                                   void** out_session) {
    if (out_session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    auto* state = static_cast<ProcessMemoryFixture*>(user_data);
    ++state->open_count;
    if (state->replacement_on_open != nullptr) {
        const auto* replacement = std::exchange(state->replacement_on_open, nullptr);
        state->replacement_status = sao_sdk_platform_memory_configure_provider(replacement);
    }
    if (state->open_status != SAO_SDK_OK)
        return state->open_status;
    *out_session = new ProcessMemorySession{state};
    ++state->live_sessions;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_memory_close(void*, void* session_value) {
    auto* session = static_cast<ProcessMemorySession*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    ++session->owner->close_count;
    if (session->owner->close_status != SAO_SDK_OK)
        return session->owner->close_status;
    --session->owner->live_sessions;
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_memory_attach(
    void*, void* session_value, const SaoSdkMemoryTargetIdentity*) {
    auto* session = static_cast<ProcessMemorySession*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->attached = true;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_memory_detach(void*, void* session_value) {
    auto* session = static_cast<ProcessMemorySession*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->attached = false;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_memory_read(void*, void*, uint64_t, void*, size_t,
                                                   size_t* out_bytes_read) {
    if (out_bytes_read != nullptr)
        *out_bytes_read = 0;
    return SAO_SDK_ERR_READ_FAULT;
}

sao_sdk_status_t SAO_SDK_CALL process_memory_enumerate(void*, void*, SaoSdkMemoryModule*,
                                                        size_t, size_t, size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    return SAO_SDK_OK;
}

SaoSdkMemoryProviderVTable make_process_memory_provider(ProcessMemoryFixture* state) {
    SaoSdkMemoryProviderVTable provider{};
    provider.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = state;
    provider.retain = process_memory_retain;
    provider.release = process_memory_release;
    provider.open_session = process_memory_open;
    provider.close_session = process_memory_close;
    provider.attach = process_memory_attach;
    provider.detach = process_memory_detach;
    provider.read = process_memory_read;
    provider.enumerate_modules = process_memory_enumerate;
    return provider;
}

struct ProcessNetFixture;

struct ProcessNetSession {
    ProcessNetFixture* owner = nullptr;
};

struct ProcessNetFixture {
    size_t retain_count = 0;
    size_t release_count = 0;
    size_t open_count = 0;
    size_t close_count = 0;
    size_t live_sessions = 0;
    sao_sdk_status_t open_status = SAO_SDK_OK;
    sao_sdk_status_t close_status = SAO_SDK_OK;
};

void SAO_SDK_CALL process_net_retain(void* user_data) {
    ++static_cast<ProcessNetFixture*>(user_data)->retain_count;
}

void SAO_SDK_CALL process_net_release(void* user_data) {
    ++static_cast<ProcessNetFixture*>(user_data)->release_count;
}

sao_sdk_status_t SAO_SDK_CALL process_net_open(void* user_data, const char*, void** out_session) {
    if (out_session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    auto* state = static_cast<ProcessNetFixture*>(user_data);
    ++state->open_count;
    if (state->open_status != SAO_SDK_OK)
        return state->open_status;
    *out_session = new ProcessNetSession{state};
    ++state->live_sessions;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_net_close(void*, void* session_value) {
    auto* session = static_cast<ProcessNetSession*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    ++session->owner->close_count;
    if (session->owner->close_status != SAO_SDK_OK)
        return session->owner->close_status;
    --session->owner->live_sessions;
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_net_capture_start(
    void*, void*, const SaoSdkNetCaptureConfig*, sao_sdk_net_packet_callback_t, void*) {
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_net_capture_stop(void*, void*) {
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL process_net_parse(void*, void*, const SaoSdkNetPacketView*,
                                                 SaoSdkNetParsedResult*, size_t, size_t,
                                                 size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    return SAO_SDK_OK;
}

SaoSdkNetProviderVTable make_process_net_provider(ProcessNetFixture* state) {
    SaoSdkNetProviderVTable provider{};
    provider.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = state;
    provider.retain = process_net_retain;
    provider.release = process_net_release;
    provider.open_session = process_net_open;
    provider.close_session = process_net_close;
    provider.capture_start = process_net_capture_start;
    provider.capture_stop = process_net_capture_stop;
    provider.parse_packet = process_net_parse;
    return provider;
}

struct ProcessProviderReset {
    ProcessProviderReset() {
        (void)sao_sdk_platform_memory_configure_provider(nullptr);
        (void)sao_sdk_platform_net_configure_provider(nullptr);
    }

    ~ProcessProviderReset() {
        (void)sao_sdk_platform_memory_configure_provider(nullptr);
        (void)sao_sdk_platform_net_configure_provider(nullptr);
    }
};

void SAO_SDK_CALL timer_probe(sao_sdk_timer_token_t timer, void* user_data) {
    static_cast<CallbackProbe*>(user_data)->timer = timer;
}

void SAO_SDK_CALL hotkey_probe(sao_sdk_hotkey_id_t hotkey, void* user_data) {
    static_cast<CallbackProbe*>(user_data)->hotkey = hotkey;
}

void SAO_SDK_CALL dialog_probe(sao_sdk_dialog_token_t dialog, int32_t button,
                               const char* input_text_utf8, size_t input_text_len,
                               void* user_data) {
    auto* probe = static_cast<CallbackProbe*>(user_data);
    probe->dialog = dialog;
    probe->dialog_button = button;
    probe->dialog_input.assign(input_text_utf8, input_text_len);
}

sao_sdk_status_t SAO_SDK_CALL render_probe(int32_t, const SaoSdkRenderHookPayload*,
                                           void* user_data) {
    ++static_cast<CallbackProbe*>(user_data)->render_calls;
    return SAO_SDK_OK;
}

struct ReentryProbe {
    SaoSdkContext* context = nullptr;
    sao_sdk_status_t destroy_status = SAO_SDK_OK;
    sao_sdk_status_t clear_status = SAO_SDK_OK;
    int callback_count = 0;
};

void SAO_SDK_CALL timer_reentry_probe(sao_sdk_timer_token_t, void* user_data) {
    auto* probe = static_cast<ReentryProbe*>(user_data);
    ++probe->callback_count;
    probe->destroy_status = sao_sdk_context_try_destroy(probe->context);
    probe->clear_status = sao_sdk_context_bind_provider(probe->context, nullptr);
}

void SAO_SDK_CALL throwing_timer_probe(sao_sdk_timer_token_t, void*) {
    throw std::runtime_error("timer callback fixture");
}

struct BlockingCallbackProbe {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    std::atomic_int calls{0};
};

void wait_in_callback(BlockingCallbackProbe* probe) {
    probe->calls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(probe->mutex);
    probe->entered = true;
    probe->condition.notify_all();
    probe->condition.wait(lock, [probe] { return probe->release; });
}

void SAO_SDK_CALL blocking_event_probe(const char*, const uint8_t*, size_t, void* user_data) {
    wait_in_callback(static_cast<BlockingCallbackProbe*>(user_data));
}

void SAO_SDK_CALL no_op_event_probe(const char*, const uint8_t*, size_t, void*) {}

void SAO_SDK_CALL blocking_hotkey_probe(sao_sdk_hotkey_id_t, void* user_data) {
    wait_in_callback(static_cast<BlockingCallbackProbe*>(user_data));
}

bool wait_for_callback(BlockingCallbackProbe& probe) {
    std::unique_lock<std::mutex> lock(probe.mutex);
    return probe.condition.wait_for(lock, std::chrono::seconds(2),
                                    [&probe] { return probe.entered; });
}

void release_callback(BlockingCallbackProbe& probe) {
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        probe.release = true;
    }
    probe.condition.notify_all();
}

struct EventReentryProbe {
    SaoSdkContext* context = nullptr;
    sao_sdk_subscription_t subscription = 0;
    sao_sdk_status_t status = SAO_SDK_ERR_INTERNAL;
    sao_sdk_status_t destroy_status = SAO_SDK_ERR_INTERNAL;
    int calls = 0;
};

struct VoidDestroyProbe {
    SaoSdkContext* context = nullptr;
    std::atomic_int calls{0};
};

void SAO_SDK_CALL void_destroy_event_probe(const char*, const uint8_t*, size_t, void* user_data) {
    auto* probe = static_cast<VoidDestroyProbe*>(user_data);
    ++probe->calls;
    sao_sdk_context_destroy(probe->context);
}

void SAO_SDK_CALL event_reentry_probe(const char*, const uint8_t*, size_t, void* user_data) {
    auto* probe = static_cast<EventReentryProbe*>(user_data);
    ++probe->calls;
    probe->destroy_status = sao_sdk_context_try_destroy(probe->context);
    probe->status = sao_sdk_unsubscribe_event(probe->context, probe->subscription);
}

struct HotkeyReentryProbe {
    SaoSdkContext* context = nullptr;
    sao_sdk_hotkey_id_t hotkey = 0;
    sao_sdk_status_t status = SAO_SDK_ERR_INTERNAL;
    int calls = 0;
};

void SAO_SDK_CALL hotkey_reentry_probe(sao_sdk_hotkey_id_t, void* user_data) {
    auto* probe = static_cast<HotkeyReentryProbe*>(user_data);
    ++probe->calls;
    probe->status = sao_sdk_unregister_hotkey(probe->context, probe->hotkey);
}

SaoSdkPanelDescriptor concurrent_panel_descriptor(const char* panel_id) {
    SaoSdkPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = panel_id;
    descriptor.title_utf8 = "SDK context lease concurrency";
    descriptor.default_width_px = 320;
    descriptor.default_height_px = 240;
    descriptor.min_width_px = 100;
    descriptor.min_height_px = 80;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = true;
    descriptor.initial_opacity = 1.0F;
    return descriptor;
}

} // namespace

TEST_CASE("SDK context ABI remains stable while provider is versioned", "[sdk][provider][abi]") {
    STATIC_REQUIRE(sizeof(SaoSdkContext) == (sizeof(void*) == 8u ? 104u : 56u));
    STATIC_REQUIRE(offsetof(SaoSdkContext, ctx_impl) == 8u);
    STATIC_REQUIRE(offsetof(SaoSdkContext, banner) == (sizeof(void*) == 8u ? 88u : 48u));
    STATIC_REQUIRE(offsetof(SaoSdkContext, gpu_hunt) == (sizeof(void*) == 8u ? 96u : 52u));

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.abi", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkProviderVTable bad{};
    bad.abi_version = 2u << 16;
    bad.struct_size = sizeof(bad);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &bad) == SAO_SDK_ERR_ABI_MISMATCH);

    bad.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
    constexpr uint32_t provider_minimum_size = static_cast<uint32_t>(
        offsetof(SaoSdkProviderVTable, release) +
        sizeof(static_cast<SaoSdkProviderVTable*>(nullptr)->release));
    bad.struct_size = provider_minimum_size - 1u;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &bad) == SAO_SDK_ERR_ABI_MISMATCH);

    ProviderFixture ownership_state;
    auto ownership = make_provider(&ownership_state);
    ownership.struct_size = provider_minimum_size;
    ownership.release = nullptr;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &ownership) == SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(ownership_state.events.empty());
    ownership.retain = nullptr;
    ownership.release = provider_release;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &ownership) == SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(ownership_state.events.empty());
    ownership.retain = provider_retain;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &ownership) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, nullptr) == SAO_SDK_OK);
    REQUIRE(ownership_state.events == std::vector<std::string>{"retain", "release"});

    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_probe, nullptr, &timer) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(sao_sdk_timer_unregister(&ctx, 1) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(ctx.tts->speak(ctx.ctx_impl, "text", 1.0f, 0.0f) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_ERR_UNSUPPORTED);

    sao_sdk_hook_token_t hook = 0;
    REQUIRE(sao_sdk_register_render_hook(&ctx, SAO_SDK_HOOK_BEFORE_PRESENT, render_probe, nullptr,
                                         &hook) == SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkHotkeySpec hotkey{};
    hotkey.binding_id_utf8 = "provider_test";
    hotkey.virtual_key = 0x41;
    sao_sdk_hotkey_id_t hotkey_id = 0;
    REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey, hotkey_probe, nullptr, &hotkey_id) ==
            SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkDialogSpec dialog{};
    dialog.title_utf8 = "title";
    dialog.message_utf8 = "message";
    sao_sdk_dialog_token_t dialog_token = 0;
    REQUIRE(sao_sdk_dialog_show(&ctx, &dialog, dialog_probe, nullptr, &dialog_token) ==
            SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkNotifySpec notify{"text", 10, 0xff00ff00u};
    sao_sdk_notify_token_t notify_token = 0;
    REQUIRE(sao_sdk_notify_show(&ctx, &notify, &notify_token) == SAO_SDK_ERR_UNSUPPORTED);

    const uint8_t json[] = {'{', '}'};
    SaoSdkOverlaySpec overlay{"surface", json, sizeof(json)};
    sao_sdk_overlay_token_t overlay_token = 0;
    REQUIRE(sao_sdk_overlay_set(&ctx, &overlay, &overlay_token) == SAO_SDK_ERR_UNSUPPORTED);

    ProviderFixture partial_state;
    auto partial = make_provider(&partial_state);
    partial.register_timer = nullptr;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &partial) == SAO_SDK_OK);
    REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_probe, nullptr, &timer) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_OK);

    sao_sdk_context_destroy(&ctx);
    REQUIRE(partial_state.events == std::vector<std::string>{"retain", "tts:stop", "release"});
}

TEST_CASE("provider translates callback tokens and tears down in reverse order",
          "[sdk][provider][lifecycle]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.lifecycle", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_provider_status(&ctx) == SAO_SDK_OK);

    CallbackProbe probe;
    REQUIRE(ctx.tts->speak(ctx.ctx_impl, "hello", 0.5f, 1.0f) == SAO_SDK_OK);
    REQUIRE(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_OK);

    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 25, timer_probe, &probe, &timer) == SAO_SDK_OK);

    SaoSdkHotkeySpec hotkey_spec{};
    hotkey_spec.binding_id_utf8 = "toggle";
    hotkey_spec.virtual_key = 0x54;
    sao_sdk_hotkey_id_t hotkey = 0;
    REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, &probe, &hotkey) ==
            SAO_SDK_OK);

    sao_sdk_hook_token_t render = 0;
    REQUIRE(sao_sdk_register_render_hook(&ctx, SAO_SDK_HOOK_BEFORE_PRESENT, render_probe, &probe,
                                         &render) == SAO_SDK_OK);

    SaoSdkDialogSpec dialog_spec{};
    dialog_spec.kind = SAO_SDK_DIALOG_INPUT;
    dialog_spec.title_utf8 = "title";
    dialog_spec.message_utf8 = "message";
    sao_sdk_dialog_token_t dialog = 0;
    REQUIRE(sao_sdk_dialog_show(&ctx, &dialog_spec, dialog_probe, &probe, &dialog) == SAO_SDK_OK);

    SaoSdkNotifySpec notify_spec{"notice", 50, 0xffffffffu};
    sao_sdk_notify_token_t notify = 0;
    REQUIRE(sao_sdk_notify_show(&ctx, &notify_spec, &notify) == SAO_SDK_OK);

    const uint8_t json[] = {'{', '}'};
    SaoSdkOverlaySpec overlay_spec{"hud", json, sizeof(json)};
    sao_sdk_overlay_token_t overlay = 0;
    REQUIRE(sao_sdk_overlay_set(&ctx, &overlay_spec, &overlay) == SAO_SDK_OK);

    REQUIRE(provider_state.last_plugin_id == "provider.lifecycle");
    REQUIRE(timer != 101u);
    REQUIRE(hotkey != 102u);
    REQUIRE(render != 103u);
    REQUIRE(dialog != 104u);
    REQUIRE(notify != 105u);
    REQUIRE(overlay != 106u);

    provider_state.timer_callback(999, provider_state.timer_user_data);
    provider_state.hotkey_callback(999, provider_state.hotkey_user_data);
    constexpr char input[] = "value";
    provider_state.dialog_callback(999, SAO_SDK_DIALOG_BUTTON_OK, input, sizeof(input) - 1,
                                   provider_state.dialog_user_data);
    SaoSdkRenderHookPayload payload{};
    REQUIRE(provider_state.render_callback(SAO_SDK_HOOK_BEFORE_PRESENT, &payload,
                                           provider_state.render_user_data) == SAO_SDK_OK);

    REQUIRE(probe.timer == timer);
    REQUIRE(probe.hotkey == hotkey);
    REQUIRE(probe.dialog == dialog);
    REQUIRE(probe.dialog_button == SAO_SDK_DIALOG_BUTTON_OK);
    REQUIRE(probe.dialog_input == "value");
    REQUIRE(probe.render_calls == 1);

    REQUIRE(sao_sdk_notify_dismiss(&ctx, notify) == SAO_SDK_OK);
    REQUIRE(sao_sdk_notify_dismiss(&ctx, notify) == SAO_SDK_ERR_NOT_FOUND);
    sao_sdk_context_destroy(&ctx);

    const std::vector<std::string> expected{
        "retain",
        "tts:speak:hello",
        "tts:stop",
        "register:timer:101",
        "register:hotkey:102",
        "register:render:103",
        "register:dialog:104",
        "register:notify:105",
        "register:overlay:106",
        "unregister:notify:105",
        "unregister:overlay:106",
        "unregister:dialog:104",
        "unregister:render:103",
        "unregister:hotkey:102",
        "unregister:timer:101",
        "release",
    };
    REQUIRE(provider_state.events == expected);
}

TEST_CASE("provider unregister failure preserves token and callback bridge for retry",
          "[sdk][provider][cleanup][retry]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.unregister.retry", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);
    CallbackProbe probe;
    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_probe, &probe, &timer) == SAO_SDK_OK);

    provider_state.fail_timer_unregister = true;
    CHECK(sao_sdk_timer_unregister(&ctx, timer) == SAO_SDK_ERR_INTERNAL);
    provider_state.timer_callback(777, provider_state.timer_user_data);
    CHECK(probe.timer == timer);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_OK);

    provider_state.fail_timer_unregister = false;
    REQUIRE(sao_sdk_timer_unregister(&ctx, timer) == SAO_SDK_OK);
    CHECK(sao_sdk_timer_unregister(&ctx, timer) == SAO_SDK_ERR_NOT_FOUND);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("provider replacement cleanup is transactional and synchronous callbacks reenter busy",
          "[sdk][provider][replacement][reentry]") {
    ProviderFixture original;
    original.fire_timer_synchronously = true;
    auto original_provider = make_provider(&original);
    ProviderFixture replacement;
    auto replacement_provider = make_provider(&replacement);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.replacement.retry", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &original_provider) == SAO_SDK_OK);

    ReentryProbe probe{&ctx};
    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_reentry_probe, &probe, &timer) == SAO_SDK_OK);
    CHECK(probe.callback_count == 1);
    CHECK(probe.destroy_status == SAO_SDK_ERR_BUSY);
    CHECK(probe.clear_status == SAO_SDK_ERR_BUSY);

    original.fail_timer_unregister = true;
    CHECK(sao_sdk_context_bind_provider(&ctx, &replacement_provider) == SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(replacement.events == std::vector<std::string>{"retain", "release"});
    original.timer_callback(0, original.timer_user_data);
    CHECK(probe.callback_count == 2);

    original.fail_timer_unregister = false;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &replacement_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("process providers publish neither candidate when net open fails",
          "[sdk][provider][transaction][process][rollback]") {
    ProcessMemoryFixture memory;
    ProcessNetFixture net;
    ProcessProviderReset reset;
    auto memory_provider = make_process_memory_provider(&memory);
    auto net_provider = make_process_net_provider(&net);
    net.open_status = SAO_SDK_ERR_NOT_INITIALIZED;

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.transaction.process", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_memory_configure_provider(&memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_net_configure_provider(&net_provider) == SAO_SDK_OK);

    CHECK(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_OK);
    CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_context_net_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(memory.open_count == 1);
    CHECK(memory.close_count == 1);
    CHECK(memory.live_sessions == 0);
    CHECK(net.open_count == 1);
    CHECK(net.live_sessions == 0);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("platform provider stays unbound when a process candidate fails",
          "[sdk][provider][transaction][platform][rollback]") {
    ProcessMemoryFixture memory;
    ProcessNetFixture net;
    ProcessProviderReset reset;
    auto memory_provider = make_process_memory_provider(&memory);
    auto net_provider = make_process_net_provider(&net);
    net.open_status = SAO_SDK_ERR_NOT_INITIALIZED;
    REQUIRE(sao_sdk_platform_memory_configure_provider(&memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_net_configure_provider(&net_provider) == SAO_SDK_OK);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.transaction.platform", "1.0", &ctx) ==
            SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(ctx.ctx_impl == nullptr);

    REQUIRE(sao_sdk_platform_memory_configure_provider(nullptr) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_net_configure_provider(nullptr) == SAO_SDK_OK);
    REQUIRE(sao_sdk_bind_context("provider.transaction.platform", "1.0", &ctx) == SAO_SDK_OK);
    CHECK(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, nullptr) == SAO_SDK_OK);

    REQUIRE(sao_sdk_platform_memory_configure_provider(&memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_net_configure_provider(&net_provider) == SAO_SDK_OK);
    CHECK(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_context_net_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("platform composite bind returns busy without replacing existing providers",
          "[sdk][provider][transaction][busy][preserve]") {
    ProviderFixture general;
    ProcessMemoryFixture memory;
    ProcessMemoryFixture replacement_memory;
    ProcessNetFixture net;
    ProcessNetFixture replacement_net;
    ProcessProviderReset reset;
    auto general_provider = make_provider(&general);
    auto memory_provider = make_process_memory_provider(&memory);
    auto replacement_memory_provider = make_process_memory_provider(&replacement_memory);
    auto net_provider = make_process_net_provider(&net);
    auto replacement_net_provider = make_process_net_provider(&replacement_net);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.transaction.preserve", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &general_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &net_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_memory_configure_provider(&replacement_memory_provider) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_net_configure_provider(&replacement_net_provider) == SAO_SDK_OK);

    CHECK(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_ERR_BUSY);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_OK);
    CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_OK);
    CHECK(sao_sdk_context_net_provider_status(&ctx) == SAO_SDK_OK);
    CHECK(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_OK);
    CHECK(general.events == std::vector<std::string>{"retain", "tts:stop"});
    CHECK(memory.live_sessions == 1);
    CHECK(net.live_sessions == 1);
    CHECK(replacement_memory.open_count == 0);
    CHECK(replacement_net.open_count == 0);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(general.events.back() == "release");
    CHECK(memory.live_sessions == 0);
    CHECK(net.live_sessions == 0);
}

TEST_CASE("process composite bind rejects a replaced owner generation before publish",
          "[sdk][provider][transaction][process_owner][generation][aba]") {
    ProcessMemoryFixture original;
    ProcessMemoryFixture replacement;
    ProcessProviderReset reset;
    auto original_provider = make_process_memory_provider(&original);
    auto replacement_provider = make_process_memory_provider(&replacement);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.transaction.generation", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_memory_configure_provider(&original_provider) == SAO_SDK_OK);
    original.replacement_on_open = &replacement_provider;

    CHECK(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_ERR_BUSY);
    CHECK(original.replacement_status == SAO_SDK_OK);
    CHECK(original.open_count == 1);
    CHECK(original.close_count == 1);
    CHECK(original.live_sessions == 0);
    CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(replacement.open_count == 0);

    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    CHECK(replacement.open_count == 1);
    CHECK(replacement.live_sessions == 1);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(replacement.live_sessions == 0);
}

TEST_CASE("failed process candidate rollback remains quarantined for retry",
          "[sdk][provider][transaction][rollback][quarantine]") {
    ProcessMemoryFixture memory;
    ProcessNetFixture net;
    ProcessProviderReset reset;
    auto memory_provider = make_process_memory_provider(&memory);
    auto net_provider = make_process_net_provider(&net);
    memory.close_status = SAO_SDK_ERR_INTERNAL;
    net.open_status = SAO_SDK_ERR_NOT_INITIALIZED;

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.transaction.quarantine", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_memory_configure_provider(&memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_net_configure_provider(&net_provider) == SAO_SDK_OK);

    CHECK(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(memory.live_sessions == 1);
    CHECK(memory.release_count == 0);

    memory.close_status = SAO_SDK_OK;
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_OK);
    CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(memory.live_sessions == 0);
    CHECK(memory.release_count == 1);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("plugin timer exceptions stop at the SDK callback barrier",
          "[sdk][provider][callback][exception]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.callback.exception", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);
    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 10, throwing_timer_probe, nullptr, &timer) == SAO_SDK_OK);
    CHECK_NOTHROW(provider_state.timer_callback(0, provider_state.timer_user_data));
    REQUIRE(sao_sdk_timer_unregister(&ctx, timer) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("general provider retain release and TTS exceptions stay behind the ABI barrier",
          "[sdk][provider][abi_barrier][exception][retry]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.abi.exception", "1.0", &ctx) == SAO_SDK_OK);

    provider_state.throw_retain = true;
    CHECK(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);
    provider_state.throw_retain = false;
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);

    provider_state.throw_tts = true;
    CHECK(ctx.tts->speak(ctx.ctx_impl, "barrier", 1.0F, 0.0F) == SAO_SDK_ERR_INTERNAL);
    provider_state.throw_tts = false;

    provider_state.throw_release = true;
    CHECK(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(ctx.ctx_impl != nullptr);
    CHECK(sao_sdk_context_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);
    provider_state.throw_release = false;
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);
}

TEST_CASE("provider capability insertion failures roll back native ownership",
          "[sdk][provider][capability][rollback][insertion]") {
    ProviderFixture provider_state;
    const auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.capability.rollback", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);

    sao_sdk_hook_token_t render = 123;
    sao_sdk_test_fail_next_render_state_insertion();
    CHECK(sao_sdk_register_render_hook(&ctx, SAO_SDK_HOOK_BEFORE_PRESENT, render_probe, nullptr,
                                       &render) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(render == 0);
    REQUIRE(provider_state.events.size() == 3);
    CHECK(provider_state.events[1] == "register:render:101");
    CHECK(provider_state.events[2] == "unregister:render:101");

    REQUIRE(sao_sdk_register_render_hook(&ctx, SAO_SDK_HOOK_BEFORE_PRESENT, render_probe, nullptr,
                                         &render) == SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_render_hook(&ctx, render) == SAO_SDK_OK);

    SaoSdkHotkeySpec hotkey_spec{};
    hotkey_spec.binding_id_utf8 = "capability.rollback.hotkey";
    hotkey_spec.virtual_key = 0x43;
    sao_sdk_hotkey_id_t hotkey = 123;
    sao_sdk_test_fail_next_hotkey_state_insertion();
    CHECK(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, nullptr, &hotkey) ==
          SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(hotkey == 0);
    REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, nullptr, &hotkey) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_hotkey(&ctx, hotkey) == SAO_SDK_OK);

    SaoSdkNotifySpec notify_spec{"rollback", 10, 0xff00ff00u};
    sao_sdk_notify_token_t notify = 123;
    sao_sdk_test_fail_next_notify_state_insertion();
    CHECK(sao_sdk_notify_show(&ctx, &notify_spec, &notify) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(notify == 0);
    REQUIRE(sao_sdk_notify_show(&ctx, &notify_spec, &notify) == SAO_SDK_OK);
    REQUIRE(sao_sdk_notify_dismiss(&ctx, notify) == SAO_SDK_OK);

    constexpr uint8_t kOverlayJson[] = {'{', '}'};
    SaoSdkOverlaySpec overlay_spec{"rollback", kOverlayJson, sizeof(kOverlayJson)};
    sao_sdk_overlay_token_t overlay = 123;
    sao_sdk_test_fail_next_overlay_state_insertion();
    CHECK(sao_sdk_overlay_set(&ctx, &overlay_spec, &overlay) == SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(overlay == 0);
    REQUIRE(sao_sdk_overlay_set(&ctx, &overlay_spec, &overlay) == SAO_SDK_OK);
    REQUIRE(sao_sdk_overlay_clear(&ctx, overlay) == SAO_SDK_OK);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(provider_state.events.back() == "release");
}

TEST_CASE("platform capability insertion failures roll back native resources",
          "[sdk][provider][platform][rollback][insertion]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.platform.rollback", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);

    sao_sdk_timer_token_t timer = 123;
    sao_sdk_test_fail_next_platform_timer_insertion();
        CHECK(sao_sdk_timer_register(&ctx, 10, timer_probe, nullptr, &timer) ==
            SAO_SDK_ERR_INTERNAL);
    CHECK(timer == 0);
    REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_probe, nullptr, &timer) == SAO_SDK_OK);
    REQUIRE(sao_sdk_timer_unregister(&ctx, timer) == SAO_SDK_OK);

    SaoSdkHotkeySpec hotkey_spec{};
    hotkey_spec.binding_id_utf8 = "platform.rollback.hotkey";
    hotkey_spec.virtual_key = 0x44;
    sao_sdk_hotkey_id_t hotkey = 123;
    sao_sdk_test_fail_next_platform_hotkey_insertion();
    CHECK(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, nullptr, &hotkey) ==
          SAO_SDK_ERR_NOT_INITIALIZED);
    CHECK(hotkey == 0);
    REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, nullptr, &hotkey) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_hotkey(&ctx, hotkey) == SAO_SDK_OK);

    SaoSdkDialogSpec dialog_spec{};
    dialog_spec.kind = SAO_SDK_DIALOG_INFO;
    dialog_spec.title_utf8 = "rollback";
    dialog_spec.message_utf8 = "rollback";
    sao_sdk_dialog_token_t dialog = 123;
    sao_sdk_test_fail_next_platform_dialog_insertion();
    CHECK(sao_sdk_dialog_show(&ctx, &dialog_spec, nullptr, nullptr, &dialog) ==
          SAO_SDK_ERR_INTERNAL);
    CHECK(dialog == 0);
    REQUIRE(sao_sdk_dialog_show(&ctx, &dialog_spec, nullptr, nullptr, &dialog) == SAO_SDK_OK);
    REQUIRE(sao_sdk_dialog_dismiss(&ctx, dialog) == SAO_SDK_OK);

    constexpr uint8_t kOverlayJson[] = {'{', '}'};
    SaoSdkOverlaySpec overlay_spec{"platform.rollback", kOverlayJson, sizeof(kOverlayJson)};
    sao_sdk_overlay_token_t overlay = 123;
    sao_sdk_test_fail_next_platform_overlay_insertion();
    CHECK(sao_sdk_overlay_set(&ctx, &overlay_spec, &overlay) == SAO_SDK_ERR_INTERNAL);
    CHECK(overlay == 0);
    REQUIRE(sao_sdk_overlay_set(&ctx, &overlay_spec, &overlay) == SAO_SDK_OK);
    REQUIRE(sao_sdk_overlay_clear(&ctx, overlay) == SAO_SDK_OK);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("event and platform hotkey owners recycle past the former fixed capacity",
          "[sdk][event][hotkey][lifetime][recycle]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("owner.recycle", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);

    SaoSdkHotkeySpec hotkey_spec{};
    hotkey_spec.binding_id_utf8 = "owner.recycle.hotkey";
    hotkey_spec.virtual_key = 0x77;
    CallbackProbe probe;
    constexpr size_t kLifecycleCount = 4097;
    for (size_t index = 0; index < kLifecycleCount; ++index) {
        CAPTURE(index);
        sao_sdk_subscription_t subscription = 0;
        REQUIRE(sao_sdk_subscribe_event(&ctx, "owner.recycle.event", no_op_event_probe, nullptr,
                                        &subscription) == SAO_SDK_OK);
        REQUIRE(sao_sdk_unsubscribe_event(&ctx, subscription) == SAO_SDK_OK);

        sao_sdk_hotkey_id_t hotkey = 0;
        REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, &probe, &hotkey) ==
                SAO_SDK_OK);
        REQUIRE(sao_sdk_unregister_hotkey(&ctx, hotkey) == SAO_SDK_OK);
    }

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("concurrent context destroy retry has one executor and busy followers",
          "[sdk][context][destroy][retry][concurrency]") {
    ProviderFixture provider_state;
    provider_state.block_release = true;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("context.destroy.serialized", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);

    auto first = std::async(std::launch::async, [&] { return sao_sdk_context_try_destroy(&ctx); });
    {
        std::unique_lock<std::mutex> lock(provider_state.release_mutex);
        REQUIRE(provider_state.release_condition.wait_for(
            lock, std::chrono::seconds(2), [&provider_state] {
                return provider_state.release_entered;
            }));
    }

    const auto retry_started = std::chrono::steady_clock::now();
    CHECK(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_ERR_BUSY);
    CHECK(std::chrono::steady_clock::now() - retry_started < std::chrono::seconds(1));

    {
        std::lock_guard<std::mutex> lock(provider_state.release_mutex);
        provider_state.allow_release = true;
    }
    provider_state.release_condition.notify_all();
    REQUIRE(first.get() == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);
}

TEST_CASE("public provider configure cannot rebuild sessions during teardown",
          "[sdk][context][destroy][provider][configure][reentry]") {
    ProviderFixture general;
    auto general_provider = make_provider(&general);
    ProcessMemoryFixture memory;
    ProcessMemoryFixture replacement_memory;
    auto memory_provider = make_process_memory_provider(&memory);
    auto replacement_memory_provider = make_process_memory_provider(&replacement_memory);
    ProcessNetFixture net;
    ProcessNetFixture replacement_net;
    auto net_provider = make_process_net_provider(&net);
    auto replacement_net_provider = make_process_net_provider(&replacement_net);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("context.destroy.configure", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &net_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &general_provider) == SAO_SDK_OK);
    general.configure_reentry_context = &ctx;
    general.configure_memory_replacement = &replacement_memory_provider;
    general.configure_net_replacement = &replacement_net_provider;

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(general.configure_memory_status == SAO_SDK_ERR_BUSY);
    CHECK(general.configure_net_status == SAO_SDK_ERR_BUSY);
    CHECK(memory.live_sessions == 0);
    CHECK(net.live_sessions == 0);
    CHECK(replacement_memory.open_count == 0);
    CHECK(replacement_memory.live_sessions == 0);
    CHECK(replacement_net.open_count == 0);
    CHECK(replacement_net.live_sessions == 0);
}

TEST_CASE("context destroy drains ordinary event and UI API leases",
          "[sdk][context][api-lease][destroy][concurrency]") {
    SECTION("subscribe registration") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("lease.event.subscribe", "1.0", &ctx) == SAO_SDK_OK);
        BlockingCallbackProbe callback_probe;
        sao_sdk_subscription_t subscription = 0;

        sao_sdk_test_arm_context_api_pause(kEventSubscribeRegistered);
        auto api = std::async(std::launch::async, [&] {
            return sao_sdk_subscribe_event(&ctx, "lease.subscribe", blocking_event_probe,
                                           &callback_probe, &subscription);
        });
        CHECK(sao_sdk_test_wait_for_context_api_pause(kEventSubscribeRegistered));
        auto destroy =
            std::async(std::launch::async, [&] { return sao_sdk_context_try_destroy(&ctx); });
        CHECK(sao_sdk_test_wait_for_context_shutdown(&ctx));
        CHECK(sao_sdk_publish_event(&ctx, "lease.closed", nullptr, 0) == SAO_SDK_ERR_BUSY);
        CHECK(destroy.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        sao_sdk_test_resume_context_api_pause(kEventSubscribeRegistered);
        CHECK(api.get() == SAO_SDK_OK);
        CHECK(subscription != 0);
        CHECK(destroy.get() == SAO_SDK_OK);
        CHECK(ctx.ctx_impl == nullptr);
    }

    SECTION("unsubscribe outside the state lock") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("lease.event.unsubscribe", "1.0", &ctx) == SAO_SDK_OK);
        BlockingCallbackProbe callback_probe;
        sao_sdk_subscription_t subscription = 0;
        REQUIRE(sao_sdk_subscribe_event(&ctx, "lease.unsubscribe", blocking_event_probe,
                                        &callback_probe, &subscription) == SAO_SDK_OK);

        sao_sdk_test_arm_context_api_pause(kEventUnsubscribeUnlocked);
        auto api = std::async(std::launch::async,
                              [&] { return sao_sdk_unsubscribe_event(&ctx, subscription); });
        CHECK(sao_sdk_test_wait_for_context_api_pause(kEventUnsubscribeUnlocked));
        auto destroy =
            std::async(std::launch::async, [&] { return sao_sdk_context_try_destroy(&ctx); });
        CHECK(sao_sdk_test_wait_for_context_shutdown(&ctx));
        CHECK(sao_sdk_publish_event(&ctx, "lease.closed", nullptr, 0) == SAO_SDK_ERR_BUSY);
        CHECK(destroy.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        sao_sdk_test_resume_context_api_pause(kEventUnsubscribeUnlocked);
        CHECK(api.get() == SAO_SDK_OK);
        CHECK(destroy.get() == SAO_SDK_OK);
        CHECK(ctx.ctx_impl == nullptr);
    }

    SECTION("panel registration") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("lease.panel.register", "1.0", &ctx) == SAO_SDK_OK);
        const auto descriptor = concurrent_panel_descriptor("lease.panel.register.panel");
        sao_sdk_ui_panel_t panel = nullptr;

        sao_sdk_test_arm_context_api_pause(kPanelRegistered);
        auto api = std::async(std::launch::async,
                              [&] { return sao_sdk_register_ui_panel(&ctx, &descriptor, &panel); });
        CHECK(sao_sdk_test_wait_for_context_api_pause(kPanelRegistered));
        auto destroy =
            std::async(std::launch::async, [&] { return sao_sdk_context_try_destroy(&ctx); });
        CHECK(sao_sdk_test_wait_for_context_shutdown(&ctx));
        CHECK(sao_sdk_publish_event(&ctx, "lease.closed", nullptr, 0) == SAO_SDK_ERR_BUSY);
        CHECK(destroy.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        sao_sdk_test_resume_context_api_pause(kPanelRegistered);
        CHECK(api.get() == SAO_SDK_OK);
        CHECK(panel != nullptr);
        CHECK(destroy.get() == SAO_SDK_OK);
        CHECK(ctx.ctx_impl == nullptr);
    }

    SECTION("panel operation") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("lease.panel.operation", "1.0", &ctx) == SAO_SDK_OK);
        const auto descriptor = concurrent_panel_descriptor("lease.panel.operation.panel");
        sao_sdk_ui_panel_t panel = nullptr;
        REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
        SaoSdkWidgetSpec widget{};
        widget.kind = SAO_SDK_UI_WIDGET_LABEL;
        widget.widget_id_utf8 = "lease-widget";
        widget.text_utf8 = "lease";
        sao_sdk_ui_widget_t widget_handle = nullptr;

        sao_sdk_test_arm_context_api_pause(kPanelOperationUnlocked);
        auto api = std::async(std::launch::async, [&] {
            return sao_sdk_panel_add_widget(&ctx, panel, &widget, &widget_handle);
        });
        CHECK(sao_sdk_test_wait_for_context_api_pause(kPanelOperationUnlocked));
        auto destroy =
            std::async(std::launch::async, [&] { return sao_sdk_context_try_destroy(&ctx); });
        CHECK(sao_sdk_test_wait_for_context_shutdown(&ctx));
        CHECK(sao_sdk_publish_event(&ctx, "lease.closed", nullptr, 0) == SAO_SDK_ERR_BUSY);
        CHECK(destroy.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        sao_sdk_test_resume_context_api_pause(kPanelOperationUnlocked);
        CHECK(api.get() == SAO_SDK_OK);
        CHECK(widget_handle != nullptr);
        CHECK(destroy.get() == SAO_SDK_OK);
        CHECK(ctx.ctx_impl == nullptr);
    }
}

TEST_CASE("event subscription drains its own copied callback snapshots",
          "[sdk][event][snapshot][concurrency][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("event.snapshot.drain", "1.0", &ctx) == SAO_SDK_OK);

    BlockingCallbackProbe probe;
    sao_sdk_subscription_t subscription = 0;
    REQUIRE(sao_sdk_subscribe_event(&ctx, "event.snapshot", blocking_event_probe, &probe,
                                    &subscription) == SAO_SDK_OK);
    void* snapshot = sao_sdk_test_event_snapshot_user_data(&ctx, subscription);
    REQUIRE(snapshot != nullptr);
    std::thread callback_thread([snapshot] { sao_sdk_test_invoke_event_snapshot(snapshot); });
    REQUIRE(wait_for_callback(probe));

    std::promise<void> unsubscribe_started;
    auto unsubscribe_started_future = unsubscribe_started.get_future();
    auto unsubscribe = std::async(std::launch::async, [&] {
        unsubscribe_started.set_value();
        return sao_sdk_unsubscribe_event(&ctx, subscription);
    });
    unsubscribe_started_future.wait();
    CHECK(unsubscribe.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    release_callback(probe);
    callback_thread.join();
    REQUIRE(unsubscribe.get() == SAO_SDK_OK);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);

    sao_sdk_test_invoke_event_snapshot(snapshot);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);
    sao_sdk_test_release_event_snapshot(snapshot);

    EventReentryProbe reentry{&ctx};
    REQUIRE(sao_sdk_subscribe_event(&ctx, "event.reentry", event_reentry_probe, &reentry,
                                    &reentry.subscription) == SAO_SDK_OK);
    void* reentry_snapshot =
        sao_sdk_test_event_snapshot_user_data(&ctx, reentry.subscription);
    REQUIRE(reentry_snapshot != nullptr);
    sao_sdk_test_invoke_event_snapshot(reentry_snapshot);
    CHECK(reentry.calls == 1);
    CHECK(reentry.destroy_status == SAO_SDK_ERR_BUSY);
    CHECK(reentry.status == SAO_SDK_ERR_BUSY);
    REQUIRE(sao_sdk_unsubscribe_event(&ctx, reentry.subscription) == SAO_SDK_OK);
    sao_sdk_test_release_event_snapshot(reentry_snapshot);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("context destroy retires event snapshots before releasing subscription state",
          "[sdk][event][snapshot][destroy][concurrency]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("event.snapshot.destroy", "1.0", &ctx) == SAO_SDK_OK);

    BlockingCallbackProbe probe;
    sao_sdk_subscription_t subscription = 0;
    REQUIRE(sao_sdk_subscribe_event(&ctx, "event.destroy", blocking_event_probe, &probe,
                                    &subscription) == SAO_SDK_OK);
    void* snapshot = sao_sdk_test_event_snapshot_user_data(&ctx, subscription);
    REQUIRE(snapshot != nullptr);
    std::thread callback_thread([snapshot] { sao_sdk_test_invoke_event_snapshot(snapshot); });
    REQUIRE(wait_for_callback(probe));

    std::promise<void> destroy_started;
    auto destroy_started_future = destroy_started.get_future();
    auto destroy = std::async(std::launch::async, [&] {
        destroy_started.set_value();
        return sao_sdk_context_try_destroy(&ctx);
    });
    destroy_started_future.wait();
    CHECK(destroy.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    release_callback(probe);
    callback_thread.join();
    REQUIRE(destroy.get() == SAO_SDK_OK);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);

    sao_sdk_test_invoke_event_snapshot(snapshot);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);
    sao_sdk_test_release_event_snapshot(snapshot);
}

TEST_CASE("void context destroy from a plugin callback quarantines for later retry",
          "[sdk][context][destroy][quarantine][callback][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("context.callback.destroy", "1.0", &ctx) == SAO_SDK_OK);

    VoidDestroyProbe probe{&ctx};
    sao_sdk_subscription_t subscription = 0;
    REQUIRE(sao_sdk_subscribe_event(&ctx, "context.callback.destroy", void_destroy_event_probe,
                                    &probe, &subscription) == SAO_SDK_OK);

    const auto start = std::chrono::steady_clock::now();
    REQUIRE(sao_sdk_publish_event(&ctx, "context.callback.destroy", nullptr, 0) == SAO_SDK_OK);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);
    CHECK(ctx.ctx_impl != nullptr);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);
}

TEST_CASE("direct vtable paths pin context state while destroy races",
          "[sdk][context][api-lease][vtable][concurrency]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("context.vtable.race", "1.0", &ctx) == SAO_SDK_OK);

    void* const ctx_impl = ctx.ctx_impl;
    const SaoSdkConfigTable* const config = ctx.config;
    const SaoSdkMemTable* const mem = ctx.mem;
    const SaoSdkNetTable* const net = ctx.net;
    const SaoSdkUiTable* const ui = ctx.ui;
    const SaoSdkEventTable* const event = ctx.event;
    const SaoSdkHotkeyTable* const hotkey = ctx.hotkey;
    const SaoSdkGpuHuntTable* const gpu = ctx.gpu_hunt;
    std::atomic_bool stop{false};
    std::atomic_int ready{0};
    constexpr int kWorkerCount = 4;
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);
    for (int index = 0; index < kWorkerCount; ++index) {
        workers.emplace_back([&, index] {
            (void)index;
            ready.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire)) {
                bool enabled = false;
                size_t bytes_read = 0;
                size_t result_count = 0;
                sao_sdk_gpu_tracker_t tracker = 0;
                (void)config->get_bool(ctx_impl, "race", &enabled);
                (void)mem->read(ctx_impl, 0, nullptr, 0, &bytes_read);
                (void)net->parse_packet(ctx_impl, nullptr, nullptr, 0,
                                        sizeof(SaoSdkNetParsedResult), &result_count);
                (void)ui->request_redraw(ctx_impl, nullptr);
                (void)event->publish(ctx_impl, "race", nullptr, 0);
                (void)hotkey->unregister_hotkey(ctx_impl, 0);
                (void)gpu->create_tracker(ctx_impl, &tracker);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kWorkerCount)
        std::this_thread::yield();

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers)
        worker.join();
    CHECK(ctx.ctx_impl == nullptr);
}

TEST_CASE("GPU ABI tail direct vtable call pins context while destroy races",
          "[sdk][provider][gpu_hunt][context][api-lease][vtable][concurrency]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("context.gpu.vtable.race", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);

    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(tracker != 0);
    void* const ctx_impl = ctx.ctx_impl;
    const SaoSdkGpuHuntTable* const gpu = ctx.gpu_hunt;
    uint64_t toggles = 0;

    sao_sdk_test_arm_context_api_pause(kGpuRuntimeTogglesLeased);
    auto api = std::async(std::launch::async,
                          [&] { return gpu->get_runtime_toggles(ctx_impl, tracker, &toggles); });
    CHECK(sao_sdk_test_wait_for_context_api_pause(kGpuRuntimeTogglesLeased));
    auto destroy =
        std::async(std::launch::async, [&] { return sao_sdk_context_try_destroy(&ctx); });
    CHECK(sao_sdk_test_wait_for_context_shutdown(&ctx));
    CHECK(destroy.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    sao_sdk_test_resume_context_api_pause(kGpuRuntimeTogglesLeased);
    CHECK(api.get() == SAO_SDK_OK);
    CHECK(destroy.get() == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);
}

TEST_CASE("platform hotkey snapshots keep their bridge alive and drain on unregister",
          "[sdk][provider][hotkey][snapshot][concurrency][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("hotkey.snapshot.drain", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);

    SaoSdkHotkeySpec spec{};
    spec.binding_id_utf8 = "hotkey_snapshot";
    spec.virtual_key = 0x48;
    BlockingCallbackProbe probe;
    sao_sdk_hotkey_id_t hotkey = 0;
    REQUIRE(sao_sdk_register_hotkey(&ctx, &spec, blocking_hotkey_probe, &probe, &hotkey) ==
            SAO_SDK_OK);
    void* snapshot = sao_sdk_test_platform_hotkey_snapshot_user_data(&ctx, hotkey);
    REQUIRE(snapshot != nullptr);
    std::thread callback_thread(
        [snapshot] { sao_sdk_test_invoke_platform_hotkey_snapshot(snapshot); });
    REQUIRE(wait_for_callback(probe));

    std::promise<void> unregister_started;
    auto unregister_started_future = unregister_started.get_future();
    auto unregister = std::async(std::launch::async, [&] {
        unregister_started.set_value();
        return sao_sdk_unregister_hotkey(&ctx, hotkey);
    });
    unregister_started_future.wait();
    CHECK(unregister.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    release_callback(probe);
    callback_thread.join();
    REQUIRE(unregister.get() == SAO_SDK_OK);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);

    sao_sdk_test_invoke_platform_hotkey_snapshot(snapshot);
    CHECK(probe.calls.load(std::memory_order_relaxed) == 1);
    sao_sdk_test_release_platform_hotkey_snapshot(snapshot);

    HotkeyReentryProbe reentry{&ctx};
    spec.binding_id_utf8 = "hotkey_reentry";
    spec.virtual_key = 0x52;
    REQUIRE(sao_sdk_register_hotkey(&ctx, &spec, hotkey_reentry_probe, &reentry,
                                    &reentry.hotkey) == SAO_SDK_OK);
    void* reentry_snapshot =
        sao_sdk_test_platform_hotkey_snapshot_user_data(&ctx, reentry.hotkey);
    REQUIRE(reentry_snapshot != nullptr);
    sao_sdk_test_invoke_platform_hotkey_snapshot(reentry_snapshot);
    CHECK(reentry.calls == 1);
    CHECK(reentry.status == SAO_SDK_ERR_BUSY);
    REQUIRE(sao_sdk_unregister_hotkey(&ctx, reentry.hotkey) == SAO_SDK_OK);
    sao_sdk_test_release_platform_hotkey_snapshot(reentry_snapshot);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("GPU provider sessions require attach and retain tracker lifetime",
          "[sdk][provider][gpu_hunt]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext owner{};
    SaoSdkContext sibling{};
    REQUIRE(sao_sdk_bind_context("provider.gpu", "1.0", &owner) == SAO_SDK_OK);
    REQUIRE(sao_sdk_bind_context("provider.gpu.sibling", "1.0", &sibling) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&owner, &provider) == SAO_SDK_OK);

    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(owner.gpu_hunt->create_tracker(owner.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(tracker != 0);
    REQUIRE(provider_state.live_gpu_sessions == 1);

    REQUIRE(owner.gpu_hunt->tick(owner.ctx_impl, tracker) == SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(sibling.gpu_hunt->attach_tracker(sibling.ctx_impl, tracker, 77) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(sibling.gpu_hunt->detach_tracker(sibling.ctx_impl, tracker) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(sibling.gpu_hunt->destroy_tracker(sibling.ctx_impl, tracker) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);

    REQUIRE(owner.gpu_hunt->attach_tracker(owner.ctx_impl, tracker, 4242) == SAO_SDK_OK);
    REQUIRE(owner.gpu_hunt->tick(owner.ctx_impl, tracker) == SAO_SDK_OK);
    REQUIRE(owner.gpu_hunt->detach_tracker(owner.ctx_impl, tracker) == SAO_SDK_OK);
    REQUIRE(owner.gpu_hunt->tick(owner.ctx_impl, tracker) == SAO_SDK_ERR_NOT_INITIALIZED);

    REQUIRE(owner.gpu_hunt->attach_tracker(owner.ctx_impl, tracker, 4243) == SAO_SDK_OK);
    sao_sdk_context_destroy(&owner);
    REQUIRE(provider_state.live_gpu_sessions == 0);
    sao_sdk_context_destroy(&sibling);

    const std::vector<std::string> expected{
        "retain",          "retain",        "gpu:open:provider.gpu",
        "gpu:attach:4242", "gpu:enum:size", "gpu:detach",
        "gpu:attach:4243", "release",       "gpu:detach",
        "gpu:close",       "release",
    };
    REQUIRE(provider_state.events == expected);
}

TEST_CASE("platform GPU provider owns configured sessions and clears fail closed",
          "[sdk][provider][gpu_hunt][platform]") {
    PlatformGpuHuntProviderReset reset;
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);

    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.platform", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);

    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(&provider) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(tracker != 0);
    REQUIRE(provider_state.live_gpu_sessions == 1);
    REQUIRE(ctx.gpu_hunt->attach_tracker(ctx.ctx_impl, tracker, 5150) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->tick(ctx.ctx_impl, tracker) == SAO_SDK_OK);

    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t unbound_tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &unbound_tracker) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(unbound_tracker == 0);
    REQUIRE(provider_state.live_gpu_sessions == 1);

    REQUIRE(ctx.gpu_hunt->detach_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    REQUIRE(provider_state.live_gpu_sessions == 0);
    sao_sdk_context_destroy(&ctx);

    const std::vector<std::string> expected{
        "retain",          "retain",        "gpu:open:provider.gpu.platform",
        "gpu:attach:5150", "gpu:enum:size", "gpu:detach",
        "gpu:close",       "release",       "release",
    };
    REQUIRE(provider_state.events == expected);
}

TEST_CASE("platform GPU provider rejects incomplete owners without replacement",
          "[sdk][provider][gpu_hunt][platform][validation]") {
    PlatformGpuHuntProviderReset reset;
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);

    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(&provider) == SAO_SDK_OK);

    ProviderFixture incomplete_state;
    auto incomplete = make_provider(&incomplete_state);
    incomplete.gpu_hunt_read = nullptr;
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(&incomplete) == SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.validation", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    sao_sdk_context_destroy(&ctx);

    REQUIRE(incomplete_state.events.empty());
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);
    const std::vector<std::string> expected{
        "retain", "retain", "gpu:open:provider.gpu.validation", "gpu:close", "release", "release",
    };
    REQUIRE(provider_state.events == expected);
}

TEST_CASE("platform GPU failed open closes a returned session once",
          "[sdk][provider][gpu_hunt][platform][exception]") {
    PlatformGpuHuntProviderReset reset;
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);

    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    provider.gpu_hunt_open_session = provider_gpu_open_failed_with_session;
    provider.gpu_hunt_close_session = provider_gpu_close_then_throw;
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(&provider) == SAO_SDK_OK);

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.exception", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(tracker == 0);
    REQUIRE(provider_state.live_gpu_sessions == 0);
    sao_sdk_context_destroy(&ctx);

    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);
    const std::vector<std::string> expected{
        "retain", "retain", "gpu:open:provider.gpu.exception", "gpu:close", "release", "release",
    };
    REQUIRE(provider_state.events == expected);
}

TEST_CASE("GPU tracker destroy preserves handle and session across detach and close retries",
          "[sdk][provider][gpu_hunt][destroy][retry]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.destroy.retry", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->attach_tracker(ctx.ctx_impl, tracker, 6060) == SAO_SDK_OK);

    provider_state.gpu_detach_status = SAO_SDK_ERR_INTERNAL;
    CHECK(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_ERR_INTERNAL);
    CHECK(provider_state.live_gpu_sessions == 1);
    provider_state.gpu_detach_status = SAO_SDK_OK;
    provider_state.gpu_close_status = SAO_SDK_ERR_INTERNAL;
    CHECK(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_ERR_INTERNAL);
    CHECK(provider_state.live_gpu_sessions == 1);
    provider_state.gpu_close_status = SAO_SDK_OK;
    REQUIRE(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    CHECK(provider_state.live_gpu_sessions == 0);
    CHECK(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("GPU provider callback tracker reentry fails busy before resource locking",
          "[sdk][provider][gpu_hunt][callback][reentry]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.callback.reentry", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);

    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->attach_tracker(ctx.ctx_impl, tracker, 6063) == SAO_SDK_OK);
    provider_state.gpu_reentry_context = &ctx;
    provider_state.gpu_reentry_tracker = tracker;

    REQUIRE(ctx.gpu_hunt->tick(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    CHECK(provider_state.gpu_destroy_status == SAO_SDK_ERR_BUSY);
    REQUIRE(provider_state.gpu_tracker_reentry_call_count ==
            provider_state.gpu_tracker_reentry_statuses.size());
    for (size_t index = 0; index < provider_state.gpu_tracker_reentry_call_count; ++index) {
        CAPTURE(index);
        CHECK(provider_state.gpu_tracker_reentry_statuses[index] == SAO_SDK_ERR_BUSY);
    }

    provider_state.gpu_reentry_context = nullptr;
    provider_state.gpu_reentry_tracker = 0;
    REQUIRE(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("platform GPU close failure keeps the adapter session retryable",
          "[sdk][provider][gpu_hunt][platform][destroy][retry]") {
    PlatformGpuHuntProviderReset reset;
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(&provider) == SAO_SDK_OK);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.platform.retry", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);

    provider_state.gpu_close_status = SAO_SDK_ERR_INTERNAL;
    CHECK(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_ERR_INTERNAL);
    CHECK(provider_state.live_gpu_sessions == 1);
    provider_state.gpu_close_status = SAO_SDK_OK;
    REQUIRE(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    CHECK(provider_state.live_gpu_sessions == 0);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);
}

TEST_CASE("GPU tick contains provider exceptions at the wire boundary",
          "[sdk][provider][gpu_hunt][tick][exception]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.gpu.tick.exception", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_OK);
    REQUIRE(ctx.gpu_hunt->attach_tracker(ctx.ctx_impl, tracker, 6061) == SAO_SDK_OK);
    provider_state.gpu_reentry_context = &ctx;
    CHECK(ctx.gpu_hunt->tick(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    CHECK(provider_state.gpu_destroy_status == SAO_SDK_ERR_BUSY);
    CHECK(provider_state.live_gpu_sessions == 1);
    provider_state.gpu_reentry_context = nullptr;
    provider_state.throw_gpu_enum = true;
    CHECK(ctx.gpu_hunt->tick(ctx.ctx_impl, tracker) == SAO_SDK_ERR_INTERNAL);
    provider_state.throw_gpu_enum = false;
    REQUIRE(ctx.gpu_hunt->destroy_tracker(ctx.ctx_impl, tracker) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}
