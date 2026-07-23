#pragma once

#include <string>
#include <string_view>

#include "native_utils.h"

namespace sao::ai_editor::native {

// Per-request provider descriptor built from `provider` payload
// (`providers.configure` schema).  `type` is normalised to one of
// "openai" (default), "anthropic", "gemini".
struct ProviderRoute {
    std::string provider_id;
    std::string type{"openai"};
    std::string transport{"chat_completions"};
    std::string endpoint;
    std::string api_key;
    std::string model;
    std::string version;      // e.g. Anthropic "anthropic-version"
    std::string api_key_env;  // fallback env var name
    Json extra_headers = Json::object();
    Json extra_body = Json::object();
};

ProviderRoute normalise_provider(const Json& provider, std::string model_hint);

[[nodiscard]] bool is_openai_responses_endpoint(
    std::string_view endpoint) noexcept;

// Rewrite an OpenAI-flavoured chat.completions body into the provider
// wire format.  On return, `out_endpoint` may be swapped to the provider's
// native endpoint (Gemini appends model + key), and `out_headers` carries
// the extra HTTP headers each provider requires (Anthropic x-api-key +
// version; Gemini x-goog-api-key when preferred).
struct ProviderRequest {
    std::string endpoint;
    std::string body_json;
    std::string authorization;  // "Bearer ..." for openai; empty otherwise
    std::string extra_headers;  // CRLF-terminated block
};

int32_t build_provider_request(const ProviderRoute& route,
                               const Json& openai_body,
                               ProviderRequest& out);

// Decode the provider-native response back to the SAO-normalised
// {ok, content, role, tool_calls, usage, finish_reason} shape.
int32_t decode_provider_response(const ProviderRoute& route,
                                 std::string_view payload,
                                 Json& out_normalised);

// Anthropic /v1/messages `stream: true` event decoder.  Consumes the
// Anthropic-native `event:` + `data:` chunks and emits the same
// {type:"delta", content:"..."} / {type:"done"} shape the OpenAI SSE
// decoder does so downstream callers see one uniform stream.
class AnthropicSseCodec final {
public:
    int32_t feed(std::string_view bytes, Json& events);
private:
    int32_t dispatch_event(Json& events);
    std::string line_buffer_;
    std::string event_data_;
    bool done_ = false;
};

// Gemini `:streamGenerateContent?alt=sse` event decoder.
class GeminiSseCodec final {
public:
    int32_t feed(std::string_view bytes, Json& events);
private:
    int32_t dispatch_event(Json& events);
    std::string line_buffer_;
    std::string event_data_;
    bool done_ = false;
};

// OpenAI /v1/responses `stream: true` event decoder.  Responses emits
// typed SSE events instead of Chat Completions chunks, so this codec maps
// output-text, reasoning, refusal, function-call, usage, and completion
// events into the same normalized stream shape consumed by the runtime.
class OpenAiResponsesSseCodec final {
public:
    int32_t feed(std::string_view bytes, Json& events);

private:
    int32_t dispatch_event(Json& events);
    int32_t merge_tool_call(const Json& item, size_t output_index,
                            bool append_arguments, Json* event);
    void emit_tool_calls_final(Json& events);

    std::string line_buffer_;
    std::string event_data_;
    Json tool_calls_accumulator_ = Json::array();
    bool tool_calls_emitted_ = false;
    bool done_ = false;
};

}  // namespace sao::ai_editor::native
