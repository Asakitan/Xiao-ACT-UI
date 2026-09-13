#include "sao/ai_editor/mcp_codec.h"

#include "native_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kDefaultMaximumMessage = 4U * 1024U * 1024U;
constexpr size_t kMaximumHeaders = 64U * 1024U;
constexpr size_t kMaximumJsonDepth = 64U;
constexpr size_t kMaximumJsonNodes = 16384U;
constexpr size_t kMaximumJsonStringBytes = 1024U * 1024U;

class BoundedRpcJsonSax final : public sao::ai_editor::native::Json::json_sax_t {
  public:
    bool null() override {
        return consume_node();
    }

    bool boolean(bool) override {
        return consume_node();
    }

    bool number_integer(number_integer_t) override {
        return consume_node();
    }

    bool number_unsigned(number_unsigned_t) override {
        return consume_node();
    }

    bool number_float(number_float_t value, const string_t&) override {
        return std::isfinite(value) && consume_node();
    }

    bool string(string_t& value) override {
        return value.size() <= kMaximumJsonStringBytes && consume_node();
    }

    bool binary(binary_t& value) override {
        return value.size() <= kMaximumJsonStringBytes && consume_node();
    }

    bool start_object(std::size_t) override {
        if (!start_container()) {
            return false;
        }
        object_keys_.emplace_back();
        return true;
    }

    bool key(string_t& value) override {
        return !object_keys_.empty() && object_keys_.back().insert(value).second &&
               value.size() <= kMaximumJsonStringBytes && consume_node();
    }

    bool end_object() override {
        if (object_keys_.empty()) {
            return false;
        }
        object_keys_.pop_back();
        return end_container();
    }

    bool start_array(std::size_t) override {
        return start_container();
    }

    bool end_array() override {
        return end_container();
    }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

    bool complete() const noexcept {
        return depth_ == 0 && object_keys_.empty();
    }

  private:
    bool consume_node() noexcept {
        if (nodes_ >= kMaximumJsonNodes) {
            return false;
        }
        ++nodes_;
        return true;
    }

    bool start_container() noexcept {
        if (depth_ >= kMaximumJsonDepth || !consume_node()) {
            return false;
        }
        ++depth_;
        return true;
    }

    bool end_container() noexcept {
        if (depth_ == 0) {
            return false;
        }
        --depth_;
        return true;
    }

    size_t depth_{};
    size_t nodes_{};
    std::vector<std::unordered_set<std::string>> object_keys_;
};

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool parse_rpc(std::string_view text, sao::ai_editor::native::Json& result) {
    if (text.empty() || !sao::ai_editor::native::valid_utf8(text)) {
        return false;
    }
    try {
        BoundedRpcJsonSax validator;
        if (!sao::ai_editor::native::Json::sax_parse(text.begin(), text.end(), &validator) ||
            !validator.complete()) {
            return false;
        }
        result = sao::ai_editor::native::Json::parse(text, nullptr, false);
        return (result.is_object() || result.is_array()) && !result.is_discarded();
    } catch (...) {
        result = sao::ai_editor::native::Json();
        return false;
    }
}

class McpDecoder final {
  public:
    explicit McpDecoder(uint32_t maximum_message) : maximum_message_(maximum_message) {}

