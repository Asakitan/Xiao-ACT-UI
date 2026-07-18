#pragma once

#include "sao/core/status.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::launcher::entity_action_routes {

inline constexpr std::int32_t kFirstDynamicToken = 0x40000000;
inline constexpr std::int32_t kLastDynamicToken = 0x7ffffffe;
inline constexpr std::size_t kMaximumRows = 4096;
inline constexpr std::size_t kMaximumStringBytes = 1024;
inline constexpr std::size_t kMaximumSnapshotStringBytes = 4 * 1024 * 1024;

inline constexpr bool is_dynamic_token(std::int32_t token) noexcept {
    return token >= kFirstDynamicToken && token <= kLastDynamicToken;
}

struct EntityActionRouteSpec {
    std::string provider_id;
    std::string category_id;
    std::string category_label;
    std::string category_icon;
    std::string row_label;
    std::string row_icon;
    std::string action_id;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const EntityActionRouteSpec&) const = default;
};

struct EntityActionRoute {
    std::int32_t token = 0;
    std::string provider_id;
    std::string category_id;
    std::string category_label;
    std::string category_icon;
    std::string row_label;
    std::string row_icon;
    std::string action_id;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const EntityActionRoute&) const = default;
};

struct EntityActionRouteSnapshot {
    std::uint64_t revision = 0;
    std::vector<EntityActionRoute> routes;

    bool operator==(const EntityActionRouteSnapshot&) const = default;
};

class EntityActionRouteStore final {
  public:
    EntityActionRouteStore() noexcept = default;

    EntityActionRouteStore(std::int32_t first_token,
                           std::int32_t last_token) noexcept
        : last_token_(last_token),
          next_token_(first_token),
          token_range_valid_(first_token >= kFirstDynamicToken &&
                             first_token <= last_token &&
                             last_token <= kLastDynamicToken) {}

    EntityActionRouteStore(const EntityActionRouteStore&) = delete;
    EntityActionRouteStore& operator=(const EntityActionRouteStore&) = delete;
    EntityActionRouteStore(EntityActionRouteStore&&) = delete;
    EntityActionRouteStore& operator=(EntityActionRouteStore&&) = delete;

    sao_status_t publish(const std::vector<EntityActionRouteSpec>& rows) noexcept {
        try {
            std::lock_guard lock(publish_mutex_);
            if (!token_range_valid_) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const sao_status_t validation_status = validate(rows);
            if (validation_status != SAO_STATUS_OK) {
                return validation_status;
            }

            const auto current = published_.load(std::memory_order_acquire);
            if (same_semantics(current.get(), rows)) {
                return SAO_STATUS_OK;
            }
            if (current != nullptr &&
                current->snapshot.revision ==
                    std::numeric_limits<std::uint64_t>::max()) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }

            std::unordered_map<IdentityView, std::int32_t, IdentityHash>
                active_tokens;
            if (current != nullptr) {
                active_tokens.reserve(current->snapshot.routes.size());
                for (const auto& route : current->snapshot.routes) {
                    active_tokens.emplace(identity_of(route), route.token);
                }
            }
            std::size_t new_identity_count = 0;
            for (const auto& row : rows) {
                if (!active_tokens.contains(identity_of(row))) {
                    ++new_identity_count;
                }
            }
            const std::int64_t available =
                static_cast<std::int64_t>(last_token_) - next_token_ + 1;
            if (available < 0 ||
                new_identity_count > static_cast<std::size_t>(available)) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }

            auto next = std::make_shared<PublishedState>();
            next->snapshot.revision =
                current == nullptr ? 1 : current->snapshot.revision + 1;
            next->snapshot.routes.reserve(rows.size());
            next->route_by_token.reserve(rows.size());

            std::int64_t candidate_next_token = next_token_;
            for (const auto& row : rows) {
                std::int32_t token = 0;
                const auto active = active_tokens.find(identity_of(row));
                if (active == active_tokens.end()) {
                    token = static_cast<std::int32_t>(candidate_next_token++);
                } else {
                    token = active->second;
                }
                const std::size_t index = next->snapshot.routes.size();
                next->snapshot.routes.push_back(make_route(row, token));
                next->route_by_token.emplace(token, index);
            }

