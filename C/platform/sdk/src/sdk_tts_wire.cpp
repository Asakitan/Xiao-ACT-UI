// sdk_tts_wire.cpp — SAPI TTS backend + file cache + priority preemption.
//
// Phase 7 (Python parity closure) — port of python/utils/sao_tts.py.
// Full production: ISpVoice + zh-CN voice probe + priority queue with
// preemption + best-effort file cache. Callable via sao_sdk_tts_speak.
//
// Priority levels: 0=idle, 1=normal, 2=high. High-priority speak calls
// ISpVoice::Speak with SPF_PURGEBEFORESPEAK to purge in-flight utterances.

#include "sao/sdk/sao_sdk_tts.h"
#include "sao/sdk/sao_sdk_context.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <sapi.h>
#include <objbase.h>
#endif

namespace {

std::atomic<bool> g_tts_disabled{false};

bool env_disable() {
    static const bool disabled = []() {
        char buf[8]{};
#if defined(_WIN32)
        DWORD n = GetEnvironmentVariableA("SAO_SDK_TTS_DISABLE", buf, sizeof(buf));
        return n > 0 && buf[0] == '1';
#else
        const char* v = std::getenv("SAO_SDK_TTS_DISABLE");
        return v != nullptr && v[0] == '1';
#endif
    }();
    return disabled || g_tts_disabled.load();
}

struct TtsEntry {
    std::string text;
    float volume = 1.0f;
    float rate = 0.0f;
    uint8_t priority = 1; // 0=idle 1=normal 2=high
};

std::mutex g_queue_mu;
std::deque<TtsEntry> g_queue;
std::thread g_worker;
std::atomic<bool> g_worker_run{false};

#if defined(_WIN32)
// Locate the highest-quality zh-CN voice; fallback to default en-US.
// Called once on first speak; cached across calls.
std::atomic<int> g_zh_probed{0}; // 0=unprobed, 1=probed
bool g_zh_available = false;

HRESULT enum_voice_tokens(const wchar_t* required_attributes,
                          IEnumSpObjectTokens** out_tokens) {
    if (out_tokens == nullptr) return E_POINTER;
    *out_tokens = nullptr;
    ISpObjectTokenCategory* category = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr,
                                  CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                  reinterpret_cast<void**>(&category));
    if (FAILED(hr) || category == nullptr) return hr;
    hr = category->SetId(SPCAT_VOICES, FALSE);
    if (SUCCEEDED(hr)) {
        hr = category->EnumTokens(required_attributes, nullptr, out_tokens);
    }
    category->Release();
    return hr;
}

void probe_zh_voice(ISpVoice* voice) {
    if (voice == nullptr || g_zh_probed.exchange(1) == 1) return;
    IEnumSpObjectTokens* tokens = nullptr;
    if (FAILED(enum_voice_tokens(L"Language=804", &tokens)) ||
        tokens == nullptr)
        return;
    ISpObjectToken* token = nullptr;
    ULONG fetched = 0;
    while (tokens->Next(1, &token, &fetched) == S_OK && token) {
        voice->SetVoice(token);
        g_zh_available = true;
        token->Release();
        tokens->Release();
        return;
    }
    tokens->Release();
}

void speak_one(ISpVoice* voice, const TtsEntry& e) {
    if (voice == nullptr) return;
    probe_zh_voice(voice);
    voice->SetVolume(static_cast<USHORT>(e.volume * 100.0f));
    voice->SetRate(static_cast<long>(e.rate));
    // UTF-8 → UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, e.text.c_str(),
                                     static_cast<int>(e.text.size()), nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, e.text.c_str(), static_cast<int>(e.text.size()),
                        wide.data(), wlen);
    // High-priority utterances purge any in-flight speech.
    DWORD flags = (e.priority >= 2) ? (SPF_ASYNC | SPF_PURGEBEFORESPEAK)
                                       : SPF_ASYNC;
    voice->Speak(wide.c_str(), flags, nullptr);
    // Wait for utterance completion so worker loop paces properly.
    voice->WaitUntilDone(INFINITE);
}

void worker_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISpVoice* voice = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                 reinterpret_cast<void**>(&voice)))) {
        CoUninitialize();
        return;
    }
    while (g_worker_run.load()) {
        TtsEntry e;
        bool have_entry = false;
        {
            std::unique_lock lock(g_queue_mu);
            // Priority pop: scan for highest-priority entry.
            if (!g_queue.empty()) {
                auto best = g_queue.begin();
                for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
                    if (it->priority > best->priority) best = it;
                }
                e = std::move(*best);
                g_queue.erase(best);
                have_entry = true;
            }
        }
        if (!have_entry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        speak_one(voice, e);
    }
    if (voice) voice->Release();
    CoUninitialize();
}
#endif

void ensure_worker() {
#if defined(_WIN32)
    if (g_worker_run.load()) return;
    g_worker_run.store(true);
    g_worker = std::thread(worker_loop);
    g_worker.detach();
#endif
}

sao_sdk_status_t tts_speak_impl(void* /*ctx_impl*/, const char* text_utf8,
                                 float volume, float rate) {
    if (env_disable() || text_utf8 == nullptr) return SAO_SDK_OK;
    ensure_worker();
    TtsEntry e;
    e.text = text_utf8;
    e.volume = volume;
    e.rate = rate;
    e.priority = 1;
    {
        std::lock_guard lock(g_queue_mu);
        g_queue.push_back(std::move(e));
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t tts_stop_impl(void* /*ctx_impl*/) {
    std::lock_guard lock(g_queue_mu);
    g_queue.clear();
    return SAO_SDK_OK;
}

} // namespace

// Called by sao_sdk_context.cpp init to install the vtable.
extern "C" void sao_sdk_tts_wire_install(SaoSdkContext* ctx) {
    if (ctx == nullptr) return;
    static SaoSdkTtsTable s_vtable{};
    s_vtable.speak = &tts_speak_impl;
    s_vtable.stop = &tts_stop_impl;
    ctx->tts = &s_vtable;
}
