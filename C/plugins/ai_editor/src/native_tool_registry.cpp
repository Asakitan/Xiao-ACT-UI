#include "native_tool_registry.h"

#include "gpu_hunt_bridge.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>

#include "tool_result_filter.h"

namespace sao::ai_editor::native {
namespace {

// True iff the runtime value satisfies a JSON-schema `type` keyword.  The
// schema draft treats `integer` as a distinct type — an accidental `5.5` for a
// `type: "integer"` field is rejected here so the tool handler can rely on
// integer-shaped `startLine` / `endLine` args.
bool value_matches_type(const Json& value, std::string_view type) {
    if (type == "string") {
        return value.is_string();
    }
    if (type == "boolean") {
        return value.is_boolean();
    }
    if (type == "integer") {
        // JSON number that fits an integer.  nlohmann's is_number_integer()
        // accepts negative + unsigned; is_number_float() carves off floats.
        return value.is_number_integer();
    }
    if (type == "number") {
        return value.is_number();
    }
    if (type == "object") {
        return value.is_object();
    }
    if (type == "array") {
        return value.is_array();
    }
    if (type == "null") {
        return value.is_null();
    }
    // Unknown / unsupported type keyword — treat as pass so schemas produced
    // by third parties (e.g. draft-04 leftovers) do not break dispatch.
    return true;
}

std::string type_name(const Json& value) {
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number_float()) return "number";
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_null()) return "null";
    return "unknown";
}

void append_error(Json& errors, std::string_view path, std::string_view reason) {
    errors.push_back(Json{{"path", std::string(path)},
                          {"reason", std::string(reason)}});
}

enum class NumericOrder : int8_t {
    less = -1,
    equal = 0,
    greater = 1,
};

NumericOrder invert_numeric_order(NumericOrder order) noexcept {
    if (order == NumericOrder::less) return NumericOrder::greater;
    if (order == NumericOrder::greater) return NumericOrder::less;
    return NumericOrder::equal;
}

NumericOrder compare_unsigned_to_double(uint64_t value,
                                        double limit) noexcept {
    if (std::isnan(limit)) return NumericOrder::equal;
    if (limit < 0.0) return NumericOrder::greater;
    constexpr double kTwoTo64 = 18446744073709551616.0;
    if (limit >= kTwoTo64) return NumericOrder::less;
    const double integral = std::floor(limit);
    const uint64_t integral_value = static_cast<uint64_t>(integral);
    if (value < integral_value) return NumericOrder::less;
    if (value > integral_value) return NumericOrder::greater;
    return integral == limit ? NumericOrder::equal : NumericOrder::less;
}

NumericOrder compare_signed_to_double(int64_t value,
                                      double limit) noexcept {
    if (std::isnan(limit)) return NumericOrder::equal;
    if (value >= 0) {
        return compare_unsigned_to_double(static_cast<uint64_t>(value), limit);
    }
    if (limit >= 0.0) return NumericOrder::less;
    const uint64_t magnitude = 0u - static_cast<uint64_t>(value);
    return invert_numeric_order(
        compare_unsigned_to_double(magnitude, -limit));
}

NumericOrder compare_numbers(const Json& left, const Json& right) noexcept {
    if (left.is_number_unsigned()) {
        const uint64_t value = left.get<uint64_t>();
        if (right.is_number_unsigned()) {
            const uint64_t other = right.get<uint64_t>();
            return value < other ? NumericOrder::less
                                 : value > other ? NumericOrder::greater
                                                 : NumericOrder::equal;
        }
        if (right.is_number_integer()) {
            const int64_t other = right.get<int64_t>();
            if (other < 0) return NumericOrder::greater;
            const uint64_t converted = static_cast<uint64_t>(other);
            return value < converted ? NumericOrder::less
                                     : value > converted ? NumericOrder::greater
                                                         : NumericOrder::equal;
        }
        return compare_unsigned_to_double(value, right.get<double>());
    }
    if (left.is_number_integer()) {
        const int64_t value = left.get<int64_t>();
        if (right.is_number_unsigned()) {
            if (value < 0) return NumericOrder::less;
            const uint64_t converted = static_cast<uint64_t>(value);
            const uint64_t other = right.get<uint64_t>();
            return converted < other ? NumericOrder::less
                                     : converted > other ? NumericOrder::greater
                                                         : NumericOrder::equal;
        }
        if (right.is_number_integer()) {
            const int64_t other = right.get<int64_t>();
            return value < other ? NumericOrder::less
                                 : value > other ? NumericOrder::greater
                                                 : NumericOrder::equal;
        }
        return compare_signed_to_double(value, right.get<double>());
    }
    const double value = left.get<double>();
    if (right.is_number_unsigned()) {
        return invert_numeric_order(
            compare_unsigned_to_double(right.get<uint64_t>(), value));
    }
    if (right.is_number_integer()) {
        return invert_numeric_order(
            compare_signed_to_double(right.get<int64_t>(), value));
    }
    const double other = right.get<double>();
    if (value < other) return NumericOrder::less;
    if (value > other) return NumericOrder::greater;
    return NumericOrder::equal;
}