            std::shared_ptr<const PublishedState> immutable = std::move(next);
            next_token_ = candidate_next_token;
            published_.store(std::move(immutable), std::memory_order_release);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t snapshot(EntityActionRouteSnapshot& out) const noexcept {
        try {
            const auto current = published_.load(std::memory_order_acquire);
            EntityActionRouteSnapshot copy;
            if (current != nullptr) {
                copy = current->snapshot;
            }
            out = std::move(copy);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t resolve(std::int32_t token,
                         EntityActionRoute& out) const noexcept {
        const auto current = published_.load(std::memory_order_acquire);
        if (current == nullptr) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        const auto found = current->route_by_token.find(token);
        if (found == current->route_by_token.end()) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        try {
            EntityActionRoute copy = current->snapshot.routes[found->second];
            out = std::move(copy);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

  private:
    struct IdentityView {
        std::string_view provider_id;
        std::string_view action_id;

        bool operator==(const IdentityView&) const = default;
    };

    struct IdentityHash {
        std::size_t operator()(const IdentityView& identity) const noexcept {
            std::size_t result = std::hash<std::string_view>{}(identity.provider_id);
            const std::size_t action_hash =
                std::hash<std::string_view>{}(identity.action_id);
            result ^= action_hash + static_cast<std::size_t>(0x9e3779b9U) +
                      (result << 6U) + (result >> 2U);
            return result;
        }
    };

    struct PublishedState {
        EntityActionRouteSnapshot snapshot;
        std::unordered_map<std::int32_t, std::size_t> route_by_token;
    };

    static bool valid_utf8(std::string_view value) noexcept {
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
            if (offset + continuation_count >= value.size()) {
                return false;
            }
            for (std::size_t index = 1; index <= continuation_count; ++index) {
                const auto next =
                    static_cast<unsigned char>(value[offset + index]);
                if ((next & 0xc0u) != 0x80u) {
                    return false;
                }
                code_point = (code_point << 6u) | (next & 0x3fu);
            }
            const bool overlong =
                (continuation_count == 1 && code_point < 0x80u) ||
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

    static bool valid_string(std::string_view value) noexcept {
        return value.size() <= kMaximumStringBytes &&
               value.find('\0') == std::string_view::npos && valid_utf8(value);
    }

    static sao_status_t validate(const std::vector<EntityActionRouteSpec>& rows) {
        if (rows.size() > kMaximumRows) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        std::size_t total_string_bytes = 0;
        std::unordered_set<IdentityView, IdentityHash> identities;
        identities.reserve(rows.size());
        for (const auto& row : rows) {
            const std::string_view strings[] = {
                row.provider_id,    row.category_id, row.category_label,
                row.category_icon,  row.row_label,   row.row_icon,
                row.action_id,
            };
            if (row.provider_id.empty() || row.action_id.empty()) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            for (const auto value : strings) {
                if (!valid_string(value) ||
                    value.size() >
                        kMaximumSnapshotStringBytes - total_string_bytes) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                total_string_bytes += value.size();
            }
            if (!identities.emplace(identity_of(row)).second) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }
        return SAO_STATUS_OK;
    }

    static bool same_identity(const EntityActionRoute& left,
                              const EntityActionRouteSpec& right) noexcept {
        return left.provider_id == right.provider_id &&
               left.action_id == right.action_id;
    }

    static IdentityView identity_of(const EntityActionRouteSpec& route) noexcept {
        return {route.provider_id, route.action_id};
    }

    static IdentityView identity_of(const EntityActionRoute& route) noexcept {
        return {route.provider_id, route.action_id};
    }

    static bool same_semantics(
        const PublishedState* current,
        const std::vector<EntityActionRouteSpec>& rows) noexcept {
        if (current == nullptr) {
            return rows.empty();
        }
        if (current->snapshot.routes.size() != rows.size()) {
            return false;
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& route = current->snapshot.routes[index];
            const auto& row = rows[index];
            if (!same_identity(route, row) ||
                route.category_id != row.category_id ||
                route.category_label != row.category_label ||
                route.category_icon != row.category_icon ||
                route.row_label != row.row_label ||
                route.row_icon != row.row_icon ||
                route.can_activate != row.can_activate ||
                route.keep_menu_open != row.keep_menu_open ||
                route.close_menu_before != row.close_menu_before) {
                return false;
            }
        }
        return true;
    }

    static EntityActionRoute make_route(const EntityActionRouteSpec& row,
                                        std::int32_t token) {
        return {
            token,
            row.provider_id,
            row.category_id,
            row.category_label,
            row.category_icon,
            row.row_label,
            row.row_icon,
            row.action_id,
            row.can_activate,
            row.keep_menu_open,
            row.close_menu_before,
        };
    }

    std::int32_t last_token_ = kLastDynamicToken;
    std::int64_t next_token_ = kFirstDynamicToken;
    bool token_range_valid_ = true;
    mutable std::mutex publish_mutex_;
    std::atomic<std::shared_ptr<const PublishedState>> published_;
};

} // namespace sao::launcher::entity_action_routes
