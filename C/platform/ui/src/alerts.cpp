// SAO Auto — game-agnostic alerts / TTS / banner / sound.
//
// See `include/sao/ui/alerts.h` for the ABI contract.

#include "sao/ui/alerts.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <sapi.h>
// sphelper.h uses ATL (`CComPtr` etc.) which BuildTools (non-ATL)
// images don't ship.  We access ISpObjectToken / IEnumSpObjectTokens
// / ISpDataKey directly via CoCreateInstance + QueryInterface below.
#  include <mmsystem.h>
#endif

namespace {

// ─── Banner queue ────────────────────────────────────────────────
//
// The queue lives here.  Producers push via `banner_show`.  Consumers
// (compositor) drain via `banner_drain`.  `banner_hide` marks a slot
// cancelled so a lagging consumer can skip rendering.

struct BannerEntry {
    uint64_t              banner_id = 0;
    uint32_t              duration_ms = 0;
    int32_t               color_id = 0;
    std::vector<uint16_t> text_storage;
    bool                  cancelled = false;
    bool                  drained   = false;
};

class BannerQueue {
public:
    uint64_t push(const uint16_t* text, size_t n,
                  uint32_t duration_ms, int32_t color_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t id = ++next_id_;
        auto entry = std::make_shared<BannerEntry>();
        entry->banner_id   = id;
        entry->duration_ms = duration_ms;
        entry->color_id    = color_id;
        entry->text_storage.assign(text, text + n);
        // Null-terminate for consumer convenience.
        entry->text_storage.push_back(0);
        entries_.push_back(std::move(entry));
        return id;
    }

    void hide(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry->banner_id == id) {
                entry->cancelled = true;
                return;
            }
        }
        retained_.erase(id);
    }

    sao_status_t drain(SaoUiAlertBanner* out, size_t capacity,
                       size_t* out_count) {
        if (out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        if (out == nullptr || capacity == 0) {
            // Query mode — count active (not cancelled, not drained).
            size_t n = 0;
            for (const auto& entry : entries_) {
                if (!entry->cancelled && !entry->drained) ++n;
            }
            *out_count = n;
            return SAO_STATUS_OK;
        }
        size_t written = 0;
        for (const auto& entry : entries_) {
            if (written >= capacity) break;
            if (entry->cancelled || entry->drained) continue;
            SaoUiAlertBanner& rec = out[written++];
            rec.banner_id   = entry->banner_id;
            rec.duration_ms = static_cast<int32_t>(entry->duration_ms);
            rec.color_id    = entry->color_id;
            rec.text_utf16  = entry->text_storage.data();
            rec.text_len    = entry->text_storage.size() > 0
                              ? entry->text_storage.size() - 1  // exclude NUL
                              : 0;
            entry->drained = true;
            retained_[entry->banner_id] = entry;
        }
        std::erase_if(entries_, [](const auto& entry) {
            return entry->cancelled || entry->drained;
        });
        *out_count = written;
        return SAO_STATUS_OK;
    }

private:
    std::mutex             mutex_;
    std::deque<std::shared_ptr<BannerEntry>> entries_;
    std::unordered_map<uint64_t, std::shared_ptr<BannerEntry>> retained_;
    uint64_t               next_id_ = 0;
};

BannerQueue& banner_queue() {
    static BannerQueue q;
    return q;
}

// ─── Sound tracking ──────────────────────────────────────────────

struct SoundState {
    std::mutex                       mutex;
    std::unordered_set<uint64_t>     active;
    uint64_t                         next_id = 0;
};

SoundState& sound_state() {
    static SoundState s;
    return s;
}

// ─── SAPI singleton ──────────────────────────────────────────────
#if defined(_WIN32)
struct SapiVoice {
    std::mutex   mutex;
    ISpVoice*    voice = nullptr;
    bool         com_initialized = false;
    std::string  current_voice_id;

    ~SapiVoice() {
        if (voice) {
            voice->Release();
            voice = nullptr;
        }
        if (com_initialized) {
            CoUninitialize();
        }
    }