bool parse_schema_size_limit(const Json& schema, std::string_view key,
                            const std::string& path, Json& errors,
                            uint64_t& output) {
    const Json& value = schema[std::string(key)];
    if (!value.is_number_integer()) {
        append_error(errors, path,
                     "schema " + std::string(key) +
                         " must be a non-negative integer");
        return false;
    }
    if (value.is_number_unsigned()) {
        output = value.get<uint64_t>();
    } else {
        const int64_t signed_value = value.get<int64_t>();
        if (signed_value < 0) {
            append_error(errors, path,
                         "schema " + std::string(key) +
                             " must be a non-negative integer");
            return false;
        }
        output = static_cast<uint64_t>(signed_value);
    }
    if (output > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        append_error(errors, path,
                     "schema " + std::string(key) +
                         " exceeds the platform size limit");
        return false;
    }
    return true;
}

void validate_recursive(const Json& value,
                        const Json& schema,
                        const std::string& path,
                        Json& errors) {
    if (!schema.is_object()) {
        // Non-object schema (e.g. `true` / `false`) — best-effort skip so we do
        // not falsely reject callers that hand us a permissive schema stub.
        return;
    }

    uint64_t minimum_items = 0u;
    uint64_t maximum_items = 0u;
    uint64_t minimum_length = 0u;
    uint64_t maximum_length = 0u;
    const bool valid_minimum_items =
        !schema.contains("minItems") ||
        parse_schema_size_limit(schema, "minItems", path, errors,
                                minimum_items);
    const bool valid_maximum_items =
        !schema.contains("maxItems") ||
        parse_schema_size_limit(schema, "maxItems", path, errors,
                                maximum_items);
    const bool valid_minimum_length =
        !schema.contains("minLength") ||
        parse_schema_size_limit(schema, "minLength", path, errors,
                                minimum_length);
    const bool valid_maximum_length =
        !schema.contains("maxLength") ||
        parse_schema_size_limit(schema, "maxLength", path, errors,
                                maximum_length);

    // `type` may be a single string or an array of strings ("value must match
    // at least one of these").  Draft-07 permits both forms; treat other
    // shapes as a skip.
    if (schema.contains("type")) {
        const auto& type_field = schema["type"];
        if (type_field.is_string()) {
            const std::string expected = type_field.get<std::string>();
            if (!value_matches_type(value, expected)) {
                append_error(errors, path,
                             "type expected " + expected + ", got " +
                                 type_name(value));
                // Continue to surface additional issues; but on hard type
                // mismatch further per-field checks are meaningless.
                return;
            }
        } else if (type_field.is_array()) {
            bool matched = false;
            std::string joined;
            for (const auto& entry : type_field) {
                if (!entry.is_string()) continue;
                if (!joined.empty()) joined += "|";
                joined += entry.get<std::string>();
                if (value_matches_type(value, entry.get<std::string>())) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                append_error(errors, path,
                             "type expected " + joined + ", got " +
                                 type_name(value));
                return;
            }
        }
    }

    // `enum` — numeric equality uses the same lossless cross-type comparator
    // as minimum/maximum; other JSON types retain exact structural equality.
    if (schema.contains("enum") && schema["enum"].is_array()) {
        const auto& allowed = schema["enum"];
        bool matched = false;
        for (const auto& candidate : allowed) {
            const bool numeric_match = candidate.is_number() && value.is_number() &&
                (!candidate.is_number_float() ||
                 std::isfinite(candidate.get<double>())) &&
                (!value.is_number_float() || std::isfinite(value.get<double>())) &&
                compare_numbers(candidate, value) == NumericOrder::equal;
            if (numeric_match ||
                (!candidate.is_number() && !value.is_number() && candidate == value)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            append_error(errors, path, "value not in enum");
        }
    }

    if (value.is_number() && schema.contains("minimum") &&
        schema["minimum"].is_number()) {
        if (compare_numbers(value, schema["minimum"]) == NumericOrder::less) {
            append_error(errors, path, "number below minimum");
        }
    }
    if (value.is_number() && schema.contains("maximum") &&
        schema["maximum"].is_number()) {
        if (compare_numbers(value, schema["maximum"]) == NumericOrder::greater) {
            append_error(errors, path, "number above maximum");
        }
    }
    if (schema.contains("minItems")) {
        if (valid_minimum_items &&
            value.is_array() &&
            static_cast<uint64_t>(value.size()) < minimum_items) {
            append_error(errors, path, "array shorter than minItems");
        }
    }
    if (schema.contains("maxItems")) {
        if (valid_maximum_items &&
            value.is_array() &&
            static_cast<uint64_t>(value.size()) > maximum_items) {
            append_error(errors, path, "array longer than maxItems");
        }
    }
    if (schema.contains("minLength")) {
        if (valid_minimum_length &&
            value.is_string() &&
            static_cast<uint64_t>(value.get_ref<const std::string&>().size()) <
                minimum_length) {
            append_error(errors, path, "string shorter than minLength");
        }
    }
    if (schema.contains("maxLength")) {
        if (valid_maximum_length &&
            value.is_string() &&
            static_cast<uint64_t>(value.get_ref<const std::string&>().size()) >
                maximum_length) {
            append_error(errors, path, "string longer than maxLength");
        }
    }

    // `required` — objects only.  Missing fields are flagged with their child
    // path so callers see `$.path` rather than the parent's path.
    if (value.is_object() && schema.contains("required") &&
        schema["required"].is_array()) {
        for (const auto& field : schema["required"]) {
            if (!field.is_string()) continue;
            const std::string name = field.get<std::string>();
            if (!value.contains(name)) {
                const std::string child_path =
                    path + (path == "$" ? "." : ".") + name;
                append_error(errors, child_path, "missing required field");
            }
        }
    }

    if (value.is_object() && schema.contains("additionalProperties") &&
        schema["additionalProperties"].is_boolean() &&
        !schema["additionalProperties"].get<bool>()) {
        const Json properties = schema.value("properties", Json::object());
        for (const auto& entry : value.items()) {
            if (!properties.is_object() || !properties.contains(entry.key())) {
                const std::string child_path =
                    path + (path == "$" ? "." : ".") + entry.key();
                append_error(errors, child_path, "additional property not allowed");
            }
        }
    }

    // `properties` — recurse for each field present in the value.
    if (value.is_object() && schema.contains("properties") &&
        schema["properties"].is_object()) {
        for (auto entry = schema["properties"].begin();
             entry != schema["properties"].end(); ++entry) {
            const std::string& field = entry.key();
            if (!value.contains(field)) continue;
            const std::string child_path =
                path + (path == "$" ? "." : ".") + field;
            validate_recursive(value[field], entry.value(), child_path, errors);
        }
    }

    // `items` — arrays only.  Draft-07 also allows an array of per-index
    // schemas; support the common "single schema" form and skip the tuple
    // form (best-effort).
    if (value.is_array() && schema.contains("items")) {
        const auto& items_schema = schema["items"];
        if (items_schema.is_object()) {
            for (size_t index = 0; index < value.size(); ++index) {
                const std::string child_path =
                    path + "[" + std::to_string(index) + "]";
                validate_recursive(value[index], items_schema, child_path,
                                   errors);
            }
        }
    }
}

Json tool_descriptor(std::string_view name,
                     std::string_view description,
                     bool read_only,
                     const Json& properties,
                     const Json& required = Json::array()) {
    Json parameters{{"type", "object"}, {"properties", properties}};
    if (!required.empty()) {
        parameters["required"] = required;
    }
    return Json{{"name", name},
                {"description", description},
                {"readOnly", read_only},
                {"parameters", std::move(parameters)}};
}

// Schema for a built-in tool.  Kept in sync with `describe()` — both share a
// single source (`kBuiltinSchemas`) so a stray "readFile no longer requires
// path" change would need to touch this table.
Json builtin_schema_for(std::string_view name) {
    if (name == "readFile") {
        return Json{{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}},
                                    {"startLine", {{"type", "integer"}}},
                                    {"endLine", {{"type", "integer"}}}}},
                    {"required", Json::array({"path"})}};
    }
    if (name == "listFiles") {
        return Json{{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}},
                                    {"pattern", {{"type", "string"}}},
                                    {"recursive", {{"type", "boolean"}}},
                                    {"limit", {{"type", "integer"}}}}}};
    }
    if (name == "searchFiles") {
        return Json{{"type", "object"},
                    {"properties", {{"query", {{"type", "string"}}},
                                    {"path", {{"type", "string"}}},
                                    {"pattern", {{"type", "string"}}},
                                    {"regex", {{"type", "boolean"}}},
                                    {"caseSensitive", {{"type", "boolean"}}},
                                    {"limit", {{"type", "integer"}}}}},
                    {"required", Json::array({"query"})}};
    }
    if (name == "editFile") {
        return Json{{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}},
                                    {"content", {{"type", "string"}}},
                                    {"startLine", {{"type", "integer"}}},
                                    {"endLine", {{"type", "integer"}}},
                                    {"confirmed", {{"type", "boolean"}}}}},
                    {"required", Json::array({"path", "content"})}};
    }
    return Json{};
}

