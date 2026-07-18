#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "native_utils.h"

namespace sao::ai_editor::native {

class ScopeStore final {
public:
    int32_t initialize(std::string_view workspace_root,
                       std::string_view system_root,
                       std::string_view plugin_roots_json);

    const std::filesystem::path& workspace_root() const noexcept;
    const std::filesystem::path& workspace_scope_root() const noexcept;

    Json describe_scopes() const;
    int32_t load_merged_config(Json& result) const;
    int32_t load_scope_config(std::string_view scope,
                              std::string_view plugin_id,
                              Json& result) const;
    int32_t save_scope_config(std::string_view scope,
                              std::string_view plugin_id,
                              const Json& value) const;

    int32_t load_registry(std::string_view directory,
                          const Json& builtins,
                          Json& result) const;
    int32_t save_registry_item(std::string_view directory,
                               std::string_view scope,
                               std::string_view plugin_id,
                               std::string_view item_id,
                               const Json& value) const;

    std::filesystem::path history_root(std::string_view scope) const;
    std::filesystem::path secret_vault_path() const;

private:
    const std::filesystem::path* scope_root(std::string_view scope,
                                            std::string_view plugin_id) const;
    int32_t load_json_file(const std::filesystem::path& path,
                           Json& result) const;

    std::filesystem::path system_root_;
    std::filesystem::path workspace_root_;
    std::filesystem::path workspace_scope_root_;
    std::map<std::string, std::filesystem::path, std::less<>> plugin_roots_;
};

}  // namespace sao::ai_editor::native
