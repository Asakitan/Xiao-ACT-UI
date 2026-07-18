#include "workflow_engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "native_runtime_internal.h"
#include "scope_store.h"
#include "sha256_helper.h"

namespace sao::ai_editor::native {
namespace {

std::filesystem::path workflow_market_module_directory() {
    HMODULE module_handle = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&workflow_market_module_directory),
            &module_handle) ||
        module_handle == nullptr) {
        return {};
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            module_handle, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            std::filesystem::path path(buffer.data());
            return path.parent_path();
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) {
            return {};
        }
    }
}

// Mirror of agent_registry.cpp::resolve_market_assets_dir — same anchor
// strategy, distinct symbol so translation units stay independent.
std::filesystem::path resolve_workflow_market_dir(std::wstring_view subdir) {
    const std::filesystem::path anchor = workflow_market_module_directory();
    if (anchor.empty()) {
        return {};
    }
    const std::wstring rel_install =
        std::wstring(L"assets/ai_editor/") + std::wstring(subdir);
    const std::wstring candidates[] = {
        rel_install,
        std::wstring(L"../") + rel_install,
        std::wstring(L"../../") + rel_install,
        std::wstring(L"../plugins/ai_editor/") + rel_install,
        std::wstring(L"../../plugins/ai_editor/") + rel_install,
        std::wstring(L"../../../plugins/ai_editor/") + rel_install,
        std::wstring(L"../../../../plugins/ai_editor/") + rel_install,
    };
    for (const auto& rel : candidates) {
        std::filesystem::path candidate = anchor / rel;
        std::error_code error;
        candidate = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            continue;
        }
        if (std::filesystem::is_directory(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

template <typename Sink>
void enumerate_workflow_market_json(const std::filesystem::path& dir,
                                    Sink&& sink) {
    if (dir.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(dir, error);
    if (error) {
        return;
    }
    for (const auto& entry : iterator) {
        std::error_code file_error;
        if (!entry.is_regular_file(file_error) || file_error) {
            continue;
        }
        const auto& path = entry.path();
        if (path.extension() != L".json") {
            continue;
        }
        std::string text;
        if (read_text_file(path, kMaximumJsonBytes, text) !=
                SAO_AI_EDITOR_OK ||
            text.empty()) {
            continue;
        }
        Json parsed = Json::parse(text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            continue;
        }
        sink(parsed);
    }
}

int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string next_execution_id() {
    static std::atomic<uint64_t> sequence{0};
    const auto ticks = static_cast<uint64_t>(unix_milliseconds());
    const auto current = sequence.fetch_add(1, std::memory_order_relaxed);
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "wf-%llx-%llx",
                  static_cast<unsigned long long>(ticks),
                  static_cast<unsigned long long>(current));
    return buffer;
}

const std::vector<WorkflowDefinition>& builtin_workflows() {
    static const std::vector<WorkflowDefinition> workflows = [] {
        std::vector<WorkflowDefinition> result;
        {
            WorkflowDefinition workflow;
            workflow.id = "review-and-fix";
            workflow.name = "Review & Fix";
            workflow.description = "Review code for issues, then generate fixes";
            workflow.icon = "\xF0\x9F\x94\xA7";  // wrench emoji
            workflow.when_to_use = "When the user wants both a review and fixes";
            workflow.builtin = true;
            workflow.scope = "builtin";
            WorkflowStep review;
            review.agent = "code-reviewer";
            review.prompt =
                "Review the following and list all issues:\n\n{{input}}";
            review.output_var = "review";
            review.label = "Reviewing code";
            workflow.steps.push_back(std::move(review));
            WorkflowStep fix;
            fix.agent = "default";
            fix.prompt =
                "Based on this review:\n\n{{review}}\n\n"
                "Generate the minimal fixes. Show only changes needed.";
            fix.output_var = "fix";
            fix.label = "Generating fixes";
            workflow.steps.push_back(std::move(fix));
            result.push_back(std::move(workflow));
        }
        {
            WorkflowDefinition workflow;
            workflow.id = "explain-and-improve";
            workflow.name = "Explain & Improve";
            workflow.description =
                "Explain code in detail, then suggest improvements";
            workflow.icon = "\xF0\x9F\x92\xA1";  // lightbulb emoji
            workflow.when_to_use =
                "When the user wants to understand and improve code";
            workflow.builtin = true;
            workflow.scope = "builtin";
            WorkflowStep explain;
            explain.agent = "explainer";
            explain.prompt = "Explain this code in detail:\n\n{{input}}";
            explain.output_var = "explanation";
            explain.label = "Analyzing code";
            workflow.steps.push_back(std::move(explain));
            WorkflowStep improve;
            improve.agent = "optimizer";
            improve.prompt =
                "Given this analysis:\n\n{{explanation}}\n\n"
                "Suggest concrete improvements for performance, readability, "
                "and maintainability.";
            improve.output_var = "improvements";
            improve.label = "Finding improvements";
            workflow.steps.push_back(std::move(improve));
            result.push_back(std::move(workflow));
        }
        {
            WorkflowDefinition workflow;
            workflow.id = "debug-trace";
            workflow.name = "Debug Trace";
            workflow.description =
                "Analyze an error, diagnose root cause, suggest fix";
            workflow.icon = "\xF0\x9F\x90\x9B";  // bug emoji
            workflow.when_to_use =
                "When the user reports a bug and wants diagnosis + fix";
            workflow.builtin = true;
            workflow.scope = "builtin";
            WorkflowStep diagnose;
            diagnose.agent = "debugger";
            diagnose.prompt =
                "Analyze this error/bug and identify the root cause:\n\n"
                "{{input}}";
            diagnose.output_var = "diagnosis";
            diagnose.label = "Diagnosing issue";
            workflow.steps.push_back(std::move(diagnose));
            WorkflowStep fix;
            fix.agent = "default";
            fix.prompt =
                "Based on this diagnosis:\n\n{{diagnosis}}\n\n"
                "Provide the minimal fix with code.";
            fix.output_var = "fix";
            fix.label = "Generating fix";
            workflow.steps.push_back(std::move(fix));
            result.push_back(std::move(workflow));
        }
        return result;
    }();
    return workflows;
}

}  // namespace

WorkflowStep WorkflowStep::from_json(const Json& value) {
    WorkflowStep step;
    if (!value.is_object()) {
        return step;
    }
    step.agent = value.value("agent", std::string{"default"});
    step.prompt = value.value("prompt", std::string{});
    step.output_var = value.value("output_var", std::string{});
    step.label = value.value("label", std::string{});
    step.group = value.value("group", std::string{});
    step.requires_confirmation = value.value("requires_confirmation", false);
    return step;
}

Json WorkflowStep::to_json() const {
    return Json{{"agent", agent},
                {"prompt", prompt},
                {"output_var", output_var},
                {"label", label},
                {"group", group},
                {"requires_confirmation", requires_confirmation}};
}

WorkflowDefinition WorkflowDefinition::from_json(const Json& value) {
    WorkflowDefinition workflow;
    if (!value.is_object()) {
        return workflow;
    }
    workflow.id = value.value("id", std::string{});
    workflow.name = value.value("name", std::string{});
    workflow.description = value.value("description", std::string{});
    workflow.icon = value.value("icon", std::string{});
    workflow.when_to_use = value.value("when_to_use", std::string{});
    workflow.builtin = value.value("builtin", false);
    workflow.scope = value.value("scope", std::string{"workspace"});
    if (value.contains("steps") && value["steps"].is_array()) {
        for (const auto& step_value : value["steps"]) {
            workflow.steps.push_back(WorkflowStep::from_json(step_value));
        }
    }
    return workflow;
}

Json WorkflowDefinition::to_json() const {
    Json steps_json = Json::array();
    for (const auto& step : this->steps) {
        steps_json.push_back(step.to_json());
    }
    return Json{{"id", id},
                {"name", name},
                {"description", description},
                {"icon", icon},
                {"when_to_use", when_to_use},
                {"builtin", builtin},
                {"scope", scope},
                {"steps", std::move(steps_json)}};
}

void WorkflowRegistry::reload(const ScopeStore& scopes) {
    std::lock_guard<std::mutex> guard(mutex_);
    workflows_.clear();
    for (const auto& workflow : builtin_workflows()) {
        workflows_.emplace(workflow.id, workflow);
    }
    // Market presets: install-time bundled workflow definitions.  Loaded
    // after built-ins so built-in ids win, and before user scopes so user
    // edits with the same id override the market copy.  Marked
    // builtin=false / scope="market" so users can override or delete them.
    enumerate_workflow_market_json(
        resolve_workflow_market_dir(L"workflows"), [this](const Json& item) {
            if (!item.contains("id") || !item["id"].is_string()) {
                return;
            }
            WorkflowDefinition workflow = WorkflowDefinition::from_json(item);
            if (workflow.id.empty()) {
                return;
            }
            const auto existing = workflows_.find(workflow.id);
            if (existing != workflows_.end() && existing->second.builtin) {
                return;  // built-in wins over market
            }
            workflow.builtin = false;
            workflow.scope = "market";
            workflows_[workflow.id] = std::move(workflow);
        });
    Json registry;
    if (scopes.load_registry("workflows", Json::array(), registry) !=
            SAO_AI_EDITOR_OK ||
        !registry.is_array()) {
        return;
    }
    for (const auto& item : registry) {
        if (!item.is_object() || !item.contains("id") ||
            !item["id"].is_string()) {
            continue;
        }
        WorkflowDefinition workflow = WorkflowDefinition::from_json(item);
        workflow.builtin = false;
        workflow.scope = item.value("scope", std::string{"workspace"});
        workflows_[workflow.id] = std::move(workflow);
    }
}

std::vector<WorkflowDefinition> WorkflowRegistry::list() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<WorkflowDefinition> result;
    result.reserve(workflows_.size());
    for (const auto& [id, definition] : workflows_) {
        (void)id;
        result.push_back(definition);
    }
    std::sort(result.begin(), result.end(),
              [](const WorkflowDefinition& lhs, const WorkflowDefinition& rhs) {
                  if (lhs.builtin != rhs.builtin) {
                      return lhs.builtin;
                  }
                  return lhs.id < rhs.id;
              });
    return result;
}

bool WorkflowRegistry::get(std::string_view id, WorkflowDefinition& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = workflows_.find(std::string(id));
    if (found == workflows_.end()) {
        return false;
    }
    out = found->second;
    return true;
}

int32_t WorkflowRegistry::save(const WorkflowDefinition& definition,
                                const ScopeStore& scopes,
                                std::string_view scope,
                                std::string_view plugin_id) {
    if (!valid_simple_id(definition.id) || definition.name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto existing = workflows_.find(definition.id);
        if (existing != workflows_.end() && existing->second.builtin) {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
    }
    Json payload = definition.to_json();
    payload.erase("builtin");
    payload.erase("scope");
    const int32_t status = scopes.save_registry_item("workflows", scope,
                                                     plugin_id, definition.id,
                                                     payload);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    WorkflowDefinition stored = definition;
    stored.builtin = false;
    stored.scope = std::string(scope) +
                   (plugin_id.empty() ? std::string{}
                                      : ":" + std::string(plugin_id));
    workflows_[definition.id] = std::move(stored);
    return SAO_AI_EDITOR_OK;
}

bool WorkflowRegistry::is_builtin(std::string_view id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = workflows_.find(std::string(id));
    return found != workflows_.end() && found->second.builtin;
}

int32_t WorkflowRegistry::export_all(std::string_view scope,
                                     Json& result) const {
    if (scope != "workspace" && scope != "system" && scope != "all" &&
        scope != "builtin") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json workflows = Json::array();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& [id, definition] : workflows_) {
            (void)id;
            if (scope == "builtin") {
                if (!definition.builtin) {
                    continue;
                }
            } else if (scope == "workspace" || scope == "system") {
                if (definition.builtin) {
                    continue;
                }
                // Match the raw scope key stored on the definition; for
                // plugin-scoped items this is "plugin:<id>" so a plain
                // "workspace"/"system" export skips them, which mirrors
                // conversation.export scope semantics.
                if (definition.scope != scope) {
                    continue;
                }
            }
            // scope == "all" → include everything (builtin + user + plugin:*).
            workflows.push_back(definition.to_json());
        }
    }
    const size_t count = workflows.size();
    result = Json{{"format", "sao-workflows/1"},
                  {"workflows", std::move(workflows)},
                  {"count", count},
                  {"exportedAt", unix_milliseconds()}};
    // See ConversationStore::export_all() for the checksum invariant.
    // Stamping happens after `result` is fully populated so the digest
    // covers every stable field the import path will re-hash.
    result["sha256"] = sha256_envelope_hex(result);
    return SAO_AI_EDITOR_OK;
}

