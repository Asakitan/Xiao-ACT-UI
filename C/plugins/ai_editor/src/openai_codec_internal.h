#pragma once

#include <string>
#include <string_view>

#include "native_utils.h"

namespace sao::ai_editor::native {

int32_t decode_openai_response_text(std::string_view input, Json& result);

class OpenAiSseCodec final {
public:
    int32_t feed(std::string_view bytes, Json& events);

private:
    int32_t dispatch_event(Json& events);

    std::string line_buffer_;
    std::string event_data_;
    bool done_ = false;
};

}  // namespace sao::ai_editor::native
