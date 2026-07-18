#include "native_tool_registry.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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

void validate_recursive(const Json& value,
                        const Json& schema,
                        const std::string& path,
                        Json& errors) {
    if (!schema.is_object()) {
        // Non-object schema (e.g. `true` / `false`) — best-effort skip so we do
        // not falsely reject callers that hand us a permissive schema stub.
        return;
    }

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

    // `enum` — the value must appear in the enum array using nlohmann's ==
    // (which handles number-vs-string ties correctly).
    if (schema.contains("enum") && schema["enum"].is_array()) {
        const auto& allowed = schema["enum"];
        bool matched = false;
        for (const auto& candidate : allowed) {
            if (candidate == value) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            append_error(errors, path, "value not in enum");
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

    // `properties` — recurse for each field present in the value.  Unlisted
    // properties are ignored (no additionalProperties support).
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
    // Append caller-registered tools.  Snapshot under lock so a concurrent
    // register / unregister cannot mutate the map while we build descriptors,
    // then release the lock before finalising `permission` (independent of the
    // registry state).
    std::vector<Json> custom_snapshot;
    {
        std::lock_guard<std::mutex> lock(custom_mutex_);
        custom_snapshot.reserve(custom_tools_.size());
        for (const auto& entry : custom_tools_) {
            Json descriptor{{"name", entry.first},
                            {"description", entry.second.description},
                            {"readOnly", entry.second.read_only},
                            {"parameters", entry.second.parameters},
                            {"custom", true}};
            custom_snapshot.push_back(std::move(descriptor));
        }
    }
    for (auto& descriptor : custom_snapshot) {
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
    // Resolve the schema for whichever tool this is (built-in or custom).  We
    // grab a snapshot of the custom tool descriptor while holding the mutex so
    // a concurrent unregister does not race the schema-vs-handler path.
    Json schema;
    bool is_custom = false;
    bool custom_read_only = true;
    bool found_tool = false;
    if (name == "readFile" || name == "listFiles" ||
        name == "searchFiles" || name == "editFile") {
        schema = builtin_schema_for(name);
        found_tool = true;
    } else {
        std::lock_guard<std::mutex> lock(custom_mutex_);
        const auto found = custom_tools_.find(std::string(name));
        if (found != custom_tools_.end()) {
            schema = found->second.parameters;
            custom_read_only = found->second.read_only;
            is_custom = true;
            found_tool = true;
        }
    }
    if (!found_tool) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
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
    if (name == "readFile") {
        return read_file(arguments, result);
    }
    if (name == "listFiles") {
        return list_files(arguments, result);
    }
    if (name == "searchFiles") {
        return search_files(arguments, result);
    }
    if (name == "editFile") {
        if (mode == "ask") {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
        if (mode == "plan" && !arguments.value("confirmed", false)) {
            result = Json{{"confirmationRequired", true},
                          {"tool", "editFile"}};
            return SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED;
        }
        return edit_file(arguments, result);
    }
    // Custom tools: sync passthrough — the runtime hands the arguments back to
    // the caller so an external handler can carry out the real work.  The
    // ask/plan gating mirrors the built-in mutating-tool behaviour so a
    // user-defined "writeSomething" tool cannot slip past permission mode.
    if (is_custom) {
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
        result = Json{{"custom", true},
                      {"name", std::string(name)},
                      {"arguments", arguments}};
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeToolRegistry::register_custom(std::string_view name,
                                            std::string_view description,
                                            const Json& parameters,
                                            bool read_only) {
    if (name.empty() || !valid_utf8(name)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Shadowing a built-in would produce two entries in describe() and confuse
    // execute() dispatch (built-ins always win).  Reject early so callers get
    // an actionable error rather than a silently-hidden custom tool.
    if (name == "readFile" || name == "listFiles" ||
        name == "searchFiles" || name == "editFile") {
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
    std::lock_guard<std::mutex> lock(custom_mutex_);
    custom_tools_[std::string(name)] = std::move(tool);
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
    custom_tools_.erase(found);
    return SAO_AI_EDITOR_OK;
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