int32_t WorkflowRegistry::import_workflow(const Json& workflow,
                                          std::string_view scope,
                                          std::string_view plugin_id,
                                          bool overwrite,
                                          const ScopeStore& scopes,
                                          std::string& out_id) {
    if (!workflow.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    WorkflowDefinition definition = WorkflowDefinition::from_json(workflow);
    if (!valid_simple_id(definition.id) || definition.name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Built-in ids are reserved — importing over them would either replace
    // the compile-time definition on next reload or silently promote a user
    // copy to look like a built-in.  Refuse both cases regardless of
    // `overwrite`.
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto existing = workflows_.find(definition.id);
        if (existing != workflows_.end() && existing->second.builtin) {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
        if (existing != workflows_.end() && !overwrite) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    // Drop transient flags so the on-disk payload matches what
    // WorkflowRegistry::save produces.
    definition.builtin = false;
    const int32_t status = save(definition, scopes, scope, plugin_id);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    out_id = definition.id;
    return SAO_AI_EDITOR_OK;
}

int32_t WorkflowRegistry::remove(std::string_view id,
                                 const ScopeStore& scopes,
                                 std::string_view scope,
                                 std::string_view plugin_id) {
    (void)scopes;
    (void)scope;
    (void)plugin_id;
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = workflows_.find(std::string(id));
    if (found == workflows_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.builtin) {
        return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
    }
    workflows_.erase(found);
    return SAO_AI_EDITOR_OK;
}

WorkflowExecution::WorkflowExecution(std::string id,
                                     WorkflowDefinition definition,
                                     Json initial_input)
    : id_(std::move(id)), definition_(std::move(definition)),
      initial_input_(std::move(initial_input)) {
    if (initial_input_.is_string()) {
        variables_["input"] = initial_input_.get<std::string>();
    } else if (initial_input_.is_object()) {
        for (const auto& [key, value] : initial_input_.items()) {
            if (value.is_string()) {
                variables_[key] = value.get<std::string>();
            } else {
                variables_[key] = value.dump();
            }
        }
    }
    if (variables_.find("input") == variables_.end()) {
        variables_["input"] = "";
    }
    status_ = "pending";
}

WorkflowExecution::~WorkflowExecution() {
    request_cancel();
    join();
}

void WorkflowExecution::seed_retry_state(
    size_t step_index,
    std::unordered_map<std::string, std::string> preserved_variables,
    std::string retry_of) {
    // Called from the dispatch thread before the worker is started, so no
    // race with run_loop.  Still take the mutex to keep memory ordering
    // clean and to match the discipline every other mutator observes.
    std::lock_guard<std::mutex> guard(mutex_);
    start_at_step_index_ = step_index;
    retry_of_ = std::move(retry_of);
    if (!preserved_variables.empty()) {
        // Keep the input value the ctor established (falling back to the
        // preserved copy if the caller supplied one).  This matches the
        // R9 semantics documented on the ctor: "input" always ends up
        // populated, even if the payload was empty.
        auto found_input = variables_.find("input");
        std::string input_copy;
        if (found_input != variables_.end()) {
            input_copy = found_input->second;
        }
        variables_ = std::move(preserved_variables);
        if (variables_.find("input") == variables_.end()) {
            variables_["input"] = std::move(input_copy);
        }
    }
    current_step_index_ = start_at_step_index_;
}

void WorkflowExecution::request_pause() noexcept {
    pause_requested_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> guard(mutex_);
    cv_.notify_all();
}

void WorkflowExecution::request_resume(const std::string& human_input)
    noexcept {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pause_requested_.store(false, std::memory_order_release);
        paused_ = false;
        human_resume_input_ = human_input;
        if (!human_input.empty()) {
            variables_["human_input"] = human_input;
        }
    }
    cv_.notify_all();
}

int32_t WorkflowExecution::confirm(std::string_view step_id, bool approved,
                                   const std::string& note) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!confirmation_pending_) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (!step_id.empty() && step_id != confirmation_step_id_) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    confirmation_pending_ = false;
    confirmation_approved_ = approved;
    confirmation_note_ = note;
    cv_.notify_all();
    return SAO_AI_EDITOR_OK;
}

