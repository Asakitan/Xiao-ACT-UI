#include "webview_bridge.h"

#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <WebView2.h>

#include "input_event_ring.h"
#include "native_runtime_internal.h"
#include "native_utils.h"
#include "window_capture_mmf.h"
#include "window_hardening.h"

// Dynamic loader signature for CreateCoreWebView2EnvironmentWithOptions.
using PFN_CreateEnvironment =
    HRESULT (STDMETHODCALLTYPE*)(PCWSTR, PCWSTR,
                                 ICoreWebView2EnvironmentOptions*,
                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

namespace sao::ai_editor::native {
namespace {

constexpr wchar_t kWindowClassName[] = L"{C82E4A03-9F16-4B7D-A5E8-2C6F1B4D8E93}";
constexpr UINT kPostWebMessage = WM_APP + 3U;

struct WebviewPostRequest final {
    std::string panel_id;
    uint64_t message_seq = 0;
    nlohmann::json message;
    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    bool accepted = false;
    bool cancelled = false;
};

class ScopedCoInitialize final {
public:
    ScopedCoInitialize() {
        hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    ~ScopedCoInitialize() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    bool valid() const noexcept {
        return SUCCEEDED(hr_);
    }
    HRESULT status() const noexcept { return hr_; }

    ScopedCoInitialize(const ScopedCoInitialize&) = delete;
    ScopedCoInitialize& operator=(const ScopedCoInitialize&) = delete;

private:
    HRESULT hr_ = S_OK;
};

class ScopedCoTaskString final {
public:
    ScopedCoTaskString() = default;
    ~ScopedCoTaskString() {
        if (value_ != nullptr) {
            CoTaskMemFree(value_);
        }
    }
    LPWSTR* addressof() noexcept { return &value_; }
    LPWSTR get() const noexcept { return value_; }
    ScopedCoTaskString(const ScopedCoTaskString&) = delete;
    ScopedCoTaskString& operator=(const ScopedCoTaskString&) = delete;

private:
    LPWSTR value_ = nullptr;
};

struct WebViewSession {
    HWND window = nullptr;
    sao_ai_editor_runtime_t runtime_handle = nullptr;
    ICoreWebView2Environment* environment = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* view = nullptr;
    EventRegistrationToken web_message_token{};
    std::wstring navigate_url;
    bool bridge_enabled = true;
    std::atomic<int32_t> status{SAO_AI_EDITOR_OK};
    std::atomic<bool> teardown_requested{false};
    mutable std::mutex mutex;
    std::condition_variable callbacks_done;
    size_t callbacks_inflight = 0;
    bool teardown_started = false;
    DWORD ui_thread_id = 0;
    std::unordered_map<WebviewPostRequest*,
                       std::shared_ptr<WebviewPostRequest>> pending_posts;
    // Phase C wire-up: MMF capture + input ring bridge state.
    // Both are non-null iff the launcher supplied a
    // WebViewConfig::sao_mmf_name_utf8; the HWND then sits off-screen
    // and the compositor layer consumes the MMF output.
    std::unique_ptr<sao::ai_editor::WindowCaptureToMmf> mmf_capture;
    std::unique_ptr<sao::ai_editor::InputEventRingReader> input_reader;
    bool off_screen = false;

    HWND window_handle() const noexcept {
        std::lock_guard<std::mutex> guard(mutex);
        return window;
    }

    bool begin_callback() noexcept {
        std::lock_guard<std::mutex> guard(mutex);
        if (teardown_started) {
            return false;
        }
        ++callbacks_inflight;
        return true;
    }

    void end_callback() noexcept {
        std::lock_guard<std::mutex> guard(mutex);
        if (callbacks_inflight > 0) {
            --callbacks_inflight;
        }
        if (teardown_started && callbacks_inflight == 0) {
            callbacks_done.notify_all();
        }
    }

    void fail(int32_t failure) noexcept {
        int32_t expected = SAO_AI_EDITOR_OK;
        status.compare_exchange_strong(expected, failure,
                                       std::memory_order_acq_rel);
        const HWND target = window_handle();
        if (target != nullptr) {
            PostMessageW(target, WM_CLOSE, 0, 0);
        }
    }
};

class CallbackLease final {
public:
    explicit CallbackLease(const std::shared_ptr<WebViewSession>& session)
        : session_(session),
          entered_(session_ && session_->begin_callback()) {}

    ~CallbackLease() {
        if (entered_) {
            session_->end_callback();
        }
    }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

    explicit operator bool() const noexcept { return entered_; }

private:
    std::shared_ptr<WebViewSession> session_;
    bool entered_ = false;
};

bool complete_post_request(const std::shared_ptr<WebviewPostRequest>& request,
                           bool accepted) noexcept {
    if (request == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(request->mutex);
        request->accepted = accepted;
        request->completed = true;
    }
    request->ready.notify_all();
    return accepted;
}

bool post_webview_message(const std::shared_ptr<WebViewSession>& session,
                          std::string_view panel_id,
                          uint64_t message_seq,
                          const nlohmann::json& message) {
    if (session == nullptr || panel_id.empty() ||
        !session->bridge_enabled ||
        session->teardown_requested.load(std::memory_order_acquire)) {
        return false;
    }
    auto request = std::make_shared<WebviewPostRequest>();
    request->panel_id = std::string(panel_id);
    request->message_seq = message_seq;
    request->message = message;
    const DWORD current_thread_id = GetCurrentThreadId();
    DWORD ui_thread_id = 0;
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> guard(session->mutex);
        if (session->teardown_started || session->window == nullptr) {
            return false;
        }
        ui_thread_id = session->ui_thread_id;
        window = session->window;
        if (ui_thread_id == current_thread_id) {
            if (session->view == nullptr) {
                return false;
            }
            try {
                const std::wstring payload = utf8_to_wide(message.dump());
                return !payload.empty() &&
                       SUCCEEDED(session->view->PostWebMessageAsJson(
                           payload.c_str()));
            } catch (...) {
                return false;
            }
        }
        session->pending_posts.emplace(request.get(), request);
    }
    if (!PostMessageW(window, kPostWebMessage, 0,
                      reinterpret_cast<LPARAM>(request.get()))) {
        {
            std::lock_guard<std::mutex> guard(session->mutex);
            session->pending_posts.erase(request.get());
        }
        return false;
    }
    std::unique_lock<std::mutex> lock(request->mutex);
    if (!request->ready.wait_for(
            lock, std::chrono::seconds(5),
            [&request] { return request->completed; })) {
        request->cancelled = true;
        return false;
    }
    return request->accepted;
}

class ScopedWebviewHandler final {
public:
    explicit ScopedWebviewHandler(NativeRuntime* runtime) noexcept
        : runtime_(runtime) {}

    ~ScopedWebviewHandler() {
        if (runtime_ != nullptr) {
            runtime_->set_webview_post_message_handler({});
        }
    }

    ScopedWebviewHandler(const ScopedWebviewHandler&) = delete;
    ScopedWebviewHandler& operator=(const ScopedWebviewHandler&) = delete;

private:
    NativeRuntime* runtime_ = nullptr;
};

class EnvironmentReadyHandler
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    explicit EnvironmentReadyHandler(std::shared_ptr<WebViewSession> session)
        : session_(std::move(session)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown ||
            iid == __uuidof(
                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = ref_.fetch_sub(1) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,
                                     ICoreWebView2Environment* environment) override;

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<WebViewSession> session_;
};

class ControllerReadyHandler
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    explicit ControllerReadyHandler(std::shared_ptr<WebViewSession> session)
        : session_(std::move(session)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown ||
            iid == __uuidof(
                ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = ref_.fetch_sub(1) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,
                                     ICoreWebView2Controller* controller) override;

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<WebViewSession> session_;
};

class WebMessageReceivedHandler
    : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    explicit WebMessageReceivedHandler(std::shared_ptr<WebViewSession> session)
        : session_(std::move(session)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown ||
            iid == __uuidof(
                ICoreWebView2WebMessageReceivedEventHandler)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = ref_.fetch_sub(1) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2* sender,
        ICoreWebView2WebMessageReceivedEventArgs* args) override;

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<WebViewSession> session_;
};

HRESULT EnvironmentReadyHandler::Invoke(HRESULT hr,
                                        ICoreWebView2Environment* environment) {
    CallbackLease callback(session_);
    if (!callback ||
        session_->teardown_requested.load(std::memory_order_acquire)) {
        return E_ABORT;
    }
    if (FAILED(hr) || environment == nullptr) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return FAILED(hr) ? hr : E_FAIL;
    }
    environment->AddRef();
    session_->environment = environment;
    HWND window = session_->window_handle();
    if (window == nullptr) {
        session_->fail(SAO_AI_EDITOR_ERR_IPC_CLOSED);
        return E_ABORT;
    }
    auto* controller_ready = new ControllerReadyHandler(session_);
    HRESULT create_hr = environment->CreateCoreWebView2Controller(
        window, controller_ready);
    controller_ready->Release();
    if (FAILED(create_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return create_hr;
    }
    return S_OK;
}

HRESULT ControllerReadyHandler::Invoke(
    HRESULT hr, ICoreWebView2Controller* controller) {
    CallbackLease callback(session_);
    if (!callback ||
        session_->teardown_requested.load(std::memory_order_acquire)) {
        return E_ABORT;
    }
    if (FAILED(hr) || controller == nullptr) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return FAILED(hr) ? hr : E_FAIL;
    }
    controller->AddRef();
    session_->controller = controller;
    HRESULT setup_hr = controller->put_IsVisible(TRUE);
    if (FAILED(setup_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return setup_hr;
    }
    RECT rect{};
    const HWND window = session_->window_handle();
    if (window == nullptr) {
        session_->fail(SAO_AI_EDITOR_ERR_IPC_CLOSED);
        return E_ABORT;
    }
    GetClientRect(window, &rect);
    setup_hr = controller->put_Bounds(rect);
    if (FAILED(setup_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return setup_hr;
    }
    ICoreWebView2* view = nullptr;
    setup_hr = controller->get_CoreWebView2(&view);
    if (FAILED(setup_hr) || view == nullptr) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return FAILED(setup_hr) ? setup_hr : E_FAIL;
    }
    session_->view = view;
    if (session_->bridge_enabled) {
        auto* handler = new WebMessageReceivedHandler(session_);
        setup_hr = view->add_WebMessageReceived(
            handler, &session_->web_message_token);
        handler->Release();
        if (FAILED(setup_hr)) {
            session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
            return setup_hr;
        }
    }
    // acquireVsCodeApi shim — matches the ambient global the VSCode
    // extension host injects.  Panels get postMessage() + setState() +
    // getState() with state persisted in sessionStorage keyed by the
    // active panelId (which the runtime sets on window before Navigate).
    static const wchar_t kAcquireShim[] =
        L"(function(){\n"
        L"  if (window.__saoVscodeApiRegistered) { return; }\n"
        L"  window.__saoVscodeApiRegistered = true;\n"
        L"  let nextRequestId = 1;\n"
        L"  const pending = new Map();\n"
        L"  const stateKey = () => 'sao.webviewPanel.state.' +\n"
        L"      (window.__saoActivePanelId || 'default');\n"
        L"  const bag = () => {\n"
        L"    try {\n"
        L"      const raw = window.sessionStorage.getItem(stateKey());\n"
        L"      return raw ? JSON.parse(raw) : undefined;\n"
        L"    } catch (e) { return undefined; }\n"
        L"  };\n"
        L"  if (window.chrome && window.chrome.webview) {\n"
        L"    window.chrome.webview.addEventListener('message', (event) => {\n"
        L"      const reply = event.data;\n"
        L"      if (!reply || reply.method !==\n"
        L"          'webviewPanel.postMessage.ack') { return; }\n"
        L"      const waiter = pending.get(reply.id);\n"
        L"      if (!waiter) { return; }\n"
        L"      pending.delete(reply.id);\n"
        L"      if (reply.ok) { waiter.resolve(reply.result); }\n"
        L"      else { waiter.reject(Object.assign(\n"
        L"          new Error((reply.error && reply.error.message) ||\n"
        L"              'native dispatch failed'),\n"
        L"          { status: reply.status, details: reply.error })); }\n"
        L"    });\n"
        L"  }\n"
        L"  window.acquireVsCodeApi = function acquireVsCodeApi() {\n"
        L"    return {\n"
        L"      postMessage(message) {\n"
        L"        const id = nextRequestId++;\n"
        L"        const envelope = {\n"
        L"          method: 'webviewPanel.postMessage',\n"
        L"          id,\n"
        L"          panelId: window.__saoActivePanelId || null,\n"
        L"          message,\n"
        L"        };\n"
        L"        return new Promise((resolve, reject) => {\n"
        L"          try {\n"
        L"            if (!window.__saoBridgeEnabled ||\n"
        L"                !window.chrome || !window.chrome.webview) {\n"
        L"              resolve(false);\n"
        L"              return;\n"
        L"            }\n"
        L"            pending.set(id, { resolve, reject });\n"
        L"            window.chrome.webview.postMessage(envelope);\n"
        L"          } catch (error) {\n"
        L"            pending.delete(id);\n"
        L"            reject(error);\n"
        L"          }\n"
        L"        });\n"
        L"      },\n"
        L"      setState(state) {\n"
        L"        try {\n"
        L"          window.sessionStorage.setItem(stateKey(),\n"
        L"              JSON.stringify(state));\n"
        L"        } catch (e) {}\n"
        L"        return state;\n"
        L"      },\n"
        L"      getState() { return bag(); },\n"
        L"    };\n"
        L"  };\n"
        L"  window.__saoSetActivePanel = function(id) {\n"
        L"    window.__saoActivePanelId = id;\n"
        L"  };\n"
        L"})();\n";
    const wchar_t* bridge_state = session_->bridge_enabled
        ? L"window.__saoBridgeEnabled = true;"
        : L"window.__saoBridgeEnabled = false;";
    setup_hr = view->AddScriptToExecuteOnDocumentCreated(bridge_state,
                                                          nullptr);
    if (FAILED(setup_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return setup_hr;
    }
    setup_hr = view->AddScriptToExecuteOnDocumentCreated(kAcquireShim,
                                                          nullptr);
    if (FAILED(setup_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return setup_hr;
    }
    if (!session_->navigate_url.empty()) {
        setup_hr = view->Navigate(session_->navigate_url.c_str());
    } else {
        setup_hr = view->NavigateToString(
            L"<html><body><h1>SAO AI Editor</h1></body></html>");
    }
    if (FAILED(setup_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return setup_hr;
    }
    return S_OK;
}

HRESULT WebMessageReceivedHandler::Invoke(
    ICoreWebView2* sender,
    ICoreWebView2WebMessageReceivedEventArgs* args) {
    CallbackLease callback(session_);
    if (!callback ||
        session_->teardown_requested.load(std::memory_order_acquire)) {
        return E_ABORT;
    }
    if (sender == nullptr || args == nullptr) {
        return E_POINTER;
    }
    auto post_reply = [sender](const nlohmann::json& reply) {
        try {
            const std::wstring wide_reply = utf8_to_wide(reply.dump());
            if (wide_reply.empty()) {
                return E_FAIL;
            }
            return sender->PostWebMessageAsJson(wide_reply.c_str());
        } catch (...) {
            return E_FAIL;
        }
    };
    ScopedCoTaskString payload;
    if (FAILED(args->get_WebMessageAsJson(payload.addressof())) ||
        payload.get() == nullptr) {
        return E_INVALIDARG;
    }
    const std::string utf8 = wide_to_utf8(payload.get());
    if (utf8.empty()) {
        return E_INVALIDARG;
    }
    nlohmann::json request_id(nullptr);
    try {
        auto message = nlohmann::json::parse(utf8, nullptr, false);
        if (message.is_string()) {
            message = nlohmann::json::parse(
                message.get_ref<const std::string&>(), nullptr, false);
        }
        if (!message.is_object()) {
            return post_reply(
                {{"method", "webviewPanel.postMessage.ack"},
                 {"id", nullptr},
                 {"ok", false},
                 {"status", SAO_AI_EDITOR_ERR_PROTOCOL},
                 {"error", {{"message", "invalid WebView message"}}}});
        }
        const std::string incoming_method =
            message.value("method", std::string{});
        request_id = message.value("id", nlohmann::json(nullptr));
        // Panel-scoped envelope from acquireVsCodeApi().postMessage(): the
        // shim tags outgoing messages with method="webviewPanel.postMessage"
        // and a panelId; route them through the postMessageToWebview
        // dispatcher so a Node-side onDidReceiveMessage handler observes
        // the emit. Missing panel ids still receive a correlated error ack.
        if (incoming_method == "webviewPanel.postMessage") {
            nlohmann::json result;
            int32_t status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            if (!message.contains("panelId") ||
                !message["panelId"].is_string() ||
                message["panelId"].get_ref<const std::string&>().empty()) {
                result = {{"message", "panelId is required"}};
            } else {
                RuntimeLease runtime_lease(session_->runtime_handle);
                NativeRuntime* runtime = runtime_lease.get();
                if (runtime == nullptr) {
                    status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
                    result = { {"message", "native runtime lease unavailable"} };
                } else {
                    nlohmann::json params{
                        {"panelId", message["panelId"]},
                        {"message", message.value("message",
                                                   nlohmann::json())}};
                    status = runtime->dispatch_extension_call(
                        "vscode.window.postMessageToWebview", params, result);
                }
            }
            nlohmann::json reply{
                {"method", "webviewPanel.postMessage.ack"},
                {"id", request_id},
                {"ok", status == SAO_AI_EDITOR_OK},
                {"status", status}};
            if (status == SAO_AI_EDITOR_OK) {
                reply["result"] = std::move(result);
            } else {
                reply["error"] = std::move(result);
            }
            return post_reply(reply);
        }
        const std::string method = incoming_method;
        nlohmann::json params =
            message.value("params", nlohmann::json::object());
        nlohmann::json result;
        RuntimeLease runtime_lease(session_->runtime_handle);
        NativeRuntime* runtime = runtime_lease.get();
        const int32_t status = runtime == nullptr
            ? SAO_AI_EDITOR_ERR_IPC_CLOSED
            : runtime->dispatch_extension_call(method, params, result);
        nlohmann::json reply{{"id", request_id},
                              {"status", status}};
        if (status == SAO_AI_EDITOR_OK) {
            reply["result"] = std::move(result);
        } else {
            reply["error"] = std::move(result);
        }
        return post_reply(reply);
    } catch (...) {
        return post_reply(
            {{"method", "webviewPanel.postMessage.ack"},
             {"id", request_id},
             {"ok", false},
             {"status", SAO_AI_EDITOR_ERR_PROTOCOL},
             {"error", {{"message", "WebView dispatch failed"}}}});
    }
}

LRESULT CALLBACK webview_wnd_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    auto* session = reinterpret_cast<WebViewSession*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (session == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_SIZE: {
        ICoreWebView2Controller* controller = nullptr;
        {
            std::lock_guard<std::mutex> guard(session->mutex);
            if (!session->teardown_started) {
                controller = session->controller;
            }
        }
        if (controller != nullptr) {
            RECT rect{};
            GetClientRect(window, &rect);
            controller->put_Bounds(rect);
        }
        return 0;
    }
    case WM_TIMER: {
        constexpr UINT_PTR kCaptureTimerId = 0xA01u;
        constexpr UINT_PTR kInputTimerId = 0xA02u;
        if (wparam == kCaptureTimerId) {
            sao::ai_editor::WindowCaptureToMmf* capture = nullptr;
            {
                std::lock_guard<std::mutex> guard(session->mutex);
                if (!session->teardown_started && session->mmf_capture)
                    capture = session->mmf_capture.get();
            }
            if (capture != nullptr)
                (void)capture->capture_and_publish();
            return 0;
        }
        if (wparam == kInputTimerId) {
            sao::ai_editor::InputEventRingReader* reader = nullptr;
            HWND target = nullptr;
            {
                std::lock_guard<std::mutex> guard(session->mutex);
                if (!session->teardown_started && session->input_reader) {
                    reader = session->input_reader.get();
                    target = session->window;
                }
            }
            if (reader != nullptr && target != nullptr) {
                std::array<sao::ai_editor::InputEvent, 32> events{};
                uint32_t dropped = 0u;
                const uint32_t got = reader->try_read_batch(
                    events.data(),
                    static_cast<uint32_t>(events.size()), dropped);
                for (uint32_t i = 0; i < got; ++i) {
                    const auto& ev = events[i];
                    switch (ev.type) {
                    case sao::ai_editor::INPUT_EVENT_MOUSE_MOVE:
                        (void)SendMessageW(target, WM_MOUSEMOVE, 0,
                                            MAKELPARAM(ev.x, ev.y));
                        break;
                    case sao::ai_editor::INPUT_EVENT_MOUSE_BUTTON: {
                        // Convention: ev.code == 1 → down, 0 → up.  ev.button
                        // is one of INPUT_MOD_* -like flags mapped to VK_LBUTTON /
                        // VK_RBUTTON / VK_MBUTTON.
                        const bool down = ev.code != 0u;
                        UINT msg = 0u;
                        if (ev.button == VK_LBUTTON)
                            msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                        else if (ev.button == VK_RBUTTON)
                            msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                        else if (ev.button == VK_MBUTTON)
                            msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                        if (msg != 0u)
                            (void)SendMessageW(
                                target, msg, 0,
                                MAKELPARAM(ev.x, ev.y));
                        break;
                    }
                    case sao::ai_editor::INPUT_EVENT_MOUSE_WHEEL:
                        (void)SendMessageW(
                            target, WM_MOUSEWHEEL,
                            MAKEWPARAM(0, ev.wheel_delta),
                            MAKELPARAM(ev.x, ev.y));
                        break;
                    case sao::ai_editor::INPUT_EVENT_KEY_DOWN:
                        (void)SendMessageW(target, WM_KEYDOWN, ev.code, 0);
                        break;
                    case sao::ai_editor::INPUT_EVENT_KEY_UP:
                        (void)SendMessageW(target, WM_KEYUP, ev.code, 0);
                        break;
                    case sao::ai_editor::INPUT_EVENT_CHAR:
                        (void)SendMessageW(target, WM_CHAR, ev.code, 0);
                        break;
                    case sao::ai_editor::INPUT_EVENT_RESIZE: {
                        ICoreWebView2Controller* controller = nullptr;
                        {
                            std::lock_guard<std::mutex> guard(
                                session->mutex);
                            if (!session->teardown_started)
                                controller = session->controller;
                        }
                        if (controller != nullptr && ev.x > 0 &&
                            ev.y > 0) {
                            RECT rect{0, 0, ev.x, ev.y};
                            controller->put_Bounds(rect);
                        }
                        break;
                    }
                    case sao::ai_editor::INPUT_EVENT_CLOSE:
                        (void)PostMessageW(target, WM_CLOSE, 0, 0);
                        break;
                    default:
                        break;
                    }
                }
            }
            return 0;
        }
        break;
    }
    case kPostWebMessage: {
        std::shared_ptr<WebviewPostRequest> request;
        ICoreWebView2* view = nullptr;
        {
            std::lock_guard<std::mutex> guard(session->mutex);
            const auto found = session->pending_posts.find(
                reinterpret_cast<WebviewPostRequest*>(lparam));
            if (found != session->pending_posts.end() &&
                !session->teardown_started) {
                request = found->second;
                view = session->view;
            }
        }
        bool accepted = false;
        if (request != nullptr) {
            std::unique_lock<std::mutex> request_lock(request->mutex);
            if (!request->cancelled && view != nullptr) {
                try {
                    const std::wstring payload =
                        utf8_to_wide(request->message.dump());
                    accepted = !payload.empty() &&
                               SUCCEEDED(view->PostWebMessageAsJson(
                                   payload.c_str()));
                } catch (...) {
                    accepted = false;
                }
            }
            request->accepted = accepted;
            request->completed = true;
            request_lock.unlock();
            request->ready.notify_all();
            std::lock_guard<std::mutex> guard(session->mutex);
            session->pending_posts.erase(request.get());
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        {
            std::unique_lock<std::mutex> lock(session->mutex);
            if (!session->teardown_started) {
                session->teardown_started = true;
                session->teardown_requested.store(true,
                                                  std::memory_order_release);
                for (const auto& [request_id, request] :
                     session->pending_posts) {
                    (void)request_id;
                    complete_post_request(request, false);
                }
                session->pending_posts.clear();
            }
            session->callbacks_done.wait(lock, [session] {
                return session->callbacks_inflight == 0;
            });
        }
        if (session->view != nullptr &&
            session->web_message_token.value != 0) {
            session->view->remove_WebMessageReceived(
                session->web_message_token);
            session->web_message_token = {};
        }
        if (session->controller != nullptr) {
            session->controller->Close();
            session->controller->Release();
            session->controller = nullptr;
        }
        if (session->view != nullptr) {
            session->view->Release();
            session->view = nullptr;
        }
        if (session->environment != nullptr) {
            session->environment->Release();
            session->environment = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->window = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

bool webview_runtime_available() {
    HMODULE module = LoadLibraryW(L"WebView2Loader.dll");
    if (module == nullptr) {
        return false;
    }
    const bool available =
        GetProcAddress(module,
                       "CreateCoreWebView2EnvironmentWithOptions") != nullptr;
    FreeLibrary(module);
    return available;
}

int32_t run_webview_bridge(const WebViewConfig& config) {
    if (config.user_data_folder.empty() ||
        !valid_utf8(config.user_data_folder)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (config.bridge_native_runtime &&
        config.runtime_handle == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    RuntimeLease runtime_lease(config.runtime_handle);
    NativeRuntime* runtime = runtime_lease.get();
    if (config.bridge_native_runtime && runtime == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    ScopedCoInitialize apartment;
    if (!apartment.valid()) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (loader == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    auto create_environment =
        reinterpret_cast<PFN_CreateEnvironment>(
            GetProcAddress(loader,
                           "CreateCoreWebView2EnvironmentWithOptions"));
    if (create_environment == nullptr) {
        FreeLibrary(loader);
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = webview_wnd_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        FreeLibrary(loader);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    auto session = std::make_shared<WebViewSession>();
    session->runtime_handle = config.runtime_handle;
    session->bridge_enabled = config.bridge_native_runtime;
    session->navigate_url = utf8_to_wide(config.url);
    if (runtime != nullptr && config.bridge_native_runtime) {
        const std::weak_ptr<WebViewSession> weak_session = session;
        runtime->set_webview_post_message_handler(
            [weak_session](std::string_view panel_id, uint64_t message_seq,
                           const Json& message) {
                const auto session = weak_session.lock();
                return post_webview_message(session, panel_id, message_seq,
                                            message);
            });
    }
            ScopedWebviewHandler handler_guard(runtime);

    const std::wstring title = config.window_title.empty()
        ? std::wstring()
        : utf8_to_wide(config.window_title);
    HWND window = CreateWindowExW(
        0, kWindowClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, std::max(320, config.width),
        std::max(240, config.height), nullptr, nullptr,
        window_class.hInstance, session.get());
    if (window == nullptr) {
        FreeLibrary(loader);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    sao::ai_editor::apply_stealth_window(window);
    {
        std::lock_guard<std::mutex> guard(session->mutex);
        session->window = window;
        session->ui_thread_id = GetCurrentThreadId();
    }

    const bool compositor_mode = !config.sao_mmf_name_utf8.empty();
    if (compositor_mode) {
        const int32_t width_px = std::max(320, config.width);
        const int32_t height_px = std::max(240, config.height);
        const std::wstring mmf_name_wide =
            utf8_to_wide(config.sao_mmf_name_utf8);
        if (!mmf_name_wide.empty()) {
            auto capture =
                std::make_unique<sao::ai_editor::WindowCaptureToMmf>();
            if (capture->init(mmf_name_wide.c_str(), window, width_px,
                              height_px)) {
                std::lock_guard<std::mutex> guard(session->mutex);
                session->mmf_capture = std::move(capture);
                session->off_screen = true;
            }
        }
        if (session->mmf_capture && !config.sao_input_ring_name_utf8.empty()) {
            const std::wstring ring_name_wide =
                utf8_to_wide(config.sao_input_ring_name_utf8);
            if (!ring_name_wide.empty()) {
                auto reader =
                    std::make_unique<sao::ai_editor::InputEventRingReader>();
                if (reader->open(ring_name_wide.c_str())) {
                    std::lock_guard<std::mutex> guard(session->mutex);
                    session->input_reader = std::move(reader);
                }
            }
        }
        if (session->off_screen) {
            // Move the visible HWND off the virtual desktop so DWM keeps
            // rendering it (PrintWindow still works) but no user pixel
            // hits the screen; the compositor layer consuming the MMF
            // is now the only visible surface.
            (void)SetWindowPos(window, HWND_TOP, -32000, -32000, width_px,
                                height_px,
                                SWP_NOACTIVATE | SWP_NOZORDER);
            (void)SetTimer(window, /*kCaptureTimerId=*/ 0xA01u, 33u,
                            nullptr);
            if (session->input_reader) {
                (void)SetTimer(window, /*kInputTimerId=*/ 0xA02u, 5u,
                                nullptr);
            }
        }
    }

    if (!compositor_mode || !session->off_screen) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }

    const std::wstring user_data =
        utf8_to_wide(config.user_data_folder);
    auto* environment_handler = new EnvironmentReadyHandler(session);
    HRESULT hr = create_environment(nullptr, user_data.c_str(), nullptr,
                                     environment_handler);
    environment_handler->Release();
    if (FAILED(hr)) {
        DestroyWindow(window);
        FreeLibrary(loader);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    MSG message{};
    BOOL message_status = FALSE;
    while ((message_status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (message_status == -1) {
        session->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
    }
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    const int32_t status = session->status.load(std::memory_order_acquire);
    FreeLibrary(loader);
    return status;
}

}  // namespace sao::ai_editor::native
