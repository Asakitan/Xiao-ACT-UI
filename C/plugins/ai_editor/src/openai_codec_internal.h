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
    void accumulate_tool_calls(const Json& delta_tool_calls, Json& events);

    std::string line_buffer_;
    std::string event_data_;
    // Streaming tool_calls arrive as delta chunks keyed by `index`; we grow
    // this array on demand and concatenate the `function.arguments` chunks so
    // downstream consumers see the final assembled JSON payload.
    Json tool_calls_accumulator_ = Json::array();
    bool done_ = false;
};

}  // namespace sao::ai_editor::native
