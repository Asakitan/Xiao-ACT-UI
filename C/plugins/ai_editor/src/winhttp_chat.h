#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "openai_codec_internal.h"

namespace sao::ai_editor::native {

// Automatic retry configuration for perform_openai_chat_with_retry.  Keys
// mirror the JSON `retry` object accepted by chat.run / provider config.
// Defaults are conservative (3 attempts, half-second base delay, ~30s cap)
// and behaviourally match the pre-retry world when max_attempts == 1.
struct RetryPolicy final {
    uint32_t max_attempts = 3;
    uint32_t initial_delay_ms = 500;
    uint32_t max_delay_ms = 30'000;
    double multiplier = 2.0;
    double jitter = 0.2;
    std::vector<uint32_t> retry_on_statuses{429, 500, 502, 503, 504};
    bool retry_on_network = true;
    std::string idempotency_key;
    bool respect_retry_after = true;

    // Parses a JSON `retry` object.  Unknown / non-object input yields the
    // default policy.  Individual fields fall back to their default when the
    // JSON key is missing or has the wrong type — invalid values do not
    // reject the whole policy so provider-side typos do not break chat.
    static RetryPolicy from_json(const Json& value);

    // True when `code` is in `retry_on_statuses`.
    [[nodiscard]] bool should_retry_status(uint32_t code) const noexcept;
};

struct HttpChatRequest final {
    std::string endpoint;
    std::string api_key;              // used only when authorization empty
    std::string authorization;        // full "Bearer ..." / "Basic ..." value
    std::string extra_headers;        // CRLF-terminated block for provider hdrs
    std::string request_json;
    std::string provider_type{"openai"};  // "openai" | "anthropic" | "gemini"
    uint32_t timeout_ms = 60'000;
    bool stream = false;
    RetryPolicy retry{};              // used by perform_openai_chat_with_retry
    // Optional pricing rule for cost estimation.  When present and shaped as
    // `{promptPer1K:number, completionPer1K:number}` (either or both fields
    // optional; missing fields treated as 0) perform_openai_chat computes
    // `costUsd = prompt_tokens/1000*promptPer1K + completion_tokens/1000*completionPer1K`
    // and attaches it to `result["metrics"]` alongside `pricingApplied:true`.
    // Empty / non-object / all-zero rules leave `costUsd = 0` and
    // `pricingApplied = false`.  The lookup itself is caller-side: the
    // runtime resolves provider+model against its pricing map and injects
    // the matched entry here before dispatching the request — winhttp_chat
    // never sees the pricing map itself.
    Json pricing_rule{};
};

class ChatCancellation final {
public:
    ~ChatCancellation();

    void cancel() noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    bool attach(HINTERNET request) noexcept;
    void detach_and_close() noexcept;

private:
    std::atomic<bool> cancelled_{false};
    HINTERNET request_ = nullptr;
};

using StreamEventCallback = std::function<void(const Json&)>;

// Fires once per scheduled retry, *before* the sleep starts, so callers can
// forward the notice to their event queue (e.g. runtime emits chat.retry).
// - attempt: 1-based index of the *upcoming* attempt (2 == "second try")
// - delay_ms: computed backoff (already includes jitter + Retry-After)
// - reason: "429" | "http_5xx" | "network"
using RetryNotifyCallback =
    std::function<void(uint32_t attempt, uint32_t delay_ms,
                       std::string_view reason)>;

int32_t perform_openai_chat(const HttpChatRequest& request,
                            ChatCancellation& cancellation,
                            const StreamEventCallback& callback,
                            Json& result);

// Wraps perform_openai_chat with exponential backoff.  Retries when the
// transport returns SAO_AI_EDITOR_ERR_HTTP with an HTTP status listed in
// `request.retry.retry_on_statuses`, or when the transport reports a
// network failure (SAO_AI_EDITOR_ERR_HTTP without a captured status,
// or SAO_AI_EDITOR_ERR_CANCELLED while the cancellation flag is *not*
// set — WinHTTP funnels transient network drops through the same
// cancellation channel).  The final attempt's transport result / status
// is returned verbatim so callers observe the same shape as
// perform_openai_chat when retries are exhausted.
int32_t perform_openai_chat_with_retry(
    const HttpChatRequest& request,
    ChatCancellation& cancellation,
    const StreamEventCallback& callback,
    const RetryNotifyCallback& on_retry,
    Json& result);

}  // namespace sao::ai_editor::native
