#include "sao/ai_editor/mcp_codec.h"

#include "native_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace {

constexpr uint32_t kDefaultMaximumMessage = 4U * 1024U * 1024U;
constexpr size_t kMaximumHeaders = 64U * 1024U;

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

bool parse_rpc(std::string_view text, sao::ai_editor::native::Json& result) {
    if (!sao::ai_editor::native::valid_utf8(text)) {
        return false;
    }
    result = sao::ai_editor::native::Json::parse(text, nullptr, false);
    return (result.is_object() || result.is_array()) && !result.is_discarded();
}

class McpDecoder final {
public:
    explicit McpDecoder(uint32_t maximum_message)
        : maximum_message_(maximum_message) {}

    int32_t feed(std::string_view bytes, sao::ai_editor::native::Json& messages) {
        if (bytes.size() > maximum_message_) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        buffer_.append(bytes);
        if (buffer_.size() > maximum_message_ + kMaximumHeaders) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        messages = sao::ai_editor::native::Json::array();
        while (!buffer_.empty()) {
            while (!buffer_.empty() &&
                   (buffer_.front() == '\r' || buffer_.front() == '\n')) {
                buffer_.erase(buffer_.begin());
            }
            if (buffer_.empty()) {
                break;
            }
            const std::string lowered = lower_ascii(
                std::string_view(buffer_).substr(
                    0, std::min<size_t>(buffer_.size(), 32)));
            if (lowered.starts_with("content-length:")) {
                const size_t separator = buffer_.find("\r\n\r\n");
                size_t delimiter_size = 4;
                size_t header_end = separator;
                if (header_end == std::string::npos) {
                    header_end = buffer_.find("\n\n");
                    delimiter_size = 2;
                }
                if (header_end == std::string::npos) {
                    if (buffer_.size() > kMaximumHeaders) {
                        return SAO_AI_EDITOR_ERR_PROTOCOL;
                    }
                    break;
                }
                const std::string headers = buffer_.substr(0, header_end);
                const std::string lower_headers = lower_ascii(headers);
                const size_t marker = lower_headers.find("content-length:");
                const size_t line_end = lower_headers.find('\n', marker);
                std::string value = headers.substr(
                    marker + std::strlen("content-length:"),
                    line_end == std::string::npos
                        ? std::string::npos
                        : line_end - marker - std::strlen("content-length:"));
                value.erase(std::remove_if(value.begin(), value.end(),
                                           [](unsigned char character) {
                                               return std::isspace(character) != 0;
                                           }),
                            value.end());
                size_t consumed = 0;
                uint64_t length = 0;
                try {
                    length = std::stoull(value, &consumed, 10);
                } catch (...) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                if (consumed != value.size() || length > maximum_message_) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                const size_t body_start = header_end + delimiter_size;
                if (buffer_.size() - body_start < length) {
                    break;
                }
                const std::string payload = buffer_.substr(
                    body_start, static_cast<size_t>(length));
                sao::ai_editor::native::Json decoded;
                if (!parse_rpc(payload, decoded)) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                messages.push_back(std::move(decoded));
                buffer_.erase(0, body_start + static_cast<size_t>(length));
                continue;
            }

            const size_t newline = buffer_.find('\n');
            if (newline == std::string::npos) {
                if (buffer_.size() > kMaximumHeaders) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                break;
            }
            std::string payload = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!payload.empty() && payload.back() == '\r') {
                payload.pop_back();
            }
            if (payload.empty()) {
                continue;
            }
            sao::ai_editor::native::Json decoded;
            if (!parse_rpc(payload, decoded)) {
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
            messages.push_back(std::move(decoded));
        }
        return SAO_AI_EDITOR_OK;
    }

private:
    uint32_t maximum_message_;
    std::string buffer_;
};

}  // namespace

struct SaoAiEditorMcpDecoder {
    explicit SaoAiEditorMcpDecoder(uint32_t maximum_message)
        : codec(maximum_message) {}

    std::mutex mutex;
    McpDecoder codec;
    std::string pending_output;
};

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_encode(const void* json_message,
                         uint32_t json_length,
                         void* frame_out,
                         uint32_t frame_cap,
                         uint32_t* out_len) {
    try {
        if ((json_message == nullptr && json_length != 0) || out_len == nullptr ||
            json_length > kDefaultMaximumMessage) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const auto input = std::string_view(
            static_cast<const char*>(json_message), json_length);
        sao::ai_editor::native::Json decoded;
        if (!parse_rpc(input, decoded)) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        const std::string header =
            "Content-Length: " + std::to_string(json_length) + "\r\n\r\n";
        const uint64_t required = header.size() + json_length;
        if (required > UINT32_MAX) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_len = static_cast<uint32_t>(required);
        if (frame_out == nullptr || frame_cap < required) {
            return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        }
        auto* output = static_cast<char*>(frame_out);
        std::memcpy(output, header.data(), header.size());
        if (json_length > 0) {
            std::memcpy(output + header.size(), json_message, json_length);
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_decoder_create(uint32_t maximum_message_bytes,
                                 sao_ai_editor_mcp_decoder_t* out_handle) {
    try {
        if (out_handle == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_handle = nullptr;
        const uint32_t maximum = maximum_message_bytes == 0
            ? kDefaultMaximumMessage
            : maximum_message_bytes;
        if (maximum < 2 || maximum > 16U * 1024U * 1024U) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        auto decoder = std::make_unique<SaoAiEditorMcpDecoder>(maximum);
        *out_handle = decoder.release();
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_decoder_feed(sao_ai_editor_mcp_decoder_t handle,
                               const void* bytes,
                               uint32_t byte_count,
                               char* messages_out,
                               uint32_t messages_cap,
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
            sao::ai_editor::native::Json messages;
            const auto input = std::string_view(static_cast<const char*>(bytes),
                                                byte_count);
            const int32_t status = handle->codec.feed(input, messages);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            handle->pending_output =
                sao::ai_editor::native::dump_json(messages);
        } else if (byte_count != 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int32_t status = sao::ai_editor::native::copy_text_to_caller(
            handle->pending_output, messages_out, messages_cap, out_len);
        if (status == SAO_AI_EDITOR_OK) {
            handle->pending_output.clear();
        }
        return status;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_decoder_destroy(sao_ai_editor_mcp_decoder_t handle) {
    try {
        delete handle;
    } catch (...) {
    }
}
