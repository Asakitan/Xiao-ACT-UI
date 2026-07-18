#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <string>

#include "openai_codec_internal.h"

namespace sao::ai_editor::native {

struct HttpChatRequest final {
    std::string endpoint;
    std::string api_key;
    std::string request_json;
    uint32_t timeout_ms = 60'000;
    bool stream = false;
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

int32_t perform_openai_chat(const HttpChatRequest& request,
                            ChatCancellation& cancellation,
                            const StreamEventCallback& callback,
                            Json& result);

}  // namespace sao::ai_editor::native
