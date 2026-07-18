// libs_vendor_bridge.cpp — 老插件依赖目录发现与语言路径格式化

#include "sao/plugins/compat/libs_vendor_bridge.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

namespace sao::plugins::compat {

namespace {

namespace fs = std::filesystem;

std::wstring path_key(const fs::path& path) {
    std::wstring key = path.lexically_normal().wstring();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
#endif
    return key;
}

bool component_equal(const fs::path& left, const fs::path& right) {
#if defined(_WIN32)
    const auto left_key = path_key(left);
    const auto right_key = path_key(right);
    return left_key == right_key;
#else
    return left == right;
#endif
}

bool is_strict_descendant(const fs::path& root, const fs::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || !component_equal(*root_it, *candidate_it)) {
            return false;
        }
    }
    return candidate_it != candidate.end();
}

std::vector<std::wstring> unique_ordered_paths(const discovered_deps_dirs& dirs) {
    std::vector<std::wstring> source;
    source.insert(source.end(), dirs.engine_dirs.begin(), dirs.engine_dirs.end());
    source.insert(source.end(), dirs.libs_dirs.begin(), dirs.libs_dirs.end());
    source.insert(source.end(), dirs.vendor_dirs.begin(), dirs.vendor_dirs.end());
    source.insert(source.end(), dirs.ordered.begin(), dirs.ordered.end());

    std::vector<std::wstring> output;
    std::unordered_set<std::wstring> seen;
    output.reserve(source.size());
    for (const auto& raw : source) {
        if (raw.empty()) {
            continue;
        }
        const fs::path normalized = fs::path(raw).lexically_normal();
        const auto key = path_key(normalized);
        if (seen.insert(key).second) {
            output.push_back(normalized.wstring());
        }
    }
    return output;
}

