#include "chat_provider_router.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace sao::ai_editor::native {
namespace {

std::string to_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string trim_ascii(std::string value) {
    const auto is_space = [](unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string provider_default_endpoint(std::string_view provider_id) {
    if (provider_id == "openai") {
        return "https://api.openai.com/v1";
    }
    if (provider_id == "anthropic") {
        return "https://api.anthropic.com/v1";
    }
    if (provider_id == "deepseek") {
        return "https://api.deepseek.com/v1";
    }
    if (provider_id == "ollama") {
        return "http://localhost:11434/v1";
    }
    if (provider_id == "gemini") {
        return "https://generativelanguage.googleapis.com/v1beta/models";
    }
    return {};
}

std::string provider_default_model(std::string_view provider_id) {
    if (provider_id == "openai") {
        return "gpt-4o";
    }
    if (provider_id == "deepseek") {
        return "deepseek-chat";
    }
    if (provider_id == "ollama") {
        return "llama3.1";
    }
    if (provider_id == "gemini") {
        return "gemini-2.0-flash";
    }
    return {};
}

std::string normalise_transport_name(std::string value) {
    value = to_lower(trim_ascii(std::move(value)));
    std::replace(value.begin(), value.end(), '-', '_');
    if (value.empty() || value == "chat" || value == "chat_completion" ||
        value == "chat_completions") {
        return "chat_completions";
    }
    if (value == "response" || value == "responses" || value == "openai_response" ||
        value == "openai_responses") {
        return "responses";
    }
    return value;
}

std::string_view endpoint_path(std::string_view endpoint) noexcept {
    const size_t scheme = endpoint.find("://");
    const size_t path_begin = scheme == std::string_view::npos ? 0 : endpoint.find('/', scheme + 3);
    if (path_begin == std::string_view::npos) {
        return {};
    }
    const size_t path_end = endpoint.find_first_of("?#", path_begin);
    return endpoint.substr(path_begin, path_end == std::string_view::npos ? std::string_view::npos
                                                                          : path_end - path_begin);
}

bool endpoint_path_ends_with(std::string_view endpoint, std::string_view suffix) noexcept {
    const std::string path = to_lower(endpoint_path(endpoint));
    return path.ends_with(to_lower(suffix));
}

std::string append_endpoint_path(std::string endpoint, std::string_view path,
                                 std::initializer_list<std::string_view> markers) {
    endpoint = trim_ascii(std::move(endpoint));
    if (endpoint.empty()) {
        return endpoint;
    }
    for (const auto marker : markers) {
        if (endpoint_path_ends_with(endpoint, marker)) {
            return endpoint;
        }
    }
    const size_t suffix = endpoint.find_first_of("?#");
    std::string tail;
    if (suffix != std::string::npos) {
        tail = endpoint.substr(suffix);
        endpoint.erase(suffix);
    }
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    endpoint.push_back('/');
    endpoint.append(path);
    endpoint += tail;
    return endpoint;
}

std::string canonical_openai_endpoint(std::string endpoint, std::string_view transport) {
    endpoint = trim_ascii(std::move(endpoint));
    if (endpoint.empty()) {
        return endpoint;
    }
    const std::string target = transport == "responses" ? "responses" : "chat/completions";
    const size_t suffix = endpoint.find_first_of("?#");
    std::string tail;
    if (suffix != std::string::npos) {
        tail = endpoint.substr(suffix);
        endpoint.erase(suffix);
    }
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    for (const std::string_view known :
         {std::string_view{"/chat/completions"}, std::string_view{"/responses"}}) {
        const std::string lowered = to_lower(endpoint);
        if (lowered.ends_with(known)) {
            endpoint.erase(endpoint.size() - known.size());
            break;
        }
    }
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    const std::string path = to_lower(endpoint_path(endpoint));
    if (path.empty() || path == "/") {
        endpoint += "/v1";
    }
    endpoint.push_back('/');
    endpoint += target;
    endpoint += tail;
    return endpoint;
}

std::string canonical_endpoint(std::string endpoint, std::string_view provider_type,
                               std::string_view transport) {
    if (provider_type == "openai") {
        return canonical_openai_endpoint(std::move(endpoint), transport);
    }
    if (provider_type == "anthropic") {
        endpoint = trim_ascii(std::move(endpoint));
        std::string base = endpoint;
        if (const size_t suffix = base.find_first_of("?#"); suffix != std::string::npos) {
            base.erase(suffix);
        }
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        const std::string path = to_lower(base).ends_with("/v1") ? "messages" : "v1/messages";
        return append_endpoint_path(std::move(endpoint), path, {"/messages"});
    }
    return trim_ascii(std::move(endpoint));
}

bool valid_header_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return std::all_of(name.begin(), name.end(), [&](unsigned char byte) {
        const bool alphanumeric = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                                  (byte >= 'a' && byte <= 'z');
        return alphanumeric || punctuation.find(static_cast<char>(byte)) != std::string_view::npos;
    });
}

bool managed_header(std::string_view name) {
    static const std::set<std::string, std::less<>> managed{
        "accept",         "authorization",       "connection",
        "content-length", "content-type",        "expect",
        "host",           "proxy-authorization", "transfer-encoding",
        "x-api-key",      "x-goog-api-key",      "anthropic-version"};
    return managed.contains(to_lower(name));
}

constexpr size_t kMaximumExtraHeaderCount = 32;
constexpr size_t kMaximumExtraHeaderNameBytes = 128;
constexpr size_t kMaximumExtraHeaderValueBytes = 4U * 1024U;
constexpr size_t kMaximumExtraHeadersBytes = 16U * 1024U;

bool has_disallowed_header_value_byte(std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte == 0 || byte == '\r' || byte == '\n' || byte == 0x7F || byte < 0x20;
    });
}

