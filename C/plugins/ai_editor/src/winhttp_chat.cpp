#include "winhttp_chat.h"

#include "chat_provider_router.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace sao::ai_editor::native {
namespace {

constexpr DWORD kWinHttpPollIntervalMs = 50;

// Best-effort extraction of a token field from provider-shaped usage
// blocks.  Tries each candidate name in order and returns the first one
// present as a numeric value.  Non-numeric entries are skipped so a bad
// server payload never fails hard on the caller side.
std::optional<int64_t> extract_usage_token_field(
    const Json& usage,
    std::initializer_list<std::string_view> field_names) {
    if (!usage.is_object()) {
        return std::nullopt;
    }
    for (const std::string_view field : field_names) {
        const auto it = usage.find(std::string(field));
        if (it == usage.end()) {
            continue;
        }
        if (it->is_number_integer()) {
            return it->get<int64_t>();
        }
        if (it->is_number()) {
            return static_cast<int64_t>(it->get<double>());
        }
    }
    return std::nullopt;
}

// Best-effort extraction of "completion tokens" from provider-shaped usage
// blocks: OpenAI/OpenAI-compat use `completion_tokens`, Anthropic uses
// `output_tokens`, Gemini's `usageMetadata` uses `candidatesTokenCount`.
// Returns std::nullopt when no numeric completion-token field is present.
std::optional<int64_t> extract_completion_tokens(const Json& usage) {
    return extract_usage_token_field(
        usage, {"completion_tokens", "output_tokens", "candidatesTokenCount"});
}

// Companion to extract_completion_tokens for input/prompt tokens.  OpenAI
// uses `prompt_tokens`, Anthropic uses `input_tokens`, Gemini's
// `usageMetadata` uses `promptTokenCount`.  Returns std::nullopt when the
// provider omits the field or reports it non-numeric.
std::optional<int64_t> extract_prompt_tokens(const Json& usage) {
    return extract_usage_token_field(
        usage, {"prompt_tokens", "input_tokens", "promptTokenCount"});
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
                                Operation&& operation,
                                bool* client_timeout_flag = nullptr) {
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
            if (client_timeout_flag != nullptr) {
                *client_timeout_flag = true;
            }
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
                     int32_t& status,
                     bool* client_timeout_flag = nullptr) {
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        if (cancellation.cancelled()) {
            status = SAO_AI_EDITOR_ERR_CANCELLED;
            return false;
        }
        status = await_winhttp_operation(
            request, operation_state, cancellation, timeout_ms, [&] {
                return WinHttpQueryDataAvailable(request.get(), nullptr) != FALSE;
            },
            client_timeout_flag);
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
                },
                client_timeout_flag);
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
    std::optional<int64_t> prompt_tokens;

    // Pull optional `promptPer1K` / `completionPer1K` numbers off the pricing
    // rule the caller injected.  Missing / non-object rule means no cost
    // math.  Zero-valued fields still count as "rule applied" so callers
    // can intentionally publish free-tier pricing without falsely reporting
    // `pricingApplied:false`.
    const bool has_pricing_rule =
        request.pricing_rule.is_object() && !request.pricing_rule.empty();
    const auto pricing_field = [&](const char* key) -> double {
        if (!request.pricing_rule.is_object()) {
            return 0.0;
        }
        const auto it = request.pricing_rule.find(key);
        if (it == request.pricing_rule.end() || !it->is_number()) {
            return 0.0;
        }
        return it->get<double>();
    };
    const double prompt_per_1k = pricing_field("promptPer1K");
    const double completion_per_1k = pricing_field("completionPer1K");

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
        if (prompt_tokens) {
            metrics["promptTokens"] = *prompt_tokens;
        }
        if (completion_tokens) {
            metrics["completionTokens"] = *completion_tokens;
            const double seconds = static_cast<double>(total_ms) / 1000.0;
            if (seconds > 0.0) {
                metrics["tokensPerSecond"] =
                    static_cast<double>(*completion_tokens) / seconds;
            }
        }
        // Cost math only fires when the caller supplied a pricing rule.
        // Without a rule we emit `pricingApplied:false` and skip `costUsd`
        // so the wire contract remains clear: a numeric `costUsd` implies
        // the caller had a rule (even if the derived cost is 0 because
        // both fields are zero-valued).  Missing prompt / completion
        // counts default to 0 in the cost formula rather than dropping the
        // whole computation — a partial usage block still yields a partial
        // (lower-bound) cost estimate.
        metrics["pricingApplied"] = has_pricing_rule;
        if (has_pricing_rule) {
            const double prompt_count = prompt_tokens
                                            ? static_cast<double>(*prompt_tokens)
                                            : 0.0;
            const double completion_count =
                completion_tokens ? static_cast<double>(*completion_tokens)
                                  : 0.0;
            const double cost_usd =
                prompt_count / 1000.0 * prompt_per_1k +
                completion_count / 1000.0 * completion_per_1k;
            metrics["costUsd"] = cost_usd;
        } else {
            metrics["costUsd"] = 0.0;
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
    // Latched by await_winhttp_operation when the wait loop hits the caller's
    // deadline (vs a server-side error).  The retry loop uses this to skip
    // network-class retries — client timeouts almost never resolve just by
    // reissuing the request, and they would blow past the timeout budget.
    bool client_timeout = false;
    const auto stamp_timeout = [&](int32_t status) -> int32_t {
        if (status != SAO_AI_EDITOR_OK && client_timeout) {
            if (!result.is_object()) {
                result = Json::object();
            }
            result["clientTimeout"] = true;
        }
        return status;
    };
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
        },
        &client_timeout);
    if (final_status != SAO_AI_EDITOR_OK) {
        return stamp_timeout(final_status);
    }
    size_t written = 0;
    while (written < request.request_json.size()) {
        const ULONGLONG send_elapsed = GetTickCount64() - send_started;
        if (send_elapsed >= timeout_ms) {
            client_timeout = true;
            return stamp_timeout(SAO_AI_EDITOR_ERR_HTTP);
        }
        const DWORD remaining =
            static_cast<DWORD>(request.request_json.size() - written);
        final_status = await_winhttp_operation(
            request_handle, operation_state, cancellation,
            static_cast<uint32_t>(timeout_ms - send_elapsed), [&] {
                return WinHttpWriteData(request_handle.get(),
                                        request.request_json.data() + written,
                                        remaining, nullptr) != FALSE;
            },
            &client_timeout);
        if (final_status != SAO_AI_EDITOR_OK) {
            return stamp_timeout(final_status);
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
        },
        &client_timeout);
    if (final_status != SAO_AI_EDITOR_OK) {
        return stamp_timeout(final_status);
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
            const auto maybe_prompt = extract_prompt_tokens(event["usage"]);
            if (maybe_prompt) {
                prompt_tokens = maybe_prompt;
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
        consume, final_status, &client_timeout);
    if (!read_ok) {
        return stamp_timeout(final_status);
    }

    if (http_status < 200 || http_status >= 300) {
        Json error_body = Json::parse(response, nullptr, false);
        result = Json{{"httpStatus", http_status},
                      {"body", error_body.is_discarded()
                                   ? Json(response)
                                   : std::move(error_body)}};
        // Best-effort Retry-After capture (retry loop honours this so the
        // sleep respects the server's advertised backoff).  Values may be
        // either delta-seconds ("30") or an HTTP-date; both are supported.
        // Anything unparseable is silently ignored.
        DWORD retry_after_size = 0;
        WinHttpQueryHeaders(request_handle.get(),
                            WINHTTP_QUERY_CUSTOM,
                            L"Retry-After", WINHTTP_NO_OUTPUT_BUFFER,
                            &retry_after_size, WINHTTP_NO_HEADER_INDEX);
        if (retry_after_size > 0 &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring header((retry_after_size / sizeof(wchar_t)) + 1,
                                L'\0');
            DWORD header_bytes = retry_after_size;
            if (WinHttpQueryHeaders(request_handle.get(),
                                    WINHTTP_QUERY_CUSTOM,
                                    L"Retry-After", header.data(),
                                    &header_bytes,
                                    WINHTTP_NO_HEADER_INDEX)) {
                header.resize(header_bytes / sizeof(wchar_t));
                // Trim NULs / whitespace on both sides.
                while (!header.empty() &&
                       (header.back() == L'\0' || header.back() == L' ')) {
                    header.pop_back();
                }
                size_t start = 0;
                while (start < header.size() && header[start] == L' ') {
                    ++start;
                }
                header.erase(0, start);
                if (!header.empty()) {
                    // Try numeric delta-seconds first (fast path).
                    bool numeric = !header.empty();
                    for (const wchar_t ch : header) {
                        if (ch < L'0' || ch > L'9') {
                            numeric = false;
                            break;
                        }
                    }
                    int64_t retry_after_ms = -1;
                    if (numeric) {
                        wchar_t* end = nullptr;
                        errno = 0;
                        const long long seconds =
                            std::wcstoll(header.c_str(), &end, 10);
                        if (errno == 0 && end != nullptr && *end == L'\0' &&
                            seconds >= 0) {
                            retry_after_ms =
                                static_cast<int64_t>(seconds) * 1000;
                        }
                    } else {
                        // HTTP-date via WinHTTP helper (RFC 7231 § 7.1.1.1).
                        SYSTEMTIME parsed{};
                        if (WinHttpTimeToSystemTime(header.c_str(), &parsed)) {
                            FILETIME then_ft{};
                            FILETIME now_ft{};
                            SystemTimeToFileTime(&parsed, &then_ft);
                            GetSystemTimeAsFileTime(&now_ft);
                            const uint64_t then =
                                (static_cast<uint64_t>(then_ft.dwHighDateTime)
                                     << 32) |
                                then_ft.dwLowDateTime;
                            const uint64_t now =
                                (static_cast<uint64_t>(now_ft.dwHighDateTime)
                                     << 32) |
                                now_ft.dwLowDateTime;
                            if (then > now) {
                                // FILETIME ticks are 100 ns.
                                retry_after_ms =
                                    static_cast<int64_t>((then - now) /
                                                          10'000ULL);
                            } else {
                                retry_after_ms = 0;
                            }
                        }
                    }
                    if (retry_after_ms >= 0) {
                        result["retryAfterMs"] = retry_after_ms;
                    }
                }
            }
        }
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
            const auto maybe_prompt = extract_prompt_tokens(result["usage"]);
            if (maybe_prompt) {
                prompt_tokens = maybe_prompt;
            }
        }
        attach_metrics(result, SteadyClock::now());
    }
    return decode_status;
}

