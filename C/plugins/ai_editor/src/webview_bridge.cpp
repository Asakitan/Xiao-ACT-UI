#include "webview_bridge.h"

#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <WebView2.h>

#include "native_runtime_internal.h"
#include "native_utils.h"

// The opaque runtime type must match the definition inside
// ai_editor_native_runtime.cpp so we can recover the concrete
// NativeRuntime instance from the C ABI handle.
struct SaoAiEditorRuntime {
    std::unique_ptr<sao::ai_editor::native::NativeRuntime> implementation;
    std::mutex dispatch_mutex;
    std::mutex event_mutex;
    std::string pending_dispatch;
    std::string pending_event;
};

// Dynamic loader signature for CreateCoreWebView2EnvironmentWithOptions.
using PFN_CreateEnvironment =
    HRESULT (STDMETHODCALLTYPE*)(PCWSTR, PCWSTR,
                                 ICoreWebView2EnvironmentOptions*,
                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

namespace sao::ai_editor::native {
namespace {

constexpr wchar_t kWindowClassName[] = L"SaoAiEditorWebViewHost";

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
        return SUCCEEDED(hr_) || hr_ == S_FALSE ||
               hr_ == RPC_E_CHANGED_MODE;
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
    NativeRuntime* runtime = nullptr;
    sao_ai_editor_runtime_t runtime_handle = nullptr;
    ICoreWebView2Environment* environment = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* view = nullptr;
    EventRegistrationToken web_message_token{};
    std::wstring navigate_url;
    bool bridge_enabled = true;
    std::atomic<int32_t> status{SAO_AI_EDITOR_OK};
    std::atomic<bool> teardown_requested{false};

    void fail(int32_t failure) noexcept {
        int32_t expected = SAO_AI_EDITOR_OK;
        status.compare_exchange_strong(expected, failure,
                                       std::memory_order_acq_rel);
        if (window != nullptr) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
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
    if (session_->teardown_requested.load(std::memory_order_acquire)) {
        return E_ABORT;
    }
    if (FAILED(hr) || environment == nullptr) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return FAILED(hr) ? hr : E_FAIL;
    }
    environment->AddRef();
    session_->environment = environment;
    auto* controller_ready = new ControllerReadyHandler(session_);
    HRESULT create_hr = environment->CreateCoreWebView2Controller(
        session_->window, controller_ready);
    controller_ready->Release();
    if (FAILED(create_hr)) {
        session_->fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED);
        return create_hr;
    }
    return S_OK;
}

HRESULT ControllerReadyHandler::Invoke(
    HRESULT hr, ICoreWebView2Controller* controller) {
    if (session_->teardown_requested.load(std::memory_order_acquire)) {
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
    GetClientRect(session_->window, &rect);
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
        L"            if (!window.chrome || !window.chrome.webview) {\n"
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
            } else if (session_->runtime == nullptr) {
                status = SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
                result = {{"message", "native runtime unavailable"}};
            } else {
                nlohmann::json params{
                    {"panelId", message["panelId"]},
                    {"message", message.value("message", nlohmann::json())}};
                status = session_->runtime->dispatch_extension_call(
                    "vscode.window.postMessageToWebview", params, result);
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
        const int32_t status = session_->runtime == nullptr
            ? SAO_AI_EDITOR_ERR_NOT_INITIALIZED
            : session_->runtime->dispatch_extension_call(
                  method, params, result);
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
    case WM_SIZE:
        if (session->controller != nullptr) {
            RECT rect{};
            GetClientRect(window, &rect);
            session->controller->put_Bounds(rect);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (!session->teardown_requested.exchange(true)) {
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
        }
        PostQuitMessage(0);
        return 0;
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
    NativeRuntime* runtime = nullptr;
    if (config.runtime_handle != nullptr) {
        runtime = config.runtime_handle->implementation.get();
        if (config.bridge_native_runtime && runtime == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
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
    session->runtime = runtime;
    session->runtime_handle = config.runtime_handle;
    session->bridge_enabled = config.bridge_native_runtime;
    session->navigate_url = utf8_to_wide(config.url);

    const std::wstring title = config.window_title.empty()
        ? std::wstring(L"SAO AI Editor WebView")
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
    session->window = window;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

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
