// SAO Auto — embedded Python-parity sound catalog and XAudio2 playback.

#include "sao/ui/sound.h"

#include "sound_assets.h"
#include "sound_sequence_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#endif

namespace sao::ui::sound_detail {
struct LinkStartAudioPlayback {
    std::mutex mutex;
    LinkStartAudioSnapshot snapshot;
#if defined(_WIN32)
    IXAudio2SourceVoice* voice{};
    struct Callbacks final : IXAudio2VoiceCallback {
        std::atomic_bool ended{};
        std::atomic_bool failed{};
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
        void STDMETHODCALLTYPE OnStreamEnd() noexcept override {
            ended.store(true, std::memory_order_release);
        }
        void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
        void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {
            failed.store(true, std::memory_order_release);
        }
    } callbacks;
#endif
};
} // namespace sao::ui::sound_detail

namespace {

std::atomic_bool g_sound_enabled{true};
std::atomic<int32_t> g_sound_volume{70};
std::atomic_uint64_t g_next_sound_group{1};
std::atomic_uint64_t g_next_event_scope{1};

struct PendingSoundEvent {
    SaoUiSoundCue cue{SAO_UI_SOUND_CLICK};
    int32_t volume{};
    sao_ui_sound_group_t group{};
    SaoUiSoundPriority priority{SAO_UI_SOUND_PRIORITY_FEEDBACK};
    bool has_offer{};
};

std::mutex g_event_mutex;
std::unordered_map<sao_ui_sound_event_scope_t, PendingSoundEvent> g_event_scopes;
thread_local std::vector<sao_ui_sound_event_scope_t> g_event_scope_stack;

bool valid_cue(SaoUiSoundCue cue) noexcept {
    return cue >= SAO_UI_SOUND_CLICK && cue < SAO_UI_SOUND_COUNT;
}

#if defined(_WIN32)

static_assert(SAO_UI_SOUND_COUNT == 15, "sound resource table must match the public cue enum");

constexpr std::array<int32_t, SAO_UI_SOUND_COUNT> kSoundResourceIds{
    SAO_UI_SOUND_ASSET_CLICK,       SAO_UI_SOUND_ASSET_MENU_OPEN, SAO_UI_SOUND_ASSET_MENU_CLOSE,
    SAO_UI_SOUND_ASSET_PANEL,       SAO_UI_SOUND_ASSET_SUBMENU,   SAO_UI_SOUND_ASSET_ALERT,
    SAO_UI_SOUND_ASSET_ALERT_CLOSE, SAO_UI_SOUND_ASSET_WELCOME,   SAO_UI_SOUND_ASSET_ALO_WELCOME,
    SAO_UI_SOUND_ASSET_LINK_START,  SAO_UI_SOUND_ASSET_NERVEGEAR, SAO_UI_SOUND_ASSET_MESSAGE,
    SAO_UI_SOUND_ASSET_SYSTEM,      SAO_UI_SOUND_ASSET_WARNING,   SAO_UI_SOUND_ASSET_EMERGENCY,
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

sao_status_t load_embedded_wave(SaoUiSoundCue cue, WaveResource* out_wave) noexcept {
    if (!valid_cue(cue) || out_wave == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&g_sound_volume), &module) || module == nullptr)
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    const HRSRC resource = FindResourceW(module,
        MAKEINTRESOURCEW(kSoundResourceIds[static_cast<size_t>(cue)]), RT_RCDATA);
    if (resource == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const auto* bytes = loaded == nullptr ? nullptr : static_cast<const uint8_t*>(LockResource(loaded));
    return bytes == nullptr || size == 0u ? SAO_STATUS_ERR_OS_CALL_FAILED
                                        : parse_pcm_wave(bytes, size, out_wave);
}

sao_status_t load_linkstart_waves(std::array<WaveResource, 3>& waves,
                                  sao::ui::sound_detail::LinkStartAudioSnapshot& output) noexcept {
    constexpr std::array<SaoUiSoundCue, 3> cues{
        SAO_UI_SOUND_LINK_START, SAO_UI_SOUND_NERVEGEAR, SAO_UI_SOUND_ALO_WELCOME};
    sao::ui::sound_detail::LinkStartAudioSnapshot candidate{};
    uint64_t frames = 0u;
    for (size_t index = 0; index < cues.size(); ++index) {
        const auto status = load_embedded_wave(cues[index], &waves[index]);
        if (status != SAO_STATUS_OK)
            return status;
        const auto& format = waves[index].format;
        const auto& first = waves[0].format;
        if (format.nSamplesPerSec != first.nSamplesPerSec || format.nChannels != first.nChannels ||
            format.nBlockAlign != first.nBlockAlign || format.wBitsPerSample != first.wBitsPerSample)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        frames += waves[index].sample_bytes / format.nBlockAlign;
        candidate.cue_end_frames[index] = frames;
    }
    candidate.sample_rate = waves[0].format.nSamplesPerSec;
    output = candidate;
    return SAO_STATUS_OK;
}

void update_sequence_clock_locked(sao::ui::sound_detail::LinkStartAudioPlayback& playback) noexcept {
    if (playback.voice == nullptr)
        return;
    XAUDIO2_VOICE_STATE state{};
    playback.voice->GetState(&state, 0u);
    const uint64_t total = playback.snapshot.cue_end_frames.back();
    if (playback.callbacks.failed.load(std::memory_order_acquire)) {
        playback.snapshot.state = sao::ui::sound_detail::LinkStartPlaybackState::interrupted;
    } else if (playback.callbacks.ended.load(std::memory_order_acquire)) {
        playback.snapshot.samples_played = total;
        playback.snapshot.state = sao::ui::sound_detail::LinkStartPlaybackState::complete;
    } else {
        playback.snapshot.samples_played = std::max(playback.snapshot.samples_played,
                                                     std::min<uint64_t>(state.SamplesPlayed, total));
    }
}

struct SoundRequest {
    SaoUiSoundCue cue{SAO_UI_SOUND_CLICK};
    int32_t volume{};
    sao_ui_sound_group_t group{};
    bool stop_group{};
    bool update_volume{};
    int32_t new_global_volume{};
    std::shared_ptr<std::vector<uint8_t>> custom_wave;
    std::array<WaveResource, 3> sequence_waves{};
    sao::ui::sound_detail::LinkStartAudioHandle sequence;
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

