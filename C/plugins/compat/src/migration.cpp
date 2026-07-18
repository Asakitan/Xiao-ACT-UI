// migration.cpp — 老 manifest 字段与 ctx 方法的稳定迁移诊断

#include "sao/plugins/compat/migration.h"

#include "sao/plugins/compat/py_v1_manifest.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace sao::plugins::compat {

namespace {

namespace fs = std::filesystem;
using ordered_json = nlohmann::ordered_json;

constexpr size_t k_max_input_path = 32768;
constexpr size_t k_max_manifest_bytes = 1024 * 1024;
constexpr size_t k_max_source_bytes = 4 * 1024 * 1024;

constexpr deprecated_entry k_deprecated_entries[] = {
    {"engine", "language", "manifest 里 'engine' 字段推荐改用 'language'", "manifest"},
    {"runtime", "language", "manifest 里 'runtime' 字段推荐改用 'language'", "manifest"},
    {"deps", "requires", "manifest 里 'deps' 数组统一到 'requires'", "manifest"},
    {"description_short", "description", "sao_menu 里 'description_short' 弃用", "manifest"},
    {"capability_id", "id", "capabilities 项里的 'capability_id' 改用 'id'", "manifest"},
    {"register_script", "register_ui_panel", "老 ctx.register_script 别名", "ctx_method"},
    {"add_hotkey", "register_hotkey", "老 ctx.add_hotkey 别名", "ctx_method"},
    {"add_menu_item", "register_menu_category", "老菜单添加接口", "ctx_method"},
};
constexpr size_t k_deprecated_count =
    sizeof(k_deprecated_entries) / sizeof(k_deprecated_entries[0]);

struct diagnostic {
    size_t entry_index = 0;
    std::string file;
    size_t line = 1;
    size_t column = 1;
};

struct source_file {
    fs::path path;
    std::string relative;
};

size_t bounded_length(const char* value) {
    size_t length = 0;
    while (length <= k_max_input_path && value[length] != '\0') {
        ++length;
    }
    return length;
}

bool utf8_to_path(std::string_view value, fs::path& output) {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return false;
    }
#if defined(_WIN32)
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const int input_length = static_cast<int>(value.size());
    const int output_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_length, nullptr, 0);
    if (output_length <= 0) {
        return false;
    }
    std::wstring wide(static_cast<size_t>(output_length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_length, wide.data(),
                            output_length) != output_length) {
        return false;
    }
    output = fs::path(std::move(wide));
#else
    output = fs::u8path(value.begin(), value.end());
#endif
    return true;
}

bool path_to_utf8(const fs::path& value, std::string& output) {
#if defined(_WIN32)
    const std::wstring wide = value.generic_wstring();
    if (wide.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const int input_length = static_cast<int>(wide.size());
    const int output_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                                  input_length, nullptr, 0, nullptr, nullptr);
    if (output_length <= 0) {
        return false;
    }
    output.assign(static_cast<size_t>(output_length), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), input_length,
                               output.data(), output_length, nullptr, nullptr) == output_length;
#else
    output = value.generic_string();
    return true;
#endif
}

bool path_is_within(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty()) {
        return candidate == root;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != fs::path(L"..");
}

bool resolve_plugin_input(const fs::path& input, fs::path& root, fs::path& manifest_path) {
    std::error_code ec;
    fs::path candidate = input;
    if (!fs::exists(candidate, ec) && !input.has_parent_path()) {
        ec.clear();
        const fs::path under_plugins = fs::current_path(ec) / L"plugins" / input;
        if (!ec && fs::exists(under_plugins, ec)) {
            candidate = under_plugins;
        }
    }
    ec.clear();
    const fs::path canonical_input = fs::canonical(candidate, ec);
    if (ec) {
        return false;
    }
    if (fs::is_directory(canonical_input, ec) && !ec) {
        root = canonical_input;
        manifest_path = fs::canonical(root / L"plugin.json", ec);
    } else if (fs::is_regular_file(canonical_input, ec) && !ec) {
        manifest_path = canonical_input;
        root = fs::canonical(canonical_input.parent_path(), ec);
    } else {
        return false;
    }
    return !ec && fs::is_regular_file(manifest_path, ec) && !ec &&
           path_is_within(root, manifest_path);
}

int32_t read_bounded_file(const fs::path& path, size_t limit, std::string& output) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (size > limit) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return SAO_ERR_HANDLE_INVALID;
    }
    output.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return stream.bad() ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

