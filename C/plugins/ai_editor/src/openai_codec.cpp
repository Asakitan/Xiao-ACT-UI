#include "sao/ai_editor/openai_codec.h"

#include "openai_codec_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace sao::ai_editor::native {
namespace {

Json normalized_tool_calls(const Json& value) {
    Json result = Json::array();
    if (!value.is_array()) {
        return result;
    }
    for (const auto& item : value) {
        if (!item.is_object()) {
            continue;
        }
        const Json function = item.value("function", Json::object());
        result.push_back({{"index", item.value("index", 0)},
                          {"id", item.value("id", "")},
                          {"type", item.value("type", "function")},
                          {"name", function.value("name", "")},
                          {"arguments", function.value("arguments", "")}});
    }
    return result;
}

std::string content_text(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    std::string result;
    if (value.is_array()) {
        for (const auto& item : value) {
            if (item.is_object() && item.contains("text") &&
                item["text"].is_string()) {
                result += item["text"].get<std::string>();
            }
        }
    }
    return result;
}

Json normalize_chunk(const Json& chunk) {
    Json record{{"type", "delta"},
                {"id", chunk.value("id", "")},
                {"model", chunk.value("model", "")}};
    if (chunk.contains("usage") && chunk["usage"].is_object()) {
        record["usage"] = chunk["usage"];
    }
    const Json choices = chunk.value("choices", Json::array());
    if (choices.is_array() && !choices.empty() && choices[0].is_object()) {
        const Json& choice = choices[0];
        const Json delta = choice.value("delta", Json::object());
        record["index"] = choice.value("index", 0);
        record["content"] = content_text(delta.value("content", Json()));
        record["thinking"] = delta.value(
            "reasoning_content", delta.value("thinking", ""));
        record["refusal"] = delta.value("refusal", "");
        record["toolCalls"] = normalized_tool_calls(
            delta.value("tool_calls", Json::array()));
        if (choice.contains("finish_reason") &&
            !choice["finish_reason"].is_null()) {
            record["finishReason"] = choice["finish_reason"];
        }
    }
    return record;
}

}  // namespace