RetryPolicy RetryPolicy::from_json(const Json& value) {
    RetryPolicy policy;
    if (!value.is_object()) {
        return policy;
    }
    const auto pick_u32 = [&](const char* key, uint32_t& out) {
        const auto it = value.find(key);
        if (it == value.end()) {
            return;
        }
        if (it->is_number_integer()) {
            const auto raw = it->get<int64_t>();
            if (raw >= 0) {
                out = static_cast<uint32_t>(
                    std::min<int64_t>(raw, std::numeric_limits<uint32_t>::max()));
            }
        } else if (it->is_number()) {
            const auto raw = it->get<double>();
            if (raw >= 0.0) {
                out = static_cast<uint32_t>(
                    std::min<double>(raw,
                                     static_cast<double>(
                                         std::numeric_limits<uint32_t>::max())));
            }
        }
    };
    const auto pick_bool = [&](const char* key, bool& out) {
        const auto it = value.find(key);
        if (it != value.end() && it->is_boolean()) {
            out = it->get<bool>();
        }
    };
    pick_u32("maxAttempts", policy.max_attempts);
    pick_u32("initialDelayMs", policy.initial_delay_ms);
    pick_u32("maxDelayMs", policy.max_delay_ms);
    if (const auto it = value.find("multiplier");
        it != value.end() && it->is_number()) {
        const auto raw = it->get<double>();
        if (std::isfinite(raw) && raw >= 1.0) {
            policy.multiplier = raw;
        }
    }
    if (const auto it = value.find("jitter");
        it != value.end() && it->is_number()) {
        const auto raw = it->get<double>();
        if (std::isfinite(raw) && raw >= 0.0 && raw <= 1.0) {
            policy.jitter = raw;
        }
    }
    if (const auto it = value.find("retryOnStatuses");
        it != value.end() && it->is_array()) {
        std::vector<uint32_t> parsed;
        parsed.reserve(it->size());
        for (const auto& entry : *it) {
            if (entry.is_number_integer()) {
                const auto raw = entry.get<int64_t>();
                if (raw >= 100 && raw <= 599) {
                    parsed.push_back(static_cast<uint32_t>(raw));
                }
            }
        }
        // Empty array = "never retry on status" — respect the caller intent
        // rather than silently reverting to defaults.
        policy.retry_on_statuses = std::move(parsed);
    }
    pick_bool("retryOnNetwork", policy.retry_on_network);
    if (const auto it = value.find("idempotencyKey");
        it != value.end() && it->is_string()) {
        policy.idempotency_key = it->get<std::string>();
    }
    pick_bool("respectRetryAfter", policy.respect_retry_after);
    // Legacy spelling: `retryAfterHeader` was the schema documented in the
    // R7 handoff; support both to avoid breaking config that landed early.
    pick_bool("retryAfterHeader", policy.respect_retry_after);
    // Cap max_attempts at a safe upper bound so a stray large integer cannot
    // wedge the runtime in a multi-minute retry loop.
    if (policy.max_attempts > 10) {
        policy.max_attempts = 10;
    }
    return policy;
}

