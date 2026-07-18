#include "native_utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace sao::ai_editor::native {
namespace {

bool component_equal(const std::filesystem::path& left,
                     const std::filesystem::path& right) {
    auto a = left.native();
    auto b = right.native();
    std::transform(a.begin(), a.end(), a.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    return a == b;
}

bool path_within(const std::filesystem::path& root,
                 const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    while (root_it != root.end()) {
        if (candidate_it == candidate.end() ||
            !component_equal(*root_it, *candidate_it)) {
            return false;
        }
        ++root_it;
        ++candidate_it;
    }
    return true;
}

}  // namespace

bool valid_utf8(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool valid_simple_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128 || value.find("..") != value.npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' ||
               character == '_' || character == '.';
    });
}

std::wstring utf8_to_wide(std::string_view value) {
    if (!valid_utf8(value) || value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length) != length) {
        return {};
    }
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

bool normalize_root(std::string_view value,
                    std::filesystem::path& result,
                    bool create) {
    if (value.empty()) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(value);
    if (wide.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::path path(wide);
    if (!path.is_absolute()) {
        return false;
    }
    if (create) {
        std::filesystem::create_directories(path, error);
        if (error) {
            return false;
        }
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        return false;
    }
    result = std::filesystem::weakly_canonical(path, error);
    return !error && result.is_absolute();
}

bool resolve_bounded_path(const std::filesystem::path& root,
                          std::string_view value,
                          bool for_write,
                          std::filesystem::path& result) {
    if (value.empty() || !valid_utf8(value)) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(value);
    if (wide.empty()) {
        return false;
    }
    std::filesystem::path requested(wide);
    std::filesystem::path candidate = requested.is_absolute()
        ? requested
        : root / requested;
    std::error_code error;
    if (for_write && !std::filesystem::exists(candidate, error)) {
        auto parent = candidate.parent_path();
        if (parent.empty()) {
            parent = root;
        }
        const auto canonical_parent = std::filesystem::weakly_canonical(
            parent, error);
        if (error || !path_within(root, canonical_parent)) {
            return false;
        }
        candidate = canonical_parent / candidate.filename();
    } else {
        candidate = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            return false;
        }
    }
    if (!path_within(root, candidate)) {
        return false;
    }
    result = candidate;
    return true;
}

int32_t read_text_file(const std::filesystem::path& path,
                       uint32_t maximum_bytes,
                       std::string& result) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_bytes) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    if (input.bad() || !valid_utf8(result)) {
        result.clear();
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t write_text_atomic(const std::filesystem::path& path,
                          std::string_view content) {
    if (!valid_utf8(content)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t copy_text_to_caller(std::string_view value,
                            char* output,
                            uint32_t capacity,
                            uint32_t* out_length) {
    if (out_length == nullptr || value.size() > UINT32_MAX) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    *out_length = static_cast<uint32_t>(value.size());
    if (output == nullptr || capacity <= value.size()) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    if (!value.empty()) {
        std::memcpy(output, value.data(), value.size());
    }
    output[value.size()] = '\0';
    return SAO_AI_EDITOR_OK;
}

Json rpc_error(const Json& id,
               int code,
               std::string_view message,
               const Json& data) {
    Json error{{"code", code}, {"message", std::string(message)}};
    if (!data.is_null()) {
        error["data"] = data;
    }
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)},
                {"sao", {{"protocolVersion", 1}}}};
}

Json rpc_result(const Json& id, const Json& result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result},
                {"sao", {{"protocolVersion", 1}}}};
}

std::string dump_json(const Json& value) {
    return value.dump(-1, ' ', false,
                      nlohmann::json::error_handler_t::strict);
}

}  // namespace sao::ai_editor::native