std::pair<size_t, size_t> line_column(std::string_view source, size_t offset) {
    size_t line = 1;
    size_t column = 1;
    const size_t end = std::min(offset, source.size());
    for (size_t index = 0; index < end; ++index) {
        if (source[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {line, column};
}

void skip_json_space_and_comments(std::string_view source, size_t& position) {
    while (position < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[position])) != 0) {
            ++position;
            continue;
        }
        if (source[position] == '/' && position + 1 < source.size() &&
            source[position + 1] == '/') {
            position += 2;
            while (position < source.size() && source[position] != '\n') {
                ++position;
            }
            continue;
        }
        break;
    }
}

size_t locate_json_key(std::string_view source, std::string_view wanted) {
    size_t position = 0;
    while (position < source.size()) {
        skip_json_space_and_comments(source, position);
        if (position >= source.size()) {
            break;
        }
        if (source[position] != '"') {
            ++position;
            continue;
        }

        const size_t start = position++;
        std::string decoded;
        bool closed = false;
        while (position < source.size()) {
            const char current = source[position++];
            if (current == '"') {
                closed = true;
                break;
            }
            if (current != '\\') {
                decoded.push_back(current);
                continue;
            }
            if (position >= source.size()) {
                break;
            }
            const char escaped = source[position++];
            if (escaped == 'u') {
                if (position + 4 > source.size()) {
                    break;
                }
                position += 4;
                decoded.push_back('?');
            } else {
                decoded.push_back(escaped);
            }
        }
        if (!closed) {
            return std::string_view::npos;
        }

        size_t after = position;
        skip_json_space_and_comments(source, after);
        if (after < source.size() && source[after] == ':' && decoded == wanted) {
            return start;
        }
    }
    return std::string_view::npos;
}

void add_manifest_diagnostic(std::string_view source, const std::string& file, size_t entry_index,
                             std::vector<diagnostic>& diagnostics) {
    const size_t offset = locate_json_key(source, k_deprecated_entries[entry_index].old_name);
    if (offset == std::string_view::npos) {
        return;
    }
    const auto [line, column] = line_column(source, offset);
    diagnostics.push_back({entry_index, file, line, column});
}

void scan_manifest(const ordered_json& manifest, std::string_view source,
                   std::vector<diagnostic>& diagnostics) {
    if (manifest.contains("engine")) {
        add_manifest_diagnostic(source, "plugin.json", 0, diagnostics);
    }
    if (manifest.contains("runtime")) {
        add_manifest_diagnostic(source, "plugin.json", 1, diagnostics);
    }
    if (manifest.contains("deps")) {
        add_manifest_diagnostic(source, "plugin.json", 2, diagnostics);
    }
    const auto menu = manifest.find("sao_menu");
    if (menu != manifest.end() && menu->is_object() && menu->contains("description_short")) {
        add_manifest_diagnostic(source, "plugin.json", 3, diagnostics);
    }
    const auto capabilities = manifest.find("capabilities");
    if (capabilities != manifest.end() && capabilities->is_array()) {
        const bool found =
            std::any_of(capabilities->begin(), capabilities->end(), [](const auto& value) {
                return value.is_object() && value.contains("capability_id");
            });
        if (found) {
            add_manifest_diagnostic(source, "plugin.json", 4, diagnostics);
        }
    }
}

void mask_range(std::string& output, size_t begin, size_t end) {
    for (size_t index = begin; index < end && index < output.size(); ++index) {
        if (output[index] != '\r' && output[index] != '\n') {
            output[index] = ' ';
        }
    }
}

bool lua_long_bracket_at(std::string_view source, size_t position, size_t& content_begin,
                         std::string& closing) {
    if (position >= source.size() || source[position] != '[') {
        return false;
    }
    size_t cursor = position + 1;
    while (cursor < source.size() && source[cursor] == '=') {
        ++cursor;
    }
    if (cursor >= source.size() || source[cursor] != '[') {
        return false;
    }
    closing = "]" + std::string(cursor - position - 1, '=') + "]";
    content_begin = cursor + 1;
    return true;
}