int32_t decode_openai_response_text(std::string_view input, Json& result) {
    if (input.empty() || input.size() > kMaximumJsonBytes ||
        !valid_utf8(input)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json parsed = Json::parse(input);
    if (!parsed.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    if (parsed.contains("error")) {
        result = Json{{"ok", false}, {"error", parsed["error"]}};
        return SAO_AI_EDITOR_OK;
    }
    const Json choices = parsed.value("choices", Json::array());
    if (!choices.is_array() || choices.empty() || !choices[0].is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    const Json& choice = choices[0];
    const Json message = choice.value("message", Json::object());
    result = Json{{"ok", true},
                  {"id", parsed.value("id", "")},
                  {"model", parsed.value("model", "")},
                  {"content", content_text(message.value("content", Json()))},
                  {"thinking", message.value(
                                   "reasoning_content",
                                   message.value("thinking", ""))},
                  {"refusal", message.value("refusal", "")},
                  {"toolCalls", normalized_tool_calls(
                                    message.value("tool_calls", Json::array()))},
                  {"finishReason", choice.value("finish_reason", "")},
                  {"usage", parsed.value("usage", Json::object())}};
    return SAO_AI_EDITOR_OK;
}

void OpenAiSseCodec::accumulate_tool_calls(const Json& delta_tool_calls,
                                           Json& events) {
    if (!delta_tool_calls.is_array()) {
        return;
    }
    for (const auto& piece : delta_tool_calls) {
        if (!piece.is_object() || !piece.contains("index") ||
            !piece["index"].is_number_integer()) {
            continue;
        }
        const int64_t signed_index = piece["index"].get<int64_t>();
        if (signed_index < 0) {
            continue;
        }
        const size_t index = static_cast<size_t>(signed_index);
        while (tool_calls_accumulator_.size() <= index) {
            const size_t slot_index = tool_calls_accumulator_.size();
            tool_calls_accumulator_.push_back(
                Json{{"index", slot_index},
                     {"id", ""},
                     {"type", "function"},
                     {"function",
                      Json{{"name", ""}, {"arguments", ""}}}});
        }
        Json& slot = tool_calls_accumulator_[index];
        if (piece.contains("id") && piece["id"].is_string()) {
            slot["id"] = piece["id"];
        }
        if (piece.contains("type") && piece["type"].is_string()) {
            slot["type"] = piece["type"];
        }
        if (piece.contains("function") && piece["function"].is_object()) {
            const Json& fn_delta = piece["function"];
            if (!slot["function"].is_object()) {
                slot["function"] =
                    Json{{"name", ""}, {"arguments", ""}};
            }
            Json& fn = slot["function"];
            if (fn_delta.contains("name") && fn_delta["name"].is_string()) {
                fn["name"] = fn_delta["name"];
            }
            if (fn_delta.contains("arguments") &&
                fn_delta["arguments"].is_string()) {
                const std::string existing = fn.value("arguments", "");
                const std::string appended =
                    fn_delta["arguments"].get<std::string>();
                fn["arguments"] = existing + appended;
            }
        }
        events.push_back(Json{{"type", "tool_delta"},
                              {"index", index},
                              {"name", slot["function"].value("name", "")},
                              {"arguments",
                               slot["function"].value("arguments", "")},
                              {"partial", slot}});
    }
}

int32_t OpenAiSseCodec::dispatch_event(Json& events) {
    if (event_data_.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    std::string payload = std::move(event_data_);
    event_data_.clear();
    if (payload == "[DONE]") {
        done_ = true;
        if (!tool_calls_accumulator_.empty()) {
            events.push_back(Json{{"type", "tool_calls_final"},
                                  {"tool_calls", tool_calls_accumulator_}});
        }
        events.push_back({{"type", "done"}});
        return SAO_AI_EDITOR_OK;
    }
    Json chunk = Json::parse(payload);
    if (!chunk.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    if (chunk.contains("error")) {
        events.push_back({{"type", "error"}, {"error", chunk["error"]}});
        return SAO_AI_EDITOR_OK;
    }
    // Accumulate tool_calls before normalizing so `tool_delta` events show up
    // in-order with the normalized delta event that carries text content.
    const Json choices = chunk.value("choices", Json::array());
    if (choices.is_array() && !choices.empty() && choices[0].is_object()) {
        const Json& delta = choices[0].value("delta", Json::object());
        if (delta.is_object() && delta.contains("tool_calls")) {
            accumulate_tool_calls(delta["tool_calls"], events);
        }
    }
    events.push_back(normalize_chunk(chunk));
    return SAO_AI_EDITOR_OK;
}

int32_t OpenAiSseCodec::feed(std::string_view bytes, Json& events) {
    events = Json::array();
    if (bytes.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (done_) {
        return std::all_of(bytes.begin(), bytes.end(), [](unsigned char value) {
                   return std::isspace(value) != 0;
               })
            ? SAO_AI_EDITOR_OK
            : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    line_buffer_.append(bytes);
    if (line_buffer_.size() > kMaximumJsonBytes) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    size_t newline = 0;
    while ((newline = line_buffer_.find('\n')) != std::string::npos) {
        std::string line = line_buffer_.substr(0, newline);
        line_buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            const int32_t status = dispatch_event(events);
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
            if (!event_data_.empty()) {
                event_data_.push_back('\n');
            }
            event_data_.append(value);
            if (event_data_.size() > kMaximumJsonBytes) {
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native

struct SaoAiEditorOpenAiSseDecoder {
    std::mutex mutex;
    sao::ai_editor::native::OpenAiSseCodec codec;
    std::string pending_output;
};

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_openai_decode_response(const void* response_json,
                                     uint32_t response_len,
                                     char* normalized_out,
                                     uint32_t normalized_cap,
                                     uint32_t* out_len) {
    try {
        if ((response_json == nullptr && response_len != 0) ||
            out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        sao::ai_editor::native::Json normalized;
        const auto input = std::string_view(
            static_cast<const char*>(response_json), response_len);
        const int32_t status =
            sao::ai_editor::native::decode_openai_response_text(input,
                                                                 normalized);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        const std::string text = sao::ai_editor::native::dump_json(normalized);
        return sao::ai_editor::native::copy_text_to_caller(
            text, normalized_out, normalized_cap, out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_openai_sse_decoder_create(
    sao_ai_editor_openai_sse_decoder_t* out_handle) {
    try {
        if (out_handle == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_handle = nullptr;
        auto decoder = std::make_unique<SaoAiEditorOpenAiSseDecoder>();
        *out_handle = decoder.release();
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_openai_sse_decoder_feed(
    sao_ai_editor_openai_sse_decoder_t handle,
    const void* bytes,
    uint32_t byte_count,
    char* events_out,
    uint32_t events_cap,
    uint32_t* out_len) {
    try {
        if (handle == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if ((bytes == nullptr && byte_count != 0) || out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (handle->pending_output.empty()) {
            sao::ai_editor::native::Json events;
            const auto input = std::string_view(static_cast<const char*>(bytes),
                                                byte_count);
            const int32_t status = handle->codec.feed(input, events);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            handle->pending_output = sao::ai_editor::native::dump_json(events);
        } else if (byte_count != 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int32_t status = sao::ai_editor::native::copy_text_to_caller(
            handle->pending_output, events_out, events_cap, out_len);
        if (status == SAO_AI_EDITOR_OK) {
            handle->pending_output.clear();
        }
        return status;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL
sao_ai_editor_openai_sse_decoder_destroy(
    sao_ai_editor_openai_sse_decoder_t handle) {
    try {
        delete handle;
    } catch (...) {
    }
}