    int32_t feed(std::string_view bytes, sao::ai_editor::native::Json& messages) {
        const size_t maximum_message = static_cast<size_t>(maximum_message_);
        if (maximum_message > (std::numeric_limits<size_t>::max)() - kMaximumHeaders) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        const size_t maximum_buffer = maximum_message + kMaximumHeaders;
        if (buffer_.size() > maximum_buffer || bytes.size() > maximum_buffer - buffer_.size()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        if (!bytes.empty()) {
            buffer_.append(bytes);
        }
        messages = sao::ai_editor::native::Json::array();
        while (!buffer_.empty()) {
            const size_t first_payload = buffer_.find_first_not_of("\r\n");
            if (first_payload == std::string::npos) {
                buffer_.clear();
                break;
            }
            if (first_payload != 0) {
                buffer_.erase(0, first_payload);
            }
            const std::string lowered = lower_ascii(
                std::string_view(buffer_).substr(0, std::min<size_t>(buffer_.size(), 32)));
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
                if (header_end > kMaximumHeaders || header_end > maximum_buffer - delimiter_size) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                const std::string headers = buffer_.substr(0, header_end);
                const std::string lower_headers = lower_ascii(headers);
                const size_t marker = lower_headers.find("content-length:");
                if (marker != 0 ||
                    lower_headers.find("\ncontent-length:", marker + 1) != std::string::npos) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                const size_t line_end = lower_headers.find('\n', marker);
                size_t value_begin = marker + std::strlen("content-length:");
                size_t value_end = line_end == std::string::npos ? headers.size() : line_end;
                while (value_begin < value_end &&
                       (headers[value_begin] == ' ' || headers[value_begin] == '\t')) {
                    ++value_begin;
                }
                while (value_end > value_begin &&
                       (headers[value_end - 1] == ' ' || headers[value_end - 1] == '\t' ||
                        headers[value_end - 1] == '\r')) {
                    --value_end;
                }
                const std::string_view value(headers.data() + value_begin, value_end - value_begin);
                uint64_t length = 0;
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), length, 10);
                if (value.empty() || parsed.ec != std::errc{} ||
                    parsed.ptr != value.data() + value.size() || length > maximum_message_) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                const size_t body_start = header_end + delimiter_size;
                const size_t payload_length = static_cast<size_t>(length);
                if (body_start > buffer_.size() || payload_length > buffer_.size() - body_start) {
                    break;
                }
                const std::string payload = buffer_.substr(body_start, payload_length);
                sao::ai_editor::native::Json decoded;
                if (!parse_rpc(payload, decoded)) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                messages.push_back(std::move(decoded));
                buffer_.erase(0, body_start + payload_length);
                continue;
            }

            const size_t newline = buffer_.find('\n');
            if (newline == std::string::npos) {
                constexpr std::string_view content_length = "content-length:";
                const bool possible_header =
                    content_length.starts_with(lowered) || lowered.starts_with(content_length);
                if ((possible_header && buffer_.size() > kMaximumHeaders) ||
                    (!possible_header && buffer_.size() > maximum_message)) {
                    return SAO_AI_EDITOR_ERR_PROTOCOL;
                }
                break;
            }
            size_t payload_length = newline;
            if (payload_length != 0 && buffer_[payload_length - 1] == '\r') {
                --payload_length;
            }
            if (payload_length > maximum_message) {
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
            std::string payload = buffer_.substr(0, payload_length);
            buffer_.erase(0, newline + 1);
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

} // namespace

struct SaoAiEditorMcpDecoder {
    explicit SaoAiEditorMcpDecoder(uint32_t maximum_message) : codec(maximum_message) {}

    std::mutex mutex;
    McpDecoder codec;
    std::string pending_output;
};

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_encode(const void* json_message, uint32_t json_length, void* frame_out,
                         uint32_t frame_cap, uint32_t* out_len) {
    try {
        if ((json_message == nullptr && json_length != 0) || out_len == nullptr ||
            json_length > kDefaultMaximumMessage) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string_view input =
            json_length == 0
                ? std::string_view{}
                : std::string_view(static_cast<const char*>(json_message), json_length);
        sao::ai_editor::native::Json decoded;
        if (!parse_rpc(input, decoded)) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        const std::string header = "Content-Length: " + std::to_string(json_length) + "\r\n\r\n";
        if (header.size() > UINT32_MAX - static_cast<uint64_t>(json_length)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const size_t required = header.size() + static_cast<size_t>(json_length);
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

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_decoder_create(
    uint32_t maximum_message_bytes, sao_ai_editor_mcp_decoder_t* out_handle) {
    try {
        if (out_handle == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_handle = nullptr;
        const uint32_t maximum =
            maximum_message_bytes == 0 ? kDefaultMaximumMessage : maximum_message_bytes;
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

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_decoder_feed(
    sao_ai_editor_mcp_decoder_t handle, const void* bytes, uint32_t byte_count, char* messages_out,
    uint32_t messages_cap, uint32_t* out_len) {
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
            const std::string_view input =
                byte_count == 0 ? std::string_view{}
                                : std::string_view(static_cast<const char*>(bytes), byte_count);
            const int32_t status = handle->codec.feed(input, messages);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            handle->pending_output = sao::ai_editor::native::dump_json(messages);
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
