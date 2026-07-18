#pragma once

#include <string>

#include "native_utils.h"

namespace sao::ai_editor::native {

// Per-request provider descriptor built from `provider` payload
// (`providers.configure` schema).  `type` is normalised to one of
// "openai" (default), "anthropic", "gemini".
struct ProviderRoute {
    std::string type{"openai"};
    std::string endpoint;
    std::string api_key;
    std::string model;
    std::string version;      // e.g. Anthropic "anthropic-version"
    std::string api_key_env;  // fallback env var name
};

ProviderRoute normalise_provider(const Json& provider, std::string model_hint);

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

}  // namespace sao::ai_editor::native