std::string mask_source_literals_and_comments(std::string_view source) {
    std::string output(source);
    size_t position = 0;
    while (position < source.size()) {
        const size_t begin = position;
        const bool line_comment =
            source[position] == '#' || (position + 1 < source.size() &&
                                        ((source[position] == '/' && source[position + 1] == '/') ||
                                         (source[position] == '-' && source[position + 1] == '-')));
        if (line_comment) {
            position += source[position] == '#' ? 1 : 2;
            size_t content_begin = 0;
            std::string closing;
            if (source[begin] == '-' &&
                lua_long_bracket_at(source, position, content_begin, closing)) {
                const auto end = source.find(closing, content_begin);
                position = end == std::string_view::npos ? source.size() : end + closing.size();
            } else {
                while (position < source.size() && source[position] != '\n') {
                    ++position;
                }
            }
            mask_range(output, begin, position);
            continue;
        }
        if (position + 1 < source.size() && source[position] == '/' &&
            source[position + 1] == '*') {
            position += 2;
            const auto end = source.find("*/", position);
            position = end == std::string_view::npos ? source.size() : end + 2;
            mask_range(output, begin, position);
            continue;
        }
        size_t content_begin = 0;
        std::string closing;
        if (lua_long_bracket_at(source, position, content_begin, closing)) {
            const auto end = source.find(closing, content_begin);
            position = end == std::string_view::npos ? source.size() : end + closing.size();
            mask_range(output, begin, position);
            continue;
        }
        if (source[position] != '\'' && source[position] != '"' && source[position] != '`') {
            ++position;
            continue;
        }

        const char quote = source[position];
        const bool triple = quote != '`' && position + 2 < source.size() &&
                            source[position + 1] == quote && source[position + 2] == quote;
        position += triple ? 3 : 1;
        while (position < source.size()) {
            if (source[position] == '\\') {
                position = std::min(position + 2, source.size());
                continue;
            }
            if (triple && position + 2 < source.size() && source[position] == quote &&
                source[position + 1] == quote && source[position + 2] == quote) {
                position += 3;
                break;
            }
            if (!triple && source[position] == quote) {
                ++position;
                break;
            }
            ++position;
        }
        mask_range(output, begin, position);
    }
    return output;
}

void scan_source_methods(std::string_view source, const std::string& file,
                         std::vector<diagnostic>& diagnostics) {
    const std::string masked = mask_source_literals_and_comments(source);
    static const std::regex legacy_call(
        R"(\b(ctx|context|plugin_ctx|plugin_context|sdk_ctx|_ctx)\s*(?:\.|->|::|:)\s*(register_script|add_hotkey|add_menu_item)\s*\()",
        std::regex::ECMAScript);
    for (auto iterator = std::sregex_iterator(masked.begin(), masked.end(), legacy_call);
         iterator != std::sregex_iterator(); ++iterator) {
        const std::smatch& match = *iterator;
        const std::string method = match[2].str();
        size_t entry_index = k_deprecated_count;
        for (size_t index = 5; index < k_deprecated_count; ++index) {
            if (method == k_deprecated_entries[index].old_name) {
                entry_index = index;
                break;
            }
        }
        if (entry_index == k_deprecated_count) {
            continue;
        }
        const size_t offset = static_cast<size_t>(match.position(2));
        const auto [line, column] = line_column(source, offset);
        diagnostics.push_back({entry_index, file, line, column});
    }
}

bool is_ignored_source_tree(const fs::path& relative) {
    if (relative.empty()) {
        return false;
    }
    const auto first = relative.begin()->wstring();
#if defined(_WIN32)
    const auto equals = [&](const wchar_t* name) { return _wcsicmp(first.c_str(), name) == 0; };
#else
    const auto equals = [&](const wchar_t* name) { return first == name; };
#endif
    return equals(L"libs") || equals(L"vendor") || equals(L".git") || equals(L"__pycache__") ||
           equals(L"build") || equals(L"bin") || equals(L"obj") || equals(L"test") ||
           equals(L"tests") || equals(L"docs") || equals(L"venv") || equals(L".venv");
}

bool is_source_file(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return extension == L".py" || extension == L".lua" || extension == L".as" ||
           extension == L".cs" || extension == L".emma";
}