int32_t discover_deps_dirs_impl(const wchar_t* plugin_dir, discovered_deps_dirs* out_dirs) {
    if (out_dirs == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_dirs = discovered_deps_dirs{};
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    std::error_code ec;
    const fs::path root = fs::canonical(fs::path(plugin_dir), ec);
    if (ec || !fs::is_directory(root, ec) || ec) {
        return SAO_ERR_HANDLE_INVALID;
    }

    std::unordered_set<std::wstring> seen;
    const auto append_if_valid = [&](const wchar_t* name, std::vector<std::wstring>& category) {
        std::error_code child_ec;
        const fs::path child = fs::canonical(root / name, child_ec);
        if (child_ec || !fs::is_directory(child, child_ec) || child_ec ||
            !is_strict_descendant(root, child)) {
            return;
        }
        const auto key = path_key(child);
        if (!seen.insert(key).second) {
            return;
        }
        const auto value = child.wstring();
        category.push_back(value);
        out_dirs->ordered.push_back(value);
    };

    append_if_valid(L"engine", out_dirs->engine_dirs);
    append_if_valid(L"libs", out_dirs->libs_dirs);
    append_if_valid(L"vendor", out_dirs->vendor_dirs);
    return SAO_OK;
}

int32_t format_language_paths_impl(const discovered_deps_dirs* dirs,
                                   sao::plugins::loader::engine_kind language,
                                   std::vector<std::wstring>& output) {
    if (dirs == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    const auto ordered = unique_ordered_paths(*dirs);
    switch (language) {
    case sao::plugins::loader::engine_kind::python:
    case sao::plugins::loader::engine_kind::emma:
    case sao::plugins::loader::engine_kind::angelscript:
    case sao::plugins::loader::engine_kind::csharp:
        output = ordered;
        return SAO_OK;
    case sao::plugins::loader::engine_kind::lua:
        output.reserve(ordered.size() * 2);
        for (const auto& directory : ordered) {
            const fs::path base(directory);
            output.push_back((base / L"?.lua").wstring());
            output.push_back((base / L"?" / L"init.lua").wstring());
        }
        return SAO_OK;
    case sao::plugins::loader::engine_kind::unknown:
    default:
        return SAO_ERR_INVALID_ARGUMENT;
    }
}

int32_t allocate_paths(const std::vector<std::wstring>& source, wchar_t*** out_paths,
                       size_t* out_count) {
    if (source.empty()) {
        return SAO_OK;
    }

    auto array = std::make_unique<wchar_t*[]>(source.size());
    std::fill_n(array.get(), source.size(), nullptr);
    std::vector<std::unique_ptr<wchar_t[]>> strings;
    strings.reserve(source.size());
    for (const auto& value : source) {
        auto string = std::make_unique<wchar_t[]>(value.size() + 1);
        std::copy(value.begin(), value.end(), string.get());
        string[value.size()] = L'\0';
        strings.push_back(std::move(string));
    }
    for (size_t index = 0; index < strings.size(); ++index) {
        array[index] = strings[index].release();
    }

    *out_count = source.size();
    *out_paths = array.release();
    return SAO_OK;
}

int32_t copy_to_wbuf(const std::wstring& src, wchar_t* out_buf, size_t out_size) {
    if (out_buf == nullptr || out_size == 0) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (src.size() + 1 > out_size) {
        out_buf[0] = L'\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::copy(src.begin(), src.end(), out_buf);
    out_buf[src.size()] = L'\0';
    return SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_discover_deps_dirs(const wchar_t* plugin_dir, discovered_deps_dirs* out_dirs) {
    try {
        return discover_deps_dirs_impl(plugin_dir, out_dirs);
    } catch (...) {
        if (out_dirs != nullptr) {
            *out_dirs = discovered_deps_dirs{};
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_deps_dirs(discovered_deps_dirs* dirs) {
    if (dirs != nullptr) {
        *dirs = discovered_deps_dirs{};
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_libs_vendor_probe(const wchar_t* plugin_dir, discovered_deps_dirs* out_dirs) {
    try {
        return discover_deps_dirs_impl(plugin_dir, out_dirs);
    } catch (...) {
        if (out_dirs != nullptr) {
            *out_dirs = discovered_deps_dirs{};
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_compat_format_python_sys_path(
    const discovered_deps_dirs* dirs, wchar_t* out_buf, size_t out_size) {
    try {
        if (dirs == nullptr) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
#if defined(_WIN32)
        constexpr wchar_t separator = L';';
#else
        constexpr wchar_t separator = L':';
#endif
        const auto ordered = unique_ordered_paths(*dirs);
        std::wstring joined;
        for (size_t index = 0; index < ordered.size(); ++index) {
            if (index != 0) {
                joined.push_back(separator);
            }
            joined += ordered[index];
        }
        return copy_to_wbuf(joined, out_buf, out_size);
    } catch (...) {
        if (out_buf != nullptr && out_size != 0) {
            out_buf[0] = L'\0';
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_compat_format_lua_package_path(
    const discovered_deps_dirs* dirs, wchar_t* out_buf, size_t out_size) {
    try {
        std::vector<std::wstring> paths;
        const int32_t status =
            format_language_paths_impl(dirs, sao::plugins::loader::engine_kind::lua, paths);
        if (status != SAO_OK) {
            return status;
        }
        std::wstring joined;
        for (size_t index = 0; index < paths.size(); ++index) {
            if (index != 0) {
                joined.push_back(L';');
            }
            joined += paths[index];
        }
        return copy_to_wbuf(joined, out_buf, out_size);
    } catch (...) {
        if (out_buf != nullptr && out_size != 0) {
            out_buf[0] = L'\0';
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_compat_format_paths_for_language(
    const discovered_deps_dirs* dirs, sao::plugins::loader::engine_kind language,
    wchar_t*** out_paths, size_t* out_count) {
    if (out_paths != nullptr) {
        *out_paths = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (out_paths == nullptr || out_count == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        std::vector<std::wstring> formatted;
        const int32_t status = format_language_paths_impl(dirs, language, formatted);
        if (status != SAO_OK) {
            return status;
        }
        return allocate_paths(formatted, out_paths, out_count);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_compat_free_paths(wchar_t** paths,
                                                                               size_t count) {
    if (paths == nullptr) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        delete[] paths[index];
    }
    delete[] paths;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_compat_has_requirements_txt(const wchar_t* plugin_dir) {
    try {
        if (plugin_dir == nullptr || plugin_dir[0] == L'\0') {
            return false;
        }
        std::error_code ec;
        const fs::path root = fs::canonical(fs::path(plugin_dir), ec);
        if (ec || !fs::is_directory(root, ec) || ec) {
            return false;
        }
        const fs::path requirements = fs::canonical(root / L"requirements.txt", ec);
        return !ec && is_strict_descendant(root, requirements) &&
               fs::is_regular_file(requirements, ec) && !ec;
    } catch (...) {
        return false;
    }
}

} // namespace sao::plugins::compat