    sao_status_t play(SaoUiSoundCue cue, int32_t volume, sao_ui_sound_group_t group) {
        auto request = std::make_shared<SoundRequest>();
        request->cue = cue;
        request->volume = volume;
        request->group = group;

        return submit(request);
    }

    sao_status_t play_custom(std::shared_ptr<std::vector<uint8_t>> bytes, int32_t volume,
                             sao_ui_sound_group_t group) {
        auto request = std::make_shared<SoundRequest>();
        request->volume = volume;
        request->group = group;
        request->custom_wave = std::move(bytes);
        return submit(request);
    }

    sao_status_t play_sequence(int32_t volume, sao_ui_sound_group_t group,
                               sao::ui::sound_detail::LinkStartAudioHandle* output) {
        auto request = std::make_shared<SoundRequest>();
        request->volume = volume;
        request->group = group;
        request->sequence = std::make_shared<sao::ui::sound_detail::LinkStartAudioPlayback>();
        const auto load_status = load_linkstart_waves(request->sequence_waves,
                                                      request->sequence->snapshot);
        if (load_status != SAO_STATUS_OK)
            return load_status;
        const auto status = submit(request);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(request->sequence->mutex);
            const auto state = request->sequence->snapshot.state;
            if (state == sao::ui::sound_detail::LinkStartPlaybackState::playing ||
                state == sao::ui::sound_detail::LinkStartPlaybackState::complete)
                *output = request->sequence;
        }
        return status;
    }

    sao_status_t update_volume(int32_t volume) {
        auto request = std::make_shared<SoundRequest>();
        request->update_volume = true;
        request->new_global_volume = volume;
        return submit(request);
    }