    bool ensure_initialized_locked() {
        if (voice) return true;
        if (!com_initialized) {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // S_OK == already initialised in this thread with the
            // same model.  RPC_E_CHANGED_MODE means somebody already
            // set STA — still usable via CoCreateInstance.
            if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
                com_initialized = SUCCEEDED(hr);
            } else {
                return false;
            }
        }
        HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr,
                                      CLSCTX_ALL, IID_ISpVoice,
                                      reinterpret_cast<void**>(&voice));
        if (FAILED(hr) || voice == nullptr) {
            voice = nullptr;
            return false;
        }
        return true;
    }

    // Enumerate SAPI voice tokens without ATL/sphelper.  Returns the
    // enumerator via out parameter — caller must Release() when done.
    static HRESULT enum_voice_tokens(IEnumSpObjectTokens** out_enum) {
        if (out_enum == nullptr) return E_POINTER;
        *out_enum = nullptr;
        // Create an ISpObjectTokenCategory for SPCAT_VOICES, then ask
        // it for the token enumerator.
        ISpObjectTokenCategory* category = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory,
                                      nullptr, CLSCTX_ALL,
                                      IID_ISpObjectTokenCategory,
                                      reinterpret_cast<void**>(&category));
        if (FAILED(hr) || category == nullptr) return hr;
        hr = category->SetId(SPCAT_VOICES, FALSE);
        if (SUCCEEDED(hr)) {
            hr = category->EnumTokens(nullptr, nullptr, out_enum);
        }
        category->Release();
        return hr;
    }

    // Extract the display description ("Name") from a SAPI voice
    // token without ATL/sphelper.  Returns a CoTaskMem UTF-16 buffer
    // that the caller must free with CoTaskMemFree.
    static HRESULT get_token_description(ISpObjectToken* token,
                                          WCHAR** out_desc) {
        if (token == nullptr || out_desc == nullptr) return E_POINTER;
        *out_desc = nullptr;
        // SAPI convention: the voice name lives in the token's
        // default value.  ISpDataKey::GetStringValue with NULL name
        // returns it.
        return token->GetStringValue(nullptr, out_desc);
    }

    bool select_voice_locked(const std::string& id) {
        if (!voice) return false;
        if (id.empty()) {
            // Reset to default.
            voice->SetVoice(nullptr);
            current_voice_id.clear();
            return true;
        }
        if (id == current_voice_id) return true;
        IEnumSpObjectTokens* tokens = nullptr;
        HRESULT hr = enum_voice_tokens(&tokens);
        if (FAILED(hr) || tokens == nullptr) return false;
        ISpObjectToken* token = nullptr;
        ULONG fetched = 0;
        bool found = false;
        while (tokens->Next(1, &token, &fetched) == S_OK && token) {
            WCHAR* desc = nullptr;
            hr = get_token_description(token, &desc);
            if (SUCCEEDED(hr) && desc) {
                // Convert UTF-16 → UTF-8 for comparison.
                int need = WideCharToMultiByte(CP_UTF8, 0, desc, -1,
                                               nullptr, 0, nullptr,
                                               nullptr);
                std::vector<char> utf8(need > 0 ? static_cast<size_t>(need) : 0u);
                if (need > 0) {
                    WideCharToMultiByte(CP_UTF8, 0, desc, -1,
                                        utf8.data(), need, nullptr,
                                        nullptr);
                }
                const std::string u8 = need > 0 ? std::string(utf8.data()) : std::string{};
                if (u8 == id) {
                    voice->SetVoice(token);
                    current_voice_id = id;
                    found = true;
                    CoTaskMemFree(desc);
                    token->Release();
                    break;
                }
                CoTaskMemFree(desc);
            }
            token->Release();
            token = nullptr;
        }
        tokens->Release();
        return found;
    }
};

SapiVoice& sapi_voice() {
    static SapiVoice v;
    return v;
}
#endif  // _WIN32

}  // namespace