int32_t WorkflowExecution::request_skip(const std::string& reason) {
    // Only honour skip when the worker is parked at a confirmation gate
    // or in a paused state — any other state is either terminal (no step
    // to skip) or actively firing a request (mid-batch skip would race
    // execute_step_with_snapshot).  Both legal states are cooperative
    // suspend points where the worker will observe skip_requested_ on
    // wake and honour it without further ceremony.
    std::lock_guard<std::mutex> guard(mutex_);
    const bool at_confirmation = confirmation_pending_;
    // paused_ is only set inside run_loop under mutex_, so this read is
    // authoritative; the atomic pause_requested_ isn't sufficient because
    // it may be set before the worker actually parks.
    const bool at_pause = paused_;
    if (!at_confirmation && !at_pause) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Also refuse when a terminal state has already been latched — belt
    // and braces in case a caller sneaks a request in the tiny window
    // between run_loop unlatching paused_ and status_ moving off "paused".
    if (status_ == "completed" || status_ == "failed" ||
        status_ == "cancelled") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    skip_requested_.store(true, std::memory_order_release);
    skip_step_index_ = current_step_index_;
    skip_reason_ = reason;
    if (at_confirmation) {
        // Fold skip into the confirmation predicate: mark the gate as no
        // longer pending so cv_.wait wakes, and pass "approved" so
        // rejection isn't recorded — the skip flag is what run_loop keys
        // on to decide "skip" vs "execute".
        confirmation_pending_ = false;
        confirmation_approved_ = true;
        confirmation_note_ = reason;
    }
    if (at_pause) {
        // Wake the paused worker without publishing human_resume_input_
        // — resume input is a distinct signal.  The pause branch will
        // observe skip_requested_ before it re-enters the batch execution
        // path.
        pause_requested_.store(false, std::memory_order_release);
        paused_ = false;
    }
    cv_.notify_all();
    return SAO_AI_EDITOR_OK;
}