bool RetryPolicy::should_retry_status(uint32_t code) const noexcept {
    return std::find(retry_on_statuses.begin(), retry_on_statuses.end(),
                     code) != retry_on_statuses.end();
}

namespace {

// Cancellable sleep — checks the cancellation flag at ~50 ms intervals so
// callers that Ctrl-C mid-backoff observe the same latency as any other
// HTTP operation.  Returns true when the full delay elapsed, false when
// cancellation woke the sleep early.
bool cancellable_sleep(uint32_t delay_ms,
                       const ChatCancellation& cancellation) {
    constexpr uint32_t slice_ms = 50;
    uint32_t remaining = delay_ms;
    while (remaining > 0) {
        if (cancellation.cancelled()) {
            return false;
        }
        const uint32_t slice = std::min(slice_ms, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return !cancellation.cancelled();
}

uint32_t compute_backoff_ms(const RetryPolicy& policy, uint32_t attempt,
                            std::mt19937_64& rng) {
    // attempt is 1-based (first retry == attempt 1 in this helper's frame).
    if (policy.initial_delay_ms == 0) {
        return 0;
    }
    double base = static_cast<double>(policy.initial_delay_ms);
    // Exponential growth: base * multiplier^(attempt-1).  Guarded against
    // multiplier<=0 upstream via from_json's `raw >= 1.0` gate.
    for (uint32_t index = 1; index < attempt; ++index) {
        base *= policy.multiplier;
        if (base >= static_cast<double>(policy.max_delay_ms)) {
            base = static_cast<double>(policy.max_delay_ms);
            break;
        }
    }
    if (base > static_cast<double>(policy.max_delay_ms)) {
        base = static_cast<double>(policy.max_delay_ms);
    }
    if (policy.jitter > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        const double random = dist(rng);
        const double scale =
            1.0 - (policy.jitter * 0.5) + (policy.jitter * random);
        base *= scale;
    }
    if (base < 0.0) {
        base = 0.0;
    }
    if (base > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        base = static_cast<double>(std::numeric_limits<uint32_t>::max());
    }
    return static_cast<uint32_t>(base);
}

// Distinguish a "server said 4xx/5xx" HTTP failure (result contains
// httpStatus) from a transport-level failure (no status captured).  Used
// to route between the retry_on_statuses / retry_on_network branches.
bool result_has_http_status(const Json& result, uint32_t& out_status) {
    if (!result.is_object()) {
        return false;
    }
    const auto it = result.find("httpStatus");
    if (it == result.end() || !it->is_number_integer()) {
        return false;
    }
    const auto raw = it->get<int64_t>();
    if (raw < 100 || raw > 599) {
        return false;
    }
    out_status = static_cast<uint32_t>(raw);
    return true;
}

// True when perform_openai_chat latched the "client timeout" sentinel via
// stamp_timeout.  Client-side deadline hits almost never recover just by
// reissuing the request within the same budget, so we exclude them from
// the network-retry branch even when retry_on_network is enabled.  This
// keeps workflows.cancel-style tests (which lean on short timeouts to
// wake a blocked HTTP wait) unaffected by the retry loop.
bool result_is_client_timeout(const Json& result) {
    return result.is_object() && result.value("clientTimeout", false);
}

}  // namespace

int32_t perform_openai_chat_with_retry(
    const HttpChatRequest& request,
    ChatCancellation& cancellation,
    const StreamEventCallback& callback,
    const RetryNotifyCallback& on_retry,
    Json& result) {
    // If the caller injected an idempotency key, splice it into
    // extra_headers so the transport layer sends the same key on every
    // retry.  We only append when the caller has not already supplied
    // the header themselves (case-insensitive check).
    HttpChatRequest working = request;
    if (!working.retry.idempotency_key.empty()) {
        const std::string lower_hdr = [&] {
            std::string tmp = working.extra_headers;
            std::transform(tmp.begin(), tmp.end(), tmp.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            return tmp;
        }();
        if (lower_hdr.find("idempotency-key:") == std::string::npos) {
            working.extra_headers +=
                "Idempotency-Key: " + working.retry.idempotency_key + "\r\n";
        }
    }

    const uint32_t max_attempts = std::max<uint32_t>(working.retry.max_attempts, 1);
    std::mt19937_64 rng(static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));

    int32_t last_status = SAO_AI_EDITOR_OK;
    Json last_result;
    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
        if (cancellation.cancelled()) {
            return SAO_AI_EDITOR_ERR_CANCELLED;
        }
        Json attempt_result;
        const int32_t status = perform_openai_chat(
            working, cancellation, callback, attempt_result);
        last_status = status;
        last_result = std::move(attempt_result);
        if (status == SAO_AI_EDITOR_OK) {
            result = std::move(last_result);
            return SAO_AI_EDITOR_OK;
        }
        // User-driven cancellation never retries.
        if (cancellation.cancelled()) {
            result = std::move(last_result);
            return SAO_AI_EDITOR_ERR_CANCELLED;
        }
        if (attempt >= max_attempts) {
            break;
        }
        std::string reason;
        bool retryable = false;
        uint32_t http_status = 0;
        if (status == SAO_AI_EDITOR_ERR_HTTP &&
            result_has_http_status(last_result, http_status) &&
            working.retry.should_retry_status(http_status)) {
            retryable = true;
            reason = (http_status == 429) ? "429" : "http_5xx";
        } else if (working.retry.retry_on_network &&
                   (status == SAO_AI_EDITOR_ERR_HTTP ||
                    status == SAO_AI_EDITOR_ERR_CANCELLED) &&
                   !result_has_http_status(last_result, http_status) &&
                   !result_is_client_timeout(last_result)) {
            // Transport-level failure: perform_openai_chat surfaces network
            // errors via SAO_AI_EDITOR_ERR_HTTP without an httpStatus body,
            // and it can also route WinHTTP failures through
            // ERR_CANCELLED (when the flag stays false).  Neither case
            // populates a result body, so treat both as "network".
            // Client-side timeouts explicitly opt out — the retry would
            // just accumulate more latency past the caller's budget.
            retryable = true;
            reason = "network";
        }
        if (!retryable) {
            break;
        }
        uint32_t delay_ms = compute_backoff_ms(working.retry, attempt, rng);
        if (working.retry.respect_retry_after && last_result.is_object() &&
            last_result.contains("retryAfterMs") &&
            last_result["retryAfterMs"].is_number_integer()) {
            const int64_t hint = last_result["retryAfterMs"].get<int64_t>();
            if (hint > 0) {
                const uint32_t clamped = static_cast<uint32_t>(std::min<int64_t>(
                    hint, static_cast<int64_t>(working.retry.max_delay_ms)));
                if (clamped > delay_ms) {
                    delay_ms = clamped;
                }
            }
        }
        if (on_retry) {
            on_retry(attempt + 1, delay_ms, reason);
        }
        if (!cancellable_sleep(delay_ms, cancellation)) {
            result = std::move(last_result);
            return SAO_AI_EDITOR_ERR_CANCELLED;
        }
    }
    result = std::move(last_result);
    return last_status;
}

}  // namespace sao::ai_editor::native
