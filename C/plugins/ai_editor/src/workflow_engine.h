#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {

class NativeRuntime;
class ScopeStore;

// One workflow step.  Multiple contiguous steps sharing the same non-empty
// group value run concurrently against the same pre-batch snapshot.
struct WorkflowStep {
    std::string agent{"default"};
    std::string prompt;
    std::string output_var;
    std::string label;
    std::string group;
    bool requires_confirmation = false;

    static WorkflowStep from_json(const Json& value);
    Json to_json() const;
};

struct WorkflowDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string icon;
    std::string when_to_use;
    bool builtin = false;
    std::vector<WorkflowStep> steps;
    std::string scope{"builtin"};

    static WorkflowDefinition from_json(const Json& value);
    Json to_json() const;
};

// Global workflow registry — mixes built-ins and disk-loaded definitions.
class WorkflowRegistry {
public:
    void reload(const ScopeStore& scopes);
    std::vector<WorkflowDefinition> list() const;
    bool get(std::string_view id, WorkflowDefinition& out) const;
    int32_t save(const WorkflowDefinition& definition,
                 const ScopeStore& scopes,
                 std::string_view scope,
                 std::string_view plugin_id);
    int32_t remove(std::string_view id,
                   const ScopeStore& scopes,
                   std::string_view scope,
                   std::string_view plugin_id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, WorkflowDefinition> workflows_;
};

// Runtime state for one running workflow execution.
class WorkflowExecution final {
public:
    WorkflowExecution(std::string id, WorkflowDefinition definition,
                      Json initial_input);
    ~WorkflowExecution();
    WorkflowExecution(const WorkflowExecution&) = delete;
    WorkflowExecution& operator=(const WorkflowExecution&) = delete;

    const std::string& id() const noexcept { return id_; }
    const WorkflowDefinition& definition() const noexcept { return definition_; }

    void start(NativeRuntime& runtime, Json provider, std::string model,
               uint32_t chat_timeout_ms);
    void request_pause() noexcept;
    void request_resume(const std::string& human_input) noexcept;
    int32_t confirm(std::string_view step_id, bool approved,
                    const std::string& note);
    void request_cancel() noexcept;
    void join() noexcept;

    Json snapshot() const;

private:
    struct GroupBatch {
        std::vector<size_t> indices;
    };

    void run_loop(NativeRuntime* runtime, Json provider, std::string model,
                  uint32_t chat_timeout_ms);
    int32_t execute_step(NativeRuntime& runtime,
                         const WorkflowStep& step,
                         const Json& provider,
                         const std::string& model,
                         uint32_t chat_timeout_ms,
                         std::string& out_content);
    int32_t execute_step_with_snapshot(
        NativeRuntime& runtime,
        const WorkflowStep& step,
        const std::unordered_map<std::string, std::string>& snapshot,
        const Json& provider,
        const std::string& model,
        uint32_t chat_timeout_ms,
        std::string& out_content,
        size_t step_index = 0);
    std::string interpolate(std::string_view text) const;
    static std::string interpolate_with(
        std::string_view text,
        const std::unordered_map<std::string, std::string>& vars);
    bool wait_for_confirmation(const WorkflowStep& step);

    std::string id_;
    WorkflowDefinition definition_;
    Json initial_input_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> pause_requested_{false};
    bool paused_ = false;
    bool confirmation_pending_ = false;
    std::string confirmation_step_id_;
    bool confirmation_approved_ = false;
    std::string confirmation_note_;
    std::string human_resume_input_;
    std::string status_{"pending"};
    std::string error_message_;
    int64_t started_at_ms_ = 0;
    int64_t completed_at_ms_ = 0;
    size_t current_step_index_ = 0;
    std::unordered_map<std::string, std::string> variables_;
    std::vector<Json> step_results_;
    std::thread worker_;
};

}  // namespace sao::ai_editor::native
