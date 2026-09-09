// SAO Auto — game-agnostic alerts / TTS / banner / sound.
//
// See `include/sao/ui/alerts.h` for the ABI contract.

#include "sao/ui/alerts.h"
#include "sao/ui/sound.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
#endif

namespace {

constexpr size_t kMaximumSapiUtf16Units = 64u * 1024u;
constexpr size_t kMaximumSapiUtf8Bytes = 64u * 1024u;

bool bounded_utf16_length(const uint16_t* value, size_t* out_length) noexcept {
    if (out_length == nullptr) return false;
    *out_length = 0u;
    if (value == nullptr) return false;
    for (size_t length = 0u; length < kMaximumSapiUtf16Units; ++length) {
        if (value[length] == 0u) {
            *out_length = length;
            return true;
        }
    }
    return false;
}

bool bounded_utf8_length(const char* value, size_t* out_length) noexcept {
    if (out_length == nullptr) return false;
    *out_length = 0u;
    if (value == nullptr) return false;
    for (size_t length = 0u; length < kMaximumSapiUtf8Bytes; ++length) {
        if (value[length] == '\0') {
            *out_length = length;
            return true;
        }
    }
    return false;
}

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
    std::mutex mutex;
    std::unordered_map<uint64_t, sao_ui_sound_group_t> active;
    uint64_t next_id = 0;
};

SoundState& sound_state() {
    static SoundState s;
    return s;
}

// ─── SAPI singleton ──────────────────────────────────────────────
#if defined(_WIN32)
struct SapiRequest {
    enum class Kind { speak, stop, enumerate };
    Kind kind = Kind::speak;
    std::wstring text;
    std::string voice_id;
    int32_t rate = 0;
    int32_t volume = 100;
    char* out_names = nullptr;
    size_t capacity = 0;
    size_t stride = 0;
    size_t* out_count = nullptr;
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
};

class SapiOwner {
public:
    ~SapiOwner() noexcept { shutdown(); }

