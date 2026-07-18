#include "chat_provider_router.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <utility>

namespace sao::ai_editor::native {
namespace {

std::string to_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

Json build_anthropic_messages(const Json& openai_messages,
                              std::string& out_system) {
    Json result = Json::array();
    for (const auto& message : openai_messages) {
        if (!message.is_object()) {
            continue;
        }
        const std::string role = message.value("role", std::string{});
        if (role == "system") {
            if (message.contains("content") && message["content"].is_string()) {
                if (!out_system.empty()) {
                    out_system.push_back('\n');
                }
                out_system += message["content"].get<std::string>();
            }
            continue;
        }
        Json converted;
        converted["role"] = role == "assistant" ? "assistant" : "user";
        if (message.contains("content") && message["content"].is_string()) {
            converted["content"] = Json::array({Json{
                {"type", "text"},
                {"text", message["content"].get<std::string>()}}});
        } else if (message.contains("content") &&
                   message["content"].is_array()) {
            converted["content"] = message["content"];
        } else {
            converted["content"] = Json::array();
        }
        result.push_back(std::move(converted));
    }
    return result;
}

Json build_gemini_contents(const Json& openai_messages,
                           std::string& out_system) {
    Json result = Json::array();
    for (const auto& message : openai_messages) {
        if (!message.is_object()) {
            continue;
        }
        const std::string role = message.value("role", std::string{});
        if (role == "system") {
            if (message.contains("content") && message["content"].is_string()) {
                if (!out_system.empty()) {
                    out_system.push_back('\n');
                }
                out_system += message["content"].get<std::string>();
            }
            continue;
        }
        Json parts = Json::array();
        if (message.contains("content") && message["content"].is_string()) {
            parts.push_back(Json{{"text",
                                    message["content"].get<std::string>()}});
        } else if (message.contains("content") &&
                   message["content"].is_array()) {
            for (const auto& part : message["content"]) {
                if (part.is_object() && part.value("type", "") == "text") {
                    parts.push_back(Json{{"text",
                                            part.value("text", std::string{})}});
                }
            }
        }
        result.push_back(Json{{"role", role == "assistant" ? "model" : "user"},
                              {"parts", std::move(parts)}});
    }
    return result;
}

std::string collect_anthropic_text(const Json& response) {
    if (!response.is_object() || !response.contains("content") ||
        !response["content"].is_array()) {
        return {};
    }
    std::string content;
    for (const auto& block : response["content"]) {
        if (block.is_object() && block.value("type", "") == "text" &&
            block.contains("text") && block["text"].is_string()) {
            content += block["text"].get<std::string>();
        }
    }
    return content;
}

std::string collect_gemini_text(const Json& response) {
    if (!response.is_object() || !response.contains("candidates") ||
        !response["candidates"].is_array() ||
        response["candidates"].empty()) {
        return {};
    }
    const auto& candidate = response["candidates"][0];
    if (!candidate.is_object() || !candidate.contains("content") ||
        !candidate["content"].is_object() ||
        !candidate["content"].contains("parts") ||
        !candidate["content"]["parts"].is_array()) {
        return {};
    }
    std::string content;
    for (const auto& part : candidate["content"]["parts"]) {
        if (part.is_object() && part.contains("text") &&
            part["text"].is_string()) {
            content += part["text"].get<std::string>();
        }
    }
    return content;
}

}  // namespace

ProviderRoute normalise_provider(const Json& provider,
                                 std::string model_hint) {
    ProviderRoute route;
    if (!provider.is_object()) {
        route.model = std::move(model_hint);
        return route;
    }
    route.type = to_lower(provider.value("type",
                                          provider.value("provider_type",
                                                          std::string{"openai"})));
    if (route.type != "openai" && route.type != "anthropic" &&
        route.type != "gemini") {
        route.type = "openai";
    }
    route.endpoint = provider.value(
        "endpoint",
        provider.value("base_url", std::string{}));
    route.api_key = provider.value("apiKey",
                                   provider.value("api_key", std::string{}));
    route.api_key_env = provider.value("apiKeyEnv", std::string{});
    route.model = !model_hint.empty()
        ? std::move(model_hint)
        : provider.value("model", std::string{});
    route.version = provider.value("version",
                                   provider.value("anthropicVersion",
                                                   std::string{}));
    if (route.type == "anthropic" && route.version.empty()) {
        route.version = "2023-06-01";
    }
    return route;
}

int32_t build_provider_request(const ProviderRoute& route,
                               const Json& openai_body,
                               ProviderRequest& out) {
    if (!openai_body.is_object() ||
        !openai_body.contains("messages") ||
        !openai_body["messages"].is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    out.endpoint.clear();
    out.body_json.clear();
    out.authorization.clear();
    out.extra_headers.clear();
    if (route.type == "openai") {
        out.endpoint = route.endpoint;
        out.body_json = openai_body.dump();
        if (!route.api_key.empty()) {
            out.authorization = "Bearer " + route.api_key;
        }
        return SAO_AI_EDITOR_OK;
    }
    if (route.type == "anthropic") {
        std::string system;
        Json anthropic_messages =
            build_anthropic_messages(openai_body["messages"], system);
        Json body{{"model", route.model.empty()
                                 ? openai_body.value("model", std::string{})
                                 : route.model},
                  {"max_tokens",
                   openai_body.value("max_tokens", 1024)},
                  {"messages", std::move(anthropic_messages)}};
        if (!system.empty()) {
            body["system"] = system;
        }
        if (openai_body.contains("temperature") &&
            openai_body["temperature"].is_number()) {
            body["temperature"] = openai_body["temperature"];
        }
        if (openai_body.value("stream", false)) {
            body["stream"] = true;
        }
        std::string endpoint = route.endpoint;
        if (endpoint.empty()) {
            endpoint = "https://api.anthropic.com";
        }
        // If caller passed the OpenAI-shaped endpoint, add the Anthropic
        // messages path.
        if (endpoint.find("/messages") == std::string::npos) {
            if (!endpoint.empty() && endpoint.back() == '/') {
                endpoint.pop_back();
            }
            endpoint += "/v1/messages";
        }
        out.endpoint = std::move(endpoint);
        out.body_json = body.dump();
        if (!route.api_key.empty()) {
            out.extra_headers += "x-api-key: " + route.api_key + "\r\n";
        }
        out.extra_headers += "anthropic-version: " + route.version + "\r\n";
        return SAO_AI_EDITOR_OK;
    }
    if (route.type == "gemini") {
        std::string system;
        Json contents =
            build_gemini_contents(openai_body["messages"], system);
        Json body{{"contents", std::move(contents)}};
        if (!system.empty()) {
            body["systemInstruction"] = Json{
                {"parts", Json::array({Json{{"text", system}}})}};
        }
        Json generation_config = Json::object();
        if (openai_body.contains("temperature") &&
            openai_body["temperature"].is_number()) {
            generation_config["temperature"] = openai_body["temperature"];
        }
        if (openai_body.contains("max_tokens") &&
            openai_body["max_tokens"].is_number()) {
            generation_config["maxOutputTokens"] = openai_body["max_tokens"];
        }
        if (!generation_config.empty()) {
            body["generationConfig"] = std::move(generation_config);
        }
        std::string endpoint = route.endpoint;
        if (endpoint.empty()) {
            endpoint =
                "https://generativelanguage.googleapis.com/v1beta/models";
        }
        const std::string model = route.model.empty()
            ? openai_body.value("model", std::string{})
            : route.model;
        const bool streaming = openai_body.value("stream", false);
        if (endpoint.find(":generateContent") == std::string::npos &&
            endpoint.find(":streamGenerateContent") == std::string::npos) {
            if (!endpoint.empty() && endpoint.back() == '/') {
                endpoint.pop_back();
            }
            endpoint += "/" + model +
                        (streaming ? ":streamGenerateContent"
                                   : ":generateContent");
        }
        if (!route.api_key.empty()) {
            const char separator = endpoint.find('?') == std::string::npos
                ? '?' : '&';
            endpoint.push_back(separator);
            endpoint += "key=" + route.api_key;
        }
        out.endpoint = std::move(endpoint);
        out.body_json = body.dump();
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

int32_t decode_provider_response(const ProviderRoute& route,
                                 std::string_view payload,
                                 Json& out_normalised) {
    if (payload.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json parsed = Json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    if (route.type == "openai") {
        // OpenAI decoding is already exposed via the C API; return the raw
        // response for the caller to route through openai_codec.
        out_normalised = std::move(parsed);
        return SAO_AI_EDITOR_OK;
    }
    if (route.type == "anthropic") {
        const std::string content = collect_anthropic_text(parsed);
        Json normalised{{"ok", true},
                        {"role", "assistant"},
                        {"content", content},
                        {"finish_reason",
                         parsed.value("stop_reason", std::string{})}};
        if (parsed.contains("usage") && parsed["usage"].is_object()) {
            normalised["usage"] = parsed["usage"];
        }
        if (parsed.contains("content")) {
            normalised["raw"] = parsed;
        }
        out_normalised = std::move(normalised);
        return SAO_AI_EDITOR_OK;
    }
    if (route.type == "gemini") {
        const std::string content = collect_gemini_text(parsed);
        Json normalised{{"ok", true},
                        {"role", "assistant"},
                        {"content", content}};
        if (parsed.contains("candidates") &&
            parsed["candidates"].is_array() &&
            !parsed["candidates"].empty()) {
            const auto& candidate = parsed["candidates"][0];
            if (candidate.is_object() &&
                candidate.contains("finishReason")) {
                normalised["finish_reason"] = candidate["finishReason"];
            }
        }
        if (parsed.contains("usageMetadata")) {
            normalised["usage"] = parsed["usageMetadata"];
        }
        normalised["raw"] = parsed;
        out_normalised = std::move(normalised);
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

namespace {

// Shared line-based SSE splitter used by both the Anthropic and Gemini
// codecs.  Extracts `data:` payloads and empty-line-delimited events; the
// caller decides how to interpret each event body.
int32_t drain_sse_lines(std::string& line_buffer, std::string& event_data,
                        std::function<int32_t()> dispatch) {
    if (line_buffer.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    size_t newline = 0;
    while ((newline = line_buffer.find('\n')) != std::string::npos) {
        std::string line = line_buffer.substr(0, newline);
        line_buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            const int32_t status = dispatch();
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            continue;
        }
        if (line.starts_with("data:")) {
            std::string_view value(line);
            value.remove_prefix(5);
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            if (!event_data.empty()) {
                event_data.push_back('\n');
            }
            event_data.append(value);
            if (event_data.size() > kMaximumJsonBytes) {
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
            continue;
        }
        // Ignore `event:` / `id:` / `retry:` lines; the payload's own
        // `type` field carries the semantic dispatch key.
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

int32_t AnthropicSseCodec::dispatch_event(Json& events) {
    if (event_data_.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    std::string payload = std::move(event_data_);
    event_data_.clear();
    Json chunk = Json::parse(payload, nullptr, false);
    if (!chunk.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    const std::string type = chunk.value("type", std::string{});
    if (type == "content_block_delta") {
        const Json& delta = chunk.contains("delta") ? chunk["delta"]
                                                     : Json::object();
        if (delta.is_object() && delta.value("type", "") == "text_delta" &&
            delta.contains("text") && delta["text"].is_string()) {
            events.push_back(Json{{"type", "delta"},
                                    {"content", delta["text"]}});
        } else if (delta.is_object() &&
                   delta.value("type", "") == "input_json_delta" &&
                   delta.contains("partial_json") &&
                   delta["partial_json"].is_string()) {
            events.push_back(Json{{"type", "tool_delta"},
                                    {"content", delta["partial_json"]}});
        }
    } else if (type == "message_delta") {
        Json summary = Json{{"type", "message_delta"}};
        if (chunk.contains("delta")) {
            summary["delta"] = chunk["delta"];
        }
        if (chunk.contains("usage")) {
            summary["usage"] = chunk["usage"];
        }
        events.push_back(std::move(summary));
    } else if (type == "message_stop") {
        done_ = true;
        events.push_back(Json{{"type", "done"}});
    } else if (type == "error") {
        events.push_back(Json{{"type", "error"},
                                {"error", chunk.value("error",
                                                       Json::object())}});
    }
    // ping/message_start/content_block_start/content_block_stop → discard
    return SAO_AI_EDITOR_OK;
}

int32_t AnthropicSseCodec::feed(std::string_view bytes, Json& events) {
    events = Json::array();
    if (bytes.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (done_) {
        return std::all_of(bytes.begin(), bytes.end(),
                           [](unsigned char value) {
                               return std::isspace(value) != 0;
                           })
            ? SAO_AI_EDITOR_OK
            : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    line_buffer_.append(bytes);
    return drain_sse_lines(line_buffer_, event_data_,
                           [&] { return dispatch_event(events); });
}

int32_t GeminiSseCodec::dispatch_event(Json& events) {
    if (event_data_.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    std::string payload = std::move(event_data_);
    event_data_.clear();
    Json chunk = Json::parse(payload, nullptr, false);
    if (!chunk.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    if (chunk.contains("error")) {
        events.push_back(Json{{"type", "error"},
                                {"error", chunk["error"]}});
        return SAO_AI_EDITOR_OK;
    }
    if (!chunk.contains("candidates") || !chunk["candidates"].is_array() ||
        chunk["candidates"].empty()) {
        return SAO_AI_EDITOR_OK;
    }
    const auto& candidate = chunk["candidates"][0];
    if (!candidate.is_object()) {
        return SAO_AI_EDITOR_OK;
    }
    if (candidate.contains("content") &&
        candidate["content"].is_object() &&
        candidate["content"].contains("parts") &&
        candidate["content"]["parts"].is_array()) {
        for (const auto& part : candidate["content"]["parts"]) {
            if (part.is_object() && part.contains("text") &&
                part["text"].is_string()) {
                events.push_back(Json{{"type", "delta"},
                                        {"content", part["text"]}});
            }
        }
    }
    const std::string finish_reason = candidate.value("finishReason",
                                                       std::string{});
    if (!finish_reason.empty() && finish_reason != "FINISH_REASON_UNSPECIFIED") {
        events.push_back(Json{{"type", "message_delta"},
                                {"finish_reason", finish_reason}});
        if (chunk.contains("usageMetadata")) {
            events.back()["usage"] = chunk["usageMetadata"];
        }
        done_ = true;
        events.push_back(Json{{"type", "done"}});
    }
    return SAO_AI_EDITOR_OK;
}

int32_t GeminiSseCodec::feed(std::string_view bytes, Json& events) {
    events = Json::array();
    if (bytes.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (done_) {
        return std::all_of(bytes.begin(), bytes.end(),
                           [](unsigned char value) {
                               return std::isspace(value) != 0;
                           })
            ? SAO_AI_EDITOR_OK
            : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    line_buffer_.append(bytes);
    return drain_sse_lines(line_buffer_, event_data_,
                           [&] { return dispatch_event(events); });
}

}  // namespace sao::ai_editor::native
