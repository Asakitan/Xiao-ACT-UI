// SAO Auto — embedded Python-parity sound catalog and XAudio2 playback.

#include "sao/ui/sound.h"

#include "sound_assets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#endif

namespace {

std::atomic_bool g_sound_enabled{true};
std::atomic<int32_t> g_sound_volume{70};

bool valid_cue(SaoUiSoundCue cue) noexcept {
    return cue >= SAO_UI_SOUND_CLICK && cue < SAO_UI_SOUND_COUNT;
}

#if defined(_WIN32)

static_assert(SAO_UI_SOUND_COUNT == 11, "sound resource table must match the public cue enum");

constexpr std::array<int32_t, SAO_UI_SOUND_COUNT> kSoundResourceIds{
    SAO_UI_SOUND_ASSET_CLICK,       SAO_UI_SOUND_ASSET_MENU_OPEN, SAO_UI_SOUND_ASSET_MENU_CLOSE,
    SAO_UI_SOUND_ASSET_PANEL,       SAO_UI_SOUND_ASSET_SUBMENU,   SAO_UI_SOUND_ASSET_ALERT,
    SAO_UI_SOUND_ASSET_ALERT_CLOSE, SAO_UI_SOUND_ASSET_WELCOME,   SAO_UI_SOUND_ASSET_ALO_WELCOME,
    SAO_UI_SOUND_ASSET_LINK_START,  SAO_UI_SOUND_ASSET_NERVEGEAR,
};

uint32_t read_u32(const uint8_t* bytes) noexcept {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

struct WaveResource {
    WAVEFORMATEX format{};
    const uint8_t* samples{};
    uint32_t sample_bytes{};
};

sao_status_t parse_pcm_wave(const uint8_t* bytes, size_t byte_count,
                            WaveResource* out_wave) noexcept {
    if (bytes == nullptr || out_wave == nullptr || byte_count < 12U ||
        std::memcmp(bytes, "RIFF", 4U) != 0 || std::memcmp(bytes + 8U, "WAVE", 4U) != 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const uint64_t declared_size = static_cast<uint64_t>(read_u32(bytes + 4U)) + 8U;
    if (declared_size > byte_count || declared_size < 12U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const size_t limit = static_cast<size_t>(declared_size);

    const uint8_t* format_bytes = nullptr;
    uint32_t format_size = 0U;
    const uint8_t* sample_bytes = nullptr;
    uint32_t sample_size = 0U;
    size_t offset = 12U;
    while (offset + 8U <= limit) {
        const uint8_t* chunk = bytes + offset;
        const uint32_t chunk_size = read_u32(chunk + 4U);
        const size_t payload_offset = offset + 8U;
        if (chunk_size > limit - payload_offset)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (std::memcmp(chunk, "fmt ", 4U) == 0 && format_bytes == nullptr) {
            format_bytes = bytes + payload_offset;
            format_size = chunk_size;
        } else if (std::memcmp(chunk, "data", 4U) == 0 && sample_bytes == nullptr) {
            sample_bytes = bytes + payload_offset;
            sample_size = chunk_size;
        }
        size_t next = payload_offset + static_cast<size_t>(chunk_size);
        if ((chunk_size & 1U) != 0U) {
            if (next >= limit)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            ++next;
        }
        offset = next;
    }
    if (format_bytes == nullptr || format_size < 16U || sample_bytes == nullptr ||
        sample_size == 0U)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    WaveResource parsed{};
    const size_t copy_size = std::min<size_t>(format_size, sizeof(parsed.format));
    std::memcpy(&parsed.format, format_bytes, copy_size);
    if (format_size == 16U)
        parsed.format.cbSize = 0U;
    if (parsed.format.wFormatTag != WAVE_FORMAT_PCM || parsed.format.nChannels == 0U ||
        parsed.format.nChannels > 2U || parsed.format.nSamplesPerSec == 0U ||
        parsed.format.wBitsPerSample != 16U || parsed.format.nBlockAlign == 0U ||
        parsed.format.nBlockAlign != parsed.format.nChannels * sizeof(int16_t) ||
        parsed.format.nAvgBytesPerSec != parsed.format.nSamplesPerSec * parsed.format.nBlockAlign ||
        sample_size % parsed.format.nBlockAlign != 0U) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    parsed.samples = sample_bytes;
    parsed.sample_bytes = sample_size;
    *out_wave = parsed;
    return SAO_STATUS_OK;
}

struct SoundRequest {
    SaoUiSoundCue cue{SAO_UI_SOUND_CLICK};
    int32_t volume{};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done{};
};

class SoundEngine final {
  public:
    SoundEngine() = default;
    SoundEngine(const SoundEngine&) = delete;
    SoundEngine& operator=(const SoundEngine&) = delete;

    ~SoundEngine() noexcept { (void)shutdown(); }

    sao_status_t play(SaoUiSoundCue cue, int32_t volume) {
        auto request = std::make_shared<SoundRequest>();
        request->cue = cue;
        request->volume = volume;

        {
            std::lock_guard lock(mutex_);
            if (!accepting_)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (worker_exited_)
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            if (!worker_.joinable()) {
                worker_ = std::thread([this] { run(); });
            }
            queue_.push_back(request);
        }

        cv_.notify_one();
        std::unique_lock done_lock(request->done_mutex);
        request->done_cv.wait(done_lock, [&request] { return request->done; });
        return request->status;
    }

    sao_status_t shutdown() noexcept {
        try {
            std::lock_guard shutdown_lock(shutdown_mutex_);
            {
                std::lock_guard lock(mutex_);
                accepting_ = false;
                shutdown_requested_ = true;
                cancel_queued_locked();
            }
            cv_.notify_all();
            if (worker_.joinable()) {
                if (worker_.get_id() == std::this_thread::get_id())
                    return SAO_STATUS_ERR_OS_CALL_FAILED;
                worker_.join();
            }
            {
                std::lock_guard lock(mutex_);
                accepting_ = true;
                worker_exited_ = false;
                shutdown_requested_ = false;
            }
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }

  private:
    struct CueState {
        WaveResource wave{};
        IXAudio2SourceVoice* voice{};
        sao_status_t load_status{SAO_STATUS_ERR_NOT_INITIALIZED};
        bool load_attempted{};
    };

    sao_status_t play_on_worker(SaoUiSoundCue cue, int32_t volume) noexcept {
        sao_status_t status = ensure_engine();
        if (status != SAO_STATUS_OK)
            return status;
        CueState& state = cues_[static_cast<size_t>(cue)];
        status = ensure_cue(cue, &state);
        if (status != SAO_STATUS_OK)
            return status;

        XAUDIO2_VOICE_STATE voice_state{};
        state.voice->GetState(&voice_state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        HRESULT result = S_OK;
        if (voice_state.BuffersQueued != 0U) {
            state.voice->DestroyVoice();
            state.voice = nullptr;
            result = engine_->CreateSourceVoice(&state.voice, &state.wave.format);
        } else {
            result = state.voice->Stop(0U);
        }
        if (SUCCEEDED(result))
            result = state.voice->SetVolume(static_cast<float>(volume) / 100.0F);
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = state.wave.sample_bytes;
        buffer.pAudioData = state.wave.samples;
        if (SUCCEEDED(result))
            result = state.voice->SubmitSourceBuffer(&buffer);
        if (SUCCEEDED(result))
            result = state.voice->Start(0U);
        if (SUCCEEDED(result))
            return SAO_STATUS_OK;
        release_engine();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    sao_status_t ensure_engine() noexcept {
        if (engine_ != nullptr && mastering_voice_ != nullptr && module_ != nullptr)
            return SAO_STATUS_OK;

        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&g_sound_volume), &module) == FALSE ||
            module == nullptr) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        IXAudio2* engine = nullptr;
        HRESULT result = XAudio2Create(&engine, 0U, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(result) || engine == nullptr)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        IXAudio2MasteringVoice* mastering_voice = nullptr;
        result = engine->CreateMasteringVoice(&mastering_voice);
        if (FAILED(result) || mastering_voice == nullptr) {
            engine->Release();
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        module_ = module;
        engine_ = engine;
        mastering_voice_ = mastering_voice;
        return SAO_STATUS_OK;
    }

    sao_status_t ensure_cue(SaoUiSoundCue cue, CueState* state) noexcept {
        if (state == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (!state->load_attempted) {
            state->load_attempted = true;
            const int32_t resource_id = kSoundResourceIds[static_cast<size_t>(cue)];
            const HRSRC resource = FindResourceW(module_, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
            if (resource == nullptr) {
                state->load_status = SAO_STATUS_ERR_NOT_FOUND;
            } else {
                const DWORD resource_size = SizeofResource(module_, resource);
                const HGLOBAL loaded = LoadResource(module_, resource);
                const auto* bytes =
                    loaded == nullptr ? nullptr : static_cast<const uint8_t*>(LockResource(loaded));
                state->load_status = resource_size == 0U || bytes == nullptr
                                         ? SAO_STATUS_ERR_OS_CALL_FAILED
                                         : parse_pcm_wave(bytes, resource_size, &state->wave);
            }
        }
        if (state->load_status != SAO_STATUS_OK)
            return state->load_status;
        if (state->voice == nullptr) {
            const HRESULT result = engine_->CreateSourceVoice(&state->voice, &state->wave.format);
            if (FAILED(result) || state->voice == nullptr) {
                release_engine();
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            }
        }
        return SAO_STATUS_OK;
    }

    void release_engine() noexcept {
        for (auto& cue : cues_) {
            if (cue.voice != nullptr) {
                cue.voice->DestroyVoice();
                cue.voice = nullptr;
            }
        }
        if (mastering_voice_ != nullptr) {
            mastering_voice_->DestroyVoice();
            mastering_voice_ = nullptr;
        }
        if (engine_ != nullptr) {
            engine_->Release();
            engine_ = nullptr;
        }
        module_ = nullptr;
    }

    static void complete(const std::shared_ptr<SoundRequest>& request,
                         sao_status_t status) noexcept {
        {
            std::lock_guard lock(request->done_mutex);
            request->status = status;
            request->done = true;
        }
        request->done_cv.notify_one();
    }

    void cancel_queued_locked() noexcept {
        for (const auto& request : queue_)
            complete(request, SAO_STATUS_ERR_CANCELLED);
        queue_.clear();
    }

    void finish_worker(sao_status_t pending_status) noexcept {
        std::deque<std::shared_ptr<SoundRequest>> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(queue_);
            worker_exited_ = true;
        }
        for (const auto& request : pending)
            complete(request, pending_status);
    }

    void run() noexcept {
        bool com_ready = false;
        bool uninitialize = false;
        sao_status_t pending_status = SAO_STATUS_ERR_CANCELLED;
        try {
            for (;;) {
                std::shared_ptr<SoundRequest> request;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return shutdown_requested_ || !queue_.empty(); });
                    if (queue_.empty())
                        break;
                    request = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (!com_ready) {
                    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    if (FAILED(com_status)) {
                        complete(request, SAO_STATUS_ERR_OS_CALL_FAILED);
                        continue;
                    }
                    com_ready = true;
                    uninitialize = true;
                }
                complete(request, play_on_worker(request->cue, request->volume));
            }
        } catch (...) {
            pending_status = SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        release_engine();
        if (uninitialize)
            CoUninitialize();
        finish_worker(pending_status);
    }

    std::array<CueState, SAO_UI_SOUND_COUNT> cues_{};
    IXAudio2* engine_{};
    IXAudio2MasteringVoice* mastering_voice_{};
    HMODULE module_{};
    std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<SoundRequest>> queue_;
    std::thread worker_;
    bool accepting_{true};
    bool worker_exited_{};
    bool shutdown_requested_{};
};

SoundEngine& sound_engine() {
    static SoundEngine engine;
    return engine;
}

#endif

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_play(SaoUiSoundCue cue, int32_t requested_volume) {
    try {
        if (!valid_cue(cue) || requested_volume < 0 || requested_volume > 100)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (!g_sound_enabled.load(std::memory_order_acquire))
            return SAO_STATUS_OK;
        const int32_t effective_volume =
            std::min(requested_volume, g_sound_volume.load(std::memory_order_acquire));
        if (effective_volume <= 0)
            return SAO_STATUS_OK;
#if defined(_WIN32)
        return sound_engine().play(cue, effective_volume);
#else
        (void)cue;
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_shutdown(void) {
    try {
#if defined(_WIN32)
    return sound_engine().shutdown();
#else
    return SAO_STATUS_OK;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_set_enabled(bool enabled) {
    g_sound_enabled.store(enabled, std::memory_order_release);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_get_enabled(bool* out_enabled) {
    if (out_enabled == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_enabled = g_sound_enabled.load(std::memory_order_acquire);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_set_volume(int32_t volume) {
    if (volume < 0 || volume > 100)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    g_sound_volume.store(volume, std::memory_order_release);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_get_volume(int32_t* out_volume) {
    if (out_volume == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_volume = g_sound_volume.load(std::memory_order_acquire);
    return SAO_STATUS_OK;
}
