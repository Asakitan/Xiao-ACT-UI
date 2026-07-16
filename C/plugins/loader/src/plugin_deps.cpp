#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/loader_status.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace sao::plugins::loader {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string distribution_name(std::string requirement) {
    const auto comment = requirement.find('#');
    if (comment != std::string::npos) requirement.resize(comment);
    requirement = trim(std::move(requirement));
    const auto extra = requirement.find('[');
    const auto version = requirement.find_first_of("<>=!~; ");
    const auto stop = std::min(extra == std::string::npos ? requirement.size() : extra,
                               version == std::string::npos ? requirement.size() : version);
    requirement.resize(stop);
    std::transform(requirement.begin(), requirement.end(), requirement.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return requirement;
}

bool dependency_present(const fs::path& root, std::string_view import_name) {
    std::error_code error;
    const auto package = root / fs::u8path(import_name);
    return fs::exists(package, error) || fs::exists(package.wstring() + L".py", error) ||
           fs::exists(package.wstring() + L".pyd", error) || fs::exists(package.wstring() + L".dll", error);
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_ensure(const wchar_t* plugin_dir,
                        bool allow_pip_install,
                        deps_bootstrap_record* out_record) {
    if (plugin_dir == nullptr || out_record == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_record = deps_bootstrap_record{};
    try {
        const fs::path root(plugin_dir);
        std::error_code error;
        if (!fs::is_directory(root, error)) return SAO_ERR_HANDLE_INVALID;
        std::vector<fs::path> search_roots;
        for (const auto* name : {L"libs", L"vendor", L"engine"}) {
            const auto path = root / name;
            if (fs::is_directory(path, error)) {
                search_roots.push_back(path);
                out_record->added_paths.push_back(fs::weakly_canonical(path, error).native());
            }
            error.clear();
        }
        const auto requirements = root / L"requirements.txt";
        if (!fs::is_regular_file(requirements, error)) return SAO_OK;
        std::ifstream input(requirements);
        if (!input) return SAO_ERR_HANDLE_INVALID;
        std::string line;
        while (std::getline(input, line)) {
            auto distribution = distribution_name(line);
            if (distribution.empty() || distribution[0] == '-') continue;
            const auto map_iterator = dist_to_import_name_map().find(distribution);
            const auto import_name = map_iterator == dist_to_import_name_map().end()
                ? distribution : map_iterator->second;
            auto found = std::find_if(search_roots.begin(), search_roots.end(),
                [&import_name](const fs::path& path) { return dependency_present(path, import_name); });
            if (found == search_roots.end()) {
                out_record->deps_summary[distribution] = "missing";
                return allow_pip_install ? SAO_PLUGINS_ERR_UNSUPPORTED
                                         : SAO_PLUGINS_ERR_DEPENDENCY_MISSING;
            }
            out_record->deps_summary[distribution] =
                _wcsicmp(found->filename().c_str(), L"libs") == 0 ? "libs" : "vendor";
        }
        return SAO_OK;
    } catch (...) {
        *out_record = deps_bootstrap_record{};
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_deps_restore(const deps_bootstrap_record*) {
}

const std::unordered_map<std::string, std::string>& dist_to_import_name_map() {
    // dist 名 → import 名 (对齐 python _IMPORT_NAME)。
    static const std::unordered_map<std::string, std::string> map_ = {
        {"pillow", "PIL"},
        {"pyyaml", "yaml"},
        {"beautifulsoup4", "bs4"},
    };
    return map_;
}

} // namespace sao::plugins::loader