int32_t WorkflowExecution::set_variable(std::string_view name,
                                        std::string_view value) {
    if (!valid_simple_id(name)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    // Reject when the run has already reached a terminal state — mutating
    // completed/failed/cancelled variables would either drift the on-disk
    // record from what the worker actually saw or race a still-persisting
    // finalisation write.  Pending is fine (the worker hasn't started yet
    // and the ctor's variable map is authoritative).
    if (status_ == "completed" || status_ == "failed" ||
        status_ == "cancelled") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    variables_[std::string(name)] = std::string(value);
    return SAO_AI_EDITOR_OK;
}

Json WorkflowExecution::variables_snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    Json out = Json::object();
    for (const auto& [key, value] : variables_) {
        out[key] = value;
    }
    return out;
}

void WorkflowExecution::request_cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> guard(mutex_);
    cv_.notify_all();
}

void WorkflowExecution::join() noexcept {
    if (worker_.joinable()) {
        worker_.join();
    }
}

Json WorkflowExecution::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    Json variables = Json::object();
    for (const auto& [key, value] : variables_) {
        variables[key] = value;
    }
    Json step_results = Json::array();
    for (const auto& item : step_results_) {
        step_results.push_back(item);
    }
    Json record{{"id", id_},
                {"workflowId", definition_.id},
                {"status", status_},
                {"currentStep", current_step_index_},
                {"totalSteps", definition_.steps.size()},
                {"paused", paused_},
                {"confirmationPending", confirmation_pending_},
                {"confirmationStepId", confirmation_step_id_},
                {"variables", std::move(variables)},
                {"stepResults", std::move(step_results)},
                {"error", error_message_},
                {"startedAt", started_at_ms_},
                {"completedAt", completed_at_ms_}};
    // Only include retry lineage when this execution was actually seeded
    // from a prior run — otherwise consumers see a stable "no retry"
    // snapshot without a nulled-out field.
    if (!retry_of_.empty()) {
        record["retryOf"] = retry_of_;
    }
    if (start_at_step_index_ != 0) {
        record["startAtStep"] = static_cast<int64_t>(start_at_step_index_);
    }
    return record;
}