// ─── Public ABI: TTS ─────────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_speak(
    const uint16_t* text_utf16,
    const char*     voice_id_utf8,
    int32_t         rate,
    int32_t         volume) {
    // Empty text is a legitimate no-op — used by callers that toggle
    // TTS without inventing sentinels for "silence".
    if (text_utf16 == nullptr || text_utf16[0] == 0) {
        return SAO_STATUS_OK;
    }
#if defined(_WIN32)
    auto& sv = sapi_voice();
    std::lock_guard<std::mutex> lock(sv.mutex);
    if (!sv.ensure_initialized_locked()) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (voice_id_utf8 && voice_id_utf8[0]) {
        // Try to select — a missing voice degrades to default rather
        // than failing the speak call.
        sv.select_voice_locked(voice_id_utf8);
    }
    // Clamp rate/volume to SAPI's documented ranges.
    if (rate   < -10) rate = -10;
    if (rate   >  10) rate = 10;
    if (volume <   0) volume = 0;
    if (volume > 100) volume = 100;
    sv.voice->SetRate(rate);
    sv.voice->SetVolume(static_cast<USHORT>(volume));
    HRESULT hr = sv.voice->Speak(
        reinterpret_cast<LPCWSTR>(text_utf16),
        SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    if (FAILED(hr)) return SAO_STATUS_ERR_OS_CALL_FAILED;
    return SAO_STATUS_OK;
#else
    (void)text_utf16; (void)voice_id_utf8; (void)rate; (void)volume;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_enum_voices(
    char*   out_names,
    size_t  capacity,
    size_t  name_stride_bytes,
    size_t* out_count) {
    if (out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if defined(_WIN32)
    auto& sv = sapi_voice();
    std::lock_guard<std::mutex> lock(sv.mutex);
    if (!sv.ensure_initialized_locked()) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    IEnumSpObjectTokens* tokens = nullptr;
    HRESULT hr = SapiVoice::enum_voice_tokens(&tokens);
    if (FAILED(hr) || tokens == nullptr) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    ULONG total = 0;
    tokens->GetCount(&total);
    if (out_names == nullptr || capacity == 0 ||
        name_stride_bytes == 0) {
        *out_count = total;
        tokens->Release();
        return SAO_STATUS_OK;
    }
    ISpObjectToken* token = nullptr;
    ULONG fetched = 0;
    size_t written = 0;
    while (written < capacity &&
           tokens->Next(1, &token, &fetched) == S_OK && token) {
        WCHAR* desc = nullptr;
        hr = SapiVoice::get_token_description(token, &desc);
        if (SUCCEEDED(hr) && desc) {
            char* slot = out_names + written * name_stride_bytes;
            int wrote = WideCharToMultiByte(
                CP_UTF8, 0, desc, -1, slot,
                static_cast<int>(name_stride_bytes), nullptr, nullptr);
            if (wrote <= 0 && name_stride_bytes > 0) {
                slot[0] = 0;
            }
            CoTaskMemFree(desc);
        } else if (name_stride_bytes > 0) {
            out_names[written * name_stride_bytes] = 0;
        }
        token->Release();
        token = nullptr;
        ++written;
    }
    tokens->Release();
    *out_count = written;
    return SAO_STATUS_OK;
#else
    (void)out_names; (void)capacity; (void)name_stride_bytes;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

// ─── Public ABI: Banner ──────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_show(
    const uint16_t* text_utf16,
    uint32_t        duration_ms,
    int32_t         color_id,
    uint64_t*       out_banner_id) {
    if (out_banner_id == nullptr || text_utf16 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    size_t n = 0;
    while (text_utf16[n] != 0) ++n;
    uint64_t id = banner_queue().push(text_utf16, n, duration_ms,
                                      color_id);
    *out_banner_id = id;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_hide(
    uint64_t banner_id) {
    banner_queue().hide(banner_id);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_drain(
    SaoUiAlertBanner* out_records,
    size_t            capacity,
    size_t*           out_count) {
    return banner_queue().drain(out_records, capacity, out_count);
}

// ─── Public ABI: Sound ───────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_sound_play(
    const uint16_t* path_utf16,
    int32_t         volume,
    uint64_t*       out_sound_id) {
    if (path_utf16 == nullptr || path_utf16[0] == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_sound_id) *out_sound_id = 0;
#if defined(_WIN32)
    // Verify file exists — PlaySoundW returns TRUE and silently plays
    // the default beep on missing files, which is not useful for
    // callers wanting a "file missing" error.
    DWORD attrs = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(path_utf16));
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    // Set the wave output volume — same-value both channels.
    DWORD vol_word = (static_cast<DWORD>(volume) * 0xFFFF / 100);
    DWORD vol_both = (vol_word << 16) | vol_word;
    waveOutSetVolume(nullptr, vol_both);
    BOOL ok = PlaySoundW(reinterpret_cast<LPCWSTR>(path_utf16),
                         nullptr, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
    if (!ok) return SAO_STATUS_ERR_OS_CALL_FAILED;
    auto& ss = sound_state();
    std::lock_guard<std::mutex> lock(ss.mutex);
    uint64_t id = ++ss.next_id;
    ss.active.insert(id);
    if (out_sound_id) *out_sound_id = id;
    return SAO_STATUS_OK;
#else
    (void)path_utf16; (void)volume;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_sound_stop(
    uint64_t sound_id) {
#if defined(_WIN32)
    auto& ss = sound_state();
    std::lock_guard<std::mutex> lock(ss.mutex);
    if (sound_id == 0) {
        // Stop all — PlaySound(NULL, NULL, 0) halts the async output.
        PlaySoundW(nullptr, nullptr, 0);
        ss.active.clear();
        return SAO_STATUS_OK;
    }
    if (ss.active.erase(sound_id) == 0) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    // winmm PlaySound has one async output channel — stopping one
    // stops the current playback.  Callers wanting overlapping sounds
    // should use a proper audio API.
    PlaySoundW(nullptr, nullptr, 0);
    return SAO_STATUS_OK;
#else
    (void)sound_id;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