    sao_status_t speak(const uint16_t* text, const char* voice_id,
                       int32_t rate, int32_t volume) {
        size_t length = 0u;
        if (!bounded_utf16_length(text, &length)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        size_t voice_length = 0u;
        if (voice_id != nullptr && !bounded_utf8_length(voice_id, &voice_length)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (!ensure_worker()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        auto request = std::make_shared<SapiRequest>();
        request->kind = SapiRequest::Kind::speak;
        request->text.assign(reinterpret_cast<const wchar_t*>(text), length);
        if (voice_id != nullptr) request->voice_id.assign(voice_id, voice_length);
        request->rate = rate;
        request->volume = volume;
        return submit_and_wait(request);
    }

    sao_status_t stop() {
        auto request = std::make_shared<SapiRequest>();
        request->kind = SapiRequest::Kind::stop;
        std::thread old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_) return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (worker_.joinable() && worker_exited_) old = std::move(worker_);
        }
        if (old.joinable()) old.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_) return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (!worker_.joinable()) {
                worker_exited_ = false;
                try {
                    worker_ = std::thread([this] { run(); });
                } catch (...) {
                    worker_ = std::thread();
                    worker_exited_ = false;
                    return SAO_STATUS_ERR_OS_CALL_FAILED;
                }
            }
            cancel_queued_locked();
            queue_.push_front(request);
        }
        cv_.notify_one();
        return wait(request);
    }

    sao_status_t enumerate(char* out_names, size_t capacity,
                           size_t stride, size_t* out_count) {
        if (out_names != nullptr && capacity != 0u && stride != 0u &&
            capacity > std::numeric_limits<size_t>::max() / stride)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        if (!ensure_worker()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        auto request = std::make_shared<SapiRequest>();
        request->kind = SapiRequest::Kind::enumerate;
        request->out_names = out_names;
        request->capacity = capacity;
        request->stride = stride;
        request->out_count = out_count;
        return submit_and_wait(request);
    }

    void shutdown_for_atexit() noexcept { shutdown(); }

private:
    static HRESULT enum_voice_tokens(IEnumSpObjectTokens** out_enum) {
        if (out_enum == nullptr) return E_POINTER;
        *out_enum = nullptr;
        ISpObjectTokenCategory* category = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                       IID_ISpObjectTokenCategory,
                                       reinterpret_cast<void**>(&category));
        if (FAILED(hr) || category == nullptr) return hr;
        hr = category->SetId(SPCAT_VOICES, FALSE);
        if (SUCCEEDED(hr)) hr = category->EnumTokens(nullptr, nullptr, out_enum);
        category->Release();
        return hr;
    }

    static HRESULT token_description(ISpObjectToken* token, WCHAR** out_desc) {
        if (token == nullptr || out_desc == nullptr) return E_POINTER;
        *out_desc = nullptr;
        return token->GetStringValue(nullptr, out_desc);
    }

    bool select_voice(const std::string& id) {
        if (id.empty()) {
            current_voice_id_.clear();
            return SUCCEEDED(voice_->SetVoice(nullptr));
        }
        if (id == current_voice_id_) return true;
        IEnumSpObjectTokens* tokens = nullptr;
        if (FAILED(enum_voice_tokens(&tokens)) || tokens == nullptr) return false;
        ISpObjectToken* token = nullptr;
        ULONG fetched = 0;
        bool found = false;
        while (tokens->Next(1, &token, &fetched) == S_OK && token != nullptr) {
            WCHAR* desc = nullptr;
            if (SUCCEEDED(token_description(token, &desc)) && desc != nullptr) {
                const int needed = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nullptr, 0, nullptr, nullptr);
                std::vector<char> utf8(needed > 0 ? static_cast<size_t>(needed) : 0u);
                if (needed > 0) {
                    WideCharToMultiByte(CP_UTF8, 0, desc, -1, utf8.data(), needed, nullptr, nullptr);
                    if (std::string(utf8.data()) == id) {
                        found = SUCCEEDED(voice_->SetVoice(token));
                        if (found) current_voice_id_ = id;
                    }
                }
                CoTaskMemFree(desc);
            }
            token->Release();
            token = nullptr;
            if (found) break;
        }
        tokens->Release();
        return found;
    }

    sao_status_t process_enumerate(const std::shared_ptr<SapiRequest>& request) {
        if (request->out_names != nullptr && request->capacity != 0u &&
            request->stride != 0u &&
            request->capacity > std::numeric_limits<size_t>::max() / request->stride)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        IEnumSpObjectTokens* tokens = nullptr;
        const HRESULT hr = enum_voice_tokens(&tokens);
        if (FAILED(hr) || tokens == nullptr) return SAO_STATUS_ERR_OS_CALL_FAILED;
        ULONG total = 0;
        tokens->GetCount(&total);
        if (request->out_names == nullptr || request->capacity == 0 || request->stride == 0) {
            if (request->out_count != nullptr) *request->out_count = total;
            tokens->Release();
            return SAO_STATUS_OK;
        }
        ISpObjectToken* token = nullptr;
        ULONG fetched = 0;
        size_t written = 0;
        while (written < request->capacity &&
               tokens->Next(1, &token, &fetched) == S_OK && token != nullptr) {
            WCHAR* desc = nullptr;
            char* slot = request->out_names + written * request->stride;
            if (SUCCEEDED(token_description(token, &desc)) && desc != nullptr) {
                const int wrote = WideCharToMultiByte(
                    CP_UTF8, 0, desc, -1, slot, static_cast<int>(request->stride), nullptr, nullptr);
                if (wrote <= 0) slot[0] = 0;
                CoTaskMemFree(desc);
            } else {
                slot[0] = 0;
            }
            token->Release();
            token = nullptr;
            ++written;
        }
        tokens->Release();
        if (request->out_count != nullptr) *request->out_count = written;
        return SAO_STATUS_OK;
    }

    sao_status_t process(const std::shared_ptr<SapiRequest>& request) {
        switch (request->kind) {
        case SapiRequest::Kind::speak:
            if (!request->voice_id.empty() && !select_voice(request->voice_id)) {
                (void)voice_->SetVoice(nullptr);
                current_voice_id_.clear();
            }
            (void)voice_->SetRate(request->rate);
            (void)voice_->SetVolume(static_cast<USHORT>(request->volume));
            return FAILED(voice_->Speak(request->text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr))
                       ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_OK;
        case SapiRequest::Kind::stop:
            return FAILED(voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr))
                       ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_OK;
        case SapiRequest::Kind::enumerate:
            return process_enumerate(request);
        }
        return SAO_STATUS_ERR_UNKNOWN;
    }

    void complete(const std::shared_ptr<SapiRequest>& request, sao_status_t status) noexcept {
        {
            std::lock_guard<std::mutex> lock(request->done_mutex);
            request->status = status;
            request->done = true;
        }
        request->done_cv.notify_one();
    }

    sao_status_t wait(const std::shared_ptr<SapiRequest>& request) {
        std::unique_lock<std::mutex> lock(request->done_mutex);
        request->done_cv.wait(lock, [&request] { return request->done; });
        return request->status;
    }

    sao_status_t submit_and_wait(const std::shared_ptr<SapiRequest>& request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_) return SAO_STATUS_ERR_NOT_INITIALIZED;
            queue_.push_back(request);
        }
        cv_.notify_one();
        return wait(request);
    }

    void cancel_queued_locked() {
        for (const auto& request : queue_)
            complete(request, SAO_STATUS_ERR_CANCELLED);
        queue_.clear();
    }

    bool ensure_worker() {
        std::thread old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_) return false;
            if (worker_.joinable() && worker_exited_) old = std::move(worker_);
        }
        if (old.joinable()) old.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return false;
        if (!worker_.joinable()) {
            worker_exited_ = false;
            try {
                worker_ = std::thread([this] { run(); });
            } catch (...) {
                worker_ = std::thread();
                worker_exited_ = false;
                return false;
            }
        }
        return true;
    }

    void run() {
        bool uninitialize = false;
        std::shared_ptr<SapiRequest> request;
        try {
            const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            uninitialize = SUCCEEDED(com_status);
            if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
                fail_pending(SAO_STATUS_ERR_OS_CALL_FAILED);
            } else {
                const HRESULT voice_status = CoCreateInstance(
                    CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                    reinterpret_cast<void**>(&voice_));
                if (FAILED(voice_status) || voice_ == nullptr) {
                    fail_pending(SAO_STATUS_ERR_OS_CALL_FAILED);
                } else {
                    for (;;) {
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            cv_.wait(lock, [this] {
                                return shutdown_requested_ || !queue_.empty();
                            });
                            if (queue_.empty()) break;
                            request = std::move(queue_.front());
                            queue_.pop_front();
                        }
                        sao_status_t status = SAO_STATUS_ERR_OS_CALL_FAILED;
                        try { status = process(request); }
                        catch (...) { status = SAO_STATUS_ERR_OS_CALL_FAILED; }
                        complete(request, status);
                        request.reset();
                    }
                }
            }
        } catch (...) {
            if (request != nullptr)
                complete(request, SAO_STATUS_ERR_OS_CALL_FAILED);
            fail_pending(SAO_STATUS_ERR_OS_CALL_FAILED);
        }
        if (voice_ != nullptr) {
            voice_->Release();
            voice_ = nullptr;
        }
        if (uninitialize) CoUninitialize();
        mark_exited();
    }

    void fail_pending(sao_status_t status) {
        std::deque<std::shared_ptr<SapiRequest>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(queue_);
        }
        for (const auto& request : pending) complete(request, status);
    }

    void mark_exited() {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_exited_ = true;
    }

    void shutdown() noexcept {
        std::thread old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            shutdown_requested_ = true;
            cancel_queued_locked();
            if (!worker_.joinable()) return;
            if (worker_.get_id() == std::this_thread::get_id()) {
                shutdown_deferred_ = true;
                cv_.notify_all();
                return;
            }
            old = std::move(worker_);
        }
        cv_.notify_one();
        if (old.joinable()) old.join();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<SapiRequest>> queue_;
    std::thread worker_;
    bool accepting_ = true;
    bool worker_exited_ = false;
    bool shutdown_requested_ = false;
    bool shutdown_deferred_ = false;
    ISpVoice* voice_ = nullptr;
    std::string current_voice_id_;
};