Json WorkflowExecution::history_record() const {
    // Reuse snapshot() so shape stays 1:1 with workflows.status; then
    // decorate with the derived display fields the history API surfaces.
    Json record = snapshot();
    record["workflowName"] = definition_.name;
    // snapshot() already reports startedAt/completedAt/status/error, but
    // ensure the schema documented in workflow.list_executions is
    // stable even if the callee did not observe every field.
    if (!record.contains("workflowName")) {
        record["workflowName"] = definition_.name;
    }
    return record;
}

int32_t WorkflowExecution::persist(
    const std::filesystem::path& history_root) const {
    if (history_root.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::error_code error;
    std::filesystem::create_directories(history_root, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    const Json record = history_record();
    // Sanitize the filename to a safe hex-ish id — WorkflowExecution ids
    // are already `wf-<hex>-<counter>` but we defend against future id
    // schemes leaking path separators.
    std::string safe_id;
    safe_id.reserve(id_.size());
    for (const char character : id_) {
        const auto unsigned_character = static_cast<unsigned char>(character);
        const bool safe =
            (unsigned_character >= '0' && unsigned_character <= '9') ||
            (unsigned_character >= 'a' && unsigned_character <= 'z') ||
            (unsigned_character >= 'A' && unsigned_character <= 'Z') ||
            unsigned_character == '-' || unsigned_character == '_';
        safe_id.push_back(safe ? static_cast<char>(unsigned_character) : '_');
    }
    if (safe_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const auto path = history_root / (utf8_to_wide(safe_id) + L".json");
    return write_text_atomic(path, record.dump(2));
}

int32_t WorkflowExecution::enumerate_history(
    const std::filesystem::path& history_root,
    std::string_view workflow_id_filter,
    std::vector<Json>& out_summaries) {
    out_summaries.clear();
    if (history_root.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(history_root, error)) {
        return SAO_AI_EDITOR_OK;
    }
    for (const auto& item :
         std::filesystem::directory_iterator(history_root, error)) {
        if (error) {
            error.clear();
            continue;
        }
        std::error_code file_error;
        if (!item.is_regular_file(file_error) ||
            item.path().extension() != L".json") {
            continue;
        }
        std::string text;
        if (read_text_file(item.path(), kMaximumJsonBytes, text) !=
            SAO_AI_EDITOR_OK) {
            continue;
        }
        Json document = Json::parse(text, nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            continue;
        }
        if (!workflow_id_filter.empty()) {
            const std::string workflow_id =
                document.value("workflowId", std::string{});
            if (workflow_id != workflow_id_filter) {
                continue;
            }
        }
        Json summary{
            {"id", document.value("id", "")},
            {"workflowId", document.value("workflowId", "")},
            {"workflowName", document.value("workflowName", "")},
            {"status", document.value("status", "")},
            {"startedAt", document.value("startedAt", int64_t{0})},
            {"completedAt", document.value("completedAt", int64_t{0})},
            {"totalSteps", document.value("totalSteps", int64_t{0})},
            {"error", document.value("error", "")}};
        out_summaries.push_back(std::move(summary));
    }
    std::sort(out_summaries.begin(), out_summaries.end(),
              [](const Json& left, const Json& right) {
                  return left.value("completedAt", int64_t{0}) >
                         right.value("completedAt", int64_t{0});
              });
    return SAO_AI_EDITOR_OK;
}

int32_t WorkflowExecution::load_history_record(
    const std::filesystem::path& history_root,
    std::string_view execution_id,
    Json& out_record) {
    if (history_root.empty() || execution_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const auto path =
        history_root / (utf8_to_wide(execution_id) + L".json");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    std::string text;
    const int32_t status = read_text_file(path, kMaximumJsonBytes, text);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    out_record = std::move(parsed);
    return SAO_AI_EDITOR_OK;
}

int32_t WorkflowExecution::delete_history_record(
    const std::filesystem::path& history_root,
    std::string_view execution_id) {
    if (history_root.empty() || execution_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const auto path =
        history_root / (utf8_to_wide(execution_id) + L".json");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    std::filesystem::remove(path, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    return SAO_AI_EDITOR_OK;
}

std::string WorkflowExecution::interpolate_with(
    std::string_view text,
    const std::unordered_map<std::string, std::string>& vars) {
    std::string output;
    output.reserve(text.size());
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t open = text.find("{{", cursor);
        if (open == std::string_view::npos) {
            output.append(text.substr(cursor));
            break;
        }
        output.append(text.substr(cursor, open - cursor));
        const size_t close = text.find("}}", open + 2);
        if (close == std::string_view::npos) {
            output.append(text.substr(open));
            break;
        }
        std::string name(text.substr(open + 2, close - open - 2));
        while (!name.empty() && std::isspace(static_cast<unsigned char>(
                                             name.front())) != 0) {
            name.erase(name.begin());
        }
        while (!name.empty() && std::isspace(static_cast<unsigned char>(
                                             name.back())) != 0) {
            name.pop_back();
        }
        const auto found = vars.find(name);
        if (found != vars.end()) {
            output.append(found->second);
        }
        cursor = close + 2;
    }
    return output;
}

std::string WorkflowExecution::interpolate(std::string_view text) const {
    // Caller holds `mutex_`.
    return interpolate_with(text, variables_);
}

WorkflowExecution::ConfirmationOutcome
WorkflowExecution::wait_for_confirmation(const WorkflowStep& step) {
    std::unique_lock<std::mutex> lock(mutex_);
    confirmation_pending_ = true;
    confirmation_step_id_ = step.output_var.empty()
        ? step.label.empty() ? std::string{"step"} : step.label
        : step.output_var;
    confirmation_approved_ = false;
    confirmation_note_.clear();
    status_ = "waiting_confirmation";
    cv_.wait(lock, [&] {
        return !confirmation_pending_ ||
               cancel_requested_.load(std::memory_order_acquire);
    });
    if (cancel_requested_.load(std::memory_order_acquire)) {
        return ConfirmationOutcome::Rejected;
    }
    status_ = "running";
    // Skip wins over approved/rejected: request_skip() sets both the flag
    // and confirmation_approved_=true so the wait predicate exits cleanly.
    // The caller (run_loop) is responsible for consuming skip_requested_
    // before rolling into the next batch, so we do NOT reset it here —
    // the batch handler needs to see the flag to route into the skip
    // branch instead of executing.
    if (skip_requested_.load(std::memory_order_acquire)) {
        return ConfirmationOutcome::Skipped;
    }
    return confirmation_approved_ ? ConfirmationOutcome::Approved
                                   : ConfirmationOutcome::Rejected;
}

int32_t WorkflowExecution::execute_step(NativeRuntime& runtime,
                                        const WorkflowStep& step,
                                        const Json& provider,
                                        const std::string& model,
                                        uint32_t chat_timeout_ms,
                                        std::string& out_content) {
    std::unordered_map<std::string, std::string> snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        snapshot = variables_;
    }
    return execute_step_with_snapshot(runtime, step, snapshot, provider, model,
                                       chat_timeout_ms, out_content);
}

int32_t WorkflowExecution::execute_step_with_snapshot(
    NativeRuntime& runtime,
    const WorkflowStep& step,
    const std::unordered_map<std::string, std::string>& snapshot,
    const Json& provider,
    const std::string& model,
    uint32_t chat_timeout_ms,
    std::string& out_content,
    size_t step_index) {
    const std::string prompt = interpolate_with(step.prompt, snapshot);
    // Streaming is opt-in per workflow invocation via
    // initial_input_["stream"] == true; when set, run_chat_sync will flip
    // stream=true itself because on_delta is non-null.  Non-streaming
    // callers (e.g. legacy fixtures returning full JSON bodies) keep the
    // old blocking semantics with an empty callback that never fires.
    const bool stream_requested =
        initial_input_.is_object() && initial_input_.value("stream", false);
    Json chat_params{{"provider", provider},
                     {"model", model},
                     {"stream", stream_requested},
                     {"timeoutMs", chat_timeout_ms},
                     {"messages",
                      Json::array({Json{{"role", "user"}, {"content", prompt}}})}};
    std::function<void(const Json&)> on_delta;
    if (stream_requested) {
        const std::string execution_id = id_;
        const std::string step_label = step.label;
        const std::string output_var = step.output_var;
        on_delta = [&runtime, execution_id, step_index, step_label, output_var](
                       const Json& event) {
            if (event.value("type", "") != "delta" ||
                !event.contains("content") || !event["content"].is_string()) {
                return;
            }
            Json payload{{"executionId", execution_id},
                         {"stepIndex", static_cast<int64_t>(step_index)},
                         {"label", step_label},
                         {"outputVar", output_var},
                         {"content", event["content"]}};
            runtime.emit_workflow_event(
                "workflow.step_delta", std::move(payload), execution_id);
        };
    }
    return runtime.run_chat_sync(chat_params, chat_timeout_ms, out_content,
                                  std::move(on_delta));
}

void WorkflowExecution::start(NativeRuntime& runtime, Json provider,
                              std::string model, uint32_t chat_timeout_ms) {
    // Legacy overload — persistence disabled.  Kept so unit tests / callers
    // that only need in-memory execution don't have to manufacture a path.
    start(runtime, std::move(provider), std::move(model), chat_timeout_ms,
          std::filesystem::path{});
}

void WorkflowExecution::start(NativeRuntime& runtime, Json provider,
                              std::string model, uint32_t chat_timeout_ms,
                              std::filesystem::path history_root) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        started_at_ms_ = unix_milliseconds();
        status_ = "running";
        history_persist_root_ = std::move(history_root);
    }
    worker_ = std::thread(&WorkflowExecution::run_loop, this, &runtime,
                          std::move(provider), std::move(model),
                          chat_timeout_ms);
}

void WorkflowExecution::run_loop(NativeRuntime* runtime, Json provider,
                                 std::string model, uint32_t chat_timeout_ms) {
    if (runtime == nullptr) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = "failed";
        error_message_ = "runtime unavailable";
        completed_at_ms_ = unix_milliseconds();
        return;
    }
    const auto& steps = definition_.steps;
    const size_t total_steps = steps.size();
    const std::string execution_id = id_;
    // Consumers subscribed to workflow.progress get a coarse-grained view of
    // the run (start-of-step + end-of-step + terminal state).  The finer
    // token-level stream is still workflow.step_delta.
    auto emit_terminal = [&](std::string_view final_status,
                              std::string_view error_text) {
        Json payload{{"executionId", execution_id},
                     {"status", std::string(final_status)},
                     {"totalSteps", static_cast<int64_t>(total_steps)}};
        if (!error_text.empty()) {
            payload["error"] = std::string(error_text);
        }
        runtime->emit_workflow_event("workflow.progress", std::move(payload),
                                      execution_id);
        // Best-effort disk snapshot — snapshot() already reflects the
        // terminal state (status_/completed_at_ms_/error_message_ committed
        // under mutex_ before emit_terminal fires).  Any write failure is
        // swallowed intentionally: a mid-shutdown IO error must not abort
        // the run.  Grab the path under lock, then release it before
        // hitting the disk to avoid stalling snapshot() consumers.
        std::filesystem::path history_root_copy;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            history_root_copy = history_persist_root_;
        }
        if (!history_root_copy.empty()) {
            const int32_t persist_status = persist(history_root_copy);
            if (persist_status != SAO_AI_EDITOR_OK) {
                std::fprintf(
                    stderr,
                    "[ai_editor] workflow persist failed: id=%s status=%d\n",
                    execution_id.c_str(),
                    static_cast<int>(persist_status));
            }
        }
    };
    // Retry runs skip previously-successful steps by starting the loop
    // partway through.  Bounds are enforced by the dispatch layer before
    // this thread is spawned, but clamp defensively so an out-of-range
    // seed still terminates cleanly rather than reading past `steps`.
    size_t index = start_at_step_index_;
    if (index > steps.size()) {
        index = steps.size();
    }
    while (index < steps.size()) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                status_ = "cancelled";
                completed_at_ms_ = unix_milliseconds();
            }
            emit_terminal("cancelled", {});
            return;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            current_step_index_ = index;
        }
        // Extend the batch across contiguous same-`group` steps (empty
        // group → single-step batch).  Compute here (before the pause /
        // confirmation gates) so a skip request while paused knows the
        // exact batch boundary to advance past.
        size_t batch_end = index + 1;
        const std::string& group_id = steps[index].group;
        if (!group_id.empty()) {
            while (batch_end < steps.size() &&
                   steps[batch_end].group == group_id) {
                ++batch_end;
            }
        }
        const size_t batch_size = batch_end - index;
        // Track whether this batch should be skipped without hitting the
        // LLM.  Set by either the pause-side skip check or the
        // wait_for_confirmation outcome; consumed by the batch-execution
        // switch further down.
        bool skip_batch = false;
        std::string skip_reason_captured;
        if (pause_requested_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex_);
            paused_ = true;
            status_ = "paused";
            cv_.wait(lock, [&] {
                return !pause_requested_.load(std::memory_order_acquire) ||
                       cancel_requested_.load(std::memory_order_acquire);
            });
            if (cancel_requested_.load(std::memory_order_acquire)) {
                status_ = "cancelled";
                completed_at_ms_ = unix_milliseconds();
                lock.unlock();
                emit_terminal("cancelled", {});
                return;
            }
            paused_ = false;
            status_ = "running";
            // A skip request while paused only affects the current batch
            // (its step_index was captured at request time; a stale skip
            // for a batch we already moved past is ignored).
            if (skip_requested_.load(std::memory_order_acquire) &&
                skip_step_index_ == index) {
                skip_batch = true;
                skip_reason_captured = skip_reason_;
                skip_requested_.store(false, std::memory_order_release);
                skip_reason_.clear();
            }
        }
        // Human confirmation applies before the batch fires; any step in
        // the batch tagged requires_confirmation gates the whole batch.
        // Once skip_batch is set we short-circuit remaining confirmations
        // (the caller already made the intent clear).
        for (size_t k = 0; k < batch_size && !skip_batch; ++k) {
            const WorkflowStep& step = steps[index + k];
            if (!step.requires_confirmation) {
                continue;
            }
            const ConfirmationOutcome outcome = wait_for_confirmation(step);
            if (outcome == ConfirmationOutcome::Skipped) {
                std::lock_guard<std::mutex> guard(mutex_);
                skip_batch = true;
                skip_reason_captured = skip_reason_;
                skip_requested_.store(false, std::memory_order_release);
                skip_reason_.clear();
                break;
            }
            if (outcome == ConfirmationOutcome::Rejected) {
                std::string terminal_status;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (cancel_requested_.load(std::memory_order_acquire)) {
                        status_ = "cancelled";
                        terminal_status = "cancelled";
                    } else {
                        status_ = "failed";
                        error_message_ = "user rejected step";
                        terminal_status = "failed";
                    }
                    completed_at_ms_ = unix_milliseconds();
                }
                emit_terminal(terminal_status,
                              terminal_status == "failed"
                                  ? "user rejected step"
                                  : "");
                return;
            }
        }

        // Skip branch: record each step in the batch as status=="skipped"
        // with empty content, emit progress events (step_started +
        // step_skipped so subscribers can render a "step ran but empty"
        // marker without special-casing missing completion), and advance
        // past the batch.  No LLM calls, no output_var mutation.
        if (skip_batch) {
            for (size_t k = 0; k < batch_size; ++k) {
                const WorkflowStep& step = steps[index + k];
                Json start_payload{{"executionId", execution_id},
                                    {"stepIndex", static_cast<int64_t>(
                                                       index + k)},
                                    {"totalSteps", static_cast<int64_t>(
                                                       total_steps)},
                                    {"status", "step_started"},
                                    {"label", step.label},
                                    {"agent", step.agent},
                                    {"outputVar", step.output_var},
                                    {"group", step.group}};
                runtime->emit_workflow_event(
                    "workflow.progress", std::move(start_payload),
                    execution_id);
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (size_t k = 0; k < batch_size; ++k) {
                    const WorkflowStep& step = steps[index + k];
                    Json step_result{{"stepIndex", index + k},
                                     {"label", step.label},
                                     {"agent", step.agent},
                                     {"outputVar", step.output_var},
                                     {"group", step.group},
                                     {"status", "skipped"},
                                     {"content", ""}};
                    if (!skip_reason_captured.empty()) {
                        step_result["skipReason"] = skip_reason_captured;
                    }
                    step_results_.push_back(std::move(step_result));
                }
            }
            for (size_t k = 0; k < batch_size; ++k) {
                const WorkflowStep& step = steps[index + k];
                Json payload{{"executionId", execution_id},
                             {"stepIndex", static_cast<int64_t>(index + k)},
                             {"totalSteps", static_cast<int64_t>(total_steps)},
                             {"status", "step_skipped"},
                             {"label", step.label},
                             {"agent", step.agent},
                             {"outputVar", step.output_var},
                             {"group", step.group},
                             {"content", ""}};
                if (!skip_reason_captured.empty()) {
                    payload["skipReason"] = skip_reason_captured;
                }
                runtime->emit_workflow_event(
                    "workflow.progress", std::move(payload), execution_id);
            }
            index = batch_end;
            continue;
        }

        // Emit workflow.progress step_started for every step in the batch
        // (parallel batches: one event per step, all before execution).
        for (size_t k = 0; k < batch_size; ++k) {
            const WorkflowStep& step = steps[index + k];
            Json payload{{"executionId", execution_id},
                         {"stepIndex", static_cast<int64_t>(index + k)},
                         {"totalSteps", static_cast<int64_t>(total_steps)},
                         {"status", "step_started"},
                         {"label", step.label},
                         {"agent", step.agent},
                         {"outputVar", step.output_var},
                         {"group", step.group}};
            runtime->emit_workflow_event("workflow.progress",
                                          std::move(payload), execution_id);
        }

        // Pre-batch snapshot lets parallel steps share a stable variable
        // set; results merge into `variables_` only after the batch joins.
        std::unordered_map<std::string, std::string> snapshot;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            snapshot = variables_;
        }
        std::vector<std::string> contents(batch_size);
        std::vector<int32_t> statuses(batch_size, SAO_AI_EDITOR_OK);
        if (batch_size == 1 || group_id.empty()) {
            const int32_t status = execute_step_with_snapshot(
                *runtime, steps[index], snapshot, provider, model,
                chat_timeout_ms, contents[0], index);
            statuses[0] = status;
        } else {
            std::vector<std::thread> workers;
            workers.reserve(batch_size);
            for (size_t k = 0; k < batch_size; ++k) {
                const size_t step_index = index + k;
                workers.emplace_back([&, k, step_index] {
                    statuses[k] = execute_step_with_snapshot(
                        *runtime, steps[step_index], snapshot, provider,
                        model, chat_timeout_ms, contents[k], step_index);
                });
            }
            for (auto& worker : workers) {
                worker.join();
            }
        }

        // Commit results in step order; the first failure short-circuits.
        bool failed = false;
        std::string failure_message;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (size_t k = 0; k < batch_size; ++k) {
                const WorkflowStep& step = steps[index + k];
                Json step_result{{"stepIndex", index + k},
                                 {"label", step.label},
                                 {"agent", step.agent},
                                 {"outputVar", step.output_var},
                                 {"group", step.group},
                                 {"status", statuses[k] == SAO_AI_EDITOR_OK
                                                ? "completed"
                                                : "failed"},
                                 {"content", contents[k]}};
                if (statuses[k] != SAO_AI_EDITOR_OK) {
                    step_results_.push_back(std::move(step_result));
                    status_ = "failed";
                    error_message_ = group_id.empty()
                        ? "chat step failed"
                        : "group step failed";
                    completed_at_ms_ = unix_milliseconds();
                    failed = true;
                    failure_message = error_message_;
                    break;
                }
                if (!step.output_var.empty()) {
                    variables_[step.output_var] = contents[k];
                }
                step_results_.push_back(std::move(step_result));
            }
        }
        // Now that mutex is released, emit per-step completion events so
        // consumers see the same ordering as step_results_.  Failed steps
        // in a batch still emit their pre-abort completions.
        for (size_t k = 0; k < batch_size; ++k) {
            const WorkflowStep& step = steps[index + k];
            const bool step_ok = statuses[k] == SAO_AI_EDITOR_OK;
            Json payload{{"executionId", execution_id},
                         {"stepIndex", static_cast<int64_t>(index + k)},
                         {"totalSteps", static_cast<int64_t>(total_steps)},
                         {"status", step_ok ? "step_completed"
                                            : "step_failed"},
                         {"label", step.label},
                         {"agent", step.agent},
                         {"outputVar", step.output_var},
                         {"group", step.group},
                         {"content", contents[k]}};
            runtime->emit_workflow_event("workflow.progress",
                                          std::move(payload), execution_id);
            if (!step_ok) {
                // stop at the first failure just like the commit loop
                break;
            }
        }
        if (failed) {
            emit_terminal("failed", failure_message);
            return;
        }
        index = batch_end;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = "completed";
        completed_at_ms_ = unix_milliseconds();
        current_step_index_ = definition_.steps.size();
    }
    emit_terminal("completed", {});
}

}  // namespace sao::ai_editor::native
