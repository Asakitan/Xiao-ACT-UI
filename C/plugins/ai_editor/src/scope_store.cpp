#include "scope_store.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace sao::ai_editor::native {
namespace {

void merge_objects(Json& destination, const Json& source) {
    for (const auto& [key, value] : source.items()) {
        if (destination.contains(key) && destination[key].is_object() &&
            value.is_object()) {
            merge_objects(destination[key], value);
        } else {
            destination[key] = value;
        }
    }
}

Json plugin_roots_from_text(std::string_view text) {
    if (text.empty()) {
        return Json::object();
    }
    Json parsed = Json::parse(text);
    if (parsed.is_object()) {
        return parsed;
    }
    Json result = Json::object();
    if (parsed.is_array()) {
        for (const auto& item : parsed) {
            if (item.is_object() && item.contains("id") &&
                item.contains("path") && item["id"].is_string() &&
                item["path"].is_string()) {
                result[item["id"].get<std::string>()] = item["path"];
            }
        }
    }
    return result;
}

}  // namespace

int32_t ScopeStore::initialize(std::string_view workspace_root,
                               std::string_view system_root,
                               std::string_view plugin_roots_json) {
    if (!normalize_root(workspace_root, workspace_root_, false) ||
        !normalize_root(system_root, system_root_, true)) {
        return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
    }
    workspace_scope_root_ = workspace_root_ / L".sao";
    std::error_code error;
    std::filesystem::create_directories(workspace_scope_root_, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    workspace_scope_root_ = std::filesystem::weakly_canonical(
        workspace_scope_root_, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    const Json plugins = plugin_roots_from_text(plugin_roots_json);
    for (const auto& [id, raw_path] : plugins.items()) {
        if (!valid_simple_id(id) || !raw_path.is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::filesystem::path root;
        if (!normalize_root(raw_path.get<std::string>(), root, false)) {
            return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
        }
        auto scope = root / L".sao";
        std::filesystem::create_directories(scope, error);
        if (error) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        scope = std::filesystem::weakly_canonical(scope, error);
        if (error) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        plugin_roots_.emplace(id, std::move(scope));
    }
    return SAO_AI_EDITOR_OK;
}

const std::filesystem::path& ScopeStore::workspace_root() const noexcept {
    return workspace_root_;
}

const std::filesystem::path& ScopeStore::workspace_scope_root() const noexcept {
    return workspace_scope_root_;
}

Json ScopeStore::describe_scopes() const {
    Json result = Json::array({
        {{"scope", "system"},
         {"path", wide_to_utf8(system_root_.native())},
         {"label", "System"}},
        {{"scope", "workspace"},
         {"path", wide_to_utf8(workspace_scope_root_.native())},
         {"label", "Workspace"}},
    });
    for (const auto& [id, path] : plugin_roots_) {
        result.push_back({{"scope", "plugin"},
                          {"pluginId", id},
                          {"path", wide_to_utf8(path.native())},
                          {"label", "Plugin: " + id}});
    }
    return result;
}

const std::filesystem::path* ScopeStore::scope_root(
    std::string_view scope,
    std::string_view plugin_id) const {
    if (scope == "system") {
        return &system_root_;
    }
    if (scope == "workspace") {
        return &workspace_scope_root_;
    }
    if (scope == "plugin") {
        const auto found = plugin_roots_.find(plugin_id);
        return found == plugin_roots_.end() ? nullptr : &found->second;
    }
    return nullptr;
}

int32_t ScopeStore::load_json_file(const std::filesystem::path& path,
                                   Json& result) const {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        result = Json::object();
        return error ? SAO_AI_EDITOR_ERR_OS_CALL_FAILED : SAO_AI_EDITOR_OK;
    }
    std::string text;
    const int32_t status = read_text_file(path, kMaximumJsonBytes, text);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json::parse(text);
    if (!result.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ScopeStore::load_scope_config(std::string_view scope,
                                      std::string_view plugin_id,
                                      Json& result) const {
    const auto* root = scope_root(scope, plugin_id);
    if (root == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return load_json_file(*root / L"config.json", result);
}

int32_t ScopeStore::save_scope_config(std::string_view scope,
                                      std::string_view plugin_id,
                                      const Json& value) const {
    const auto* root = scope_root(scope, plugin_id);
    if (root == nullptr || !value.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return write_text_atomic(*root / L"config.json", value.dump(2));
}

int32_t ScopeStore::load_merged_config(Json& result) const {
    result = Json::object();
    Json current;
    int32_t status = load_scope_config("system", "", current);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    merge_objects(result, current);
    status = load_scope_config("workspace", "", current);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    merge_objects(result, current);
    for (const auto& [id, _path] : plugin_roots_) {
        status = load_scope_config("plugin", id, current);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        merge_objects(result, current);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ScopeStore::load_registry(std::string_view directory,
                                  const Json& builtins,
                                  Json& result) const {
    if (!valid_simple_id(directory) || !builtins.is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::map<std::string, Json, std::less<>> merged;
    for (const auto& item : builtins) {
        if (item.is_object() && item.contains("id") &&
            item["id"].is_string()) {
            Json value = item;
            value["builtin"] = true;
            value["scope"] = "builtin";
            merged[item["id"].get<std::string>()] = std::move(value);
        }
    }

    std::vector<std::pair<std::string, const std::filesystem::path*>> roots{
        {"system", &system_root_}, {"workspace", &workspace_scope_root_}};
    for (const auto& [id, path] : plugin_roots_) {
        roots.emplace_back("plugin:" + id, &path);
    }
    for (const auto& [scope, root] : roots) {
        const auto registry_root = *root / utf8_to_wide(directory);
        std::error_code error;
        if (!std::filesystem::is_directory(registry_root, error)) {
            continue;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(
                 registry_root, error)) {
            if (error) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            if (entry.is_regular_file(error) &&
                entry.path().extension() == L".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            Json value;
            const int32_t status = load_json_file(file, value);
            if (status != SAO_AI_EDITOR_OK || !value.contains("id") ||
                !value["id"].is_string()) {
                return status == SAO_AI_EDITOR_OK
                    ? SAO_AI_EDITOR_ERR_INVALID_ARGUMENT
                    : status;
            }
            value["builtin"] = false;
            value["scope"] = scope;
            const std::string item_id = value["id"].get<std::string>();
            merged[item_id] = std::move(value);
        }
    }
    result = Json::array();
    for (auto& [id, value] : merged) {
        (void)id;
        result.push_back(std::move(value));
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ScopeStore::save_registry_item(std::string_view directory,
                                       std::string_view scope,
                                       std::string_view plugin_id,
                                       std::string_view item_id,
                                       const Json& value) const {
    const auto* root = scope_root(scope, plugin_id);
    if (root == nullptr || !valid_simple_id(directory) ||
        !valid_simple_id(item_id) || !value.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json stored = value;
    stored["id"] = item_id;
    stored.erase("builtin");
    stored.erase("scope");
    const auto path = *root / utf8_to_wide(directory) /
                      (utf8_to_wide(item_id) + L".json");
    return write_text_atomic(path, stored.dump(2));
}

std::filesystem::path ScopeStore::history_root(std::string_view scope) const {
    if (scope == "system") {
        return system_root_ / L"chat_history";
    }
    return workspace_scope_root_ / L"chat_history";
}

std::filesystem::path ScopeStore::secret_vault_path() const {
    return system_root_ / L"secrets" / L"ai_editor.vault.json";
}

}  // namespace sao::ai_editor::native