SapiOwner* g_sapi_owner = nullptr;
void shutdown_sapi_owner() noexcept {
    if (g_sapi_owner != nullptr) g_sapi_owner->shutdown_for_atexit();
}
SapiOwner& sapi_owner() {
    static SapiOwner* owner = [] {
        auto* value = new SapiOwner();
        g_sapi_owner = value;
        std::atexit(shutdown_sapi_owner);
        return value;
    }();
    return *owner;
}
#endif  // _WIN32

}  // namespace

// ─── Public ABI: TTS ─────────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_speak(
    const uint16_t* text_utf16,
    const char* voice_id_utf8,
    int32_t rate,
    int32_t volume) {
    try {
    if (text_utf16 == nullptr || text_utf16[0] == 0) return SAO_STATUS_OK;
#if defined(_WIN32)
    rate = std::clamp(rate, -10, 10);
    volume = std::clamp(volume, 0, 100);
    return sapi_owner().speak(text_utf16, voice_id_utf8, rate, volume);
#else
    (void)voice_id_utf8; (void)rate; (void)volume;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_stop(void) {
    try {
#if defined(_WIN32)
    return sapi_owner().stop();
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_enum_voices(
    char* out_names, size_t capacity, size_t name_stride_bytes, size_t* out_count) {
    try {
    if (out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if defined(_WIN32)
    if (name_stride_bytes > static_cast<size_t>(INT_MAX))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return sapi_owner().enumerate(out_names, capacity, name_stride_bytes, out_count);
#else
    (void)out_names; (void)capacity; (void)name_stride_bytes;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Public ABI: Banner ──────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_show(
    const uint16_t* text_utf16,
    uint32_t        duration_ms,
    int32_t         color_id,
    uint64_t*       out_banner_id) {
    try {
    if (out_banner_id == nullptr || text_utf16 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    size_t n = 0u;
    if (!bounded_utf16_length(text_utf16, &n))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    uint64_t id = banner_queue().push(text_utf16, n, duration_ms,
                                      color_id);
    *out_banner_id = id;
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_hide(
    uint64_t banner_id) {
    try {
    banner_queue().hide(banner_id);
    return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_drain(
    SaoUiAlertBanner* out_records,
    size_t            capacity,
    size_t*           out_count) {
    try {
    return banner_queue().drain(out_records, capacity, out_count);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Public ABI: Sound ───────────────────────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_sound_play(
    const uint16_t* path_utf16,
    int32_t         volume,
    uint64_t*       out_sound_id) {
    try {
    size_t path_length = 0u;
    if (!bounded_utf16_length(path_utf16, &path_length) || path_length == 0u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_sound_id) *out_sound_id = 0;
#if defined(_WIN32)
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    sao_ui_sound_group_t group = 0;
    sao_status_t status = sao_ui_sound_group_create(&group);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_sound_play_wav_utf16(path_utf16, volume, group);
    if (status != SAO_STATUS_OK) {
        (void)sao_ui_sound_group_destroy(group);
        return status;
    }
    auto& ss = sound_state();
    std::lock_guard<std::mutex> lock(ss.mutex);
    uint64_t id = ++ss.next_id;
    ss.active.emplace(id, group);
    if (out_sound_id) *out_sound_id = id;
    return SAO_STATUS_OK;
#else
    (void)path_utf16; (void)volume;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_sound_stop(
    uint64_t sound_id) {
    try {
#if defined(_WIN32)
    auto& ss = sound_state();
    std::lock_guard<std::mutex> lock(ss.mutex);
    if (sound_id == 0) {
        for (const auto& [id, group] : ss.active) {
            (void)id;
            (void)sao_ui_sound_group_stop(group);
        }
        ss.active.clear();
        return SAO_STATUS_OK;
    }
    const auto found = ss.active.find(sound_id);
    if (found == ss.active.end()) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    const sao_ui_sound_group_t group = found->second;
    ss.active.erase(found);
    (void)sao_ui_sound_group_stop(group);
    return SAO_STATUS_OK;
#else
    (void)sound_id;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
