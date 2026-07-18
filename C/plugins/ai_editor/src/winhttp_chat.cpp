#include "winhttp_chat.h"

#include "chat_provider_router.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

namespace sao::ai_editor::native {
namespace {

constexpr DWORD kWinHttpPollIntervalMs = 50;

// Best-effort extraction of "completion tokens" from provider-shaped usage
// blocks: OpenAI/OpenAI-compat use `completion_tokens`, Anthropic uses
// `output_tokens`, Gemini's `usageMetadata` uses `candidatesTokenCount`.
// Returns std::nullopt when no numeric completion-token field is present.
std::optional<int64_t> extract_completion_tokens(const Json& usage) {
    if (!usage.is_object()) {
        return std::nullopt;
    }
    for (const std::string_view field :
         {"completion_tokens", "output_tokens", "candidatesTokenCount"}) {
        const auto it = usage.find(std::string(field));
        if (it != usage.end() && it->is_number_integer()) {
            return it->get<int64_t>();
        }
        if (it != usage.end() && it->is_number()) {
            return static_cast<int64_t>(it->get<double>());
        }
    }
    return std::nullopt;
}

class InternetHandle final {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~InternetHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    HINTERNET handle_ = nullptr;
};

class EventHandle final {
public:
    EventHandle() noexcept
        : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~EventHandle() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

struct AsyncOperationState final {
    EventHandle completed;
    EventHandle handle_closed;
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<DWORD> transferred{0};

    [[nodiscard]] bool valid() const noexcept {
        return completed && handle_closed;
    }

    void prepare() noexcept {
        error.store(ERROR_SUCCESS, std::memory_order_relaxed);
        transferred.store(0, std::memory_order_relaxed);
        ResetEvent(completed.get());
    }

