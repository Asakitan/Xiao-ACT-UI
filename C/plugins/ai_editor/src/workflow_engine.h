#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
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

    // Snapshot workflow definitions for export.
    // scope options:
    //   "workspace" — user workflows currently attached to the workspace scope
    //   "system"    — user workflows currently attached to the system scope
    //   "builtin"   — the compiled-in built-in workflows
    //   "all"       — everything the registry knows about (workspace + system +
    //                 plugin:* + builtin).  Only user workflows keep their
    //                 storage scope; built-ins are tagged scope="builtin".
    // Populates `result` with a sao-workflows/1 envelope.
    int32_t export_all(std::string_view scope, Json& result) const;

    // Import a single workflow definition into the target scope.  Mirrors
    // ConversationStore::import_conversation: honours `overwrite` on
    // conflict, blocks any attempt to shadow a built-in, and yields the id
    // that was actually written.  Callers stage two passes for atomicity
    // (see dispatch_workflow) so the store either fully accepts or fully
    // rejects a batch.
    int32_t import_workflow(const Json& workflow,
                            std::string_view scope,
                            std::string_view plugin_id,
                            bool overwrite,
                            const ScopeStore& scopes,
                            std::string& out_id);

    // Look up whether an id is currently owned by a built-in; used by the
    // dispatch layer to fail atomically before any import writes happen.
    bool is_builtin(std::string_view id) const;

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

    // Retry support: seed the execution so run_loop begins at
    // `step_index` instead of 0.  Optional `preserved_variables` overrides
    // the variable map set up by the constructor — used by
    // workflow.retry when keepVariables=true to carry successful step
    // outputs from a previous run.  `retry_of` records the original
    // execution id so it flows into snapshot()/history_record() and
    // becomes discoverable from the persisted record.  Must be called
    // before start(); once the worker thread is spawned these knobs are
    // read-only.
    void seed_retry_state(size_t step_index,
                          std::unordered_map<std::string, std::string>
                              preserved_variables,
                          std::string retry_of);

    void start(NativeRuntime& runtime, Json provider, std::string model,
               uint32_t chat_timeout_ms);
    // Overload that also takes a directory to persist a full history record
    // into when the run reaches a terminal state.  Empty path disables
    // persistence (used by legacy tests and callers that don't want the disk
    // side-effect).  `history_root` must already exist or be creatable — the
    // executor treats persistence as best-effort and never aborts the run
    // for a write failure.
    void start(NativeRuntime& runtime, Json provider, std::string model,
               uint32_t chat_timeout_ms,
               std::filesystem::path history_root);
    void request_pause() noexcept;
    void request_resume(const std::string& human_input) noexcept;
    int32_t confirm(std::string_view step_id, bool approved,
                    const std::string& note);
    // Skip the current step (batch).  Legal only when the execution is
    // parked in "waiting_confirmation" or "paused"; any other state returns
    // INVALID_ARGUMENT.  The skip is honoured cooperatively — the worker
    // records the affected step(s) as status="skipped" with empty output,
    // then advances past the batch without hitting the LLM.
    int32_t request_skip(const std::string& reason);
    // Mutate a variable while the workflow is live (running / paused /
    // waiting_confirmation).  Terminal states (completed/failed/cancelled)
    // reject with INVALID_ARGUMENT — snapshotting is fine but scribbling
    // over completed runs would confuse history consumers.  Variable names
    // must match valid_simple_id rules so history_record round-trips
    // cleanly.  Only affects subsequent batches — the pre-batch snapshot
    // R2 introduced guarantees an in-flight batch stays consistent.
    int32_t set_variable(std::string_view name, std::string_view value);
    // Point-in-time copy of variables_ (thread-safe).  Same shape as the
    // `variables` field snapshot() emits, exposed separately so callers can
    // ask "what does this execution's variable map look like right now?"
    // without pulling the full workflow snapshot.
    Json variables_snapshot() const;
    void request_cancel() noexcept;
    void join() noexcept;

    Json snapshot() const;

    // Extended snapshot that adds workflowName + normalised fields the
    // history reader expects.  Same content as snapshot() plus the derived
    // display metadata; safe to call from any thread.
    Json history_record() const;

    // Best-effort disk persistence: writes `<history_root>/<id>.json` with
    // the full history_record() payload atomically.  Returns
    // SAO_AI_EDITOR_OK on success, a specific status on failure but the
    // run itself is never aborted — callers already treat persistence as
    // advisory.
    int32_t persist(const std::filesystem::path& history_root) const;

    // Enumerate persisted execution records under `history_root`.  Fills
    // `out_summaries` with summary payloads (id/workflowId/workflowName/
    // status/completedAt/totalSteps) sorted by completedAt descending; the
    // caller applies the final limit + workflowId filter.  Missing roots
    // are treated as empty.
    static int32_t enumerate_history(
        const std::filesystem::path& history_root,
        std::string_view workflow_id_filter,
        std::vector<Json>& out_summaries);

    // Load a full persisted record by id from `history_root`.  Returns
    // SAO_AI_EDITOR_ERR_NOT_FOUND when the file does not exist.
    static int32_t load_history_record(
        const std::filesystem::path& history_root,
        std::string_view execution_id,
        Json& out_record);

    // Delete a persisted record by id.  Returns
    // SAO_AI_EDITOR_ERR_NOT_FOUND when the file did not exist.
    static int32_t delete_history_record(
        const std::filesystem::path& history_root,
        std::string_view execution_id);

    // Publicly reusable prompt interpolator — swaps {{name}} against
    // `vars` in-place using the same rules run_loop applies before
    // handing prompts to the LLM.  Exposed so workflow.dry_run (and any
    // future preview surface) can share the exact substitution logic
    // without duplicating it and drifting.
    static std::string interpolate_with(
        std::string_view text,
        const std::unordered_map<std::string, std::string>& vars);

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
    // Confirmation gate return value.  Skip is folded into the same wait
    // as approve/reject so run_loop can serve all three outcomes from one
    // condition-variable predicate instead of racing multiple signals.
    enum class ConfirmationOutcome {
        Rejected,  // user declined — run_loop should terminate as "failed"
        Approved,  // proceed to execute the batch
        Skipped,   // batch marked "skipped" without hitting the LLM
    };
    ConfirmationOutcome wait_for_confirmation(const WorkflowStep& step);

    std::string id_;
    WorkflowDefinition definition_;
    Json initial_input_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> pause_requested_{false};
    // Skip request is intentionally atomic so wait_for_confirmation /
    // pause branch can peek it without taking mutex_ (they still take the
    // mutex to consume it — the atomic only gates the fast path).  The
    // step index it targets is captured under mutex_ at request time so a
    // "skip current step" call names the batch that was live when the
    // caller decided; a subsequent skip would refuse if the worker has
    // moved on.
    std::atomic<bool> skip_requested_{false};
    size_t skip_step_index_ = 0;
    std::string skip_reason_;
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
    // Empty ↔ persistence disabled.  Populated by the start() overload that
    // wires up disk history; run_loop() writes to this directory once the
    // execution reaches a terminal state.
    std::filesystem::path history_persist_root_;
    // Retry knobs.  Both default to the "fresh run" values so unchanged
    // callers observe original semantics; workflow.retry populates them
    // before start() to graft onto a prior failed execution.
    size_t start_at_step_index_ = 0;
    std::string retry_of_;
};

}  // namespace sao::ai_editor::native
