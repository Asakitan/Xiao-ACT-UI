#pragma once

#include "entity_action_routes_internal.h"

#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao_plugins/sao_status.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::launcher::entity_provider_catalog {

namespace loader = sao::plugins::loader;

using SnapshotCatalogFn = std::int32_t(SAO_PLUGINS_CALL*)(
    loader::entity_provider_catalog_callback callback, void* user_data);
using SnapshotCatalogV2Fn = std::int32_t(SAO_PLUGINS_CALL*)(
    loader::entity_provider_catalog_callback_v2 callback, void* user_data);

struct OwnedEntityProviderRow {
    std::string category_id;
    std::string category_label;
    std::string category_icon;
    double category_priority = 0.0;
    std::string row_label;
    std::string row_icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const OwnedEntityProviderRow&) const = default;
};

struct OwnedEntityProvider {
    std::uint32_t snapshot_abi_version = loader::kEntitySnapshotAbiVersion1;
    loader::entity_snapshot_content_token_t content_token =
        loader::kInvalidEntitySnapshotContentToken;
    std::uint32_t row_stride_bytes = 0;
    std::string provider_id;
    std::string owner_plugin_id;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::vector<OwnedEntityProviderRow> rows;

    bool operator==(const OwnedEntityProvider&) const = default;
};

struct OwnedEntityRootActionRef {
    std::string provider_id;
    std::string action_id;

    bool operator==(const OwnedEntityRootActionRef&) const = default;
};

struct OwnedEntityRootContribution {
    std::string owner_plugin_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::vector<OwnedEntityRootActionRef> actions;

    bool operator==(const OwnedEntityRootContribution&) const = default;
};

struct OwnedEntityProviderCatalog {
    std::uint32_t abi_version = loader::kEntitySnapshotAbiVersion1;
    std::uint64_t revision = 0;
    loader::entity_snapshot_content_token_t content_token =
        loader::kInvalidEntitySnapshotContentToken;
    std::uint32_t provider_stride_bytes = 0;
    std::uint32_t root_contribution_stride_bytes = 0;
    std::vector<OwnedEntityProvider> providers;
    std::vector<OwnedEntityRootContribution> root_contributions;

    bool operator==(const OwnedEntityProviderCatalog&) const = default;
};