int32_t collect_source_files(const fs::path& root, std::vector<source_file>& output) {
    std::error_code ec;
    fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied,
                                              ec);
    const fs::recursive_directory_iterator end;
    while (!ec && iterator != end) {
        const fs::path relative = iterator->path().lexically_relative(root);
        if (iterator->is_directory(ec)) {
            if (!ec && is_ignored_source_tree(relative)) {
                iterator.disable_recursion_pending();
            }
        } else if (!ec && iterator->is_regular_file(ec) && !ec &&
                   is_source_file(iterator->path()) && !is_ignored_source_tree(relative)) {
            const fs::path canonical = fs::canonical(iterator->path(), ec);
            std::string stable_relative;
            if (!ec && path_is_within(root, canonical) &&
                path_to_utf8(relative.lexically_normal(), stable_relative)) {
                output.push_back({canonical, std::move(stable_relative)});
            }
        }
        iterator.increment(ec);
    }
    if (ec) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    std::sort(output.begin(), output.end(), [](const source_file& left, const source_file& right) {
        return left.relative < right.relative;
    });
    return SAO_OK;
}

char* duplicate_report(const std::string& report) {
    auto* output = static_cast<char*>(std::malloc(report.size() + 1));
    if (output != nullptr) {
        std::memcpy(output, report.c_str(), report.size() + 1);
    }
    return output;
}

int32_t scan_deprecated_impl(const char* plugin_id_utf8, char** out_report_json_utf8) {
    if (plugin_id_utf8 == nullptr || out_report_json_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_report_json_utf8 = nullptr;
    const size_t input_length = bounded_length(plugin_id_utf8);
    if (input_length == 0 || input_length > k_max_input_path) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    fs::path input;
    if (!utf8_to_path(std::string_view(plugin_id_utf8, input_length), input)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    fs::path root;
    fs::path manifest_path;
    if (!resolve_plugin_input(input, root, manifest_path)) {
        return SAO_ERR_HANDLE_INVALID;
    }

    std::string manifest_source;
    int32_t status = read_bounded_file(manifest_path, k_max_manifest_bytes, manifest_source);
    if (status != SAO_OK) {
        return status;
    }
    const auto json = ordered_json::parse(manifest_source, nullptr, false, true);
    if (json.is_discarded() || !json.is_object()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    sao::plugins::loader::plugin_manifest manifest{};
    char* parse_error = nullptr;
    status = sao_plugins_compat_parse_manifest_json(manifest_source.data(), manifest_source.size(),
                                                    &manifest, &parse_error);
    sao_plugins_compat_free_string(parse_error);
    if (status != SAO_OK) {
        return status;
    }

    std::vector<diagnostic> diagnostics;
    scan_manifest(json, manifest_source, diagnostics);
    std::vector<source_file> source_files;
    status = collect_source_files(root, source_files);
    if (status != SAO_OK) {
        return status;
    }
    for (const auto& file : source_files) {
        std::string source;
        status = read_bounded_file(file.path, k_max_source_bytes, source);
        if (status != SAO_OK) {
            return status;
        }
        scan_source_methods(source, file.relative, diagnostics);
    }

    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const diagnostic& left, const diagnostic& right) {
                  return std::tie(left.entry_index, left.file, left.line, left.column) <
                         std::tie(right.entry_index, right.file, right.line, right.column);
              });
    diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end(),
                                  [](const diagnostic& left, const diagnostic& right) {
                                      return left.entry_index == right.entry_index;
                                  }),
                      diagnostics.end());

    ordered_json report = ordered_json::object();
    report["plugin_id"] = manifest.plugin_id;
    report["diagnostics"] = ordered_json::array();
    for (const auto& item : diagnostics) {
        const auto& entry = k_deprecated_entries[item.entry_index];
        ordered_json value = ordered_json::object();
        value["category"] = entry.category;
        value["old_name"] = entry.old_name;
        value["new_name"] = entry.new_name;
        value["reason"] = entry.reason;
        value["file"] = item.file;
        value["line"] = item.line;
        value["column"] = item.column;
        report["diagnostics"].push_back(std::move(value));
    }
    report["count"] = diagnostics.size();

    const std::string serialized = report.dump();
    *out_report_json_utf8 = duplicate_report(serialized);
    return *out_report_json_utf8 == nullptr ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API const deprecated_entry* SAO_PLUGINS_CALL
sao_plugins_compat_deprecated_entries(size_t* out_count) {
    if (out_count != nullptr) {
        *out_count = k_deprecated_count;
    }
    return k_deprecated_entries;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_scan_deprecated(const char* plugin_id_utf8, char** out_report_json_utf8) {
    if (out_report_json_utf8 != nullptr) {
        *out_report_json_utf8 = nullptr;
    }
    try {
        return scan_deprecated_impl(plugin_id_utf8, out_report_json_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::compat