    void complete(DWORD operation_error, DWORD operation_transferred) noexcept {
        error.store(operation_error, std::memory_order_release);
        transferred.store(operation_transferred, std::memory_order_release);
        SetEvent(completed.get());
    }
};

void CALLBACK winhttp_status_callback(HINTERNET,
                                      DWORD_PTR context,
                                      DWORD status,
                                      void* status_information,
                                      DWORD status_information_length) noexcept {
    auto* state = reinterpret_cast<AsyncOperationState*>(context);
    if (state == nullptr) {
        return;
    }
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        state->complete(ERROR_SUCCESS, 0);
        return;
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
        if (status_information != nullptr &&
            status_information_length == sizeof(DWORD)) {
            state->complete(ERROR_SUCCESS,
                            *static_cast<DWORD*>(status_information));
        } else {
            state->complete(ERROR_INVALID_DATA, 0);
        }
        return;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        state->complete(ERROR_SUCCESS, status_information_length);
        return;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        if (status_information != nullptr &&
            status_information_length == sizeof(DWORD)) {
            state->complete(ERROR_SUCCESS,
                            *static_cast<DWORD*>(status_information));
        } else {
            state->complete(ERROR_INVALID_DATA, 0);
        }
        return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        if (status_information != nullptr &&
            status_information_length == sizeof(WINHTTP_ASYNC_RESULT)) {
            const auto* result =
                static_cast<const WINHTTP_ASYNC_RESULT*>(status_information);
            state->complete(result->dwError, 0);
        } else {
            state->complete(ERROR_INVALID_DATA, 0);
        }
        return;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(state->handle_closed.get());
        return;
    default:
        return;
    }
}

class AsyncRequestHandle final {
public:
    AsyncRequestHandle(HINTERNET handle, AsyncOperationState& state) noexcept
        : handle_(handle), state_(state) {}
    ~AsyncRequestHandle() { close(); }
    AsyncRequestHandle(const AsyncRequestHandle&) = delete;
    AsyncRequestHandle& operator=(const AsyncRequestHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    void callbacks_registered() noexcept { callbacks_registered_ = true; }

    void close() noexcept {
        if (handle_ == nullptr) {
            return;
        }
        const bool close_started = WinHttpCloseHandle(handle_) != FALSE;
        handle_ = nullptr;
        if (close_started && callbacks_registered_) {
            WaitForSingleObject(state_.handle_closed.get(), INFINITE);
        }
    }

private:
    HINTERNET handle_ = nullptr;
    AsyncOperationState& state_;
    bool callbacks_registered_ = false;
};

struct CrackedUrl final {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool crack_url(std::string_view endpoint, CrackedUrl& result) {
    if (endpoint.empty() || endpoint.size() > 16'384 || !valid_utf8(endpoint)) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(endpoint);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
                         &components)) {
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    result.host.assign(components.lpszHostName, components.dwHostNameLength);
    result.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        result.path.append(components.lpszExtraInfo,
                           components.dwExtraInfoLength);
    }
    if (result.path.empty()) {
        result.path = L"/";
    }
    result.port = components.nPort;
    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return !result.host.empty();
}

int32_t map_http_status(const ChatCancellation& cancellation, DWORD error) {
    if (cancellation.cancelled() ||
        error == ERROR_WINHTTP_OPERATION_CANCELLED) {
        return SAO_AI_EDITOR_ERR_CANCELLED;
    }
    if (error == ERROR_SUCCESS) {
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_HTTP;
}

template <typename Operation>
int32_t await_winhttp_operation(AsyncRequestHandle& request,
                                AsyncOperationState& state,
                                const ChatCancellation& cancellation,
                                uint32_t timeout_ms,
                                Operation&& operation) {
    state.prepare();
    if (!operation()) {
        return map_http_status(cancellation, GetLastError());
    }
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (cancellation.cancelled()) {
            request.close();
            return SAO_AI_EDITOR_ERR_CANCELLED;
        }
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeout_ms) {
            request.close();
            return SAO_AI_EDITOR_ERR_HTTP;
        }
        const DWORD wait_ms = static_cast<DWORD>(std::min<ULONGLONG>(
            kWinHttpPollIntervalMs, timeout_ms - elapsed));
        const DWORD wait = WaitForSingleObject(state.completed.get(), wait_ms);
        if (wait == WAIT_OBJECT_0) {
            return map_http_status(
                cancellation, state.error.load(std::memory_order_acquire));
        }
        if (wait != WAIT_TIMEOUT) {
            request.close();
            return SAO_AI_EDITOR_ERR_HTTP;
        }
    }
}

bool append_response(AsyncRequestHandle& request,
                     AsyncOperationState& operation_state,
                     ChatCancellation& cancellation,
                     uint32_t timeout_ms,
                     std::string& response,
                     const std::function<int32_t(std::string_view)>& consume,
                     int32_t& status) {
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        if (cancellation.cancelled()) {
            status = SAO_AI_EDITOR_ERR_CANCELLED;
            return false;
        }
        status = await_winhttp_operation(
            request, operation_state, cancellation, timeout_ms, [&] {
                return WinHttpQueryDataAvailable(request.get(), nullptr) != FALSE;
        });
        if (status != SAO_AI_EDITOR_OK) {
            return false;
        }
        DWORD available =
            operation_state.transferred.load(std::memory_order_acquire);
        if (available == 0) {
            return true;
        }
        while (available > 0) {
            const DWORD requested =
                std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            status = await_winhttp_operation(
                request, operation_state, cancellation, timeout_ms, [&] {
                    return WinHttpReadData(request.get(), buffer.data(), requested,
                                           nullptr) != FALSE;
            });
            if (status != SAO_AI_EDITOR_OK) {
                return false;
            }
            const DWORD received =
                operation_state.transferred.load(std::memory_order_acquire);
            if (received == 0) {
                return true;
            }
            if (response.size() + received > kMaximumJsonBytes) {
                status = SAO_AI_EDITOR_ERR_PROTOCOL;
                return false;
            }
            const std::string_view chunk(buffer.data(), received);
            if (consume) {
                status = consume(chunk);
                if (status != SAO_AI_EDITOR_OK) {
                    return false;
                }
            } else {
                response.append(chunk);
            }
            available -= received;
        }
    }
}

}  // namespace

ChatCancellation::~ChatCancellation() = default;

void ChatCancellation::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
}

bool ChatCancellation::cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
}

bool ChatCancellation::attach(HINTERNET request) noexcept {
    if (cancelled()) {
        WinHttpCloseHandle(request);
        return false;
    }
    request_ = request;
    return true;
}

void ChatCancellation::detach_and_close() noexcept {
    if (request_ != nullptr) {
        WinHttpCloseHandle(request_);
        request_ = nullptr;
    }
}