int32_t append_extra_headers(const Json& extra_headers, std::string& output) {
    if (extra_headers.is_null() || extra_headers.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    if (!extra_headers.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const size_t existing_count =
        static_cast<size_t>(std::count(output.begin(), output.end(), '\n'));
    if (existing_count > kMaximumExtraHeaderCount ||
        extra_headers.size() > kMaximumExtraHeaderCount - existing_count) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    size_t total_size = output.size();
    for (const auto& [name, value] : extra_headers.items()) {
        if (!valid_header_name(name) || managed_header(name) ||
            name.size() > kMaximumExtraHeaderNameBytes || !value.is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string header_value = value.get<std::string>();
        if (!valid_utf8(name) || !valid_utf8(header_value) ||
            header_value.size() > kMaximumExtraHeaderValueBytes ||
            has_disallowed_header_value_byte(header_value)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const size_t line_size = name.size() + 2U + header_value.size() + 2U;
        if (line_size > kMaximumExtraHeadersBytes ||
            total_size > kMaximumExtraHeadersBytes - line_size) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        output += name;
        output += ": ";
        output += header_value;
        output += "\r\n";
        total_size += line_size;
    }
    return SAO_AI_EDITOR_OK;
}

bool protected_body_key(std::string_view provider_type, std::string_view transport,
                        std::string_view parent_key, std::string_view key) {
    static const std::set<std::string, std::less<>> universal{"apikey", "api_key", "authorization"};
    static const std::set<std::string, std::less<>> chat_completions{"frequency_penalty",
                                                                     "logit_bias",
                                                                     "logprobs",
                                                                     "max_tokens",
                                                                     "messages",
                                                                     "model",
                                                                     "n",
                                                                     "parallel_tool_calls",
                                                                     "presence_penalty",
                                                                     "response_format",
                                                                     "seed",
                                                                     "stop",
                                                                     "stream",
                                                                     "temperature",
                                                                     "tool_choice",
                                                                     "tools",
                                                                     "top_logprobs",
                                                                     "top_p",
                                                                     "user"};
    static const std::set<std::string, std::less<>> responses{"frequency_penalty",
                                                              "input",
                                                              "instructions",
                                                              "max_output_tokens",
                                                              "model",
                                                              "parallel_tool_calls",
                                                              "presence_penalty",
                                                              "stream",
                                                              "temperature",
                                                              "text",
                                                              "tool_choice",
                                                              "tools",
                                                              "top_logprobs",
                                                              "top_p",
                                                              "user"};
    static const std::set<std::string, std::less<>> anthropic{
        "messages",    "model", "stop_sequences", "stream", "system",    "temperature",
        "tool_choice", "tools", "top_k",          "top_p",  "max_tokens"};
    static const std::set<std::string, std::less<>> gemini{"contents", "model",
                                                           "systeminstruction"};
    static const std::set<std::string, std::less<>> gemini_generation{"maxoutputtokens",
                                                                      "responsemimetype",
                                                                      "seed",
                                                                      "stopsequences",
                                                                      "temperature",
                                                                      "topk",
                                                                      "topp"};

    const std::string lowered = to_lower(key);
    const std::string parent = to_lower(parent_key);
    if (!parent.empty()) {
        return provider_type == "gemini" && parent == "generationconfig" &&
               gemini_generation.contains(lowered);
    }
    if (universal.contains(lowered)) {
        return true;
    }
    if (provider_type == "openai") {
        return transport == "responses" ? responses.contains(lowered)
                                        : chat_completions.contains(lowered);
    }
    if (provider_type == "anthropic") {
        return anthropic.contains(lowered);
    }
    if (provider_type == "gemini") {
        return gemini.contains(lowered) || gemini_generation.contains(lowered);
    }
    return false;
}

int32_t merge_extra_body(Json& destination, const Json& extra, std::string_view provider_type,
                         std::string_view transport, std::string_view parent_key = {}) {
    if (extra.is_null() || extra.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    if (!destination.is_object() || !extra.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    for (const auto& [key, value] : extra.items()) {
        if (protected_body_key(provider_type, transport, parent_key, key)) {
            continue;
        }
        if (value.is_object()) {
            Json merged = destination.contains(key) && destination[key].is_object()
                              ? destination[key]
                              : Json::object();
            const int32_t status = merge_extra_body(merged, value, provider_type, transport, key);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            destination[key] = std::move(merged);
        } else {
            destination[key] = value;
        }
    }
    return SAO_AI_EDITOR_OK;
}

Json build_anthropic_messages(const Json& openai_messages, std::string& out_system) {
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
            converted["content"] = Json::array(
                {Json{{"type", "text"}, {"text", message["content"].get<std::string>()}}});
        } else if (message.contains("content") && message["content"].is_array()) {
            converted["content"] = message["content"];
        } else {
            converted["content"] = Json::array();
        }
        result.push_back(std::move(converted));
    }
    return result;
}

Json build_gemini_contents(const Json& openai_messages, std::string& out_system) {
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
            parts.push_back(Json{{"text", message["content"].get<std::string>()}});
        } else if (message.contains("content") && message["content"].is_array()) {
            for (const auto& part : message["content"]) {
                if (part.is_object() && part.value("type", "") == "text") {
                    parts.push_back(Json{{"text", part.value("text", std::string{})}});
                }
            }
        }
        result.push_back(
            Json{{"role", role == "assistant" ? "model" : "user"}, {"parts", std::move(parts)}});
    }
    return result;
}

bool append_instruction_content(const Json& content, std::string& instructions) {
    const auto append_text = [&](std::string_view text) {
        if (!instructions.empty()) {
            instructions.push_back('\n');
        }
        instructions.append(text);
    };
    if (content.is_string()) {
        append_text(content.get_ref<const std::string&>());
        return true;
    }
    if (!content.is_array()) {
        return content.is_null();
    }
    for (const auto& part : content) {
        if (!part.is_object()) {
            return false;
        }
        const std::string type = part.value("type", std::string{});
        if ((type == "text" || type == "input_text" || type == "output_text") &&
            part.contains("text") && part["text"].is_string()) {
            append_text(part["text"].get_ref<const std::string&>());
            continue;
        }
        return false;
    }
    return true;
}

bool responses_message_content(const Json& content, Json& converted) {
    if (content.is_string()) {
        converted = content;
        return true;
    }
    if (content.is_null()) {
        converted = Json::array();
        return true;
    }
    if (!content.is_array()) {
        return false;
    }
    converted = Json::array();
    for (const auto& part : content) {
        if (!part.is_object()) {
            return false;
        }
        const std::string type = part.value("type", std::string{});
        if (type == "text" || type == "input_text") {
            if (!part.contains("text") || !part["text"].is_string()) {
                return false;
            }
            converted.push_back(Json{{"type", "input_text"}, {"text", part["text"]}});
            continue;
        }
        if (type == "image_url") {
            if (!part.contains("image_url")) {
                return false;
            }
            const Json& image = part["image_url"];
            if (image.is_string()) {
                converted.push_back(Json{{"type", "input_image"}, {"image_url", image}});
                continue;
            }
            if (image.is_object() && image.contains("url") && image["url"].is_string()) {
                Json input_image{{"type", "input_image"}, {"image_url", image["url"]}};
                if (image.contains("detail") && image["detail"].is_string()) {
                    input_image["detail"] = image["detail"];
                }
                converted.push_back(std::move(input_image));
                continue;
            }
            return false;
        }
        if (type == "input_image" || type == "input_file") {
            converted.push_back(part);
            continue;
        }
        return false;
    }
    return true;
}

bool responses_function_call(const Json& tool_call, size_t index, Json& converted) {
    if (!tool_call.is_object()) {
        return false;
    }
    const Json function = tool_call.value("function", Json::object());
    if (!function.is_object() || !function.contains("name") || !function["name"].is_string()) {
        return false;
    }
    std::string arguments = "{}";
    if (function.contains("arguments")) {
        if (function["arguments"].is_string()) {
            arguments = function["arguments"].get<std::string>();
        } else if (function["arguments"].is_object()) {
            arguments = function["arguments"].dump();
        } else {
            return false;
        }
    }
    const std::string call_id = tool_call.value("id", "call_" + std::to_string(index));
    converted = Json{{"type", "function_call"},
                     {"call_id", call_id},
                     {"name", function["name"]},
                     {"arguments", std::move(arguments)}};
    return true;
}

int32_t build_responses_input(const Json& openai_messages, Json& input, std::string& instructions) {
    if (!openai_messages.is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    input = Json::array();
    size_t tool_index = 0;
    for (const auto& message : openai_messages) {
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string role = message["role"].get<std::string>();
        const Json content = message.value("content", Json());
        if (role == "system" || role == "developer") {
            if (!append_instruction_content(content, instructions)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            continue;
        }
        if (role == "tool") {
            if (!message.contains("tool_call_id") || !message["tool_call_id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            std::string output;
            if (content.is_string()) {
                output = content.get<std::string>();
            } else if (content.is_null()) {
                output.clear();
            } else {
                output = content.dump();
            }
            input.push_back(Json{{"type", "function_call_output"},
                                 {"call_id", message["tool_call_id"]},
                                 {"output", std::move(output)}});
            continue;
        }
        if (role != "user" && role != "assistant") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json converted_content;
        if (!responses_message_content(content, converted_content)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool has_content = converted_content.is_string()
                                     ? !converted_content.get_ref<const std::string&>().empty()
                                     : !converted_content.empty();
        if (has_content) {
            input.push_back(Json{{"role", role}, {"content", std::move(converted_content)}});
        }
        if (message.contains("tool_calls")) {
            if (role != "assistant" || !message["tool_calls"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& tool_call : message["tool_calls"]) {
                Json converted_call;
                if (!responses_function_call(tool_call, tool_index++, converted_call)) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                input.push_back(std::move(converted_call));
            }
        }
    }
    return input.empty() ? SAO_AI_EDITOR_ERR_INVALID_ARGUMENT : SAO_AI_EDITOR_OK;
}

int32_t build_responses_tools(const Json& openai_tools, Json& tools) {
    if (!openai_tools.is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    tools = Json::array();
    for (const auto& tool : openai_tools) {
        if (!tool.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (tool.value("type", std::string{}) != "function") {
            tools.push_back(tool);
            continue;
        }
        if (tool.contains("name") && tool["name"].is_string()) {
            tools.push_back(tool);
            continue;
        }
        const Json function = tool.value("function", Json::object());
        if (!function.is_object() || !function.contains("name") || !function["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json converted{{"type", "function"},
                       {"name", function["name"]},
                       {"parameters", function.value("parameters", Json::object())}};
        for (const char* key : {"description", "strict"}) {
            if (function.contains(key)) {
                converted[key] = function[key];
            }
        }
        tools.push_back(std::move(converted));
    }
    return SAO_AI_EDITOR_OK;
}

int32_t build_responses_body(const Json& openai_body, Json& body) {
    Json input;
    std::string instructions;
    const int32_t input_status =
        build_responses_input(openai_body["messages"], input, instructions);
    if (input_status != SAO_AI_EDITOR_OK) {
        return input_status;
    }
    body = Json{{"model", openai_body.value("model", std::string{})},
                {"input", std::move(input)},
                {"stream", openai_body.value("stream", false)}};
    if (!instructions.empty()) {
        body["instructions"] = std::move(instructions);
    }
    if (openai_body.contains("max_tokens")) {
        body["max_output_tokens"] = openai_body["max_tokens"];
    }
    for (const char* key : {"temperature", "top_p", "frequency_penalty", "presence_penalty",
                            "parallel_tool_calls", "top_logprobs"}) {
        if (openai_body.contains(key)) {
            body[key] = openai_body[key];
        }
    }
    if (openai_body.contains("tools")) {
        Json tools;
        const int32_t tools_status = build_responses_tools(openai_body["tools"], tools);
        if (tools_status != SAO_AI_EDITOR_OK) {
            return tools_status;
        }
        body["tools"] = std::move(tools);
    }
    if (openai_body.contains("tool_choice")) {
        const Json& choice = openai_body["tool_choice"];
        if (choice.is_object() && choice.value("type", std::string{}) == "function" &&
            choice.contains("function") && choice["function"].is_object() &&
            choice["function"].contains("name") && choice["function"]["name"].is_string()) {
            body["tool_choice"] = Json{{"type", "function"}, {"name", choice["function"]["name"]}};
        } else {
            body["tool_choice"] = choice;
        }
    }
    if (openai_body.contains("response_format")) {
        const Json& format = openai_body["response_format"];
        if (!format.is_object() || !format.contains("type") || !format["type"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json responses_format;
        if (format["type"] == "json_schema") {
            const Json schema = format.value("json_schema", Json::object());
            if (!schema.is_object() || !schema.contains("name") || !schema["name"].is_string() ||
                !schema.contains("schema")) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            responses_format = Json{
                {"type", "json_schema"}, {"name", schema["name"]}, {"schema", schema["schema"]}};
            for (const char* key : {"description", "strict"}) {
                if (schema.contains(key)) {
                    responses_format[key] = schema[key];
                }
            }
        } else if (format["type"] == "json_object" || format["type"] == "text") {
            responses_format = format;
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        body["text"] = Json{{"format", std::move(responses_format)}};
    }
    return SAO_AI_EDITOR_OK;
}

std::string collect_anthropic_text(const Json& response) {
    if (!response.is_object() || !response.contains("content") || !response["content"].is_array()) {
        return {};
    }
    std::string content;
    for (const auto& block : response["content"]) {
        if (block.is_object() && block.value("type", "") == "text" && block.contains("text") &&
            block["text"].is_string()) {
            content += block["text"].get<std::string>();
        }
    }
    return content;
}

std::string collect_gemini_text(const Json& response) {
    if (!response.is_object() || !response.contains("candidates") ||
        !response["candidates"].is_array() || response["candidates"].empty()) {
        return {};
    }
    const auto& candidate = response["candidates"][0];
    if (!candidate.is_object() || !candidate.contains("content") ||
        !candidate["content"].is_object() || !candidate["content"].contains("parts") ||
        !candidate["content"]["parts"].is_array()) {
        return {};
    }
    std::string content;
    for (const auto& part : candidate["content"]["parts"]) {
        if (part.is_object() && part.contains("text") && part["text"].is_string()) {
            content += part["text"].get<std::string>();
        }
    }
    return content;
}

int32_t decode_openai_responses(const Json& response, Json& out_normalised) {
    if (!response.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    const std::string response_status =
        to_lower(trim_ascii(response.value("status", std::string{})));
    const auto provider_error = response.find("error");
    const bool has_provider_error = provider_error != response.end() && !provider_error->is_null();
    const bool failed =
        response_status == "failed" || response_status == "error" || has_provider_error;
    const Json output = response.value("output", Json::array());
    if (!output.is_array() && !failed) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    std::string content;
    std::string refusal;
    std::string thinking;
    Json tool_calls = Json::array();
    size_t tool_index = 0;
    for (const auto& item : output.is_array() ? output : Json::array()) {
        if (!item.is_object()) {
            continue;
        }
        const std::string type = item.value("type", std::string{});
        if (type == "message") {
            const Json parts = item.value("content", Json::array());
            if (!parts.is_array()) {
                if (!failed) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                continue;
            }
            for (const auto& part : parts) {
                if (!part.is_object()) {
                    continue;
                }
                const std::string part_type = part.value("type", std::string{});
                if (part_type == "output_text" && part.contains("text") &&
                    part["text"].is_string()) {
                    content += part["text"].get<std::string>();
                } else if (part_type == "refusal" && part.contains("refusal") &&
                           part["refusal"].is_string()) {
                    refusal += part["refusal"].get<std::string>();
                }
            }
            continue;
        }
        if (type == "function_call") {
            tool_calls.push_back(
                Json{{"index", tool_index++},
                     {"id", item.value("call_id", item.value("id", std::string{}))},
                     {"type", "function"},
                     {"name", item.value("name", std::string{})},
                     {"arguments", item.value("arguments", std::string{})}});
            continue;
        }
        if (type == "reasoning") {
            if (item.contains("summary") && item["summary"].is_array()) {
                for (const auto& summary : item["summary"]) {
                    if (summary.is_object() && summary.contains("text") &&
                        summary["text"].is_string()) {
                        thinking += summary["text"].get<std::string>();
                    }
                }
            }
            if (item.contains("content") && item["content"].is_array()) {
                for (const auto& reasoning : item["content"]) {
                    if (reasoning.is_object() && reasoning.contains("text") &&
                        reasoning["text"].is_string()) {
                        thinking += reasoning["text"].get<std::string>();
                    }
                }
            }
        }
    }
    if (content.empty() && response.contains("output_text") &&
        response["output_text"].is_string()) {
        content = response["output_text"].get<std::string>();
    }
    std::string finish_reason = response.value("status", std::string{});
    if (response.contains("incomplete_details") && response["incomplete_details"].is_object()) {
        finish_reason = response["incomplete_details"].value("reason", finish_reason);
    }
    out_normalised = Json{{"ok", !failed},
                          {"id", response.value("id", std::string{})},
                          {"model", response.value("model", std::string{})},
                          {"status", response.value("status", std::string{})},
                          {"role", "assistant"},
                          {"content", std::move(content)},
                          {"thinking", std::move(thinking)},
                          {"refusal", std::move(refusal)},
                          {"toolCalls", std::move(tool_calls)},
                          {"finishReason", std::move(finish_reason)},
                          {"usage", response.value("usage", Json::object())},
                          {"raw", response}};
    if (!failed) {
        return SAO_AI_EDITOR_OK;
    }
    if (has_provider_error) {
        out_normalised["error"] = *provider_error;
    } else {
        out_normalised["error"] =
            Json{{"status", response_status}, {"message", "OpenAI Responses request failed"}};
    }
    return SAO_AI_EDITOR_ERR_HTTP;
}

} // namespace

bool is_openai_responses_endpoint(std::string_view endpoint) noexcept {
    return endpoint_path_ends_with(endpoint, "/responses");
}

ProviderRoute normalise_provider(const Json& provider, std::string model_hint) {
    ProviderRoute route;
    if (!provider.is_object()) {
        route.model = std::move(model_hint);
        return route;
    }
    route.provider_id = to_lower(trim_ascii(provider.value(
        "id", provider.value("provider",
                             provider.value("type", provider.value("provider_type",
                                                                   std::string{"openai"}))))));
    route.type = to_lower(
        trim_ascii(provider.value("type", provider.value("provider_type", route.provider_id))));
    if (route.provider_id.empty()) {
        route.provider_id = route.type.empty() ? "openai" : route.type;
    }
    if (route.type == "deepseek" || route.type == "ollama" || route.type == "custom") {
        route.type = "openai";
    }
    if (route.type != "openai" && route.type != "anthropic" && route.type != "gemini") {
        route.type = "openai";
    }
    route.endpoint =
        trim_ascii(provider.value("endpoint", provider.value("base_url", std::string{})));
    if (route.endpoint.empty()) {
        route.endpoint = provider_default_endpoint(route.provider_id);
    }
    if (provider.contains("transport")) {
        route.transport = provider["transport"].is_string()
                              ? normalise_transport_name(provider["transport"].get<std::string>())
                              : std::string{"invalid"};
    } else {
        route.transport =
            is_openai_responses_endpoint(route.endpoint) ? "responses" : "chat_completions";
    }
    route.endpoint = canonical_endpoint(std::move(route.endpoint), route.type, route.transport);
    route.api_key = provider.value("apiKey", provider.value("api_key", std::string{}));
    route.api_key_env = provider.value("apiKeyEnv", std::string{});
    route.model =
        !model_hint.empty() ? std::move(model_hint) : provider.value("model", std::string{});
    route.model = trim_ascii(std::move(route.model));
    if (route.model.empty()) {
        route.model = provider_default_model(route.provider_id);
    }
    route.version = provider.value("version", provider.value("anthropicVersion", std::string{}));
    if (route.type == "anthropic" && route.version.empty()) {
        route.version = "2023-06-01";
    }
    route.extra_headers =
        provider.value("extra_headers", provider.value("extraHeaders", Json::object()));
    route.extra_body = provider.value("extra_body", provider.value("extraBody", Json::object()));
    return route;
}

int32_t build_provider_request(const ProviderRoute& route, const Json& openai_body,
                               ProviderRequest& out) {
    if (!openai_body.is_object() || !openai_body.contains("messages") ||
        !openai_body["messages"].is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    out.endpoint.clear();
    out.body_json.clear();
    out.authorization.clear();
    out.extra_headers.clear();
    if (route.transport != "chat_completions" && route.transport != "responses") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (route.type != "openai" && route.transport == "responses") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if ((!route.api_key.empty() &&
         (route.api_key.size() > kMaximumExtraHeaderValueBytes || !valid_utf8(route.api_key) ||
          has_disallowed_header_value_byte(route.api_key))) ||
        (!route.version.empty() &&
         (route.version.size() > kMaximumExtraHeaderValueBytes || !valid_utf8(route.version) ||
          has_disallowed_header_value_byte(route.version)))) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (route.type == "openai") {
        Json body;
        if (route.transport == "responses") {
            const int32_t response_status = build_responses_body(openai_body, body);
            if (response_status != SAO_AI_EDITOR_OK) {
                return response_status;
            }
        } else {
            body = openai_body;
        }
        const int32_t body_status =
            merge_extra_body(body, route.extra_body, route.type, route.transport);
        if (body_status != SAO_AI_EDITOR_OK) {
            return body_status;
        }
        out.endpoint = canonical_endpoint(route.endpoint, route.type, route.transport);
        out.body_json = body.dump();
        if (!route.api_key.empty()) {
            out.authorization = "Bearer " + route.api_key;
        }
        return append_extra_headers(route.extra_headers, out.extra_headers);
    }
    if (route.type == "anthropic") {
        std::string system;
        Json anthropic_messages = build_anthropic_messages(openai_body["messages"], system);
        Json body{{"model",
                   route.model.empty() ? openai_body.value("model", std::string{}) : route.model},
                  {"max_tokens", openai_body.value("max_tokens", 1024)},
                  {"messages", std::move(anthropic_messages)}};
        if (!system.empty()) {
            body["system"] = system;
        }
        if (openai_body.contains("temperature") && openai_body["temperature"].is_number()) {
            body["temperature"] = openai_body["temperature"];
        }
        // top_p / top_k share OpenAI-style names on Anthropic's native
        // /v1/messages API so they can be forwarded verbatim.  OpenAI-only
        // knobs (frequency_penalty, presence_penalty, seed, logit_bias,
        // logprobs, top_logprobs, n, user, parallel_tool_calls,
        // response_format) are deliberately dropped because Anthropic would
        // 400 on unknown fields.
        if (openai_body.contains("top_p") && openai_body["top_p"].is_number()) {
            body["top_p"] = openai_body["top_p"];
        }
        if (openai_body.contains("top_k") && openai_body["top_k"].is_number_integer()) {
            body["top_k"] = openai_body["top_k"];
        }
        // OpenAI's `stop` becomes `stop_sequences` (Anthropic only accepts
        // an array).  A lone string is upgraded to a single-element array so
        // callers don't have to know the wire distinction.
        if (openai_body.contains("stop")) {
            const Json& stop_value = openai_body["stop"];
            if (stop_value.is_string()) {
                body["stop_sequences"] = Json::array({stop_value});
            } else if (stop_value.is_array()) {
                body["stop_sequences"] = stop_value;
            }
        }
        if (openai_body.value("stream", false)) {
            body["stream"] = true;
        }
        const int32_t body_status =
            merge_extra_body(body, route.extra_body, route.type, route.transport);
        if (body_status != SAO_AI_EDITOR_OK) {
            return body_status;
        }
        std::string endpoint = route.endpoint;
        if (endpoint.empty()) {
            endpoint = "https://api.anthropic.com/v1";
        }
        out.endpoint = canonical_endpoint(std::move(endpoint), route.type, route.transport);
        out.body_json = body.dump();
        if (!route.api_key.empty()) {
            out.extra_headers += "x-api-key: " + route.api_key + "\r\n";
        }
        out.extra_headers += "anthropic-version: " + route.version + "\r\n";
        return append_extra_headers(route.extra_headers, out.extra_headers);
    }
    if (route.type == "gemini") {
        std::string system;
        Json contents = build_gemini_contents(openai_body["messages"], system);
        Json body{{"contents", std::move(contents)}};
        if (!system.empty()) {
            body["systemInstruction"] = Json{{"parts", Json::array({Json{{"text", system}}})}};
        }
        Json generation_config = Json::object();
        if (openai_body.contains("temperature") && openai_body["temperature"].is_number()) {
            generation_config["temperature"] = openai_body["temperature"];
        }
        if (openai_body.contains("max_tokens") && openai_body["max_tokens"].is_number()) {
            generation_config["maxOutputTokens"] = openai_body["max_tokens"];
        }
        // Gemini's generationConfig accepts topP / topK / seed under
        // camelCase names and requires stopSequences as an array.  Every
        // OpenAI-only knob (frequency_penalty, presence_penalty, logit_bias,
        // logprobs, top_logprobs, n, user, parallel_tool_calls) is silently
        // dropped since Gemini would 400 on unknown fields.
        if (openai_body.contains("top_p") && openai_body["top_p"].is_number()) {
            generation_config["topP"] = openai_body["top_p"];
        }
        if (openai_body.contains("top_k") && openai_body["top_k"].is_number_integer()) {
            generation_config["topK"] = openai_body["top_k"];
        }
        if (openai_body.contains("seed") && openai_body["seed"].is_number_integer()) {
            generation_config["seed"] = openai_body["seed"];
        }
        if (openai_body.contains("stop")) {
            const Json& stop_value = openai_body["stop"];
            if (stop_value.is_string()) {
                generation_config["stopSequences"] = Json::array({stop_value});
            } else if (stop_value.is_array()) {
                generation_config["stopSequences"] = stop_value;
            }
        }
        // response_format=={"type":"json_object"} → the Gemini equivalent is
        // toggling responseMimeType.  We stay conservative and only convert
        // the exact json_object shape; any other response_format is dropped
        // because Gemini would 400 on an unknown structure.
        if (openai_body.contains("response_format") && openai_body["response_format"].is_object()) {
            const auto& response_format = openai_body["response_format"];
            const std::string format_type = response_format.value("type", std::string{});
            if (format_type == "json_object") {
                generation_config["responseMimeType"] = "application/json";
            }
        }
        if (!generation_config.empty()) {
            body["generationConfig"] = std::move(generation_config);
        }
        const int32_t body_status =
            merge_extra_body(body, route.extra_body, route.type, route.transport);
        if (body_status != SAO_AI_EDITOR_OK) {
            return body_status;
        }
        std::string endpoint = route.endpoint;
        if (endpoint.empty()) {
            endpoint = "https://generativelanguage.googleapis.com/v1beta/models";
        }
        const std::string model =
            route.model.empty() ? openai_body.value("model", std::string{}) : route.model;
        const bool streaming = openai_body.value("stream", false);
        const std::string gemini_path = to_lower(endpoint_path(endpoint));
        if (gemini_path.find(":generatecontent") == std::string::npos &&
            gemini_path.find(":streamgeneratecontent") == std::string::npos) {
            const size_t suffix = endpoint.find_first_of("?#");
            std::string tail;
            if (suffix != std::string::npos) {
                tail = endpoint.substr(suffix);
                endpoint.erase(suffix);
            }
            while (!endpoint.empty() && endpoint.back() == '/') {
                endpoint.pop_back();
            }
            endpoint += "/" + model + (streaming ? ":streamGenerateContent" : ":generateContent");
            endpoint += tail;
        }
        out.endpoint = std::move(endpoint);
        out.body_json = body.dump();
        if (!route.api_key.empty()) {
            out.extra_headers += "x-goog-api-key: " + route.api_key + "\r\n";
        }
        return append_extra_headers(route.extra_headers, out.extra_headers);
    }
    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

int32_t decode_provider_response(const ProviderRoute& route, std::string_view payload,
                                 Json& out_normalised) {
    if (payload.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json parsed = Json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    if (route.type == "openai") {
        if (route.transport == "responses" || is_openai_responses_endpoint(route.endpoint)) {
            return decode_openai_responses(parsed, out_normalised);
        }
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
                        {"finish_reason", parsed.value("stop_reason", std::string{})}};
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
        Json normalised{{"ok", true}, {"role", "assistant"}, {"content", content}};
        if (parsed.contains("candidates") && parsed["candidates"].is_array() &&
            !parsed["candidates"].empty()) {
            const auto& candidate = parsed["candidates"][0];
            if (candidate.is_object() && candidate.contains("finishReason")) {
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

} // namespace

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
        const Json& delta = chunk.contains("delta") ? chunk["delta"] : Json::object();
        if (delta.is_object() && delta.value("type", "") == "text_delta" &&
            delta.contains("text") && delta["text"].is_string()) {
            events.push_back(Json{{"type", "delta"}, {"content", delta["text"]}});
        } else if (delta.is_object() && delta.value("type", "") == "input_json_delta" &&
                   delta.contains("partial_json") && delta["partial_json"].is_string()) {
            events.push_back(Json{{"type", "tool_delta"}, {"content", delta["partial_json"]}});
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
        events.push_back(Json{{"type", "error"}, {"error", chunk.value("error", Json::object())}});
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
                           [](unsigned char value) { return std::isspace(value) != 0; })
                   ? SAO_AI_EDITOR_OK
                   : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    line_buffer_.append(bytes);
    return drain_sse_lines(line_buffer_, event_data_, [&] { return dispatch_event(events); });
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
        events.push_back(Json{{"type", "error"}, {"error", chunk["error"]}});
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
    if (candidate.contains("content") && candidate["content"].is_object() &&
        candidate["content"].contains("parts") && candidate["content"]["parts"].is_array()) {
        for (const auto& part : candidate["content"]["parts"]) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                events.push_back(Json{{"type", "delta"}, {"content", part["text"]}});
            }
        }
    }
    const std::string finish_reason = candidate.value("finishReason", std::string{});
    if (!finish_reason.empty() && finish_reason != "FINISH_REASON_UNSPECIFIED") {
        events.push_back(Json{{"type", "message_delta"}, {"finish_reason", finish_reason}});
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
                           [](unsigned char value) { return std::isspace(value) != 0; })
                   ? SAO_AI_EDITOR_OK
                   : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    line_buffer_.append(bytes);
    return drain_sse_lines(line_buffer_, event_data_, [&] { return dispatch_event(events); });
}

int32_t OpenAiResponsesSseCodec::merge_tool_call(const Json& item, size_t output_index,
                                                 bool append_arguments, Json* event) {
    if (!item.is_object() || output_index > 1024U) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    Json* slot = nullptr;
    const std::string incoming_id = item.value("call_id", item.value("id", std::string{}));
    for (auto& existing : tool_calls_accumulator_) {
        if (!existing.is_object()) {
            continue;
        }
        const bool same_id =
            !incoming_id.empty() && existing.value("id", std::string{}) == incoming_id;
        const bool same_index = existing.value("response_output_index",
                                               std::numeric_limits<size_t>::max()) == output_index;
        if (same_id || same_index) {
            slot = &existing;
            break;
        }
    }
    if (slot == nullptr) {
        tool_calls_accumulator_.push_back(
            Json{{"index", tool_calls_accumulator_.size()},
                 {"response_output_index", output_index},
                 {"id", ""},
                 {"type", "function"},
                 {"function", Json{{"name", ""}, {"arguments", ""}}}});
        slot = &tool_calls_accumulator_.back();
    }
    const std::string call_id = item.value("call_id", std::string{});
    const std::string item_id = item.value("id", std::string{});
    if (!call_id.empty()) {
        (*slot)["id"] = call_id;
    } else if ((*slot).value("id", std::string{}).empty() && !item_id.empty()) {
        (*slot)["id"] = item_id;
    }
    Json& function = (*slot)["function"];
    if (item.contains("name") && item["name"].is_string()) {
        function["name"] = item["name"];
    }
    if (item.contains("arguments") && item["arguments"].is_string()) {
        const std::string incoming = item["arguments"].get<std::string>();
        function["arguments"] =
            append_arguments ? function.value("arguments", std::string{}) + incoming : incoming;
    }
    if (event != nullptr) {
        Json partial = *slot;
        partial.erase("response_output_index");
        *event = Json{{"type", "tool_delta"},
                      {"index", (*slot)["index"]},
                      {"name", function.value("name", std::string{})},
                      {"arguments", function.value("arguments", std::string{})},
                      {"partial", std::move(partial)}};
    }
    return SAO_AI_EDITOR_OK;
}

void OpenAiResponsesSseCodec::emit_tool_calls_final(Json& events) {
    if (tool_calls_emitted_ || tool_calls_accumulator_.empty()) {
        return;
    }
    Json calls = Json::array();
    for (const auto& stored : tool_calls_accumulator_) {
        Json call = stored;
        call.erase("response_output_index");
        calls.push_back(std::move(call));
    }
    events.push_back(Json{{"type", "tool_calls_final"}, {"tool_calls", std::move(calls)}});
    tool_calls_emitted_ = true;
}

int32_t OpenAiResponsesSseCodec::dispatch_event(Json& events) {
    if (event_data_.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    std::string payload = std::move(event_data_);
    event_data_.clear();
    if (done_) {
        return payload == "[DONE]" ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (payload == "[DONE]") {
        emit_tool_calls_final(events);
        events.push_back(Json{{"type", "done"}});
        done_ = true;
        return SAO_AI_EDITOR_OK;
    }
    Json chunk = Json::parse(payload, nullptr, false);
    if (!chunk.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    const std::string type = chunk.value("type", std::string{});
    if (type == "response.output_text.delta") {
        if (!chunk.contains("delta") || !chunk["delta"].is_string()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        events.push_back(Json{{"type", "delta"},
                              {"content", chunk["delta"]},
                              {"index", chunk.value("output_index", 0)}});
        return SAO_AI_EDITOR_OK;
    }
    if (type == "response.refusal.delta") {
        if (!chunk.contains("delta") || !chunk["delta"].is_string()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        events.push_back(Json{{"type", "delta"},
                              {"content", ""},
                              {"refusal", chunk["delta"]},
                              {"index", chunk.value("output_index", 0)}});
        return SAO_AI_EDITOR_OK;
    }
    if (type == "response.reasoning_text.delta" ||
        type == "response.reasoning_summary_text.delta") {
        if (!chunk.contains("delta") || !chunk["delta"].is_string()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        events.push_back(Json{{"type", "delta"},
                              {"content", ""},
                              {"thinking", chunk["delta"]},
                              {"index", chunk.value("output_index", 0)}});
        return SAO_AI_EDITOR_OK;
    }
    if (type == "response.output_item.added" || type == "response.output_item.done") {
        const Json item = chunk.value("item", Json::object());
        if (item.is_object() && item.value("type", std::string{}) == "function_call") {
            const int32_t status =
                merge_tool_call(item, chunk.value("output_index", size_t{0}), false, nullptr);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        return SAO_AI_EDITOR_OK;
    }
    if (type == "response.function_call_arguments.delta" ||
        type == "response.function_call_arguments.done") {
        const char* field = type.ends_with(".delta") ? "delta" : "arguments";
        if (!chunk.contains(field) || !chunk[field].is_string()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        Json item{{"id", chunk.value("item_id", std::string{})},
                  {"call_id", chunk.value("call_id", std::string{})},
                  {"name", chunk.value("name", std::string{})},
                  {"arguments", chunk[field]}};
        Json event;
        const int32_t status = merge_tool_call(item, chunk.value("output_index", size_t{0}),
                                               type.ends_with(".delta"), &event);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        events.push_back(std::move(event));
        return SAO_AI_EDITOR_OK;
    }
    if (type == "response.completed" || type == "response.incomplete" ||
        type == "response.failed") {
        const Json response = chunk.value("response", Json::object());
        if (!response.is_object()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        const Json output = response.value("output", Json::array());
        if (output.is_array()) {
            for (size_t index = 0; index < output.size(); ++index) {
                const Json& item = output[index];
                if (item.is_object() && item.value("type", std::string{}) == "function_call") {
                    const int32_t status = merge_tool_call(item, index, false, nullptr);
                    if (status != SAO_AI_EDITOR_OK) {
                        return status;
                    }
                }
            }
        }
        const std::string response_status =
            to_lower(trim_ascii(response.value("status", std::string{})));
        const bool failed = type == "response.failed" || response_status == "failed" ||
                            response_status == "error" ||
                            (response.contains("error") && !response["error"].is_null());
        if (failed) {
            Json error = response.value("error", Json());
            if (error.is_null()) {
                error = Json{{"status", response_status},
                             {"message", "OpenAI Responses request failed"}};
            }
            events.push_back(
                Json{{"type", "error"}, {"error", std::move(error)}, {"response", response}});
        }
        Json summary{{"type", "message_delta"},
                     {"finish_reason", response.value("status", std::string{})}};
        if (response.contains("incomplete_details") && response["incomplete_details"].is_object()) {
            summary["finish_reason"] =
                response["incomplete_details"].value("reason", summary["finish_reason"]);
        }
        if (response.contains("usage") && response["usage"].is_object()) {
            summary["usage"] = response["usage"];
        }
        Json normalised;
        const int32_t decode_status = decode_openai_responses(response, normalised);
        if (decode_status != SAO_AI_EDITOR_OK && decode_status != SAO_AI_EDITOR_ERR_HTTP) {
            return decode_status;
        }
        for (const char* key : {"content", "thinking", "refusal", "toolCalls"}) {
            if (normalised.contains(key)) {
                summary[key] = normalised[key];
            }
        }
        events.push_back(std::move(summary));
        emit_tool_calls_final(events);
        events.push_back(Json{{"type", "done"}});
        done_ = true;
        return SAO_AI_EDITOR_OK;
    }
    if (type == "error" || type == "response.error") {
        events.push_back(Json{{"type", "error"},
                              {"error", chunk.value("error", chunk)},
                              {"response", chunk.value("response", Json())}});
    }
    // Lifecycle and unknown future event types carry no user-visible delta.
    return SAO_AI_EDITOR_OK;
}

int32_t OpenAiResponsesSseCodec::feed(std::string_view bytes, Json& events) {
    events = Json::array();
    if (bytes.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (done_) {
        const bool whitespace_only =
            std::all_of(bytes.begin(), bytes.end(),
                        [](unsigned char value) { return std::isspace(value) != 0; });
        if (whitespace_only) {
            return SAO_AI_EDITOR_OK;
        }
    }
    line_buffer_.append(bytes);
    return drain_sse_lines(line_buffer_, event_data_, [&] { return dispatch_event(events); });
}

} // namespace sao::ai_editor::native
