#include "native_tool_registry.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <regex>
#include <sstream>
#include <vector>

namespace sao::ai_editor::native {
namespace {

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
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
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
