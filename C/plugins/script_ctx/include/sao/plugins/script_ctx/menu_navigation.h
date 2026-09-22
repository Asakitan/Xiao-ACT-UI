#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::plugins::script_ctx::menu_navigation {

inline constexpr size_t max_nodes = 4096;
inline constexpr size_t max_text_bytes = 1024 * 1024;
inline constexpr const char* navigation_prefix = "@sao.nav/";

struct Row {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload_json{"{}"};
    bool can_activate{true};
    bool keep_open{};
    bool close_before{};
    bool operator==(const Row&) const = default;
};

struct Node {
    std::string key;
    Row row;
    bool submenu{};
    std::vector<Node> children;
};

enum class NavigationResult { not_navigation, handled, stale };

class State {
    struct Route {
        std::string id;
        std::string child_key;
        bool enabled{};
    };
    struct PendingNavigation {
        std::vector<std::string> target;
        bool consumed{};
    };
    std::vector<std::string> path_;
    std::vector<Row> rows_;
    std::unordered_map<std::string, Route> routes_;
    // Candidate replacement consumes the request even when its publication is abandoned.
    std::shared_ptr<PendingNavigation> pending_path_;
    // Candidate rollback must never rewind issued navigation identities.
    inline static std::atomic<uint64_t> next_id_{1};
    inline static constexpr char submenu_suffix[] = "  ›";

    static std::string allocate_id() {
        auto next = next_id_.load(std::memory_order_relaxed);
        while (next != (std::numeric_limits<uint64_t>::max)()) {
            if (next_id_.compare_exchange_weak(next, next + 1, std::memory_order_relaxed))
                return std::string(navigation_prefix) + std::to_string(next);
        }
        return {};
    }

    static bool validate(const std::vector<Node>& nodes, size_t& count,
                         size_t& bytes, std::unordered_set<std::string>& actions,
                         std::string& error) {
        std::vector<const std::vector<Node>*> levels{&nodes};
        while (!levels.empty()) {
            const auto* level = levels.back();
            levels.pop_back();
            if (level->size() > (level == &nodes ? 1024u : 1023u)) {
                error = "submenu visible row budget exceeded"; return false;
            }
            std::unordered_set<std::string> keys;
            for (const auto& node : *level) {
                if (++count > max_nodes) { error = "submenu node budget exceeded"; return false; }
                if (node.key.empty() || node.key.size() > 1024 || !keys.insert(node.key).second) {
                    error = "missing or duplicate submenu node identity"; return false;
                }
                for (const auto* text : {&node.key, &node.row.label, &node.row.icon,
                                        &node.row.action_id, &node.row.payload_json}) {
                    if (text->find('\0') != std::string::npos || text->size() > max_text_bytes - bytes) {
                        error = "submenu string budget or embedded NUL"; return false;
                    }
                    bytes += text->size();
                }
                const size_t suffix_bytes = node.submenu ? sizeof(submenu_suffix) - 1 : 0;
                if (node.row.label.empty() || node.row.label.size() > 1024 - suffix_bytes ||
                    node.row.icon.size() > 256) {
                    error = "invalid submenu label or icon"; return false;
                }
                if (node.row.action_id.starts_with(navigation_prefix) ||
                    (!node.row.action_id.empty() && !actions.insert(node.row.action_id).second)) {
                    error = "invalid or duplicate submenu action"; return false;
                }
                if (node.submenu) {
                    levels.push_back(&node.children);
                } else if (!node.children.empty() || node.row.action_id.empty()) {
                    error = "invalid submenu leaf action"; return false;
                }
            }
        }
        return true;
    }

  public:
    const std::vector<Row>& rows() const noexcept { return rows_; }
    const std::vector<std::string>& path() const noexcept { return path_; }

    bool replace(const std::vector<Node>& tree, std::string& error) {
        auto pending = std::exchange(pending_path_, {});
        const bool navigating = pending && !pending->consumed;
        if (navigating) pending->consumed = true;
        size_t count = 0, bytes = 0;
        std::unordered_set<std::string> actions;
        if (!validate(tree, count, bytes, actions, error)) return false;
        auto path = navigating ? std::move(pending->target) : path_;
        const auto* level = &tree;
        size_t matched = 0;
        for (const auto& key : path) {
            const auto found = std::find_if(level->begin(), level->end(), [&](const Node& node) {
                return node.key == key && node.submenu && node.row.can_activate;
            });
            if (found == level->end()) break;
            level = &found->children;
            ++matched;
        }
        path.resize(matched);
        std::unordered_map<std::string, Route> routes;
        std::vector<Row> rows;
        rows.reserve(level->size() + 1);
        const bool same_page = path == path_;
        const auto make_route = [&](const std::string& child_key, bool enabled) {
            const std::string key = child_key.empty() ? "back:" : "open:" + child_key;
            Route route;
            const auto old = routes_.find(key);
            if (same_page && old != routes_.end() && old->second.enabled == enabled)
                route.id = old->second.id;
            else route.id = allocate_id();
            if (route.id.empty()) return std::string{};
            route.child_key = child_key;
            route.enabled = enabled;
            const auto id = route.id;
            routes.emplace(key, std::move(route));
            return id;
        };
        if (!path.empty()) {
            Row back;
            back.label = "← Back / 返回";
            back.icon = "←";
            back.action_id = make_route({}, true);
            if (back.action_id.empty()) { error = "submenu identity exhausted"; return false; }
            back.keep_open = true;
            rows.push_back(std::move(back));
        }
        for (const auto& node : *level) {
            Row row = node.row;
            if (node.submenu) {
                row.action_id = make_route(node.key, row.can_activate);
                if (row.action_id.empty()) { error = "submenu identity exhausted"; return false; }
                row.label += submenu_suffix;
                row.payload_json = "{}";
                row.keep_open = true;
                row.close_before = false;
            }
            rows.push_back(std::move(row));
        }
        path_.swap(path);
        rows_.swap(rows);
        routes_.swap(routes);
        error.clear();
        return true;
    }

    NavigationResult activate(const std::string& action) {
        if (!action.starts_with(navigation_prefix)) return NavigationResult::not_navigation;
        if (pending_path_ && !pending_path_->consumed) return NavigationResult::stale;
        for (const auto& [key, route] : routes_) {
            (void)key;
            if (route.id != action) continue;
            if (!route.enabled) return NavigationResult::stale;
            auto target = path_;
            if (route.child_key.empty()) target.pop_back();
            else target.push_back(route.child_key);
            // Keep the published page intact until replace commits the requested page.
            pending_path_ = std::make_shared<PendingNavigation>(PendingNavigation{std::move(target)});
            return NavigationResult::handled;
        }
        return NavigationResult::stale;
    }
};

}