    sao_status_t stop_group(sao_ui_sound_group_t group) {
        if (group == 0)
            return SAO_STATUS_OK;
        auto request = std::make_shared<SoundRequest>();
        request->group = group;
        request->stop_group = true;
        return submit(request);
    }

  private:
    sao_status_t submit(const std::shared_ptr<SoundRequest>& request) {
        {
            std::lock_guard lock(mutex_);
            if (!accepting_)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (worker_exited_)
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            if (queue_.size() >= 64U)
                return SAO_STATUS_ERR_TIMEOUT;
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

  public:
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
        struct VoiceSlot {
            IXAudio2SourceVoice* voice{};
            sao_ui_sound_group_t group{};
            int32_t requested_volume{};
        };
        std::array<VoiceSlot, 2> voices{};
        sao_status_t load_status{SAO_STATUS_ERR_NOT_INITIALIZED};
        bool load_attempted{};
    };

    struct CustomVoiceSlot {
        IXAudio2SourceVoice* voice{};
        sao_ui_sound_group_t group{};
        int32_t requested_volume{};
        std::shared_ptr<std::vector<uint8_t>> bytes;
    };

    struct SequenceVoiceSlot {
        IXAudio2SourceVoice* voice{};
        sao_ui_sound_group_t group{};
        int32_t requested_volume{};
        sao::ui::sound_detail::LinkStartAudioHandle playback;
    };

    static void release_sequence_slot(SequenceVoiceSlot& slot) noexcept {
        const auto playback = slot.playback;
        if (slot.voice != nullptr && playback) {
            std::lock_guard lock(playback->mutex);
            update_sequence_clock_locked(*playback);
            if (playback->snapshot.state != sao::ui::sound_detail::LinkStartPlaybackState::complete)
                playback->snapshot.state = sao::ui::sound_detail::LinkStartPlaybackState::interrupted;
            playback->voice = nullptr;
        }
        if (slot.voice != nullptr)
            slot.voice->DestroyVoice();
        slot = {};
    }

    void prune_sequence_voices_on_worker() noexcept {
        for (auto& slot : sequence_voices_) {
            if (slot.voice == nullptr)
                continue;
            bool finished = false;
            {
                std::lock_guard lock(slot.playback->mutex);
                update_sequence_clock_locked(*slot.playback);
                finished = slot.playback->snapshot.state ==
                               sao::ui::sound_detail::LinkStartPlaybackState::complete ||
                           slot.playback->snapshot.state ==
                               sao::ui::sound_detail::LinkStartPlaybackState::interrupted;
            }
            if (finished)
                release_sequence_slot(slot);
        }
    }

    sao_status_t play_sequence_on_worker(const SoundRequest& request) noexcept {
        const int32_t volume = std::min(request.volume, g_sound_volume.load(std::memory_order_acquire));
        if (!g_sound_enabled.load(std::memory_order_acquire) || volume <= 0)
            return SAO_STATUS_OK;
        const auto status = ensure_engine();
        if (status != SAO_STATUS_OK)
            return status;
        prune_sequence_voices_on_worker();
        SequenceVoiceSlot* selected = nullptr;
        for (auto& slot : sequence_voices_) {
            if (slot.voice == nullptr) {
                selected = &slot;
                break;
            }
        }
        if (selected == nullptr)
            return SAO_STATUS_ERR_TIMEOUT;
        IXAudio2SourceVoice* voice = nullptr;
        HRESULT result = engine_->CreateSourceVoice(&voice, &request.sequence_waves[0].format,
            0u, XAUDIO2_DEFAULT_FREQ_RATIO, &request.sequence->callbacks);
        if (FAILED(result) || voice == nullptr)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        result = voice->SetVolume(static_cast<float>(volume) / 100.0F);
        for (size_t index = 0; SUCCEEDED(result) && index < request.sequence_waves.size(); ++index) {
            XAUDIO2_BUFFER buffer{};
            buffer.Flags = index + 1u == request.sequence_waves.size() ? XAUDIO2_END_OF_STREAM : 0u;
            buffer.AudioBytes = request.sequence_waves[index].sample_bytes;
            buffer.pAudioData = request.sequence_waves[index].samples;
            result = voice->SubmitSourceBuffer(&buffer);
        }
        if (SUCCEEDED(result))
            result = voice->Start(0u);
        if (FAILED(result)) {
            voice->DestroyVoice();
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        {
            std::lock_guard lock(request.sequence->mutex);
            request.sequence->voice = voice;
            request.sequence->snapshot.state = sao::ui::sound_detail::LinkStartPlaybackState::playing;
        }
        *selected = {voice, request.group, request.volume, request.sequence};
        return SAO_STATUS_OK;
    }

    static bool short_feedback_cue(SaoUiSoundCue cue) noexcept {
        return cue == SAO_UI_SOUND_CLICK || cue == SAO_UI_SOUND_SUBMENU ||
               cue == SAO_UI_SOUND_ALERT_CLOSE || cue == SAO_UI_SOUND_MESSAGE;
    }

    void stop_group_on_worker(sao_ui_sound_group_t group) noexcept {
        if (group == 0)
            return;
        for (auto& cue : cues_) {
            for (auto& slot : cue.voices) {
                if (slot.voice != nullptr && slot.group == group) {
                    slot.voice->DestroyVoice();
                    slot.voice = nullptr;
                    slot.group = 0;
                    slot.requested_volume = 0;
                }
            }
        }
        for (auto& slot : custom_voices_) {
            if (slot.voice != nullptr && slot.group == group) {
                slot.voice->DestroyVoice();
                slot = {};
            }
        }
        for (auto& slot : sequence_voices_) {
            if (slot.group == group)
                release_sequence_slot(slot);
        }
    }

    sao_status_t play_on_worker(SaoUiSoundCue cue, int32_t volume,
                                sao_ui_sound_group_t group) noexcept {
        const int32_t requested_volume = volume;
        volume = std::min(volume, g_sound_volume.load(std::memory_order_acquire));
        if (!g_sound_enabled.load(std::memory_order_acquire) || volume <= 0)
            return SAO_STATUS_OK;
        sao_status_t status = ensure_engine();
        if (status != SAO_STATUS_OK)
            return status;
        CueState& state = cues_[static_cast<size_t>(cue)];
        status = ensure_cue(cue, &state);
        if (status != SAO_STATUS_OK)
            return status;

        const size_t cue_index = static_cast<size_t>(cue);
        if (short_feedback_cue(cue)) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - last_short_play_[cue_index];
            if (elapsed < std::chrono::milliseconds(80))
                return SAO_STATUS_OK;
            last_short_play_[cue_index] = now;
        }

        const size_t voice_limit = short_feedback_cue(cue) ? state.voices.size() : 1U;
        CueState::VoiceSlot* selected = nullptr;
        for (size_t index = 0; index < voice_limit; ++index) {
            auto& slot = state.voices[index];
            if (slot.voice != nullptr) {
                XAUDIO2_VOICE_STATE voice_state{};
                slot.voice->GetState(&voice_state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (voice_state.BuffersQueued == 0U) {
                    slot.voice->DestroyVoice();
                    slot.voice = nullptr;
                    slot.group = 0;
                    slot.requested_volume = 0;
                }
            }
            if (slot.voice == nullptr) {
                selected = &slot;
                break;
            }
        }
        if (selected == nullptr) {
            selected = &state.voices[0];
            selected->voice->DestroyVoice();
            selected->voice = nullptr;
            selected->group = 0;
            selected->requested_volume = 0;
        }
        HRESULT result = engine_->CreateSourceVoice(&selected->voice, &state.wave.format);
        if (SUCCEEDED(result) && selected->voice != nullptr)
            selected->group = group;
        else if (SUCCEEDED(result))
            result = E_FAIL;
        if (SUCCEEDED(result))
            result = selected->voice->SetVolume(static_cast<float>(volume) / 100.0F);
        if (SUCCEEDED(result))
            selected->requested_volume = requested_volume;
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = state.wave.sample_bytes;
        buffer.pAudioData = state.wave.samples;
        if (SUCCEEDED(result))
            result = selected->voice->SubmitSourceBuffer(&buffer);
        if (SUCCEEDED(result))
            result = selected->voice->Start(0U);
        if (SUCCEEDED(result))
            return SAO_STATUS_OK;
        release_engine();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    sao_status_t play_custom_on_worker(const std::shared_ptr<std::vector<uint8_t>>& bytes,
                                       int32_t requested_volume,
                                       sao_ui_sound_group_t group) noexcept {
        if (bytes == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const int32_t volume =
            std::min(requested_volume, g_sound_volume.load(std::memory_order_acquire));
        if (!g_sound_enabled.load(std::memory_order_acquire) || volume <= 0)
            return SAO_STATUS_OK;
        sao_status_t status = ensure_engine();
        if (status != SAO_STATUS_OK)
            return status;
        WaveResource wave{};
        status = parse_pcm_wave(bytes->data(), bytes->size(), &wave);
        if (status != SAO_STATUS_OK)
            return status;
        CustomVoiceSlot* selected = nullptr;
        for (auto& slot : custom_voices_) {
            if (slot.voice != nullptr) {
                XAUDIO2_VOICE_STATE state{};
                slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (state.BuffersQueued == 0U) {
                    slot.voice->DestroyVoice();
                    slot = {};
                }
            }
            if (selected == nullptr && slot.voice == nullptr)
                selected = &slot;
        }
        if (selected == nullptr) {
            selected = &custom_voices_[0];
            selected->voice->DestroyVoice();
            *selected = {};
        }
        HRESULT result = engine_->CreateSourceVoice(&selected->voice, &wave.format);
        if (FAILED(result) || selected->voice == nullptr) {
            *selected = {};
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        selected->group = group;
        selected->requested_volume = requested_volume;
        selected->bytes = bytes;
        result = selected->voice->SetVolume(static_cast<float>(volume) / 100.0F);
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = wave.sample_bytes;
        buffer.pAudioData = wave.samples;
        if (SUCCEEDED(result))
            result = selected->voice->SubmitSourceBuffer(&buffer);
        if (SUCCEEDED(result))
            result = selected->voice->Start(0U);
        if (SUCCEEDED(result))
            return SAO_STATUS_OK;
        selected->voice->DestroyVoice();
        *selected = {};
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    void update_active_volume_on_worker(int32_t global_volume) noexcept {
        const auto update = [global_volume](auto& slot) {
            if (slot.voice != nullptr) {
                const float volume =
                    static_cast<float>(std::min(slot.requested_volume, global_volume)) / 100.0F;
                (void)slot.voice->SetVolume(volume);
            }
        };
        for (auto& cue : cues_)
            for (auto& slot : cue.voices)
                update(slot);
        for (auto& slot : custom_voices_)
            update(slot);
        for (auto& slot : sequence_voices_)
            update(slot);
    }

    void prune_finished_voices_on_worker() noexcept {
        const auto prune = [](auto& slot) {
            if (slot.voice == nullptr)
                return;
            XAUDIO2_VOICE_STATE state{};
            slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0U) {
                slot.voice->DestroyVoice();
                slot = {};
            }
        };
        for (auto& cue : cues_)
            for (auto& slot : cue.voices)
                prune(slot);
        for (auto& slot : custom_voices_)
            prune(slot);
        prune_sequence_voices_on_worker();
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
            state->load_status = load_embedded_wave(cue, &state->wave);
        }
        if (state->load_status != SAO_STATUS_OK)
            return state->load_status;
        return SAO_STATUS_OK;
    }

    void release_engine() noexcept {
        for (auto& slot : sequence_voices_)
            release_sequence_slot(slot);
        for (auto& cue : cues_) {
            for (auto& slot : cue.voices) {
                if (slot.voice != nullptr) {
                    slot.voice->DestroyVoice();
                    slot.voice = nullptr;
                    slot.group = 0;
                    slot.requested_volume = 0;
                }
            }
        }
        for (auto& slot : custom_voices_) {
            if (slot.voice != nullptr)
                slot.voice->DestroyVoice();
            slot = {};
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
                    cv_.wait_for(lock, std::chrono::milliseconds(50),
                                 [this] { return shutdown_requested_ || !queue_.empty(); });
                    if (queue_.empty()) {
                        if (shutdown_requested_)
                            break;
                        lock.unlock();
                        prune_finished_voices_on_worker();
                        continue;
                    }
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
                if (request->stop_group) {
                    stop_group_on_worker(request->group);
                    complete(request, SAO_STATUS_OK);
                } else if (request->update_volume) {
                    update_active_volume_on_worker(request->new_global_volume);
                    complete(request, SAO_STATUS_OK);
                } else if (request->sequence) {
                    complete(request, play_sequence_on_worker(*request));
                } else if (request->custom_wave != nullptr) {
                    complete(request, play_custom_on_worker(request->custom_wave, request->volume,
                                                            request->group));
                } else {
                    complete(request,
                             play_on_worker(request->cue, request->volume, request->group));
                }
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
    std::array<CustomVoiceSlot, 8> custom_voices_{};
    std::array<SequenceVoiceSlot, 4> sequence_voices_{};
    std::array<std::chrono::steady_clock::time_point, SAO_UI_SOUND_COUNT> last_short_play_{};
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

#if defined(_WIN32)
constexpr uint64_t kMaximumCustomWaveBytes = 16ull * 1024ull * 1024ull;

sao_status_t read_wave_file(const wchar_t* path, std::shared_ptr<std::vector<uint8_t>>* out_bytes) {
    if (path == nullptr || path[0] == L'\0' || out_bytes == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND
                   ? SAO_STATUS_ERR_NOT_FOUND
                   : SAO_STATUS_ERR_OS_CALL_FAILED;
    struct FileClose {
        HANDLE value;
        ~FileClose() {
            CloseHandle(value);
        }
    } close{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaximumCustomWaveBytes) {

        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes->size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes->size() - offset, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(file, bytes->data() + offset, chunk, &read, nullptr) || read == 0) {

            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        offset += read;
    }

    WaveResource wave{};
    const sao_status_t status = parse_pcm_wave(bytes->data(), bytes->size(), &wave);
    if (status != SAO_STATUS_OK)
        return status;
    *out_bytes = std::move(bytes);
    return SAO_STATUS_OK;
}

bool utf8_path_to_wide(const char* path, std::wstring* out) {
    if (path == nullptr || out == nullptr)
        return false;
    const size_t length = strnlen_s(path, 65536u);
    if (length == 0 || length >= 65536u || length > INT_MAX)
        return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                          static_cast<int>(length), nullptr, 0);
    if (count <= 0)
        return false;
    out->assign(static_cast<size_t>(count), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(length),
                               out->data(), count) == count;
}

#endif

} // namespace

namespace sao::ui::sound_detail {

sao_status_t linkstart_audio_info(LinkStartAudioSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
#if defined(_WIN32)
    std::array<WaveResource, 3> waves{};
    return load_linkstart_waves(waves, *out_snapshot);
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

sao_status_t linkstart_audio_begin(sao_ui_sound_group_t group, int32_t requested_volume,
                                   LinkStartAudioHandle* out_playback) noexcept {
    if (out_playback == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out_playback->reset();
    if (group == 0u || requested_volume < 0 || requested_volume > 100)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!g_sound_enabled.load(std::memory_order_acquire) || requested_volume == 0 ||
        g_sound_volume.load(std::memory_order_acquire) == 0)
        return SAO_STATUS_OK;
#if defined(_WIN32)
    try {
        return sound_engine().play_sequence(requested_volume, group, out_playback);
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

sao_status_t linkstart_audio_snapshot(const LinkStartAudioHandle& playback,
                                      LinkStartAudioSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    if (!playback)
        return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(_WIN32)
    try {
        std::lock_guard lock(playback->mutex);
        update_sequence_clock_locked(*playback);
        *out_snapshot = playback->snapshot;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::ui::sound_detail

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_play(SaoUiSoundCue cue, int32_t requested_volume) {
    return sao_ui_sound_play_in_group(cue, requested_volume, 0);
}

SaoUiSoundPriority priority_for_cue(SaoUiSoundCue cue) noexcept {
    if (cue == SAO_UI_SOUND_EMERGENCY)
        return SAO_UI_SOUND_PRIORITY_EMERGENCY;
    if (cue == SAO_UI_SOUND_WARNING || cue == SAO_UI_SOUND_ALERT || cue == SAO_UI_SOUND_ALERT_CLOSE)
        return SAO_UI_SOUND_PRIORITY_ALERT;
    if (cue == SAO_UI_SOUND_PANEL || cue == SAO_UI_SOUND_WELCOME || cue == SAO_UI_SOUND_ALO_WELCOME)
        return SAO_UI_SOUND_PRIORITY_PANEL;
    if (cue == SAO_UI_SOUND_MENU_OPEN || cue == SAO_UI_SOUND_MENU_CLOSE ||
        cue == SAO_UI_SOUND_SUBMENU || cue == SAO_UI_SOUND_LINK_START ||
        cue == SAO_UI_SOUND_NERVEGEAR || cue == SAO_UI_SOUND_SYSTEM)
        return SAO_UI_SOUND_PRIORITY_NAVIGATION;
    return SAO_UI_SOUND_PRIORITY_FEEDBACK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_play_in_group(SaoUiSoundCue cue,
                                                               int32_t requested_volume,
                                                               sao_ui_sound_group_t group) {
    try {
        if (!valid_cue(cue) || requested_volume < 0 || requested_volume > 100)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (group == 0 && !g_event_scope_stack.empty()) {
            return sao_ui_sound_event_offer(g_event_scope_stack.back(), cue, requested_volume,
                                            priority_for_cue(cue));
        }
        if (!g_sound_enabled.load(std::memory_order_acquire))
            return SAO_STATUS_OK;
        const int32_t effective_volume =
            std::min(requested_volume, g_sound_volume.load(std::memory_order_acquire));
        if (effective_volume <= 0)
            return SAO_STATUS_OK;
#if defined(_WIN32)
        return sound_engine().play(cue, requested_volume, group);
#else
        (void)cue;
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_play_semantic(SaoUiSoundSemantic semantic,
                                                               int32_t requested_volume,
                                                               sao_ui_sound_group_t group) {
    SaoUiSoundCue cue = SAO_UI_SOUND_CLICK;
    if (semantic == SAO_UI_SOUND_SEMANTIC_CONFIRM)
        cue = SAO_UI_SOUND_CLICK;
    else if (semantic == SAO_UI_SOUND_SEMANTIC_CANCEL)
        cue = SAO_UI_SOUND_ALERT_CLOSE;
    else
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return sao_ui_sound_play_in_group(cue, requested_volume, group);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_play_wav_utf16(const uint16_t* path_utf16,
                                                                int32_t requested_volume,
                                                                sao_ui_sound_group_t group) {
    if (path_utf16 == nullptr || requested_volume < 0 || requested_volume > 100)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    const auto* path = reinterpret_cast<const wchar_t*>(path_utf16);
    const size_t length = wcsnlen_s(path, 32768u);
    if (length == 0 || length >= 32768u)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::shared_ptr<std::vector<uint8_t>> bytes;
        const sao_status_t status = read_wave_file(path, &bytes);
        if (status != SAO_STATUS_OK)
            return status;
        return sound_engine().play_custom(std::move(bytes), requested_volume, group);
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#else
    (void)group;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_play_wav_utf8(const char* path_utf8,
                                                               int32_t requested_volume,
                                                               sao_ui_sound_group_t group) {
    if (path_utf8 == nullptr || requested_volume < 0 || requested_volume > 100)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    try {
        std::wstring path;
        if (!utf8_path_to_wide(path_utf8, &path))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return sao_ui_sound_play_wav_utf16(reinterpret_cast<const uint16_t*>(path.c_str()),
                                           requested_volume, group);
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#else
    (void)group;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_group_create(sao_ui_sound_group_t* out_group) {
    if (out_group == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const sao_ui_sound_group_t next = g_next_sound_group.fetch_add(1, std::memory_order_relaxed);
    if (next == 0)
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    *out_group = next;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_group_stop(sao_ui_sound_group_t group) {
    try {
        if (group == 0)
            return SAO_STATUS_OK;
#if defined(_WIN32)
        return sound_engine().stop_group(group);
#else
        return SAO_STATUS_OK;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_group_destroy(sao_ui_sound_group_t group) {
    return sao_ui_sound_group_stop(group);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_sound_event_begin(sao_ui_sound_group_t group, sao_ui_sound_event_scope_t* out_scope) {
    try {
        if (out_scope == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const sao_ui_sound_event_scope_t scope =
            g_next_event_scope.fetch_add(1, std::memory_order_relaxed);
        if (scope == 0)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        std::lock_guard lock(g_event_mutex);
        if (g_event_scopes.size() >= 256U)
            return SAO_STATUS_ERR_TIMEOUT;
        g_event_scope_stack.push_back(scope);
        try {
            g_event_scopes.emplace(scope, PendingSoundEvent{.group = group});
        } catch (...) {
            g_event_scope_stack.pop_back();
            throw;
        }
        *out_scope = scope;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_event_offer(sao_ui_sound_event_scope_t scope,
                                                             SaoUiSoundCue cue,
                                                             int32_t requested_volume,
                                                             SaoUiSoundPriority priority) {
    try {
        if (!valid_cue(cue) || requested_volume < 0 || requested_volume > 100 ||
            priority < SAO_UI_SOUND_PRIORITY_FEEDBACK || priority > SAO_UI_SOUND_PRIORITY_EMERGENCY)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_event_mutex);
        const auto found = g_event_scopes.find(scope);
        if (found == g_event_scopes.end())
            return SAO_STATUS_ERR_NOT_FOUND;
        PendingSoundEvent& pending = found->second;
        if (!pending.has_offer || priority > pending.priority) {
            pending.cue = cue;
            pending.volume = requested_volume;
            pending.priority = priority;
            pending.has_offer = true;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_event_commit(sao_ui_sound_event_scope_t scope) {
    try {
        PendingSoundEvent pending{};
        sao_ui_sound_event_scope_t parent = 0;
        {
            std::lock_guard lock(g_event_mutex);
            const auto found = g_event_scopes.find(scope);
            if (found == g_event_scopes.end())
                return SAO_STATUS_ERR_NOT_FOUND;
            if (!g_event_scope_stack.empty() && g_event_scope_stack.back() == scope) {
                g_event_scope_stack.pop_back();
                if (!g_event_scope_stack.empty())
                    parent = g_event_scope_stack.back();
            }
            pending = found->second;
            g_event_scopes.erase(found);
            if (parent != 0 && pending.has_offer) {
                const auto parent_found = g_event_scopes.find(parent);
                if (parent_found != g_event_scopes.end() &&
                    (!parent_found->second.has_offer ||
                     pending.priority > parent_found->second.priority)) {
                    pending.group = parent_found->second.group;
                    parent_found->second = pending;
                }
            }
        }
        if (parent != 0)
            return SAO_STATUS_OK;
        return pending.has_offer
                   ? sao_ui_sound_play_in_group(pending.cue, pending.volume, pending.group)
                   : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_event_cancel(sao_ui_sound_event_scope_t scope) {
    try {
        std::lock_guard lock(g_event_mutex);
        const auto erased = g_event_scopes.erase(scope);
        if (!g_event_scope_stack.empty() && g_event_scope_stack.back() == scope)
            g_event_scope_stack.pop_back();
        return erased != 0 ? SAO_STATUS_OK : SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
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
    const bool previous = g_sound_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (!enabled && previous)
        return sao_ui_sound_shutdown();
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
    const int32_t previous = g_sound_volume.exchange(volume, std::memory_order_acq_rel);
    if (volume == 0 && previous != 0)
        return sao_ui_sound_shutdown();
#if defined(_WIN32)
    if (volume != 0 && volume != previous)
        return sound_engine().update_volume(volume);
#endif
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sound_get_volume(int32_t* out_volume) {
    if (out_volume == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_volume = g_sound_volume.load(std::memory_order_acquire);
    return SAO_STATUS_OK;
}