int32_t perform_openai_chat(const HttpChatRequest& request,
                            ChatCancellation& cancellation,
                            const StreamEventCallback& callback,
                            Json& result) {
    using SteadyClock = std::chrono::steady_clock;
    // Anchor: request assembly starts here.  We record it before URL cracking
    // so misbehaving arguments still show up as `totalMs` on the error path.
    const auto request_started_at = SteadyClock::now();
    std::optional<SteadyClock::time_point> first_token_at;
    std::optional<int64_t> completion_tokens;

    const auto attach_metrics = [&](Json& target,
                                    const SteadyClock::time_point& done_at) {
        const auto total_ms = std::chrono::duration_cast<
                                  std::chrono::milliseconds>(
                                  done_at - request_started_at)
                                  .count();
        Json metrics{{"totalMs", total_ms}};
        if (first_token_at) {
            const auto ttf_ms = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    *first_token_at - request_started_at)
                                    .count();
            metrics["ttfMs"] = ttf_ms;
        }
        if (completion_tokens) {
            metrics["completionTokens"] = *completion_tokens;
            const double seconds = static_cast<double>(total_ms) / 1000.0;
            if (seconds > 0.0) {
                metrics["tokensPerSecond"] =
                    static_cast<double>(*completion_tokens) / seconds;
            }
        }
        metrics["provider_type"] = request.provider_type;
        if (target.is_object()) {
            target["metrics"] = std::move(metrics);
        }
    };

    CrackedUrl url;
    if (!crack_url(request.endpoint, url) || request.request_json.empty() ||
        request.request_json.size() > kMaximumJsonBytes ||
        !valid_utf8(request.request_json) || !valid_utf8(request.api_key)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    InternetHandle session(WinHttpOpen(
        L"SAO-AI-Editor/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    const uint32_t timeout_ms =
        std::clamp(request.timeout_ms, 1'000U, 600'000U);
    const int timeout = static_cast<int>(timeout_ms);
    if (!WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout)) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    InternetHandle connection(
        WinHttpConnect(session.get(), url.host.c_str(), url.port, 0));
    if (!connection) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    AsyncOperationState operation_state;
    if (!operation_state.valid()) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    AsyncRequestHandle request_handle(WinHttpOpenRequest(
        connection.get(), L"POST", url.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags),
        operation_state);
    if (!request_handle) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    if (!WinHttpSetTimeouts(request_handle.get(), timeout, timeout, timeout,
                            timeout)) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    DWORD_PTR callback_context =
        reinterpret_cast<DWORD_PTR>(&operation_state);
    if (!WinHttpSetOption(request_handle.get(), WINHTTP_OPTION_CONTEXT_VALUE,
                          &callback_context, sizeof(callback_context))) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    constexpr DWORD callback_flags =
        WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE |
        WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE |
        WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE |
        WINHTTP_CALLBACK_STATUS_READ_COMPLETE |
        WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE |
        WINHTTP_CALLBACK_STATUS_REQUEST_ERROR |
        WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING;
    if (WinHttpSetStatusCallback(request_handle.get(), winhttp_status_callback,
                                 callback_flags, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    request_handle.callbacks_registered();
    DWORD receive_response_timeout = timeout_ms;
    if (!WinHttpSetOption(request_handle.get(),
                          WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                          &receive_response_timeout,
                          sizeof(receive_response_timeout))) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    if (cancellation.cancelled()) {
        return SAO_AI_EDITOR_ERR_CANCELLED;
    }

    int32_t final_status = SAO_AI_EDITOR_OK;
    std::wstring authorization_line;
    if (!request.authorization.empty() &&
        valid_utf8(request.authorization)) {
        authorization_line =
            L"Authorization: " + utf8_to_wide(request.authorization) + L"\r\n";
    } else if (request.authorization.empty() && !request.api_key.empty() &&
               request.provider_type != "anthropic" &&
               request.provider_type != "gemini") {
        authorization_line =
            L"Authorization: Bearer " + utf8_to_wide(request.api_key) +
            L"\r\n";
    }
    std::wstring extras;
    if (!request.extra_headers.empty() && valid_utf8(request.extra_headers)) {
        extras = utf8_to_wide(request.extra_headers);
    }
    const std::wstring headers =
        L"Content-Type: application/json\r\nAccept: " +
        std::wstring(request.stream ? L"text/event-stream" : L"application/json") +
        L"\r\n" + authorization_line + extras;
    const ULONGLONG send_started = GetTickCount64();
    final_status = await_winhttp_operation(
        request_handle, operation_state, cancellation, timeout_ms, [&] {
            return WinHttpSendRequest(
                       request_handle.get(), headers.c_str(),
                       static_cast<DWORD>(headers.size()),
                       WINHTTP_NO_REQUEST_DATA, 0,
                       static_cast<DWORD>(request.request_json.size()),
                       callback_context) != FALSE;
        });
    if (final_status != SAO_AI_EDITOR_OK) {
        return final_status;
    }
    size_t written = 0;
    while (written < request.request_json.size()) {
        const ULONGLONG send_elapsed = GetTickCount64() - send_started;
        if (send_elapsed >= timeout_ms) {
            return SAO_AI_EDITOR_ERR_HTTP;
        }
        const DWORD remaining =
            static_cast<DWORD>(request.request_json.size() - written);
        final_status = await_winhttp_operation(
            request_handle, operation_state, cancellation,
            static_cast<uint32_t>(timeout_ms - send_elapsed), [&] {
                return WinHttpWriteData(request_handle.get(),
                                        request.request_json.data() + written,
                                        remaining, nullptr) != FALSE;
            });
        if (final_status != SAO_AI_EDITOR_OK) {
            return final_status;
        }
        const DWORD chunk_written =
            operation_state.transferred.load(std::memory_order_acquire);
        if (chunk_written == 0 || chunk_written > remaining) {
            return SAO_AI_EDITOR_ERR_HTTP;
        }
        written += chunk_written;
    }
    final_status = await_winhttp_operation(
        request_handle, operation_state, cancellation, timeout_ms, [&] {
            return WinHttpReceiveResponse(request_handle.get(), nullptr) != FALSE;
        });
    if (final_status != SAO_AI_EDITOR_OK) {
        return final_status;
    }
    // Non-stream requests only get one payload from the server, so treat the
    // moment we receive headers as the first-token proxy.  Streaming requests
    // overwrite this the first time a `type=="delta"` event is delivered.
    if (!request.stream) {
        first_token_at = SteadyClock::now();
    }

    DWORD http_status = 0;
    DWORD status_size = sizeof(http_status);
    if (!WinHttpQueryHeaders(request_handle.get(),
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &http_status,
                             &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return map_http_status(cancellation, GetLastError());
    }

    std::string response;
    using StreamCodec = std::variant<OpenAiSseCodec,
                                     AnthropicSseCodec,
                                     GeminiSseCodec>;
    StreamCodec stream_codec = [&]() -> StreamCodec {
        if (request.provider_type == "anthropic") {
            return StreamCodec{std::in_place_type<AnthropicSseCodec>};
        }
        if (request.provider_type == "gemini") {
            return StreamCodec{std::in_place_type<GeminiSseCodec>};
        }
        return StreamCodec{std::in_place_type<OpenAiSseCodec>};
    }();
    const auto feed_codec = [&](std::string_view chunk, Json& events) -> int32_t {
        return std::visit(
            [&](auto& codec) -> int32_t { return codec.feed(chunk, events); },
            stream_codec);
    };
    const auto observe_event_for_metrics = [&](const Json& event) {
        if (!event.is_object()) {
            return;
        }
        const std::string type = event.value("type", std::string{});
        if (!first_token_at && type == "delta" && event.contains("content") &&
            event["content"].is_string() &&
            !event["content"].get<std::string>().empty()) {
            first_token_at = SteadyClock::now();
        }
        if (event.contains("usage") && event["usage"].is_object()) {
            const auto maybe_completion =
                extract_completion_tokens(event["usage"]);
            if (maybe_completion) {
                completion_tokens = maybe_completion;
            }
        }
    };
    const auto consume = request.stream
        ? std::function<int32_t(std::string_view)>(
              [&](std::string_view chunk) -> int32_t {
                  Json events;
                  const int32_t status = feed_codec(chunk, events);
                  if (status != SAO_AI_EDITOR_OK) {
                      return status;
                  }
                  for (const auto& event : events) {
                      observe_event_for_metrics(event);
                      callback(event);
                  }
                  return SAO_AI_EDITOR_OK;
              })
        : std::function<int32_t(std::string_view)>();
    const bool read_ok = append_response(
        request_handle, operation_state, cancellation, timeout_ms, response,
        consume, final_status);
    if (!read_ok) {
        return final_status;
    }

    if (http_status < 200 || http_status >= 300) {
        Json error_body = Json::parse(response, nullptr, false);
        result = Json{{"httpStatus", http_status},
                      {"body", error_body.is_discarded()
                                   ? Json(response)
                                   : std::move(error_body)}};
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    if (request.stream) {
        Json trailing;
        final_status = feed_codec("\n\n", trailing);
        if (final_status != SAO_AI_EDITOR_OK) {
            return final_status;
        }
        for (const auto& event : trailing) {
            observe_event_for_metrics(event);
            callback(event);
        }
        result = Json{{"ok", true}, {"stream", true}};
        attach_metrics(result, SteadyClock::now());
        return SAO_AI_EDITOR_OK;
    }
    int32_t decode_status = SAO_AI_EDITOR_OK;
    if (request.provider_type == "anthropic" ||
        request.provider_type == "gemini") {
        ProviderRoute route;
        route.type = request.provider_type;
        decode_status = decode_provider_response(route, response, result);
    } else {
        decode_status = decode_openai_response_text(response, result);
    }
    if (decode_status == SAO_AI_EDITOR_OK) {
        if (result.is_object() && result.contains("usage") &&
            result["usage"].is_object()) {
            const auto maybe_completion =
                extract_completion_tokens(result["usage"]);
            if (maybe_completion) {
                completion_tokens = maybe_completion;
            }
        }
        attach_metrics(result, SteadyClock::now());
    }
    return decode_status;
}

}  // namespace sao::ai_editor::native
