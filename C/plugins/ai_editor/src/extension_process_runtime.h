#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::native {

class ExtensionProcessRuntime final {
  public:
    using Json = nlohmann::json;
    using EventSink = std::function<void(std::string_view, const Json&)>;

    ExtensionProcessRuntime(std::filesystem::path workspace_root, EventSink event_sink);
    ~ExtensionProcessRuntime();

    ExtensionProcessRuntime(const ExtensionProcessRuntime&) = delete;
    ExtensionProcessRuntime& operator=(const ExtensionProcessRuntime&) = delete;

    int32_t create_terminal(const Json& params, Json& result);
    int32_t send_terminal_text(const Json& params, Json& result);
    int32_t show_terminal(const Json& params, Json& result);
    int32_t hide_terminal(const Json& params, Json& result);
    int32_t resize_terminal(const Json& params, Json& result);
    int32_t dispose_terminal(const Json& params, Json& result);
    int32_t list_terminals(Json& result) const;

    int32_t execute_task(const Json& params, Json& result);
    int32_t terminate_task(const Json& params, Json& result);
    int32_t task_status(const Json& params, Json& result) const;
    int32_t list_task_executions(Json& result) const;

    int32_t start_debug_session(const Json& params, Json& result);
    int32_t send_debug_message(const Json& params, Json& result);
    int32_t stop_debug_session(const Json& params, Json& result);
    int32_t debug_status(const Json& params, Json& result) const;
    int32_t list_debug_sessions(Json& result) const;

    void retire_owner(std::string_view extension_id, uint64_t generation) noexcept;
    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sao::ai_editor::native
