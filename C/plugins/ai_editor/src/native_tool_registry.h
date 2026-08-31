#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "scope_store.h"

namespace sao::ai_editor::native {

class ToolResultFilterRegistry;

// Best-effort JSON Schema draft-07 subset validator.  Supports type / properties
// / required / items / enum / additionalProperties:false / minimum / maximum /
// minItems / maxItems / minLength / maxLength and skips fields it does not
// recognise (pattern, format, oneOf/anyOf/allOf/not, $ref, …) so early
// adoption never rejects a request the runtime would otherwise accept.  On
// success returns SAO_AI_EDITOR_OK; on failure returns
// SAO_AI_EDITOR_ERR_INVALID_ARGUMENT and populates `errors` with an array of
// `{path, reason}` objects.  `path` uses JSONPath-lite (`$`, `$.field`,
// `$.arr[0]`) so callers can point at the offending argument.
int32_t validate_json_against_schema(const Json& arguments,
                                     const Json& schema,
                                     Json& errors);

class NativeToolRegistry final {
public:
    using CustomExecuteFn =
        int32_t (*)(const Json& arguments, Json& result, void* user);

    using CustomToolOwner = const void*;

    struct CustomToolDescriptor final {
        std::string name;
        std::string description;
        Json parameters;
        bool read_only = true;
        CustomExecuteFn execute_fn = nullptr;
        void* execute_user = nullptr;
        std::shared_ptr<void> execute_lifetime;
        bool explicit_confirmation_required = false;
    };

    NativeToolRegistry(const ScopeStore& scopes,
                       uint32_t maximum_file_bytes,
                       uint32_t maximum_search_results) noexcept;

    // Wire in a filter registry so tools.call output runs through the
    // compression chain before it hits the JSON-RPC wire.  Passed by
    // pointer (may be null) so tests + non-runtime consumers can skip the
    // filter path entirely.  Ownership stays with the caller (the runtime).
    void set_filter_registry(const ToolResultFilterRegistry* registry) noexcept;

    Json describe(std::string_view mode) const;
    int32_t execute(std::string_view mode,
                    std::string_view name,
                    const Json& arguments,
                    Json& result) const;

    // Register a caller-defined tool.  The tool is kept only in-memory
    // (per-runtime) — it does not survive a runtime restart.  Names must be
    // non-empty and cannot shadow a built-in (readFile/listFiles/…); collisions
    // fail with SAO_AI_EDITOR_ERR_INVALID_ARGUMENT.  Re-registering the same
    // custom name updates the descriptor. `execute_fn` is optional; when it
    // is null execute() preserves the historical passthrough response. The
    // callback user pointer is non-owning and must outlive the registration.
    int32_t register_custom(std::string_view name,
                            std::string_view description,
                            const Json& parameters,
                            bool read_only,
                            CustomExecuteFn execute_fn = nullptr,
                            void* execute_user = nullptr,
                            bool explicit_confirmation_required = false);

    int32_t upsert_custom_batch(
        const std::vector<CustomToolDescriptor>& descriptors,
        CustomToolOwner owner);
    int32_t remove_custom_batch(const std::vector<std::string>& names,
                                CustomToolOwner owner);

    // Remove a previously registered custom tool.  Returning NOT_FOUND lets
    // callers distinguish "already gone" from "argument was rubbish".
    int32_t unregister_custom(std::string_view name);

    // Register an alias so `tools.call <alias>` dispatches to the same handler
    // as `tools.call <target>`.  Both built-in and custom targets are legal;
    // the target must already exist at registration time.  Aliases share the
    // same namespace as tools (built-in / custom / other aliases) — any
    // collision with an existing name is rejected with INVALID_ARGUMENT so the
    // caller can distinguish that from NOT_FOUND (unknown target).  Re-runs
    // with the same alias name are treated as collisions too; the caller must
    // unregister first if they want to rebind.
    int32_t register_alias(std::string_view alias, std::string_view target);
    int32_t unregister_alias(std::string_view alias);

    // Resolve an alias chain (single hop today, but the loop future-proofs
    // us if we ever allow alias-of-alias).  Returns the canonical tool name
    // or the input unchanged when no alias matches.  Only used by the
    // dispatch layer + tests; safe to call under the runtime's store_mutex_
    // because it reads a copy under the internal custom_mutex_.
    std::string resolve_alias(std::string_view name) const;

    // Hook management.  Hooks are keyed by caller-defined id so multiple
    // hooks per phase are allowed (audit-log, telemetry, ...).  `phase` must
    // be one of "before" / "after" / "error"; `tool_filter` is an optional
    // whitelist (empty = match every tool including aliases post-resolution);
    // `emit_event` is the sao.event name used when the hook fires.
    // Re-registering the same id updates the descriptor in place, which lets
    // the UI switch a hook's phase without racing an unregister.
    int32_t register_hook(std::string_view id,
                          std::string_view phase,
                          const std::vector<std::string>& tool_filter,
                          std::string_view emit_event);
    int32_t unregister_hook(std::string_view id);

    struct HookFire final {
        std::string id;
        std::string emit_event;
    };
    // Snapshot the hooks matching (phase, resolved_tool_name).  Returned by
    // value so the dispatch layer can iterate + emit events without holding
    // the internal mutex — hooks may transitively call back into the runtime
    // and grabbing the mutex again would deadlock.
    std::vector<HookFire> snapshot_hooks(std::string_view phase,
                                         std::string_view tool_name) const;

private:
    int32_t read_file(const Json& arguments, Json& result) const;
    int32_t list_files(const Json& arguments, Json& result) const;
    int32_t search_files(const Json& arguments, Json& result) const;
    int32_t edit_file(const Json& arguments, Json& result) const;

    // Non-locking helpers used by both register_alias() and the constructor
    // for the built-in name check.  Kept close to the built-in dispatch list
    // in .cpp so the two stay in sync.
    static bool is_builtin_name(std::string_view name) noexcept;

    struct CustomTool final {
        std::string description;
        Json parameters;
        bool read_only = true;
        CustomExecuteFn execute_fn = nullptr;
        void* execute_user = nullptr;
        std::shared_ptr<void> execute_lifetime;
        bool explicit_confirmation_required = false;
        CustomToolOwner owner = nullptr;
    };

    struct AliasEntry final {
        std::string target;
    };

    struct Hook final {
        std::string id;
        std::string phase;                    // "before" / "after" / "error"
        std::vector<std::string> tool_filter; // empty = match all
        std::string emit_event;
    };

    const ScopeStore& scopes_;
    uint32_t maximum_file_bytes_;
    uint32_t maximum_search_results_;
    // describe() / execute() are const on the public surface (matching the
    // built-in "runtime is stateless" contract), so we guard the custom-tool
    // map with a mutex + mark it mutable.  All access happens on the runtime
    // dispatch thread today, but the mutex costs nothing here and future
    // parallel dispatch would otherwise race the map.
    // The aliases_ and hooks_ maps share the same mutex — they mutate together
    // with custom_tools_ (register_alias() checks collisions against the
    // custom map) so a single guard keeps register/unregister atomic.
    mutable std::mutex custom_mutex_;
    std::unordered_map<std::string, CustomTool> custom_tools_;
    std::unordered_map<std::string, AliasEntry> aliases_;
    std::unordered_map<std::string, Hook> hooks_;
    // Non-owning — the NativeRuntime keeps the concrete registry alive for the
    // lifetime of the tool registry.  Guarded by `custom_mutex_` so a runtime
    // that mutates it (currently only at construction) never races execute().
    const ToolResultFilterRegistry* filter_registry_ = nullptr;
};

}  // namespace sao::ai_editor::native