bool wildcard_match(std::wstring_view text, std::wstring_view pattern) {
    size_t text_index = 0;
    size_t pattern_index = 0;
    size_t star = std::wstring_view::npos;
    size_t retry = 0;
    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == L'?' ||
             std::towlower(pattern[pattern_index]) ==
                 std::towlower(text[text_index]))) {
            ++text_index;
            ++pattern_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == L'*') {
            star = pattern_index++;
            retry = text_index;
        } else if (star != std::wstring_view::npos) {
            pattern_index = star + 1;
            text_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

std::string relative_utf8(const std::filesystem::path& path,
                          const std::filesystem::path& root) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return wide_to_utf8((error ? path : relative).generic_wstring());
}

}  // namespace

int32_t validate_json_against_schema(const Json& arguments,
                                     const Json& schema,
                                     Json& errors) {
    errors = Json::array();
    // A missing / non-object schema is intentionally treated as "no
    // constraints" so tools registered without a parameters schema still
    // dispatch.  Only object-shaped schemas participate in validation.
    if (!schema.is_object() || schema.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    validate_recursive(arguments, schema, "$", errors);
    if (!errors.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return SAO_AI_EDITOR_OK;
}

NativeToolRegistry::NativeToolRegistry(const ScopeStore& scopes,
                                       uint32_t maximum_file_bytes,
                                       uint32_t maximum_search_results) noexcept
    : scopes_(scopes),
      maximum_file_bytes_(maximum_file_bytes == 0
                              ? kDefaultMaximumFileBytes
                              : maximum_file_bytes),
      maximum_search_results_(maximum_search_results == 0
                                  ? kDefaultSearchResults
                                  : maximum_search_results) {}

Json NativeToolRegistry::describe(std::string_view mode) const {
    Json tools = Json::array({
        tool_descriptor(
            "readFile", "Read a UTF-8 workspace file", true,
            {{"path", {{"type", "string"}}},
             {"startLine", {{"type", "integer"}}},
             {"endLine", {{"type", "integer"}}}},
            {"path"}),
        tool_descriptor(
            "listFiles", "List workspace files and directories", true,
            {{"path", {{"type", "string"}}},
             {"pattern", {{"type", "string"}}},
             {"recursive", {{"type", "boolean"}}},
             {"limit", {{"type", "integer"}}}}),
        tool_descriptor(
            "searchFiles", "Search UTF-8 workspace files", true,
            {{"query", {{"type", "string"}}},
             {"path", {{"type", "string"}}},
             {"pattern", {{"type", "string"}}},
             {"regex", {{"type", "boolean"}}},
             {"caseSensitive", {{"type", "boolean"}}},
             {"limit", {{"type", "integer"}}}},
            {"query"}),
        tool_descriptor(
            "editFile", "Create or replace a UTF-8 workspace file", false,
            {{"path", {{"type", "string"}}},
             {"content", {{"type", "string"}}},
             {"startLine", {{"type", "integer"}}},
             {"endLine", {{"type", "integer"}}},
             {"confirmed", {{"type", "boolean"}}}},
            {"path", "content"}),
    });
    // gpuHunt.* tools are wired in through a dedicated bridge so this
    // registry file does not need to include gpu_hunt / rt_io headers.
    // They live in the same descriptor list, so the LLM and describe()
    // consumers see them alongside the file tools.
    append_gpu_hunt_tool_descriptors(tools);
    // Append caller-registered tools + aliases.  Snapshot under lock so a
    // concurrent register / unregister cannot mutate the maps while we build
    // descriptors, then release the lock before finalising `permission`
    // (independent of the registry state).  Aliases inherit their target's
    // readOnly + description so LLM tool-choice sees consistent metadata for
    // both names; the extra `aliasOf` field lets debug UIs surface the link.
    std::vector<Json> custom_snapshot;
    std::vector<Json> alias_snapshot;
    {
        std::lock_guard<std::mutex> lock(custom_mutex_);
        custom_snapshot.reserve(custom_tools_.size());
        for (const auto& entry : custom_tools_) {
            Json descriptor{{"name", entry.first},
                            {"description", entry.second.description},
                            {"readOnly", entry.second.read_only},
                            {"parameters", entry.second.parameters},
                            {"custom", true}};
            if (entry.second.explicit_confirmation_required) {
                descriptor["explicitConfirmationRequired"] = true;
            }
            custom_snapshot.push_back(std::move(descriptor));
        }
        alias_snapshot.reserve(aliases_.size());
        for (const auto& entry : aliases_) {
            const std::string& alias_name = entry.first;
            const std::string& target_name = entry.second.target;
            // Inherit the target's descriptor so tool selectors + schema
            // validators treat the alias identically to the canonical name.
            Json descriptor{{"name", alias_name},
                            {"aliasOf", target_name}};
            if (is_builtin_name(target_name)) {
                descriptor["description"] =
                    "Alias of " + target_name;
                descriptor["readOnly"] = target_name != "editFile";
                descriptor["parameters"] = builtin_schema_for(target_name);
            } else {
                const auto found = custom_tools_.find(target_name);
                if (found != custom_tools_.end()) {
                    descriptor["description"] =
                        "Alias of " + target_name;
                    descriptor["readOnly"] = found->second.read_only;
                    descriptor["parameters"] = found->second.parameters;
                    descriptor["custom"] = true;
                    if (found->second.explicit_confirmation_required) {
                        descriptor["explicitConfirmationRequired"] = true;
                    }
                }
                // A dangling alias whose target was unregistered simply
                // surfaces name + aliasOf; execute() will return NOT_FOUND
                // on any call, which the UI can render as a stale entry.
            }
            alias_snapshot.push_back(std::move(descriptor));
        }
    }
    for (auto& descriptor : custom_snapshot) {
        tools.push_back(std::move(descriptor));
    }
    for (auto& descriptor : alias_snapshot) {
        tools.push_back(std::move(descriptor));
    }
    for (auto& tool : tools) {
        const bool mutating = !tool.value("readOnly", false);
        std::string permission = "allowed";
        if (mutating && mode == "ask") {
            permission = "disabled";
        } else if (mutating && mode == "plan") {
            permission = "confirm";
        }
        tool["permission"] = permission;
    }
    return tools;
}

int32_t NativeToolRegistry::execute(std::string_view mode,
                                    std::string_view name,
                                    const Json& arguments,
                                    Json& result) const {
    if (!arguments.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Resolve the schema for whichever tool this is (built-in / custom /
    // alias).  We grab a snapshot of the custom-tool + alias state while
    // holding the mutex so a concurrent unregister does not race the
    // schema-vs-handler path.  Aliases are resolved up-front to a canonical
    // name so the rest of this function operates on the target.
    std::string resolved_name(name);
    Json schema;
    bool is_custom = false;
    bool custom_read_only = true;
    CustomExecuteFn custom_execute_fn = nullptr;
    void* custom_execute_user = nullptr;
    std::shared_ptr<void> custom_execute_lifetime;
    bool found_tool = false;
    bool is_gpu_hunt = false;
    if (is_builtin_name(resolved_name)) {
        schema = builtin_schema_for(resolved_name);
        found_tool = true;
    } else if (is_gpu_hunt_tool_name(resolved_name)) {
        schema = gpu_hunt_tool_schema(resolved_name);
        is_gpu_hunt = true;
        found_tool = true;
    } else {
        std::lock_guard<std::mutex> lock(custom_mutex_);
        // Alias hop: check aliases_ first so an alias whose target became a
        // built-in name still routes correctly.  Single-hop is enforced —
        // register_alias() already rejects target-is-alias so this cannot
        // loop, but we cap at one hop defensively.
        const auto alias_found = aliases_.find(resolved_name);
        if (alias_found != aliases_.end()) {
            resolved_name = alias_found->second.target;
        }
        if (is_builtin_name(resolved_name)) {
            schema = builtin_schema_for(resolved_name);
            found_tool = true;
        } else {
            const auto found = custom_tools_.find(resolved_name);
            if (found != custom_tools_.end()) {
                schema = found->second.parameters;
                custom_read_only = found->second.read_only;
                custom_execute_fn = found->second.execute_fn;
                custom_execute_user = found->second.execute_user;
                custom_execute_lifetime = found->second.execute_lifetime;
                is_custom = true;
                found_tool = true;
            }
        }
    }
    if (!found_tool) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    // From this point downstream handlers see the canonical target name.
    const std::string_view name_v = resolved_name;
    name = name_v;
    // JSON-schema pre-flight: run before any handler-specific `path` /
    // `content` checks so callers get a structured `validationErrors` list
    // instead of a single generic "invalid argument".  Best-effort — an empty
    // schema silently passes.
    {
        Json validation_errors;
        const int32_t validation_status =
            validate_json_against_schema(arguments, schema, validation_errors);
        if (validation_status != SAO_AI_EDITOR_OK) {
            result = Json{{"validationErrors", std::move(validation_errors)}};
            return validation_status;
        }
    }
    // Dispatch to the concrete handler.  Captured into a status variable so
    // we can run the filter chain over `result` on the OK path before
    // returning to the caller (JSON-RPC error paths bypass compression by
    // design — a validation-error payload is already tiny and the LLM needs
    // to see the raw structure to correct its next call).
    int32_t dispatch_status = SAO_AI_EDITOR_ERR_NOT_FOUND;
    if (name == "readFile") {
        dispatch_status = read_file(arguments, result);
    } else if (name == "listFiles") {
        dispatch_status = list_files(arguments, result);
    } else if (name == "searchFiles") {
        dispatch_status = search_files(arguments, result);
    } else if (name == "editFile") {
        if (mode == "ask") {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
        if (mode == "plan" && !arguments.value("confirmed", false)) {
            result = Json{{"confirmationRequired", true},
                          {"tool", "editFile"}};
            return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
        }
        dispatch_status = edit_file(arguments, result);
    } else if (is_gpu_hunt) {
        // gpu_hunt tools observe / drive a live tracker; they never touch
        // the workspace or spawn processes, so ask/plan gating does not
        // apply.  Errors on missing state (not locked, no attach) are
        // surfaced as `ok:false` responses rather than JSON-RPC errors.
        dispatch_status = dispatch_gpu_hunt_tool(name, arguments, result);
    } else if (is_custom) {
        // Custom tools share the built-in mutating-tool permission gates. A
        // native registration may provide a direct callback; callback-less
        // tools preserve the historical passthrough response used by the
        // public tools.register API.
        if (!custom_read_only && mode == "ask") {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
        if (!custom_read_only && mode == "plan" &&
            !arguments.value("confirmed", false)) {
            result = Json{{"confirmationRequired", true},
                          {"tool", std::string(name)},
                          {"custom", true}};
            return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
        }
        if (custom_execute_fn != nullptr) {
            dispatch_status =
                custom_execute_fn(arguments, result, custom_execute_user);
        } else {
            result = Json{{"custom", true},
                          {"name", std::string(name)},
                          {"arguments", arguments}};
            dispatch_status = SAO_AI_EDITOR_OK;
        }
    } else {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    // Filter chain: apply on OK results only.  A filter that fires records
    // its id into compressionInfo so downstream telemetry can attribute
    // savings + the LLM can opt-out via arguments in a follow-up call.
    // The filter registry is a non-owning pointer (see set_filter_registry);
    // a null registry is the "no compression" happy path used by unit tests
    // that instantiate NativeToolRegistry directly.
    if (dispatch_status == SAO_AI_EDITOR_OK && filter_registry_ != nullptr) {
        std::vector<std::string> fired;
        const bool any_fired = filter_registry_->apply_filters(
            std::string(name), arguments, result, fired);
        if (any_fired && result.is_object()) {
            // Filters that touch a single field (e.g. ReadFileTruncate) already
            // populate compressionInfo themselves; only wrap the top-level
            // "which filters ran" metadata when the field is not already set,
            // so multi-filter runs still report the full chain.
            if (!result.contains("compressionInfo") ||
                !result["compressionInfo"].is_object()) {
                result["compressionInfo"] = Json{{"filterIds", fired}};
            } else {
                // Merge fired ids into existing compressionInfo.filterIds so
                // the final list is a union of what every filter reported.
                Json& info = result["compressionInfo"];
                Json existing = info.value("filterIds", Json::array());
                if (!existing.is_array()) {
                    existing = Json::array();
                }
                for (const auto& fid : fired) {
                    bool present = false;
                    for (const auto& entry : existing) {
                        if (entry.is_string() && entry.get<std::string>() == fid) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) {
                        existing.push_back(fid);
                    }
                }
                info["filterIds"] = std::move(existing);
            }
        }
    }
    return dispatch_status;
}

void NativeToolRegistry::set_filter_registry(
    const ToolResultFilterRegistry* registry) noexcept {
    // The registry is read on every execute() call.  We do not take the
    // custom_mutex_ here because the runtime installs the pointer exactly
    // once during construction (before any dispatch thread touches the
    // registry); using memory_order semantics via a plain assignment matches
    // that lifecycle and keeps execute() lock-free.
    filter_registry_ = registry;
}

bool NativeToolRegistry::is_builtin_name(std::string_view name) noexcept {
    return name == "readFile" || name == "listFiles" ||
           name == "searchFiles" || name == "editFile";
}

int32_t NativeToolRegistry::register_custom(std::string_view name,
                                            std::string_view description,
                                            const Json& parameters,
                                            bool read_only,
                                            CustomExecuteFn execute_fn,
                                            void* execute_user,
                                            bool explicit_confirmation_required) {
    if (name.empty() || !valid_utf8(name)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Shadowing a built-in would produce two entries in describe() and confuse
    // execute() dispatch (built-ins always win).  Reject early so callers get
    // an actionable error rather than a silently-hidden custom tool.
    if (is_builtin_name(name) || is_gpu_hunt_tool_name(name)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // `parameters` is optional — default to an empty object schema so
    // describe() always emits a valid JSON-schema-ish descriptor.  When the
    // caller supplies parameters, only accept an object; a stray array/string
    // would confuse downstream OpenAI-tool converters.
    Json schema;
    if (parameters.is_null()) {
        schema = Json{{"type", "object"}, {"properties", Json::object()}};
    } else if (parameters.is_object()) {
        schema = parameters;
    } else {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    CustomTool tool;
    tool.description = std::string(description);
    tool.parameters = std::move(schema);
    tool.read_only = read_only;
    tool.execute_fn = execute_fn;
    tool.execute_user = execute_user;
    tool.execute_lifetime = {};
    tool.explicit_confirmation_required = explicit_confirmation_required;
    tool.owner = nullptr;
    std::lock_guard<std::mutex> lock(custom_mutex_);
    // Reject a custom-tool registration whose name collides with an existing
    // alias.  Re-registering an existing custom name still updates the
    // descriptor in place (that's the documented "update" contract) — only
    // the alias collision is new.
    if (aliases_.find(std::string(name)) != aliases_.end()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const auto existing = custom_tools_.find(std::string(name));
    if (existing != custom_tools_.end() && existing->second.owner != nullptr) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    custom_tools_[std::string(name)] = std::move(tool);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::upsert_custom_batch(
    const std::vector<CustomToolDescriptor>& descriptors,
    CustomToolOwner owner) {
    if (owner == nullptr || descriptors.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::unordered_set<std::string> names;
    names.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        if (descriptor.name.empty() || !valid_utf8(descriptor.name) ||
            is_builtin_name(descriptor.name) ||
            is_gpu_hunt_tool_name(descriptor.name) ||
            !names.emplace(descriptor.name).second) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (!descriptor.parameters.is_null() &&
            !descriptor.parameters.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }

    std::lock_guard<std::mutex> lock(custom_mutex_);
    for (const auto& descriptor : descriptors) {
        if (aliases_.find(descriptor.name) != aliases_.end()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const auto existing = custom_tools_.find(descriptor.name);
        if (existing != custom_tools_.end() &&
            existing->second.owner != owner) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
    }

    std::unordered_map<std::string, CustomTool> next = custom_tools_;
    for (const auto& descriptor : descriptors) {
        CustomTool tool;
        tool.description = descriptor.description;
        tool.parameters = descriptor.parameters.is_null()
                              ? Json{{"type", "object"},
                                     {"properties", Json::object()}}
                              : descriptor.parameters;
        tool.read_only = descriptor.read_only;
        tool.execute_fn = descriptor.execute_fn;
        tool.execute_user = descriptor.execute_user;
        tool.execute_lifetime = descriptor.execute_lifetime;
        tool.explicit_confirmation_required =
            descriptor.explicit_confirmation_required;
        tool.owner = owner;
        next[descriptor.name] = std::move(tool);
    }
    custom_tools_.swap(next);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::remove_custom_batch(
    const std::vector<std::string>& names, CustomToolOwner owner) {
    if (owner == nullptr || names.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::unordered_set<std::string> unique_names;
    unique_names.reserve(names.size());
    for (const auto& name : names) {
        if (name.empty() || !valid_utf8(name) ||
            !unique_names.emplace(name).second) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }

    std::lock_guard<std::mutex> lock(custom_mutex_);
    for (const auto& name : names) {
        const auto found = custom_tools_.find(name);
        if (found == custom_tools_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.owner != owner) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
    }

    std::unordered_map<std::string, CustomTool> next = custom_tools_;
    for (const auto& name : names) {
        next.erase(name);
    }
    custom_tools_.swap(next);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::unregister_custom(std::string_view name) {
    if (name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(custom_mutex_);
    const auto found = custom_tools_.find(std::string(name));
    if (found == custom_tools_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.owner != nullptr) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    custom_tools_.erase(found);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::register_alias(std::string_view alias,
                                           std::string_view target) {
    if (alias.empty() || !valid_utf8(alias) || target.empty() ||
        !valid_utf8(target)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (alias == target) {
        // A self-alias is meaningless and would silently succeed under any
        // "unique name" check; reject so misuse is loud.
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(custom_mutex_);
    // Collision: alias name must not shadow a built-in / gpu_hunt tool /
    // custom / other alias.
    if (is_builtin_name(alias) ||
        is_gpu_hunt_tool_name(alias) ||
        custom_tools_.find(std::string(alias)) != custom_tools_.end() ||
        aliases_.find(std::string(alias)) != aliases_.end()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Target must resolve to something dispatchable *right now*.  Chained
    // aliasing (alias -> alias -> tool) is rejected because it complicates
    // resolution + describe() would need cycle detection.
    const bool target_ok =
        is_builtin_name(target) ||
        is_gpu_hunt_tool_name(target) ||
        custom_tools_.find(std::string(target)) != custom_tools_.end();
    if (!target_ok) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    aliases_[std::string(alias)] = AliasEntry{std::string(target)};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::unregister_alias(std::string_view alias) {
    if (alias.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(custom_mutex_);
    const auto found = aliases_.find(std::string(alias));
    if (found == aliases_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    aliases_.erase(found);
    return SAO_AI_EDITOR_OK;
}

std::string NativeToolRegistry::resolve_alias(std::string_view name) const {
    if (name.empty()) {
        return std::string(name);
    }
    std::lock_guard<std::mutex> lock(custom_mutex_);
    const auto found = aliases_.find(std::string(name));
    if (found == aliases_.end()) {
        return std::string(name);
    }
    return found->second.target;
}

int32_t NativeToolRegistry::register_hook(
    std::string_view id, std::string_view phase,
    const std::vector<std::string>& tool_filter,
    std::string_view emit_event) {
    if (id.empty() || !valid_utf8(id) || emit_event.empty() ||
        !valid_utf8(emit_event)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (phase != "before" && phase != "after" && phase != "error") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Hook hook;
    hook.id = std::string(id);
    hook.phase = std::string(phase);
    hook.tool_filter = tool_filter;
    hook.emit_event = std::string(emit_event);
    std::lock_guard<std::mutex> lock(custom_mutex_);
    hooks_[hook.id] = std::move(hook);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::unregister_hook(std::string_view id) {
    if (id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(custom_mutex_);
    const auto found = hooks_.find(std::string(id));
    if (found == hooks_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    hooks_.erase(found);
    return SAO_AI_EDITOR_OK;
}

std::vector<NativeToolRegistry::HookFire>
NativeToolRegistry::snapshot_hooks(std::string_view phase,
                                    std::string_view tool_name) const {
    std::vector<HookFire> matched;
    std::lock_guard<std::mutex> lock(custom_mutex_);
    matched.reserve(hooks_.size());
    for (const auto& entry : hooks_) {
        const Hook& hook = entry.second;
        if (hook.phase != phase) {
            continue;
        }
        // Empty filter = match any tool.  Otherwise the resolved tool name
        // must appear in the whitelist.  Comparing against the canonical
        // (post-alias) name means a filter of ["readFile"] fires for both a
        // direct call and any alias that lands on readFile.
        if (!hook.tool_filter.empty()) {
            bool matches = false;
            for (const auto& allowed : hook.tool_filter) {
                if (allowed == tool_name) {
                    matches = true;
                    break;
                }
            }
            if (!matches) {
                continue;
            }
        }
        matched.push_back(HookFire{hook.id, hook.emit_event});
    }
    return matched;
}

int32_t NativeToolRegistry::read_file(const Json& arguments,
                                      Json& result) const {
    if (!arguments.contains("path") || !arguments["path"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::filesystem::path path;
    if (!resolve_bounded_path(scopes_.workspace_root(),
                              arguments["path"].get_ref<const std::string&>(),
                              false, path)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    std::string content;
    const int32_t status = read_text_file(path, maximum_file_bytes_, content);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const int64_t start = std::max<int64_t>(0, arguments.value("startLine", 0));
    const int64_t end = std::max<int64_t>(0, arguments.value("endLine", 0));
    if (start > 0) {
        std::istringstream input(content);
        std::ostringstream selected;
        std::string line;
        int64_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (line_number >= start && (end == 0 || line_number <= end)) {
                selected << line;
                if (!input.eof()) {
                    selected << '\n';
                }
            }
            if (end > 0 && line_number >= end) {
                break;
            }
        }
        content = selected.str();
    }
    result = Json{{"path", relative_utf8(path, scopes_.workspace_root())},
                  {"content", std::move(content)},
                  {"startLine", start},
                  {"endLine", end}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::list_files(const Json& arguments,
                                       Json& result) const {
    const std::string raw_path = arguments.value("path", ".");
    std::filesystem::path root;
    if (!resolve_bounded_path(scopes_.workspace_root(), raw_path, false, root)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::wstring pattern = utf8_to_wide(arguments.value("pattern", "*"));
    const bool recursive = arguments.value("recursive", false);
    const uint32_t limit = std::clamp(
        arguments.value("limit", maximum_search_results_), 1U,
        maximum_search_results_);
    Json entries = Json::array();
    auto append = [&](const std::filesystem::directory_entry& entry) {
        if (entries.size() >= limit ||
            !wildcard_match(entry.path().filename().native(), pattern)) {
            return;
        }
        std::filesystem::path bounded;
        const std::string absolute = wide_to_utf8(entry.path().native());
        if (!resolve_bounded_path(scopes_.workspace_root(), absolute, false,
                                  bounded)) {
            return;
        }
        const bool directory = entry.is_directory(error);
        const auto size = directory ? uintmax_t{0} : entry.file_size(error);
        entries.push_back({{"name", relative_utf8(bounded, root)},
                           {"type", directory ? "directory" : "file"},
                           {"size", error ? uintmax_t{0} : size}});
        error.clear();
    };
    if (recursive) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root, error)) {
            if (error || entries.size() >= limit) {
                break;
            }
            append(entry);
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (error || entries.size() >= limit) {
                break;
            }
            append(entry);
        }
    }
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result = Json{{"path", relative_utf8(root, scopes_.workspace_root())},
                  {"entries", std::move(entries)}};
    result["total"] = result["entries"].size();
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::search_files(const Json& arguments,
                                         Json& result) const {
    if (!arguments.contains("query") || !arguments["query"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string query = arguments["query"].get<std::string>();
    if (query.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::filesystem::path root;
    if (!resolve_bounded_path(scopes_.workspace_root(),
                              arguments.value("path", "."), false, root)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    const bool use_regex = arguments.value("regex", false);
    const bool case_sensitive = arguments.value("caseSensitive", false);
    const auto flags = case_sensitive
        ? std::regex_constants::ECMAScript
        : std::regex_constants::ECMAScript | std::regex_constants::icase;
    const std::regex expression(use_regex ? query
                                          : std::regex_replace(
                                                query,
                                                std::regex(R"([.^$|()\[\]{}*+?\\])"),
                                                R"(\$&)"),
                                flags);
    const std::wstring pattern = utf8_to_wide(arguments.value("pattern", "*"));
    const uint32_t limit = std::clamp(
        arguments.value("limit", maximum_search_results_), 1U,
        maximum_search_results_);
    Json matches = Json::array();
    std::error_code error;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root, error)) {
        if (error || matches.size() >= limit) {
            break;
        }
        if (!entry.is_regular_file(error) ||
            !wildcard_match(entry.path().filename().native(), pattern)) {
            continue;
        }
        std::filesystem::path bounded;
        const std::string absolute = wide_to_utf8(entry.path().native());
        if (!resolve_bounded_path(scopes_.workspace_root(), absolute, false,
                                  bounded)) {
            continue;
        }
        std::string content;
        if (read_text_file(bounded, maximum_file_bytes_, content) !=
            SAO_AI_EDITOR_OK) {
            continue;
        }
        std::istringstream input(content);
        std::string line;
        uint32_t line_number = 0;
        while (std::getline(input, line) && matches.size() < limit) {
            ++line_number;
            if (std::regex_search(line, expression)) {
                matches.push_back({{"file", relative_utf8(bounded, root)},
                                   {"line", line_number},
                                   {"text", line.substr(0, 500)}});
            }
        }
    }
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result = Json{{"query", query}, {"results", std::move(matches)}};
    result["total"] = result["results"].size();
    return SAO_AI_EDITOR_OK;
}

int32_t NativeToolRegistry::edit_file(const Json& arguments,
                                      Json& result) const {
    if (!arguments.contains("path") || !arguments["path"].is_string() ||
        !arguments.contains("content") || !arguments["content"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::filesystem::path path;
    if (!resolve_bounded_path(scopes_.workspace_root(),
                              arguments["path"].get_ref<const std::string&>(),
                              true, path)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    std::string content = arguments["content"].get<std::string>();
    const int64_t start = std::max<int64_t>(0, arguments.value("startLine", 0));
    const int64_t end = std::max<int64_t>(0, arguments.value("endLine", 0));
    if (start > 0) {
        std::string existing;
        int32_t status = read_text_file(path, maximum_file_bytes_, existing);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        std::vector<std::string> lines;
        std::istringstream input(existing);
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
        if (start > static_cast<int64_t>(lines.size()) + 1 ||
            (end > 0 && end < start)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> replacement;
        std::istringstream replacement_input(content);
        while (std::getline(replacement_input, line)) {
            replacement.push_back(line);
        }
        const auto first = lines.begin() + std::min<size_t>(
            static_cast<size_t>(start - 1), lines.size());
        const auto last_index = end > 0
            ? std::min<size_t>(static_cast<size_t>(end), lines.size())
            : std::min<size_t>(static_cast<size_t>(start), lines.size());
        lines.erase(first, lines.begin() + last_index);
        lines.insert(lines.begin() + static_cast<ptrdiff_t>(start - 1),
                     replacement.begin(), replacement.end());
        std::ostringstream output;
        for (size_t index = 0; index < lines.size(); ++index) {
            output << lines[index];
            if (index + 1 < lines.size() || !existing.empty()) {
                output << '\n';
            }
        }
        content = output.str();
    }
    if (content.size() > maximum_file_bytes_) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = write_text_atomic(path, content);
    if (status == SAO_AI_EDITOR_OK) {
        result = Json{{"ok", true},
                      {"path", relative_utf8(path, scopes_.workspace_root())},
                      {"bytesWritten", content.size()}};
    }
    return status;
}

}  // namespace sao::ai_editor::native
