#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "scope_store.h"

namespace sao::ai_editor::native {

// Best-effort JSON Schema draft-07 subset validator.  Supports type / properties
// / required / items / enum and skips fields it does not recognise (pattern,
// format, oneOf/anyOf/allOf/not, $ref, additionalProperties, …) so early
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
    NativeToolRegistry(const ScopeStore& scopes,
                       uint32_t maximum_file_bytes,
                       uint32_t maximum_search_results) noexcept;

    Json describe(std::string_view mode) const;
    int32_t execute(std::string_view mode,
                    std::string_view name,
                    const Json& arguments,
                    Json& result) const;

    // Register a caller-defined tool.  The tool is kept only in-memory
    // (per-runtime) — it does not survive a runtime restart.  Names must be
    // non-empty and cannot shadow a built-in (readFile/listFiles/…); collisions
    // fail with SAO_AI_EDITOR_ERR_INVALID_ARGUMENT.  Re-registering the same
    // custom name updates the descriptor.
    int32_t register_custom(std::string_view name,
                            std::string_view description,
                            const Json& parameters,
                            bool read_only);
    // Remove a previously registered custom tool.  Returning NOT_FOUND lets
    // callers distinguish "already gone" from "argument was rubbish".
    int32_t unregister_custom(std::string_view name);

private:
    int32_t read_file(const Json& arguments, Json& result) const;
    int32_t list_files(const Json& arguments, Json& result) const;
    int32_t search_files(const Json& arguments, Json& result) const;
    int32_t edit_file(const Json& arguments, Json& result) const;

    struct CustomTool final {
        std::string description;
        Json parameters;
        bool read_only = true;
    };

    const ScopeStore& scopes_;
    uint32_t maximum_file_bytes_;
    uint32_t maximum_search_results_;
    // describe() / execute() are const on the public surface (matching the
    // built-in "runtime is stateless" contract), so we guard the custom-tool
    // map with a mutex + mark it mutable.  All access happens on the runtime
    // dispatch thread today, but the mutex costs nothing here and future
    // parallel dispatch would otherwise race the map.
    mutable std::mutex custom_mutex_;
    std::unordered_map<std::string, CustomTool> custom_tools_;
};

}  // namespace sao::ai_editor::native