namespace detail {

inline constexpr std::size_t kMaximumCatalogProviders = 4096;
inline constexpr std::size_t kMaximumCatalogRows = entity_action_routes::kMaximumRows;
inline constexpr std::size_t kMaximumRootContributions = 59;
inline constexpr std::size_t kMaximumRootActions = 1024;
inline constexpr std::size_t kMaximumViewStringBytes = 16 * 1024;

inline bool valid_utf8(std::string_view value) noexcept {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7f) {
            ++offset;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1;
            code_point = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2;
            code_point = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3;
            code_point = first & 0x07u;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size())
            return false;
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0u) != 0x80u)
                return false;
            code_point = (code_point << 6u) | (next & 0x3fu);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80u) ||
                              (continuation_count == 2 && code_point < 0x800u) ||
                              (continuation_count == 3 && code_point < 0x10000u);
        if (overlong || code_point > 0x10ffffu ||
            (code_point >= 0xd800u && code_point <= 0xdfffu)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

inline bool copy_string(const char* value, bool required, std::size_t maximum_bytes,
                        std::size_t& total_string_bytes, std::string& out) {
    if (value == nullptr)
        return !required;
    std::size_t length = 0;
    while (length <= maximum_bytes && value[length] != '\0')
        ++length;
    const std::string_view view(value, length);
    if (length > maximum_bytes || (required && length == 0) || !valid_utf8(view) ||
        length > entity_action_routes::kMaximumSnapshotStringBytes - total_string_bytes) {
        return false;
    }
    out.assign(view);
    total_string_bytes += length;
    return true;
}

template <typename RowView>
inline bool copy_row_fields(const RowView& view, std::size_t& total_string_bytes,
                            OwnedEntityProviderRow& out) {
    if (!std::isfinite(view.category_priority)) {
        return false;
    }
    OwnedEntityProviderRow candidate;
    if (!copy_string(view.category_id_utf8, true, kMaximumViewStringBytes, total_string_bytes,
                     candidate.category_id) ||
        !copy_string(view.category_label_utf8, false, kMaximumViewStringBytes, total_string_bytes,
                     candidate.category_label) ||
        !copy_string(view.category_icon_utf8, false, kMaximumViewStringBytes, total_string_bytes,
                     candidate.category_icon) ||
        !copy_string(view.row_label_utf8, true, kMaximumViewStringBytes, total_string_bytes,
                     candidate.row_label) ||
        !copy_string(view.row_icon_utf8, false, kMaximumViewStringBytes, total_string_bytes,
                     candidate.row_icon) ||
        !copy_string(view.action_id_utf8, true, kMaximumViewStringBytes, total_string_bytes,
                     candidate.action_id)) {
        return false;
    }
    if (view.payload_json_utf8 == nullptr) {
        constexpr std::string_view kDefaultPayload = "{}";
        if (kDefaultPayload.size() >
            entity_action_routes::kMaximumSnapshotStringBytes - total_string_bytes) {
            return false;
        }
        candidate.payload_json = kDefaultPayload;
        total_string_bytes += kDefaultPayload.size();
    } else if (!copy_string(view.payload_json_utf8, false,
                            entity_action_routes::kMaximumPayloadBytes, total_string_bytes,
                            candidate.payload_json)) {
        return false;
    }
    candidate.category_priority = view.category_priority;
    candidate.can_activate = view.can_activate != 0;
    candidate.keep_menu_open = view.keep_menu_open != 0;
    candidate.close_menu_before = view.close_menu_before != 0;
    out = std::move(candidate);
    return true;
}

inline bool copy_row(const loader::entity_menu_row& view, std::size_t& total_string_bytes,
                     OwnedEntityProviderRow& out) {
    return view.struct_size >= loader::kEntityMenuRowRequiredPrefixSize &&
           copy_row_fields(view, total_string_bytes, out);
}

inline std::int32_t validate_stride(const void* base, std::uint32_t count,
                                    std::uint32_t stride_bytes, std::size_t required_prefix,
                                    std::size_t alignment) noexcept {
    if (stride_bytes < required_prefix) {
        return loader::SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
    if (stride_bytes % alignment != 0 ||
        (count > 0 &&
         (base == nullptr || reinterpret_cast<std::uintptr_t>(base) % alignment != 0))) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    return SAO_OK;
}

inline bool checked_stride_offset(std::uint32_t index, std::uint32_t count,
                                  std::uint32_t stride_bytes, std::size_t& out) noexcept {
    if (index >= count || stride_bytes == 0 ||
        index > std::numeric_limits<std::size_t>::max() / stride_bytes) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(index) * stride_bytes;
    if (stride_bytes > std::numeric_limits<std::size_t>::max() - offset) {
        return false;
    }
    out = offset;
    return true;
}

template <typename View>
inline bool copy_stride_view(const void* base, std::uint32_t count, std::uint32_t stride_bytes,
                             std::uint32_t index, std::size_t required_prefix, View& out) noexcept {
    std::size_t offset = 0;
    if (!checked_stride_offset(index, count, stride_bytes, offset)) {
        return false;
    }
    const auto base_address = reinterpret_cast<std::uintptr_t>(base);
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base_address) {
        return false;
    }
    const auto source_address = base_address + offset;
    if (stride_bytes > std::numeric_limits<std::uintptr_t>::max() - source_address) {
        return false;
    }
    const auto* source = reinterpret_cast<const std::byte*>(source_address);
    std::uint32_t struct_size = 0;
    std::memcpy(&struct_size, source, sizeof(struct_size));
    if (struct_size < required_prefix || struct_size > stride_bytes) {
        return false;
    }
    View candidate{};
    std::memcpy(&candidate, source,
                (std::min)(static_cast<std::size_t>(struct_size), sizeof(candidate)));
    out = candidate;
    return true;
}

struct CopyContext {
    OwnedEntityProviderCatalog candidate;
    sao_status_t status = SAO_STATUS_OK;
};

inline std::int32_t SAO_PLUGINS_CALL
copy_catalog_callback(const loader::entity_provider_catalog_view* view, void* user_data) noexcept {
    if (view == nullptr || user_data == nullptr ||
        view->struct_size < loader::kEntityProviderCatalogViewRequiredPrefixSize) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& context = *static_cast<CopyContext*>(user_data);
    try {
        if (view->provider_count > kMaximumCatalogProviders ||
            (view->provider_count > 0 && view->providers == nullptr) ||
            view->root_contribution_count > kMaximumRootContributions ||
            (view->root_contribution_count > 0 && view->root_contributions == nullptr)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        OwnedEntityProviderCatalog candidate;
        candidate.revision = view->revision;
        candidate.providers.reserve(view->provider_count);
        std::size_t total_rows = 0;
        std::size_t total_string_bytes = 0;
        std::unordered_set<std::string> provider_ids;
        provider_ids.reserve(view->provider_count);
        for (std::uint32_t provider_index = 0; provider_index < view->provider_count;
             ++provider_index) {
            const auto& provider_view = view->providers[provider_index];
            if (provider_view.struct_size < loader::kEntityProviderViewRequiredPrefixSize ||
                provider_view.generation == 0 ||
                provider_view.row_count > kMaximumCatalogRows - total_rows ||
                (provider_view.row_count > 0 && provider_view.rows == nullptr)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            OwnedEntityProvider provider;
            if (!copy_string(provider_view.provider_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, provider.provider_id) ||
                !copy_string(provider_view.owner_plugin_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, provider.owner_plugin_id)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            provider.generation = provider_view.generation;
            provider.revision = provider_view.revision;
            if (!provider_ids.insert(provider.provider_id).second) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            provider.rows.reserve(provider_view.row_count);
            for (std::uint32_t row_index = 0; row_index < provider_view.row_count; ++row_index) {
                OwnedEntityProviderRow row;
                if (!copy_row(provider_view.rows[row_index], total_string_bytes, row)) {
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                provider.rows.push_back(std::move(row));
            }
            total_rows += provider.rows.size();
            candidate.providers.push_back(std::move(provider));
        }
        std::size_t total_root_actions = 0;
        candidate.root_contributions.reserve(view->root_contribution_count);
        for (std::uint32_t root_index = 0; root_index < view->root_contribution_count;
             ++root_index) {
            const auto& root_view = view->root_contributions[root_index];
            if (root_view.struct_size < loader::kEntityRootContributionViewRequiredPrefixSize ||
                !std::isfinite(root_view.priority) ||
                root_view.action_count > kMaximumRootActions - total_root_actions ||
                (root_view.action_count > 0 && root_view.actions == nullptr)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            OwnedEntityRootContribution root;
            if (!copy_string(root_view.owner_plugin_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.owner_plugin_id) ||
                !copy_string(root_view.contribution_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.contribution_id) ||
                !copy_string(root_view.root_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.root_id) ||
                !copy_string(root_view.name_utf8, true, kMaximumViewStringBytes, total_string_bytes,
                             root.name) ||
                !copy_string(root_view.icon_utf8, false, kMaximumViewStringBytes,
                             total_string_bytes, root.icon)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            root.priority = root_view.priority;
            root.actions.reserve(root_view.action_count);
            for (std::uint32_t action_index = 0; action_index < root_view.action_count;
                 ++action_index) {
                const auto& action_view = root_view.actions[action_index];
                if (action_view.struct_size < loader::kEntityRootActionRefViewRequiredPrefixSize) {
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                OwnedEntityRootActionRef action;
                if (!copy_string(action_view.provider_id_utf8, true, kMaximumViewStringBytes,
                                 total_string_bytes, action.provider_id) ||
                    !copy_string(action_view.action_id_utf8, true, kMaximumViewStringBytes,
                                 total_string_bytes, action.action_id)) {
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                root.actions.push_back(std::move(action));
            }
            total_root_actions += root.actions.size();
            candidate.root_contributions.push_back(std::move(root));
        }
        context.candidate = std::move(candidate);
        return SAO_OK;
    } catch (...) {
        context.status = SAO_STATUS_ERR_UNKNOWN;
        return SAO_ERR_OS_CALL_FAILED;
    }
}

struct CopyContextV2 {
    OwnedEntityProviderCatalog candidate;
    sao_status_t status = SAO_STATUS_OK;
    std::int32_t callback_status = SAO_OK;
    bool callback_called = false;
};

inline std::int32_t reject_v2(CopyContextV2& context, std::int32_t status) noexcept {
    context.callback_status = status;
    return status;
}

inline std::int32_t SAO_PLUGINS_CALL
copy_catalog_callback_v2(const loader::entity_provider_catalog_view_v2* view,
                         void* user_data) noexcept {
    if (user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& context = *static_cast<CopyContextV2*>(user_data);
    context.callback_called = true;
    if (view == nullptr) {
        return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
    }
    if (view->struct_size < loader::kEntityProviderCatalogViewV2RequiredPrefixSize ||
        view->abi_version != loader::kEntitySnapshotAbiVersion2) {
        return reject_v2(context, loader::SAO_PLUGINS_ERR_ABI_MISMATCH);
    }
    if (view->content_token == loader::kInvalidEntitySnapshotContentToken ||
        view->provider_count > kMaximumCatalogProviders ||
        (view->provider_count > 0 && view->providers == nullptr) ||
        view->root_contribution_count > kMaximumRootContributions ||
        (view->root_contribution_count > 0 && view->root_contributions == nullptr)) {
        return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
    }
    std::int32_t stride_status =
        validate_stride(view->providers, view->provider_count, view->provider_stride_bytes,
                        loader::kEntityProviderViewV2RequiredPrefixSize,
                        alignof(loader::entity_provider_view_v2));
    if (stride_status != SAO_OK) {
        return reject_v2(context, stride_status);
    }
    stride_status = validate_stride(view->root_contributions, view->root_contribution_count,
                                    view->root_contribution_stride_bytes,
                                    loader::kEntityRootContributionViewV2RequiredPrefixSize,
                                    alignof(loader::entity_root_contribution_view_v2));
    if (stride_status != SAO_OK) {
        return reject_v2(context, stride_status);
    }
    try {
        OwnedEntityProviderCatalog candidate;
        candidate.abi_version = view->abi_version;
        candidate.revision = view->revision;
        candidate.content_token = view->content_token;
        candidate.provider_stride_bytes = view->provider_stride_bytes;
        candidate.root_contribution_stride_bytes = view->root_contribution_stride_bytes;
        candidate.providers.reserve(view->provider_count);
        std::size_t total_rows = 0;
        std::size_t total_string_bytes = 0;
        std::unordered_set<std::string> provider_ids;
        provider_ids.reserve(view->provider_count);
        for (std::uint32_t provider_index = 0; provider_index < view->provider_count;
             ++provider_index) {
            loader::entity_provider_view_v2 provider_view{};
            if (!copy_stride_view(view->providers, view->provider_count,
                                  view->provider_stride_bytes, provider_index,
                                  loader::kEntityProviderViewV2RequiredPrefixSize,
                                  provider_view)) {
                return reject_v2(context, loader::SAO_PLUGINS_ERR_ABI_MISMATCH);
            }
            if (provider_view.snapshot_abi_version != loader::kEntitySnapshotAbiVersion1 &&
                provider_view.snapshot_abi_version != loader::kEntitySnapshotAbiVersion2) {
                return reject_v2(context, loader::SAO_PLUGINS_ERR_ABI_MISMATCH);
            }
            if (provider_view.content_token == loader::kInvalidEntitySnapshotContentToken ||
                provider_view.generation == 0 ||
                provider_view.row_count > kMaximumCatalogRows - total_rows ||
                (provider_view.row_count > 0 && provider_view.rows == nullptr)) {
                return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
            }
            stride_status = validate_stride(provider_view.rows, provider_view.row_count,
                                            provider_view.row_stride_bytes,
                                            loader::kEntityMenuRowV2RequiredPrefixSize,
                                            alignof(loader::entity_menu_row_v2));
            if (stride_status != SAO_OK) {
                return reject_v2(context, stride_status);
            }
            OwnedEntityProvider provider;
            provider.snapshot_abi_version = provider_view.snapshot_abi_version;
            provider.content_token = provider_view.content_token;
            provider.row_stride_bytes = provider_view.row_stride_bytes;
            if (!copy_string(provider_view.provider_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, provider.provider_id) ||
                !copy_string(provider_view.owner_plugin_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, provider.owner_plugin_id)) {
                return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
            }
            provider.generation = provider_view.generation;
            provider.revision = provider_view.revision;
            if (!provider_ids.insert(provider.provider_id).second) {
                return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
            }
            provider.rows.reserve(provider_view.row_count);
            for (std::uint32_t row_index = 0; row_index < provider_view.row_count; ++row_index) {
                loader::entity_menu_row_v2 row_view{};
                if (!copy_stride_view(provider_view.rows, provider_view.row_count,
                                      provider_view.row_stride_bytes, row_index,
                                      loader::kEntityMenuRowV2RequiredPrefixSize, row_view)) {
                    return reject_v2(context, loader::SAO_PLUGINS_ERR_ABI_MISMATCH);
                }
                OwnedEntityProviderRow row;
                if (!copy_row_fields(row_view, total_string_bytes, row)) {
                    return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
                }
                provider.rows.push_back(std::move(row));
            }
            total_rows += provider.rows.size();
            candidate.providers.push_back(std::move(provider));
        }

        std::size_t total_root_actions = 0;
        candidate.root_contributions.reserve(view->root_contribution_count);
        for (std::uint32_t root_index = 0; root_index < view->root_contribution_count;
             ++root_index) {
            loader::entity_root_contribution_view_v2 root_view{};
            if (!copy_stride_view(view->root_contributions, view->root_contribution_count,
                                  view->root_contribution_stride_bytes, root_index,
                                  loader::kEntityRootContributionViewV2RequiredPrefixSize,
                                  root_view)) {
                return reject_v2(context, loader::SAO_PLUGINS_ERR_ABI_MISMATCH);
            }
            if (!std::isfinite(root_view.priority) ||
                root_view.action_count > kMaximumRootActions - total_root_actions ||
                (root_view.action_count > 0 && root_view.actions == nullptr)) {
                return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
            }
            stride_status = validate_stride(root_view.actions, root_view.action_count,
                                            root_view.action_stride_bytes,
                                            loader::kEntityRootActionRefViewV2RequiredPrefixSize,
                                            alignof(loader::entity_root_action_ref_view_v2));
            if (stride_status != SAO_OK) {
                return reject_v2(context, stride_status);
            }
            OwnedEntityRootContribution root;
            if (!copy_string(root_view.owner_plugin_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.owner_plugin_id) ||
                !copy_string(root_view.contribution_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.contribution_id) ||
                !copy_string(root_view.root_id_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.root_id) ||
                !copy_string(root_view.name_utf8, true, kMaximumViewStringBytes,
                             total_string_bytes, root.name) ||
                !copy_string(root_view.icon_utf8, false, kMaximumViewStringBytes,
                             total_string_bytes, root.icon)) {
                return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
            }
            root.priority = root_view.priority;
            root.actions.reserve(root_view.action_count);
            for (std::uint32_t action_index = 0; action_index < root_view.action_count;
                 ++action_index) {
                loader::entity_root_action_ref_view_v2 action_view{};
                if (!copy_stride_view(root_view.actions, root_view.action_count,
                                      root_view.action_stride_bytes, action_index,
                                      loader::kEntityRootActionRefViewV2RequiredPrefixSize,
                                      action_view)) {
                    return reject_v2(context, loader::SAO_PLUGINS_ERR_ABI_MISMATCH);
                }
                OwnedEntityRootActionRef action;
                if (!copy_string(action_view.provider_id_utf8, true, kMaximumViewStringBytes,
                                 total_string_bytes, action.provider_id) ||
                    !copy_string(action_view.action_id_utf8, true, kMaximumViewStringBytes,
                                 total_string_bytes, action.action_id)) {
                    return reject_v2(context, SAO_ERR_INVALID_ARGUMENT);
                }
                root.actions.push_back(std::move(action));
            }
            total_root_actions += root.actions.size();
            candidate.root_contributions.push_back(std::move(root));
        }
        context.candidate = std::move(candidate);
        context.callback_status = SAO_OK;
        return SAO_OK;
    } catch (...) {
        context.status = SAO_STATUS_ERR_UNKNOWN;
        return reject_v2(context, SAO_ERR_OS_CALL_FAILED);
    }
}

struct CategoryMetadata {
    std::string_view label;
    std::string_view icon;
    double priority = 0.0;
};

struct RouteIdentity {
    std::string_view provider_id;
    std::string_view action_id;

    bool operator==(const RouteIdentity&) const = default;
};

struct RouteIdentityHash {
    std::size_t operator()(const RouteIdentity& identity) const noexcept {
        std::size_t result = std::hash<std::string_view>{}(identity.provider_id);
        const auto action_hash = std::hash<std::string_view>{}(identity.action_id);
        result ^=
            action_hash + static_cast<std::size_t>(0x9e3779b9U) + (result << 6U) + (result >> 2U);
        return result;
    }
};

} // namespace detail

inline sao_status_t map_loader_status(std::int32_t status) noexcept {
    switch (status) {
    case SAO_OK:
        return SAO_STATUS_OK;
    case SAO_ERR_INVALID_ARGUMENT:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case SAO_ERR_NOT_INITIALIZED:
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    case SAO_ERR_HANDLE_INVALID:
        return SAO_STATUS_ERR_HANDLE_INVALID;
    case SAO_ERR_BUFFER_TOO_SMALL:
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    case SAO_ERR_OS_CALL_FAILED:
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    case SAO_ERR_NOT_IMPLEMENTED:
    case loader::SAO_PLUGINS_ERR_UNSUPPORTED:
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    case loader::SAO_PLUGINS_ERR_ALREADY_EXISTS:
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    case loader::SAO_PLUGINS_ERR_DEPENDENCY_MISSING:
        return SAO_STATUS_ERR_NOT_FOUND;
    case loader::SAO_PLUGINS_ERR_DEPENDENCY_CYCLE:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case loader::SAO_PLUGINS_ERR_ABI_MISMATCH:
    case loader::SAO_PLUGINS_ERR_VERSION_MISMATCH:
        return SAO_STATUS_ERR_ABI_MISMATCH;
    case loader::SAO_PLUGINS_ERR_CAPABILITY_MISMATCH:
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    case loader::SAO_PLUGINS_ERR_BUSY:
        return SAO_STATUS_ERR_TIMEOUT;
    case loader::SAO_PLUGINS_ERR_NOT_OWNER:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    default:
        return SAO_STATUS_ERR_SCRIPT_RUNTIME;
    }
}

inline sao_status_t snapshot(SnapshotCatalogFn snapshot_fn,
                             OwnedEntityProviderCatalog& out) noexcept {
    if (snapshot_fn == nullptr)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    try {
        detail::CopyContext context;
        const std::int32_t loader_status = snapshot_fn(&detail::copy_catalog_callback, &context);
        if (context.status != SAO_STATUS_OK)
            return context.status;
        const sao_status_t status = map_loader_status(loader_status);
        if (status != SAO_STATUS_OK)
            return status;
        out = std::move(context.candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

inline sao_status_t snapshot_v2(SnapshotCatalogV2Fn snapshot_fn,
                                OwnedEntityProviderCatalog& out) noexcept {
    if (snapshot_fn == nullptr)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    try {
        detail::CopyContextV2 context;
        const std::int32_t loader_status =
            snapshot_fn(&detail::copy_catalog_callback_v2, &context);
        if (context.status != SAO_STATUS_OK)
            return context.status;
        if (context.callback_called && context.callback_status != SAO_OK)
            return map_loader_status(context.callback_status);
        const sao_status_t status = map_loader_status(loader_status);
        if (status != SAO_STATUS_OK) {
            return context.callback_called && status == SAO_STATUS_ERR_NOT_IMPLEMENTED
                       ? SAO_STATUS_ERR_INVALID_ARGUMENT
                       : status;
        }
        if (!context.callback_called)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out = std::move(context.candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

inline sao_status_t snapshot_v2_first(SnapshotCatalogV2Fn snapshot_v2_fn,
                                      SnapshotCatalogFn snapshot_v1_fn,
                                      OwnedEntityProviderCatalog& out) noexcept {
    if (snapshot_v2_fn != nullptr) {
        const sao_status_t status = snapshot_v2(snapshot_v2_fn, out);
        if (status != SAO_STATUS_ERR_NOT_IMPLEMENTED)
            return status;
    }
    return snapshot(snapshot_v1_fn, out);
}

inline sao_status_t
build_routes(const OwnedEntityProviderCatalog& catalog,
             std::vector<entity_action_routes::EntityActionRouteSpec>& out) noexcept {
    try {
        std::vector<entity_action_routes::EntityActionRouteSpec> candidate;
        std::size_t row_count = 0;
        for (const auto& provider : catalog.providers) {
            if (provider.provider_id.empty() || provider.generation == 0 ||
                provider.rows.size() > entity_action_routes::kMaximumRows - row_count) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            row_count += provider.rows.size();
        }
        candidate.reserve(row_count);
        std::unordered_map<std::string_view, detail::CategoryMetadata> categories;
        std::unordered_set<detail::RouteIdentity, detail::RouteIdentityHash> identities;
        categories.reserve(row_count);
        identities.reserve(row_count);
        for (const auto& provider : catalog.providers) {
            for (const auto& row : provider.rows) {
                if (row.category_id.empty() || row.action_id.empty() ||
                    !std::isfinite(row.category_priority)) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                const auto [category, inserted] =
                    categories.emplace(row.category_id, detail::CategoryMetadata{
                                                            row.category_label,
                                                            row.category_icon,
                                                            row.category_priority,
                                                        });
                if (!inserted && (category->second.label != row.category_label ||
                                  category->second.icon != row.category_icon ||
                                  category->second.priority != row.category_priority)) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                if (!identities.emplace(detail::RouteIdentity{provider.provider_id, row.action_id})
                         .second) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                entity_action_routes::EntityActionRouteSpec route;
                route.provider_id = provider.provider_id;
                route.provider_generation = provider.generation;
                route.category_id = row.category_id;
                route.category_label = row.category_label;
                route.category_icon = row.category_icon;
                route.category_priority = row.category_priority;
                route.row_label = row.row_label;
                route.row_icon = row.row_icon;
                route.action_id = row.action_id;
                route.payload_json = row.payload_json;
                route.can_activate = row.can_activate;
                route.keep_menu_open = row.keep_menu_open;
                route.close_menu_before = row.close_menu_before;
                candidate.push_back(std::move(route));
            }
        }
        std::sort(candidate.begin(), candidate.end(), [](const auto& left, const auto& right) {
            if (left.category_priority != right.category_priority) {
                return left.category_priority < right.category_priority;
            }
            if (left.category_id != right.category_id) {
                return left.category_id < right.category_id;
            }
            if (left.provider_id != right.provider_id) {
                return left.provider_id < right.provider_id;
            }
            return left.action_id < right.action_id;
        });
        entity_action_routes::EntityActionRouteStore validation_store;
        entity_action_routes::EntityActionRouteStore::PreparedPublication validation;
        const sao_status_t validation_status = validation_store.prepare(candidate, validation);
        if (validation_status != SAO_STATUS_OK)
            return validation_status;
        out = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::launcher::entity_provider_catalog
