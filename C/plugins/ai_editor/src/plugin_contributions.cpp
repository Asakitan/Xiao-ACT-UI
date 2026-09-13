// SAO AI Editor — plugin manifest contributions (Python parity).

#include "plugin_contributions.h"

#include "sao/ai_editor/mcp_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace sao::ai_editor::native {

namespace {

constexpr std::size_t kMaximumContributionEntries = 4096;
constexpr std::size_t kMaximumContributionDepth = 64;
constexpr std::size_t kMaximumContributionNodes = 16384;
constexpr std::size_t kMaximumContributionStringBytes = 64U * 1024U;
constexpr std::size_t kMaximumContributionInventoryBytes = 1U * 1024U * 1024U;
constexpr std::size_t kMaximumContributionAggregateBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumContributionDiagnosticBytes = 16U * 1024U;
constexpr std::size_t kMaximumContributionDiagnosticMessageBytes = 1024U;

bool valid_manifest_string_byte(unsigned char byte) noexcept {
    return byte >= 0x20 || byte == '\t' || byte == '\n' || byte == '\r';
}

class ManifestHandle final {
  public:
    ManifestHandle() = default;
    explicit ManifestHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ManifestHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ManifestHandle(const ManifestHandle&) = delete;
    ManifestHandle& operator=(const ManifestHandle&) = delete;

    HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

int32_t read_pinned_plugin_manifest(const std::filesystem::path& canonical_root,
                                    const std::filesystem::path& canonical_manifest,
                                    std::string& text) {
    text.clear();
    if (canonical_manifest.parent_path() != canonical_root) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    ManifestHandle root_handle(CreateFileW(
        canonical_root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (root_handle.get() == INVALID_HANDLE_VALUE) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    FILE_BASIC_INFO root_info{};
    if (!GetFileInformationByHandleEx(root_handle.get(), FileBasicInfo, &root_info,
                                      sizeof(root_info))) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if ((root_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (root_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }

    ManifestHandle manifest_handle(CreateFileW(
        canonical_manifest.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (manifest_handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? SAO_AI_EDITOR_ERR_NOT_FOUND
                   : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    FILE_BASIC_INFO manifest_info{};
    LARGE_INTEGER manifest_size{};
    if (!GetFileInformationByHandleEx(manifest_handle.get(), FileBasicInfo, &manifest_info,
                                      sizeof(manifest_info)) ||
        !GetFileSizeEx(manifest_handle.get(), &manifest_size)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if ((manifest_info.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    if (manifest_size.QuadPart < 0 ||
        static_cast<uint64_t>(manifest_size.QuadPart) > kMaximumExtensionManifestBytes) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    text.resize(static_cast<std::size_t>(manifest_size.QuadPart));
    std::size_t offset = 0;
    while (offset < text.size()) {
        DWORD bytes_read = 0;
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 64U * 1024U));
        if (!ReadFile(manifest_handle.get(), text.data() + offset, chunk, &bytes_read, nullptr) ||
            bytes_read == 0) {
            text.clear();
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        offset += bytes_read;
    }
    std::filesystem::path rebound_manifest;
    if (!resolve_bounded_path(canonical_root, "plugin.json", false, rebound_manifest) ||
        rebound_manifest != canonical_manifest) {
        text.clear();
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    return SAO_AI_EDITOR_OK;
}

void truncate_utf8(std::string& value, std::size_t maximum_bytes) {
    if (value.size() <= maximum_bytes) {
        return;
    }
    if (maximum_bytes < 4) {
        value.clear();
        return;
    }
    std::size_t prefix = maximum_bytes - 3;
    while (prefix != 0 && !valid_utf8(std::string_view(value.data(), prefix))) {
        --prefix;
    }
    value.resize(prefix);
    value.append("...");
}

void push_diagnostic(std::vector<std::string>& diagnostics, std::string message) {
    if (diagnostics.size() >= kMaximumPluginContributionDiagnostics) {
        return;
    }
    if (!valid_utf8(message)) {
        message = "plugin contribution diagnostic omitted invalid UTF-8 text";
    }
    truncate_utf8(message, kMaximumContributionDiagnosticMessageBytes);
    if (message.empty() ||
        std::find(diagnostics.begin(), diagnostics.end(), message) != diagnostics.end()) {
        return;
    }
    std::size_t used_bytes = 0;
    for (const auto& existing : diagnostics) {
        if (existing.size() > kMaximumContributionDiagnosticBytes - used_bytes) {
            return;
        }
        used_bytes += existing.size();
    }
    if (used_bytes >= kMaximumContributionDiagnosticBytes) {
        return;
    }
    truncate_utf8(message, kMaximumContributionDiagnosticBytes - used_bytes);
    if (message.empty()) {
        return;
    }
    diagnostics.push_back(std::move(message));
}

void push_optional_diagnostic(std::vector<std::string>* diagnostics, std::string message) {
    if (diagnostics != nullptr) {
        push_diagnostic(*diagnostics, std::move(message));
    }
}

bool json_within_manifest_budget(const Json& value, std::size_t depth, std::size_t& nodes) {
    if (depth > kMaximumContributionDepth || nodes >= kMaximumContributionNodes) {
        return false;
    }
    ++nodes;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return text.size() <= kMaximumContributionStringBytes && valid_utf8(text) &&
               std::none_of(text.begin(), text.end(), [](unsigned char byte) {
                   return !valid_manifest_string_byte(byte) || byte == 0x7f;
               });
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (!json_within_manifest_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (key.size() > 1024 || !valid_utf8(key) ||
                std::any_of(key.begin(), key.end(),
                            [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }) ||
                !json_within_manifest_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        return false;
    } else if (!value.is_null() && !value.is_boolean() && !value.is_number()) {
        return false;
    }
    return true;
}

ManifestTextParseResult parse_manifest_text_impl(std::string_view text, Json& result) {
    if (!valid_utf8(text) || text.find('\0') != std::string_view::npos) {
        return ManifestTextParseResult::invalid_value;
    }
    bool duplicate_key = false;
    std::vector<std::unordered_set<std::string>> object_keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            if (object_keys.empty() || !parsed.is_string() ||
                !object_keys.back().insert(parsed.get_ref<const std::string&>()).second) {
                duplicate_key = true;
            }
        } else if (event == Json::parse_event_t::object_end && !object_keys.empty()) {
            object_keys.pop_back();
        }
        return true;
    };
    result = Json::parse(text, callback, false, false);
    if (result.is_discarded()) {
        return ManifestTextParseResult::invalid;
    }
    if (duplicate_key) {
        return ManifestTextParseResult::duplicate_key;
    }
    std::size_t nodes = 0;
    return json_within_manifest_budget(result, 0, nodes) ? ManifestTextParseResult::ok
                                                         : ManifestTextParseResult::invalid_value;
}

bool valid_contribution_id(std::string_view value) {
    return !value.empty() && value.size() <= 256 && valid_utf8(value) &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; });
}

bool valid_string_array(const Json& value) {
    if (!value.is_array()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const Json& item) {
        return item.is_string() && valid_contribution_id(item.get_ref<const std::string&>());
    });
}

void owner_tag(Json& item, std::string_view owner_id,
               const ManifestContributionInventory* inventory = nullptr) {
    const std::string owner(owner_id);
    for (const char* key :
         {"extensionPath", "_extensionPath", "extensionIndex", "ownerPath", "ownerIndex",
          "available", "applied", "generation", "runtimeState", "runtimeOnly", "frontendSnapshot",
          "runtimeAvailable", "dynamicSource", "surfaceEvidence"}) {
        item.erase(key);
    }
    item["extensionId"] = owner;
    item["extension_id"] = owner;
    item["_extensionId"] = owner;
    item["owner"] = owner;
    item["ownerId"] = owner;
    item["source"] = "manifest";
    if (inventory != nullptr && !inventory->owner_path.empty() &&
        inventory->owner_index.has_value()) {
        item["extensionPath"] = inventory->owner_path;
        item["_extensionPath"] = inventory->owner_path;
        item["extensionIndex"] = *inventory->owner_index;
        item["ownerPath"] = inventory->owner_path;
        item["ownerIndex"] = *inventory->owner_index;
    }
}

std::optional<std::string> contribution_fingerprint(const Json& source, std::string_view owner_id) {
    if (!source.is_object()) {
        return std::nullopt;
    }
    Json normalized = source;
    for (const char* key :
         {"extensionId", "extension_id", "_extensionId", "owner", "ownerId", "source",
          "runtimeAvailable", "dynamicSource", "surfaceEvidence", "configurationIndex", "nodePath",
          "extensionPath", "_extensionPath", "extensionIndex", "ownerPath", "ownerIndex"}) {
        normalized.erase(key);
    }
    owner_tag(normalized, owner_id);
    try {
        return normalized.dump();
    } catch (...) {
        return std::nullopt;
    }
}

bool append_owned(const Json& source, Json& target, std::string_view owner_id,
                  ManifestContributionInventory& inventory, std::vector<std::string>* diagnostics,
                  std::string_view kind) {
    if (!source.is_object()) {
        push_optional_diagnostic(diagnostics, "skipped non-object " + std::string(kind) +
                                                  " contribution for \"" + std::string(owner_id) +
                                                  "\"");
        return false;
    }
    if (inventory.total_entries >= kMaximumContributionEntries) {
        inventory.truncated = true;
        push_optional_diagnostic(diagnostics, "truncated contribution inventory for \"" +
                                                  std::string(owner_id) + "\"");
        return false;
    }
    Json item = source;
    owner_tag(item, owner_id, &inventory);
    std::size_t serialized_size = 0;
    try {
        serialized_size = item.dump().size();
    } catch (...) {
        push_optional_diagnostic(diagnostics, "skipped unserializable " + std::string(kind) +
                                                  " contribution for \"" + std::string(owner_id) +
                                                  "\"");
        return false;
    }
    if (serialized_size > kMaximumContributionInventoryBytes - inventory.total_bytes) {
        inventory.truncated = true;
        push_optional_diagnostic(diagnostics, "truncated contribution inventory for \"" +
                                                  std::string(owner_id) + "\"");
        return false;
    }
    target.push_back(std::move(item));
    ++inventory.total_entries;
    inventory.total_bytes += serialized_size;
    return true;
}

std::string string_member(const Json& value, std::string_view key) {
    if (!value.is_object()) {
        return {};
    }
    const auto found = value.find(std::string(key));
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

bool remember_unique_id(std::unordered_set<std::string>& ids, const std::string& id,
                        std::string_view kind, std::string_view owner_id,
                        std::vector<std::string>* diagnostics, bool* duplicate_found = nullptr) {
    if (ids.insert(id).second) {
        return true;
    }
    push_optional_diagnostic(diagnostics, "skipped duplicate " + std::string(kind) + " id \"" + id +
                                              "\" for \"" + std::string(owner_id) + "\"");
    if (duplicate_found != nullptr) {
        *duplicate_found = true;
    }
    return false;
}

void append_unique_array(const Json& source, Json& target, std::string_view id_key,
                         std::unordered_set<std::string>& ids, std::string_view owner_id,
                         ManifestContributionInventory& inventory,
                         std::vector<std::string>* diagnostics, std::string_view kind,
                         bool* duplicate_found) {
    const auto append = [&](const Json& item) {
        if (!item.is_object()) {
            push_optional_diagnostic(diagnostics, "skipped non-object " + std::string(kind) +
                                                      " contribution for \"" +
                                                      std::string(owner_id) + "\"");
            return;
        }
        const std::string id = string_member(item, id_key);
        if (!valid_contribution_id(id)) {
            push_optional_diagnostic(diagnostics, "skipped " + std::string(kind) +
                                                      " with invalid id for \"" +
                                                      std::string(owner_id) + "\"");
            return;
        }
        bool valid_shape = true;
        if (kind == "notebook" || kind == "custom editor") {
            const auto selector = item.find("selector");
            valid_shape = selector == item.end() || selector->is_array();
        } else if (kind == "debugger") {
            const auto attributes = item.find("configurationAttributes");
            const auto initials = item.find("initialConfigurations");
            const auto snippets = item.find("configurationSnippets");
            const auto variables = item.find("variables");
            const auto languages = item.find("languages");
            valid_shape = (attributes == item.end() || attributes->is_object()) &&
                          (initials == item.end() || initials->is_array()) &&
                          (snippets == item.end() || snippets->is_array()) &&
                          (variables == item.end() || variables->is_object()) &&
                          (languages == item.end() || valid_string_array(*languages));
        } else if (kind == "task definition") {
            const auto required = item.find("required");
            const auto properties = item.find("properties");
            valid_shape = (required == item.end() || valid_string_array(*required)) &&
                          (properties == item.end() || properties->is_object());
        }
        if (!valid_shape) {
            push_optional_diagnostic(diagnostics, "skipped " + std::string(kind) +
                                                      " with invalid shape for \"" +
                                                      std::string(owner_id) + "\"");
            return;
        }
        if (!remember_unique_id(ids, id, kind, owner_id, diagnostics, duplicate_found)) {
            return;
        }
        (void)append_owned(item, target, owner_id, inventory, diagnostics, kind);
    };
    if (!source.is_array()) {
        push_optional_diagnostic(diagnostics, "skipped non-array " + std::string(kind) +
                                                  " contributions for \"" + std::string(owner_id) +
                                                  "\"");
        return;
    }
    for (const auto& item : source) {
        if (inventory.truncated) {
            break;
        }
        append(item);
    }
}

bool valid_configuration_name(std::string_view value) {
    return !value.empty() && value.size() <= 1024 && valid_utf8(value) &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; });
}

void append_configuration_node(const Json& node, std::size_t configuration_index,
                               const std::string& node_path,
                               const std::optional<std::string>& inherited_scope,
                               const std::unordered_set<std::string>& inherited_restricted,
                               std::string_view display_name, Json& target,
                               std::unordered_set<std::string>& fingerprints,
                               std::string_view owner_id, ManifestContributionInventory& inventory,
                               std::vector<std::string>* diagnostics, std::size_t depth,
                               bool* duplicate_found) {
    if (inventory.truncated) {
        return;
    }
    if (!node.is_object() || depth > kMaximumContributionDepth) {
        push_optional_diagnostic(diagnostics, "skipped invalid configuration node for \"" +
                                                  std::string(owner_id) + "\"");
        return;
    }

    for (const char* key : {"id", "title", "description", "markdownDescription"}) {
        const auto value = node.find(key);
        if (value != node.end() &&
            (!value->is_string() ||
             value->get_ref<const std::string&>().size() > kMaximumContributionStringBytes ||
             !valid_utf8(value->get_ref<const std::string&>()) ||
             value->get_ref<const std::string&>().find('\0') != std::string::npos)) {
            push_optional_diagnostic(diagnostics,
                                     "skipped configuration node with invalid text for \"" +
                                         std::string(owner_id) + "\"");
            return;
        }
    }
    const auto order = node.find("order");
    if (order != node.end() && !order->is_number()) {
        push_optional_diagnostic(diagnostics,
                                 "skipped configuration node with invalid order for \"" +
                                     std::string(owner_id) + "\"");
        return;
    }

    std::optional<std::string> scope = inherited_scope;
    const auto node_scope = node.find("scope");
    if (node_scope != node.end() && !node_scope->is_null()) {
        if (!node_scope->is_string() ||
            !valid_configuration_name(node_scope->get_ref<const std::string&>())) {
            push_optional_diagnostic(diagnostics,
                                     "skipped configuration node with invalid scope for \"" +
                                         std::string(owner_id) + "\"");
            return;
        }
        scope = node_scope->get<std::string>();
    }

    std::unordered_set<std::string> restricted = inherited_restricted;
    const auto restricted_properties = node.find("restrictedProperties");
    if (restricted_properties != node.end() && !restricted_properties->is_null()) {
        if (!restricted_properties->is_array()) {
            push_optional_diagnostic(
                diagnostics, "skipped configuration node with invalid restrictedProperties for \"" +
                                 std::string(owner_id) + "\"");
            return;
        }
        for (const auto& value : *restricted_properties) {
            if (!value.is_string() ||
                !valid_configuration_name(value.get_ref<const std::string&>())) {
                push_optional_diagnostic(
                    diagnostics,
                    "skipped configuration node with invalid restricted property for \"" +
                        std::string(owner_id) + "\"");
                return;
            }
            restricted.insert(value.get<std::string>());
        }
    }

    const std::string node_id = string_member(node, "id");
    const std::string title = string_member(node, "title");
    const Json extension_info{{"id", std::string(owner_id)},
                              {"displayName", std::string(display_name)}};
    Json section{{"id", node_id}, {"title", title}, {"extensionInfo", extension_info}};
    if (order != node.end()) {
        section["order"] = *order;
    }

    Json properties = Json::object();
    const auto raw_properties = node.find("properties");
    if (raw_properties != node.end()) {
        if (!raw_properties->is_object()) {
            push_optional_diagnostic(
                diagnostics, "skipped configuration node with non-object properties for \"" +
                                 std::string(owner_id) + "\"");
            return;
        }
        std::size_t property_index = 0;
        for (const auto& [key, raw_schema] : raw_properties->items()) {
            const std::size_t current_index = property_index++;
            if (!valid_configuration_name(key) || !raw_schema.is_object()) {
                push_optional_diagnostic(diagnostics,
                                         "skipped invalid configuration property for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            Json schema = raw_schema;
            schema.erase("_extensionId");
            schema.erase("_propertyOrder");
            schema.erase("section");
            schema.erase("source");
            const auto property_scope = schema.find("scope");
            bool has_property_scope = false;
            if (property_scope != schema.end()) {
                if (property_scope->is_null()) {
                    schema.erase(property_scope);
                } else if (!property_scope->is_string() ||
                           !valid_configuration_name(
                               property_scope->get_ref<const std::string&>())) {
                    push_optional_diagnostic(
                        diagnostics, "skipped configuration property with invalid scope for \"" +
                                         std::string(owner_id) + "\"");
                    continue;
                } else {
                    has_property_scope = true;
                }
            }
            if (key.front() != '[' && scope.has_value() && !has_property_scope) {
                schema["scope"] = *scope;
            }
            if (restricted.contains(key) &&
                (!schema.contains("restricted") || schema["restricted"].is_null())) {
                schema["restricted"] = true;
            }
            schema["_propertyOrder"] = current_index;
            schema["section"] = section;
            schema["source"] = extension_info;
            properties[key] = std::move(schema);
        }
    }

    if (!properties.empty()) {
        Json item{{"id", node_id},
                  {"title", title},
                  {"display_name", std::string(display_name)},
                  {"extensionInfo", extension_info},
                  {"configurationIndex", configuration_index},
                  {"nodePath", node_path},
                  {"properties", std::move(properties)}};
        const auto markdown_description = node.find("markdownDescription");
        const auto description = node.find("description");
        if (markdown_description != node.end()) {
            item["description"] = *markdown_description;
        } else if (description != node.end()) {
            item["description"] = *description;
        } else {
            item["description"] = "";
        }
        if (order != node.end()) {
            item["order"] = *order;
        }
        if (scope.has_value()) {
            item["scope"] = *scope;
        }
        std::vector<std::string> sorted_restricted(restricted.begin(), restricted.end());
        std::sort(sorted_restricted.begin(), sorted_restricted.end());
        item["restrictedProperties"] = sorted_restricted;
        const auto fingerprint = contribution_fingerprint(item, owner_id);
        if (!fingerprint.has_value()) {
            push_optional_diagnostic(diagnostics,
                                     "skipped unserializable configuration contribution for \"" +
                                         std::string(owner_id) + "\"");
        } else if (!fingerprints.insert(*fingerprint).second) {
            if (duplicate_found != nullptr) {
                *duplicate_found = true;
            }
            push_optional_diagnostic(diagnostics,
                                     "skipped duplicate configuration contribution for \"" +
                                         std::string(owner_id) + "\"");
        } else {
            (void)append_owned(item, target, owner_id, inventory, diagnostics, "configuration");
        }
    }

    const auto all_of = node.find("allOf");
    if (all_of == node.end()) {
        return;
    }
    if (!all_of->is_array()) {
        push_optional_diagnostic(diagnostics,
                                 "skipped configuration allOf with invalid shape for \"" +
                                     std::string(owner_id) + "\"");
        return;
    }
    for (std::size_t index = 0; index < all_of->size(); ++index) {
        if (inventory.truncated) {
            break;
        }
        const std::string child_path = node_path.empty()
                                           ? "allOf[" + std::to_string(index) + "]"
                                           : node_path + ".allOf[" + std::to_string(index) + "]";
        append_configuration_node((*all_of)[index], configuration_index, child_path, scope,
                                  restricted, display_name, target, fingerprints, owner_id,
                                  inventory, diagnostics, depth + 1, duplicate_found);
    }
}

void append_configuration_contributions(const Json& source, std::size_t& next_configuration_index,
                                        std::string_view display_name, Json& target,
                                        std::unordered_set<std::string>& fingerprints,
                                        std::string_view owner_id,
                                        ManifestContributionInventory& inventory,
                                        std::vector<std::string>* diagnostics,
                                        bool* duplicate_found) {
    if (source.is_object()) {
        append_configuration_node(source, next_configuration_index++, "", std::nullopt, {},
                                  display_name, target, fingerprints, owner_id, inventory,
                                  diagnostics, 0, duplicate_found);
        return;
    }
    if (!source.is_array()) {
        push_optional_diagnostic(diagnostics,
                                 "skipped configuration contributions with invalid shape for \"" +
                                     std::string(owner_id) + "\"");
        return;
    }
    for (const auto& node : source) {
        const std::size_t configuration_index = next_configuration_index++;
        append_configuration_node(node, configuration_index, "", std::nullopt, {}, display_name,
                                  target, fingerprints, owner_id, inventory, diagnostics, 0,
                                  duplicate_found);
        if (inventory.truncated) {
            break;
        }
    }
}

bool finalize_inventory_budget(ManifestContributionInventory& inventory,
                               std::size_t additional_bytes = 0) {
    if (additional_bytes > kMaximumContributionInventoryBytes) {
        return false;
    }
    const std::size_t maximum_inventory_bytes =
        kMaximumContributionInventoryBytes - additional_bytes;
    const std::array<std::pair<const char*, Json*>, 11> families{{
        {"commands", &inventory.commands},
        {"viewContainers", &inventory.view_containers},
        {"views", &inventory.views},
        {"menus", &inventory.menus},
        {"configurations", &inventory.configurations},
        {"notebooks", &inventory.notebooks},
        {"debuggers", &inventory.debuggers},
        {"taskDefinitions", &inventory.task_definitions},
        {"customEditors", &inventory.custom_editors},
        {"mcpServers", &inventory.mcp_servers},
        {"chatProviders", &inventory.chat_providers},
    }};
    const std::array<Json*, 11> family_values{
        &inventory.commands,    &inventory.view_containers,  &inventory.views,
        &inventory.menus,       &inventory.configurations,   &inventory.notebooks,
        &inventory.debuggers,   &inventory.task_definitions, &inventory.custom_editors,
        &inventory.mcp_servers, &inventory.chat_providers};
    const std::array<Json*, 11> trim_order{
        &inventory.custom_editors, &inventory.task_definitions, &inventory.debuggers,
        &inventory.notebooks,      &inventory.configurations,   &inventory.menus,
        &inventory.views,          &inventory.view_containers,  &inventory.commands,
        &inventory.chat_providers, &inventory.mcp_servers};
    std::size_t total_entries = 0;
    std::size_t total_bytes = 0;
    try {
        for (const Json* family : family_values) {
            if (!family->is_array()) {
                return false;
            }
            for (const auto& item : *family) {
                const std::size_t bytes = item.dump().size();
                if (bytes > std::numeric_limits<std::size_t>::max() - total_bytes) {
                    return false;
                }
                total_bytes += bytes;
                ++total_entries;
            }
        }
        inventory.total_entries = total_entries;
        inventory.total_bytes = total_bytes;
        for (;;) {
            Json envelope{{"available", true},
                          {"summary", inventory.summary()},
                          {"totalEntries", inventory.total_entries},
                          {"serializedBytes", inventory.total_bytes},
                          {"truncated", inventory.truncated}};
            std::size_t comma_bytes = 0;
            for (const auto& [name, family] : families) {
                envelope[name] = Json::array();
                if (family->size() > 1) {
                    comma_bytes += family->size() - 1;
                }
            }
            const std::size_t envelope_bytes = envelope.dump().size();
            if (inventory.total_bytes > std::numeric_limits<std::size_t>::max() - comma_bytes ||
                envelope_bytes >
                    std::numeric_limits<std::size_t>::max() - inventory.total_bytes - comma_bytes) {
                return false;
            }
            const std::size_t serialized_bytes =
                envelope_bytes + inventory.total_bytes + comma_bytes;
            if (inventory.total_entries <= kMaximumContributionEntries &&
                serialized_bytes <= maximum_inventory_bytes) {
                return true;
            }

            std::size_t bytes_to_remove = serialized_bytes > maximum_inventory_bytes
                                              ? serialized_bytes - maximum_inventory_bytes
                                              : 0;
            std::size_t entries_to_remove =
                inventory.total_entries > kMaximumContributionEntries
                    ? inventory.total_entries - kMaximumContributionEntries
                    : 0;
            bool removed = false;
            for (auto family = trim_order.begin();
                 family != trim_order.end() && (bytes_to_remove != 0 || entries_to_remove != 0);
                 ++family) {
                while (!(**family).empty() && (bytes_to_remove != 0 || entries_to_remove != 0)) {
                    const std::size_t item_bytes = (**family).back().dump().size();
                    const std::size_t removed_bytes =
                        item_bytes + ((**family).size() > 1 ? 1U : 0U);
                    inventory.total_bytes -= item_bytes;
                    --inventory.total_entries;
                    (**family).erase(std::prev((**family).end()));
                    bytes_to_remove =
                        removed_bytes >= bytes_to_remove ? 0 : bytes_to_remove - removed_bytes;
                    if (entries_to_remove != 0) {
                        --entries_to_remove;
                    }
                    inventory.truncated = true;
                    removed = true;
                }
            }
            if (!removed) {
                return false;
            }
        }
    } catch (...) {
        return false;
    }
}

} // namespace

ManifestTextParseResult parse_manifest_text_strict(std::string_view text, Json& result) {
    try {
        return parse_manifest_text_impl(text, result);
    } catch (...) {
        result = Json();
        return ManifestTextParseResult::invalid;
    }
}

Json ManifestContributionInventory::summary() const {
    std::size_t activity_bar_items = 0;
    for (const auto& container : view_containers) {
        if (string_member(container, "location") == "activitybar") {
            ++activity_bar_items;
        }
    }
    std::size_t tree_views = 0;
    std::size_t webview_views = 0;
    for (const auto& view : views) {
        if (string_member(view, "runtimeKind") == "webviewView") {
            ++webview_views;
        } else {
            ++tree_views;
        }
    }
    return Json{{"commands", commands.size()},
                {"viewContainers", view_containers.size()},
                {"activityBarItems", activity_bar_items},
                {"views", views.size()},
                {"treeViews", tree_views},
                {"webviewViews", webview_views},
                {"menus", menus.size()},
                {"configurations", configurations.size()},
                {"notebooks", notebooks.size()},
                {"debuggers", debuggers.size()},
                {"taskDefinitions", task_definitions.size()},
                {"customEditors", custom_editors.size()},
                {"mcpServers", mcp_servers.size()},
                {"chatProviders", chat_providers.size()},
                {"dynamicSurfaces", total_entries},
                {"serializedBytes", total_bytes},
                {"truncated", truncated}};
}

Json ManifestContributionInventory::to_json() const {
    return Json{{"available", true},
                {"commands", commands},
                {"viewContainers", view_containers},
                {"views", views},
                {"menus", menus},
                {"configurations", configurations},
                {"notebooks", notebooks},
                {"debuggers", debuggers},
                {"taskDefinitions", task_definitions},
                {"customEditors", custom_editors},
                {"mcpServers", mcp_servers},
                {"chatProviders", chat_providers},
                {"summary", summary()},
                {"totalEntries", total_entries},
                {"serializedBytes", total_bytes},
                {"truncated", truncated}};
}

bool parse_manifest_contribution_inventory(const Json& manifest, std::string_view owner_id,
                                           ManifestContributionInventory& out,
                                           std::vector<std::string>* diagnostics,
                                           std::string_view owner_path,
                                           std::optional<std::size_t> owner_index) {
    out = {};
    if (!manifest.is_object()) {
        push_optional_diagnostic(diagnostics, "manifest contribution root is not an object");
        return false;
    }
    if (!valid_utf8(owner_id) || !valid_simple_id(owner_id)) {
        push_optional_diagnostic(diagnostics, "manifest contribution owner id is invalid");
        return false;
    }
    if (owner_index.has_value() &&
        (owner_path.empty() || owner_path.size() > kMaximumContributionStringBytes ||
         !valid_utf8(owner_path) ||
         std::any_of(owner_path.begin(), owner_path.end(),
                     [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }))) {
        push_optional_diagnostic(diagnostics, "manifest contribution owner path is invalid");
        return false;
    }
    if (!owner_index.has_value() && !owner_path.empty()) {
        push_optional_diagnostic(diagnostics, "manifest contribution owner index is missing");
        return false;
    }
    out.owner_path = std::string(owner_path);
    out.owner_index = owner_index;
    std::size_t nodes = 0;
    try {
        if (!json_within_manifest_budget(manifest, 0, nodes) ||
            manifest.dump().size() > kMaximumExtensionManifestBytes) {
            push_optional_diagnostic(diagnostics,
                                     "manifest contribution inventory exceeds limits for \"" +
                                         std::string(owner_id) + "\"");
            return false;
        }
    } catch (...) {
        push_optional_diagnostic(diagnostics,
                                 "manifest contribution inventory is not serializable for \"" +
                                     std::string(owner_id) + "\"");
        return false;
    }

    const auto contributes = manifest.find("contributes");
    if (contributes != manifest.end() && !contributes->is_object() && !contributes->is_null()) {
        push_optional_diagnostic(diagnostics, "manifest contributes field is not an object for \"" +
                                                  std::string(owner_id) + "\"");
        return false;
    }
    const Json empty = Json::object();
    const Json& contribution_root =
        contributes != manifest.end() && contributes->is_object() ? *contributes : empty;
    std::string display_name = string_member(manifest, "displayName");
    if (display_name.empty()) {
        display_name = string_member(manifest, "name");
    }
    if (display_name.empty()) {
        display_name = std::string(owner_id);
    }

    std::unordered_set<std::string> command_ids;
    std::unordered_set<std::string> container_ids;
    std::unordered_set<std::string> view_ids;
    std::unordered_set<std::string> menu_fingerprints;
    std::unordered_set<std::string> configuration_fingerprints;
    std::unordered_set<std::string> notebook_ids;
    std::unordered_set<std::string> debugger_ids;
    std::unordered_set<std::string> task_definition_ids;
    std::unordered_set<std::string> custom_editor_ids;
    std::unordered_set<std::string> mcp_ids;
    std::unordered_set<std::string> chat_provider_ids;
    bool duplicate_contribution = false;

    const auto append_mcp = [&](const Json& servers) {
        if (!servers.is_object()) {
            push_optional_diagnostic(diagnostics,
                                     "skipped non-object MCP server contributions for \"" +
                                         std::string(owner_id) + "\"");
            return;
        }
        for (const auto& [raw_id, config] : servers.items()) {
            if (out.truncated) {
                break;
            }
            if (!valid_manifest_provider_id(raw_id)) {
                push_optional_diagnostic(diagnostics, "skipped MCP server with invalid id for \"" +
                                                          std::string(owner_id) + "\"");
                continue;
            }
            if (!config.is_object()) {
                push_optional_diagnostic(diagnostics,
                                         "skipped MCP server with invalid configuration for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            const std::string id = std::string(owner_id) + "." + raw_id;
            if (!valid_manifest_provider_id(id)) {
                push_optional_diagnostic(diagnostics,
                                         "skipped invalid namespaced MCP server id for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            if (!remember_unique_id(mcp_ids, raw_id, "MCP server", owner_id, diagnostics,
                                    &duplicate_contribution)) {
                continue;
            }
            Json item = config;
            item["rawId"] = raw_id;
            item["id"] = id;
            (void)append_owned(item, out.mcp_servers, owner_id, out, diagnostics, "MCP server");
        }
    };
    const auto append_chat = [&](const Json& providers) {
        if (!providers.is_array()) {
            push_optional_diagnostic(diagnostics,
                                     "skipped non-array chat provider contributions for \"" +
                                         std::string(owner_id) + "\"");
            return;
        }
        for (const auto& raw : providers) {
            if (out.truncated) {
                break;
            }
            if (!raw.is_object()) {
                push_optional_diagnostic(diagnostics,
                                         "skipped non-object chat provider contribution for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            const std::string raw_id = string_member(raw, "id");
            if (!valid_manifest_provider_id(raw_id)) {
                push_optional_diagnostic(diagnostics,
                                         "skipped unsafe chat provider id in plugin \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            const std::string id = std::string(owner_id) + "." + raw_id;
            if (!valid_manifest_provider_id(id)) {
                push_optional_diagnostic(diagnostics,
                                         "skipped invalid namespaced chat provider id for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            if (!remember_unique_id(chat_provider_ids, raw_id, "chat provider", owner_id,
                                    diagnostics, &duplicate_contribution)) {
                continue;
            }
            Json item = raw;
            item["rawId"] = raw_id;
            item["id"] = id;
            (void)append_owned(item, out.chat_providers, owner_id, out, diagnostics,
                               "chat provider");
        }
    };
    const auto top_mcp = manifest.find("mcpServers");
    if (top_mcp != manifest.end()) {
        append_mcp(*top_mcp);
    }
    const auto contributed_mcp = contribution_root.find("mcpServers");
    if (contributed_mcp != contribution_root.end()) {
        append_mcp(*contributed_mcp);
    }
    const auto top_chat = manifest.find("chatProviders");
    if (top_chat != manifest.end()) {
        append_chat(*top_chat);
    }
    const auto contributed_chat = contribution_root.find("chatProviders");
    if (contributed_chat != contribution_root.end()) {
        append_chat(*contributed_chat);
    }

    const auto commands = contribution_root.find("commands");
    if (commands != contribution_root.end() && !commands->is_array()) {
        push_optional_diagnostic(diagnostics, "skipped non-array command contributions for \"" +
                                                  std::string(owner_id) + "\"");
    } else if (commands != contribution_root.end()) {
        for (const auto& raw : *commands) {
            if (out.truncated) {
                break;
            }
            if (!raw.is_object()) {
                push_optional_diagnostic(diagnostics,
                                         "skipped non-object command contribution for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            const std::string command = string_member(raw, "command");
            if (!valid_contribution_id(command)) {
                push_optional_diagnostic(diagnostics, "skipped command with invalid id for \"" +
                                                          std::string(owner_id) + "\"");
                continue;
            }
            if (!remember_unique_id(command_ids, command, "command", owner_id, diagnostics,
                                    &duplicate_contribution)) {
                continue;
            }
            Json item = raw;
            if (!item.contains("title") || !item["title"].is_string()) {
                item["title"] = command;
            }
            item["runtimeAvailable"] = false;
            item["dynamicSource"] = "manifest";
            (void)append_owned(item, out.commands, owner_id, out, diagnostics, "command");
        }
    }

    const auto containers = contribution_root.find("viewsContainers");
    if (containers != contribution_root.end() && !containers->is_object()) {
        push_optional_diagnostic(diagnostics,
                                 "skipped non-object view container contributions for \"" +
                                     std::string(owner_id) + "\"");
    } else if (containers != contribution_root.end()) {
        for (const auto& [location, list] : containers->items()) {
            if (!valid_contribution_id(location)) {
                push_optional_diagnostic(diagnostics,
                                         "skipped view container location with invalid id for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            if (!list.is_array()) {
                push_optional_diagnostic(diagnostics,
                                         "skipped non-array view container location for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            for (const auto& raw : list) {
                if (out.truncated) {
                    break;
                }
                if (!raw.is_object()) {
                    push_optional_diagnostic(
                        diagnostics, "skipped non-object view container contribution for \"" +
                                         std::string(owner_id) + "\"");
                    continue;
                }
                const std::string id = string_member(raw, "id");
                if (!valid_contribution_id(id)) {
                    push_optional_diagnostic(diagnostics,
                                             "skipped view container with invalid id for \"" +
                                                 std::string(owner_id) + "\"");
                    continue;
                }
                if (!remember_unique_id(container_ids, id, "view container", owner_id, diagnostics,
                                        &duplicate_contribution)) {
                    continue;
                }
                Json item = raw;
                item["location"] = location;
                item["runtimeAvailable"] = false;
                item["dynamicSource"] = "manifest";
                (void)append_owned(item, out.view_containers, owner_id, out, diagnostics,
                                   "view container");
            }
        }
    }

    const auto views = contribution_root.find("views");
    if (views != contribution_root.end() && !views->is_object()) {
        push_optional_diagnostic(diagnostics, "skipped non-object view contributions for \"" +
                                                  std::string(owner_id) + "\"");
    } else if (views != contribution_root.end()) {
        for (const auto& [container, list] : views->items()) {
            if (!valid_contribution_id(container)) {
                push_optional_diagnostic(diagnostics,
                                         "skipped view location with invalid id for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            if (!list.is_array()) {
                push_optional_diagnostic(diagnostics, "skipped non-array view location for \"" +
                                                          std::string(owner_id) + "\"");
                continue;
            }
            for (const auto& raw : list) {
                if (out.truncated) {
                    break;
                }
                if (!raw.is_object()) {
                    push_optional_diagnostic(diagnostics,
                                             "skipped non-object view contribution for \"" +
                                                 std::string(owner_id) + "\"");
                    continue;
                }
                const std::string id = string_member(raw, "id");
                if (!valid_contribution_id(id)) {
                    push_optional_diagnostic(diagnostics, "skipped view with invalid id for \"" +
                                                              std::string(owner_id) + "\"");
                    continue;
                }
                const auto declared_type = raw.find("type");
                if (declared_type != raw.end() &&
                    (!declared_type->is_string() ||
                     (declared_type->get_ref<const std::string&>() != "tree" &&
                      declared_type->get_ref<const std::string&>() != "webview"))) {
                    push_optional_diagnostic(diagnostics, "skipped view with invalid type for \"" +
                                                              std::string(owner_id) + "\"");
                    continue;
                }
                if (!remember_unique_id(view_ids, id, "view", owner_id, diagnostics,
                                        &duplicate_contribution)) {
                    continue;
                }
                Json item = raw;
                item["container"] = container;
                const std::string type = string_member(item, "type");
                item["runtimeKind"] = type == "webview" ? "webviewView" : "treeView";
                item["runtimeAvailable"] = false;
                item["dynamicSource"] = "manifest";
                (void)append_owned(item, out.views, owner_id, out, diagnostics, "view");
            }
        }
    }

    const auto menus = contribution_root.find("menus");
    if (menus != contribution_root.end() && !menus->is_object()) {
        push_optional_diagnostic(diagnostics, "skipped non-object menu contributions for \"" +
                                                  std::string(owner_id) + "\"");
    } else if (menus != contribution_root.end()) {
        for (const auto& [menu, list] : menus->items()) {
            if (!valid_contribution_id(menu)) {
                push_optional_diagnostic(diagnostics,
                                         "skipped menu location with invalid id for \"" +
                                             std::string(owner_id) + "\"");
                continue;
            }
            if (!list.is_array()) {
                push_optional_diagnostic(diagnostics, "skipped non-array menu location for \"" +
                                                          std::string(owner_id) + "\"");
                continue;
            }
            for (const auto& raw : list) {
                if (out.truncated) {
                    break;
                }
                if (!raw.is_object()) {
                    push_optional_diagnostic(diagnostics,
                                             "skipped non-object menu contribution for \"" +
                                                 std::string(owner_id) + "\"");
                    continue;
                }
                const std::string command = string_member(raw, "command");
                const std::string submenu = string_member(raw, "submenu");
                const bool declares_command = raw.contains("command");
                const bool declares_submenu = raw.contains("submenu");
                if (declares_command == declares_submenu ||
                    (declares_command && !valid_contribution_id(command)) ||
                    (declares_submenu && !valid_contribution_id(submenu))) {
                    push_optional_diagnostic(
                        diagnostics,
                        "skipped menu contribution with invalid command/submenu shape for \"" +
                            std::string(owner_id) + "\"");
                    continue;
                }
                Json item = raw;
                item["menu"] = menu;
                const auto fingerprint = contribution_fingerprint(item, owner_id);
                if (!fingerprint.has_value()) {
                    push_optional_diagnostic(diagnostics,
                                             "skipped unserializable menu contribution for \"" +
                                                 std::string(owner_id) + "\"");
                    continue;
                }
                if (!menu_fingerprints.insert(*fingerprint).second) {
                    duplicate_contribution = true;
                    push_optional_diagnostic(diagnostics,
                                             "skipped duplicate menu contribution for \"" +
                                                 std::string(owner_id) + "\"");
                    continue;
                }
                item["runtimeAvailable"] = false;
                item["dynamicSource"] = "manifest";
                (void)append_owned(item, out.menus, owner_id, out, diagnostics, "menu");
            }
        }
    }

    std::size_t next_configuration_index = 0;
    for (const char* key : {"configuration", "configurations"}) {
        const auto value = contribution_root.find(key);
        if (value != contribution_root.end()) {
            append_configuration_contributions(*value, next_configuration_index, display_name,
                                               out.configurations, configuration_fingerprints,
                                               owner_id, out, diagnostics, &duplicate_contribution);
        }
    }
    for (const auto& [key, target, kind, id_key, ids] :
         {std::tuple<const char*, Json*, const char*, const char*,
                     std::unordered_set<std::string>*>{"notebooks", &out.notebooks, "notebook",
                                                       "type", &notebook_ids},
          {"debuggers", &out.debuggers, "debugger", "type", &debugger_ids},
          {"taskDefinitions", &out.task_definitions, "task definition", "type",
           &task_definition_ids},
          {"customEditors", &out.custom_editors, "custom editor", "viewType",
           &custom_editor_ids}}) {
        const auto value = contribution_root.find(key);
        if (value != contribution_root.end()) {
            append_unique_array(*value, *target, id_key, *ids, owner_id, out, diagnostics, kind,
                                &duplicate_contribution);
        }
    }

    if (duplicate_contribution) {
        return false;
    }

    const auto mark_manifest_only = [](Json& items, std::string_view kind) {
        for (auto& item : items) {
            if (!item.is_object()) {
                continue;
            }
            const std::string evidence_kind =
                kind == "view" ? item.value("runtimeKind", std::string{"treeView"})
                               : std::string(kind);
            item["runtimeAvailable"] = false;
            item["dynamicSource"] = "manifest";
            Json evidence{{"kind", evidence_kind},
                          {"runtimeAvailable", false},
                          {"readiness", "manifest-only"},
                          {"readinessScore", 0},
                          {"readinessIssues", Json::array({"provider-unavailable"})}};
            if (evidence_kind == "command") {
                evidence["command"] = item.value("command", std::string{});
            } else if (evidence_kind == "menu") {
                evidence["menu"] = item.value("menu", std::string{});
                evidence["command"] = item.value("command", std::string{});
                evidence["submenu"] = item.value("submenu", std::string{});
            } else if (evidence_kind == "viewContainer") {
                evidence["location"] = item.value("location", std::string{});
                evidence["viewCount"] = 0;
            } else if (evidence_kind == "treeView" || evidence_kind == "webviewView") {
                evidence["container"] = item.value("container", std::string{});
            } else if (evidence_kind == "configuration") {
                const Json properties = item.value("properties", Json::object());
                evidence["propertyCount"] = properties.is_object() ? properties.size() : 0;
                Json property_keys = Json::array();
                if (properties.is_object()) {
                    for (const auto& [key, value] : properties.items()) {
                        (void)value;
                        property_keys.push_back(key);
                    }
                }
                evidence["propertyKeys"] = std::move(property_keys);
            } else if (evidence_kind == "taskDefinition") {
                const Json required = item.value("required", Json::array());
                const Json properties = item.value("properties", Json::object());
                evidence["requiredCount"] = required.is_array() ? required.size() : 0;
                evidence["propertyCount"] = properties.is_object() ? properties.size() : 0;
                evidence["requiredKeys"] = required.is_array() ? required : Json::array();
                Json property_keys = Json::array();
                if (properties.is_object()) {
                    for (const auto& [key, value] : properties.items()) {
                        (void)value;
                        property_keys.push_back(key);
                    }
                }
                evidence["propertyKeys"] = std::move(property_keys);
            } else if (evidence_kind == "debugger") {
                const Json attributes = item.value("configurationAttributes", Json::object());
                const Json initials = item.value("initialConfigurations", Json::array());
                const Json snippets = item.value("configurationSnippets", Json::array());
                const Json variables = item.value("variables", Json::object());
                const Json languages = item.value("languages", Json::array());
                evidence["configurationAttributeScopeCount"] =
                    attributes.is_object() ? attributes.size() : 0;
                evidence["initialConfigurationCount"] = initials.is_array() ? initials.size() : 0;
                evidence["configurationSnippetCount"] = snippets.is_array() ? snippets.size() : 0;
                evidence["variableCount"] = variables.is_object() ? variables.size() : 0;
                evidence["languageCount"] = languages.is_array() ? languages.size() : 0;
            } else if (evidence_kind == "notebook" || evidence_kind == "customEditor") {
                const Json selector = item.value("selector", Json::array());
                evidence["selectorCount"] = selector.is_array() ? selector.size() : 0;
            }
            item["surfaceEvidence"] = std::move(evidence);
        }
    };
    mark_manifest_only(out.commands, "command");
    mark_manifest_only(out.view_containers, "viewContainer");
    mark_manifest_only(out.views, "view");
    mark_manifest_only(out.menus, "menu");
    mark_manifest_only(out.configurations, "configuration");
    mark_manifest_only(out.notebooks, "notebook");
    mark_manifest_only(out.debuggers, "debugger");
    mark_manifest_only(out.task_definitions, "taskDefinition");
    mark_manifest_only(out.custom_editors, "customEditor");
    mark_manifest_only(out.mcp_servers, "mcpServer");
    mark_manifest_only(out.chat_providers, "chatProvider");
    if (!finalize_inventory_budget(out)) {
        push_optional_diagnostic(diagnostics,
                                 "manifest contribution inventory could not be bounded for \"" +
                                     std::string(owner_id) + "\"");
        return false;
    }
    if (out.truncated) {
        push_optional_diagnostic(diagnostics, "truncated contribution inventory for \"" +
                                                  std::string(owner_id) + "\"");
        return false;
    }
    return true;
}

bool valid_manifest_provider_id(std::string_view raw_id) noexcept {
    return valid_utf8(raw_id) && valid_simple_id(raw_id);
}

bool scan_plugin_root(const std::filesystem::path& root_dir, std::string_view plugin_id,
                      PluginContributions& out) {
    if (!out.mcp_servers.is_object() || !out.chat_providers.is_array() ||
        !out.inventories.is_array()) {
        push_diagnostic(out.diagnostics, "plugin contribution accumulator has an invalid shape");
        return false;
    }
    if (!valid_utf8(plugin_id) || !valid_simple_id(plugin_id)) {
        push_diagnostic(out.diagnostics,
                        "skipped plugin root with invalid id \"" + std::string(plugin_id) + "\"");
        return false;
    }
    if (out.inventories.is_array() &&
        std::any_of(out.inventories.begin(), out.inventories.end(), [&](const Json& inventory) {
            return inventory.is_object() && string_member(inventory, "extensionId") == plugin_id;
        })) {
        push_diagnostic(out.diagnostics,
                        "skipped duplicate plugin root for \"" + std::string(plugin_id) + "\"");
        return false;
    }
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root_dir, error);
    if (error || !canonical_root.is_absolute() ||
        !std::filesystem::is_directory(canonical_root, error) || error) {
        push_diagnostic(out.diagnostics, "skipped plugin root with invalid path for \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    const std::string canonical_root_utf8 = wide_to_utf8(canonical_root.native());
    if (canonical_root_utf8.empty() || !valid_utf8(canonical_root_utf8)) {
        push_diagnostic(out.diagnostics, "skipped plugin root with invalid path encoding for \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    std::filesystem::path manifest;
    if (!resolve_bounded_path(canonical_root, "plugin.json", false, manifest)) {
        push_diagnostic(out.diagnostics, "skipped plugin manifest outside root for \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    std::string text;
    const int32_t read_status = read_pinned_plugin_manifest(canonical_root, manifest, text);
    if (read_status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return false;
    }
    if (read_status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        push_diagnostic(out.diagnostics,
                        "skipped oversized manifest for plugin \"" + std::string(plugin_id) + "\"");
        return false;
    }
    if (read_status != SAO_AI_EDITOR_OK) {
        push_diagnostic(out.diagnostics,
                        "skipped changed or invalid manifest binding for plugin \"" +
                            std::string(plugin_id) + "\"");
        return false;
    }
    if (!valid_utf8(text)) {
        push_diagnostic(out.diagnostics, "skipped manifest with invalid UTF-8 for plugin \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    Json data;
    ManifestTextParseResult parse_status = ManifestTextParseResult::invalid;
    try {
        parse_status = parse_manifest_text_strict(text, data);
    } catch (...) {
        push_diagnostic(out.diagnostics, "skipped unparseable manifest for plugin \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    if (parse_status == ManifestTextParseResult::duplicate_key) {
        push_diagnostic(out.diagnostics,
                        "skipped manifest with duplicate object keys for plugin \"" +
                            std::string(plugin_id) + "\"");
        return false;
    }
    if (parse_status != ManifestTextParseResult::ok) {
        push_diagnostic(out.diagnostics, "skipped unparseable manifest for plugin \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    if (!data.is_object()) {
        push_diagnostic(out.diagnostics, "skipped non-object manifest for plugin \"" +
                                             std::string(plugin_id) + "\"");
        return false;
    }
    const std::size_t owner_index = out.inventories.size();
    ManifestContributionInventory inventory;
    if (!parse_manifest_contribution_inventory(data, plugin_id, inventory, &out.diagnostics,
                                               canonical_root_utf8, owner_index)) {
        return false;
    }
    Json owner_metadata{{"extensionId", plugin_id},         {"extensionPath", canonical_root_utf8},
                        {"extensionIndex", owner_index},    {"ownerId", plugin_id},
                        {"ownerPath", canonical_root_utf8}, {"ownerIndex", owner_index}};
    const std::size_t owner_metadata_bytes = owner_metadata.dump().size() - 1;
    if (!finalize_inventory_budget(inventory, owner_metadata_bytes)) {
        push_diagnostic(out.diagnostics,
                        "plugin contribution owner metadata exceeds the inventory budget");
        return false;
    }
    if (inventory.truncated) {
        push_diagnostic(out.diagnostics,
                        "truncated contribution inventory for \"" + std::string(plugin_id) + "\"");
        return false;
    }
    Json inventory_json = inventory.to_json();
    for (auto& [key, value] : owner_metadata.items()) {
        inventory_json[key] = std::move(value);
    }
    Json next_inventories = out.inventories;
    Json next_mcp_servers = out.mcp_servers;
    Json next_chat_providers = out.chat_providers;
    next_inventories.push_back(std::move(inventory_json));
    for (const auto& server : inventory.mcp_servers) {
        const std::string name = server.value("id", std::string{});
        if (name.empty()) {
            continue;
        }
        if (next_mcp_servers.contains(name)) {
            push_diagnostic(out.diagnostics,
                            "skipped duplicate namespaced MCP server id \"" + name + "\"");
            return false;
        }
        next_mcp_servers[name] = server;
    }
    for (const auto& provider : inventory.chat_providers) {
        const std::string id = provider.value("id", std::string{});
        const bool duplicate =
            std::any_of(next_chat_providers.begin(), next_chat_providers.end(),
                        [&](const Json& existing) { return string_member(existing, "id") == id; });
        if (id.empty() || duplicate) {
            if (duplicate) {
                push_diagnostic(out.diagnostics,
                                "skipped duplicate namespaced chat provider id \"" + id + "\"");
            }
            return false;
        }
        next_chat_providers.push_back(provider);
    }
    try {
        std::size_t aggregate_entries = 0;
        for (const auto& installed : next_inventories) {
            if (!installed.is_object()) {
                return false;
            }
            const auto count = installed.find("totalEntries");
            if (count == installed.end() || !count->is_number_unsigned()) {
                return false;
            }
            const std::size_t value = count->get<std::size_t>();
            if (value > kMaximumContributionEntries - aggregate_entries) {
                push_diagnostic(out.diagnostics,
                                "plugin contribution aggregate entry limit reached");
                return false;
            }
            aggregate_entries += value;
        }
        const Json aggregate{{"inventories", next_inventories},
                             {"mcpServers", next_mcp_servers},
                             {"chatProviders", next_chat_providers}};
        if (aggregate.dump().size() > kMaximumContributionAggregateBytes) {
            push_diagnostic(out.diagnostics, "plugin contribution aggregate byte limit reached");
            return false;
        }
    } catch (...) {
        push_diagnostic(out.diagnostics, "plugin contribution aggregate could not be bounded");
        return false;
    }
    out.inventories = std::move(next_inventories);
    out.mcp_servers = std::move(next_mcp_servers);
    out.chat_providers = std::move(next_chat_providers);
    return true;
}

void scan_plugin_contributions(const Json& plugin_roots, PluginContributions& out) {
    if (!plugin_roots.is_object()) {
        push_diagnostic(out.diagnostics, "plugin root inventory is not an object");
        return;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    for (const auto& [id, path_value] : plugin_roots.items()) {
        if (roots.size() >= kMaximumContributionEntries) {
            push_diagnostic(out.diagnostics, "truncated plugin root inventory");
            break;
        }
        if (!valid_utf8(id) || !valid_simple_id(id)) {
            push_diagnostic(out.diagnostics, "skipped plugin root with invalid id");
            continue;
        }
        if (!path_value.is_string()) {
            push_diagnostic(out.diagnostics,
                            "skipped plugin root with non-string path for \"" + id + "\"");
            continue;
        }
        const std::string raw_path = path_value.get<std::string>();
        if (raw_path.empty() || raw_path.find('\0') != std::string::npos || !valid_utf8(raw_path)) {
            push_diagnostic(out.diagnostics,
                            "skipped plugin root with invalid path encoding for \"" + id + "\"");
            continue;
        }
        const std::wstring wide_path = utf8_to_wide(raw_path);
        if (wide_path.empty()) {
            push_diagnostic(out.diagnostics,
                            "skipped plugin root with invalid path for \"" + id + "\"");
            continue;
        }
        std::filesystem::path root(wide_path);
        std::error_code error;
        root = std::filesystem::weakly_canonical(root, error);
        if (error || !root.is_absolute()) {
            push_diagnostic(out.diagnostics,
                            "skipped plugin root with non-canonical path for \"" + id + "\"");
            continue;
        }
        roots.emplace_back(id, std::move(root));
    }
    std::sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    });
    for (const auto& [id, root] : roots) {
        scan_plugin_root(root, id, out);
    }
}

bool plugin_mcp_autostart_enabled(const Json& mcp_settings) {
    if (!mcp_settings.is_object()) {
        return false;
    }
    const auto read_flag = [&](std::string_view key, bool fallback, bool& value) {
        const auto found = mcp_settings.find(std::string(key));
        if (found == mcp_settings.end()) {
            value = fallback;
            return true;
        }
        if (!found->is_boolean()) {
            return false;
        }
        value = found->get<bool>();
        return true;
    };
    bool enabled = false;
    bool discovery_enabled = false;
    bool autostart = false;
    bool workspace_trusted = false;
    if (!read_flag("enabled", true, enabled) ||
        !read_flag("discovery_enabled", true, discovery_enabled) ||
        !read_flag("autostart", false, autostart) ||
        !read_flag("workspace_trusted", false, workspace_trusted)) {
        return false;
    }
    return enabled && discovery_enabled && autostart && workspace_trusted;
}

int32_t register_plugin_mcp_servers(SaoAiEditorMcpClient* client, const Json& servers,
                                    std::vector<std::string>& diagnostics) {
    if (client == nullptr || !servers.is_object()) {
        return 0;
    }
    int32_t registered = 0;
    std::size_t registered_bytes = 0;
    for (const auto& [name, raw_config] : servers.items()) {
        if (!valid_manifest_provider_id(name)) {
            push_diagnostic(diagnostics, "skipped plugin MCP server with invalid id");
            continue;
        }
        if (!raw_config.is_object()) {
            push_diagnostic(diagnostics,
                            "plugin MCP server \"" + name + "\" has an invalid configuration");
            continue;
        }
        std::string serialized;
        try {
            Json config = raw_config;
            config["name"] = name;
            serialized = dump_json(config);
        } catch (...) {
            push_diagnostic(diagnostics,
                            "plugin MCP server \"" + name + "\" has an invalid configuration");
            continue;
        }
        if (serialized.size() > kMaximumExtensionManifestBytes) {
            push_diagnostic(diagnostics, "plugin MCP server \"" + name +
                                             "\" exceeds the 1 MiB registration limit");
            continue;
        }
        if (serialized.size() > kMaximumContributionAggregateBytes - registered_bytes) {
            push_diagnostic(diagnostics, "plugin MCP registration aggregate byte limit reached");
            break;
        }
        const int32_t status = sao_ai_editor_mcp_client_register(
            client, serialized.data(), static_cast<uint32_t>(serialized.size()));
        if (status == SAO_AI_EDITOR_OK) {
            ++registered;
            registered_bytes += serialized.size();
        } else {
            push_diagnostic(diagnostics, "plugin MCP server \"" + name +
                                             "\" failed to register (status " +
                                             std::to_string(status) + ")");
        }
    }
    return registered;
}

} // namespace sao::ai_editor::native