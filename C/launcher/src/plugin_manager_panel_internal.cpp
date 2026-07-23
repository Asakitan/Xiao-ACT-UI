#include "plugin_manager_panel_internal.h"

#include "sao/ui/panel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#if (defined(SAO_LINKED_PLUGINS) || defined(SAO_LAUNCHER_PLUGIN_MANAGER_WITH_LOADER)) &&           \
    __has_include("sao/plugins/loader/plugin_lifecycle.h") &&                                      \
                  __has_include("sao/plugins/loader/plugin_registry.h")
#define SAO_LAUNCHER_PLUGIN_MANAGER_HAS_LOADER 1
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#else
#define SAO_LAUNCHER_PLUGIN_MANAGER_HAS_LOADER 0
#endif

namespace sao::launcher::plugin_manager_panel {
namespace {

using Json = nlohmann::json;

constexpr char kThemeOverride[] =
    R"({"colors":{"APP_BG":"#0b1018","APP_CARD":"#131b27","APP_BORDER":"#2a3850","APP_TEXT":"#eef4ff","APP_TEXT_2":"#c9d5e7","APP_TEXT_DIM":"#8292a8","APP_ACCENT":"#63b3ff","APP_BLUE":"#63b3ff","APP_GREEN":"#57d39b","APP_RED":"#ff6f7f","APP_ORANGE":"#f0a45d","APP_GOLD":"#e9ca72"}})";

constexpr std::string_view kActionRefresh = "plugin_manager.refresh";
constexpr std::string_view kActionReloadAll = "plugin_manager.reload_all";
constexpr std::string_view kActionEnable = "plugin_manager.enable";
constexpr std::string_view kActionDisable = "plugin_manager.disable";
constexpr std::string_view kActionReload = "plugin_manager.reload";

std::string clamp_utf8(std::string value, std::size_t maximum) {
    if (value.size() <= maximum)
        return value;
    if (maximum <= 3U)
        return value.substr(0, maximum);
    std::size_t end = maximum - 3U;
    while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
        --end;
    value.resize(end);
    value.append("...");
    return value;
}

Json text_node(std::string text, std::string_view style = "value", std::int32_t height = 22) {
    return Json{{"type", "text"},
                {"text", clamp_utf8(std::move(text), 4096U)},
                {"style", style},
                {"height", height}};
}

Json badge_node(std::string text, std::string_view style = "muted") {
    return Json{{"type", "badge"},
                {"text", clamp_utf8(std::move(text), 256U)},
                {"style", style},
                {"height", 22}};
}

Json button_node(std::string id, std::string label, std::string action, Json payload = Json(),
                 std::string_view style = "default", bool disabled = false) {
    Json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", std::move(label)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 28}};
    if (!payload.is_null())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

Json row_node(Json children) {
    return Json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

Json card_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "card"},
                {"title", clamp_utf8(std::move(title), 512U)},
                {"accent", accent},
                {"children", std::move(children)}};
}

std::string_view state_style(PluginState state) noexcept {
    static constexpr std::array<std::string_view, 13> styles{
        "accent", "accent", "warn",  "warn", "warn", "warn", "ok",
        "muted",  "warn",   "muted", "bad",  "warn", "warn",
    };
    const auto index = static_cast<std::size_t>(state);
    return index < styles.size() ? styles[index] : "muted";
}

std::string_view source_label(PluginSource source) noexcept {
    static constexpr std::array<std::string_view, 3> labels{
        "Unknown source / 来源未知",
        "Built-in / 内置",
        "User / 用户",
    };
    const auto index = static_cast<std::size_t>(source);
    return index < labels.size() ? labels[index] : labels.front();
}

std::string status_message(std::string_view operation, sao_status_t status) {
    return std::string(operation) + " failed (status " + std::to_string(status) + ").";
}

#if SAO_LAUNCHER_PLUGIN_MANAGER_HAS_LOADER

sao_status_t map_loader_status(std::int32_t status) noexcept {
    namespace Loader = sao::plugins::loader;
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
    case Loader::SAO_PLUGINS_ERR_UNSUPPORTED:
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    case Loader::SAO_PLUGINS_ERR_ALREADY_EXISTS:
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    case Loader::SAO_PLUGINS_ERR_NOT_FOUND:
    case Loader::SAO_PLUGINS_ERR_DEPENDENCY_MISSING:
        return SAO_STATUS_ERR_NOT_FOUND;
    case Loader::SAO_PLUGINS_ERR_DEPENDENCY_CYCLE:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case Loader::SAO_PLUGINS_ERR_ABI_MISMATCH:
    case Loader::SAO_PLUGINS_ERR_VERSION_MISMATCH:
        return SAO_STATUS_ERR_ABI_MISMATCH;
    case Loader::SAO_PLUGINS_ERR_CAPABILITY_MISMATCH:
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    case Loader::SAO_PLUGINS_ERR_BUSY:
        return SAO_STATUS_ERR_TIMEOUT;
    case Loader::SAO_PLUGINS_ERR_NOT_OWNER:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    default:
        return SAO_STATUS_ERR_SCRIPT_RUNTIME;
    }
}

PluginState map_loader_state(sao::plugins::loader::lifecycle_state state) noexcept {
    static constexpr std::array<PluginState, 13> states{
        PluginState::unknown,       PluginState::discovered,
        PluginState::validating,    PluginState::resolving_dependencies,
        PluginState::bootstrapping, PluginState::loading,
        PluginState::loaded_active, PluginState::loaded_disabled,
        PluginState::unloading,     PluginState::unloaded,
        PluginState::failed,        PluginState::enabling,
        PluginState::disabling,
    };
    const auto index = static_cast<std::size_t>(state);
    return index < states.size() ? states[index] : PluginState::unknown;
}

Snapshot snapshot_loader() {
    namespace Loader = sao::plugins::loader;
    Snapshot snapshot;
    const Loader::registry_handle_t registry = Loader::sao_plugins_registry_instance();
    if (registry == nullptr) {
        snapshot.error_message = "Plugin loader registry unavailable / 插件加载器注册表不可用。";
        return snapshot;
    }

    snapshot.loader_available = true;
    const auto manifests = Loader::snapshot_manifests(registry);
    snapshot.plugins.reserve(manifests.size());
    for (const auto& manifest : manifests) {
        PluginSnapshot plugin;
        plugin.plugin_id = manifest.plugin_id;
        plugin.name = manifest.name.empty() ? manifest.plugin_id : manifest.name;
        plugin.version = manifest.version.empty() ? "unknown" : manifest.version;
        plugin.language = std::string(Loader::engine_kind_name(manifest.language));
        if (plugin.language.empty())
            plugin.language = "unknown";
        plugin.description = manifest.description;
        plugin.source_path = manifest.source_path;
        plugin.source = manifest.user_installed ? PluginSource::user : PluginSource::built_in;
        plugin.manifest_enabled = manifest.enabled;
        const Loader::plugin_handle_t handle =
            Loader::sao_plugins_registry_find(registry, manifest.plugin_id.c_str());
        plugin.state = handle == nullptr
                           ? PluginState::unknown
                           : map_loader_state(Loader::sao_plugins_lifecycle_state(handle));
        snapshot.plugins.push_back(std::move(plugin));
    }
    std::ranges::sort(snapshot.plugins,
                      [](const PluginSnapshot& left, const PluginSnapshot& right) {
                          if (left.name != right.name)
                              return left.name < right.name;
                          return left.plugin_id < right.plugin_id;
                      });
    return snapshot;
}

sao_status_t run_loader_operation(
    std::string_view plugin_id,
    std::int32_t(SAO_PLUGINS_CALL* operation)(sao::plugins::loader::plugin_handle_t)) {
    namespace Loader = sao::plugins::loader;
    if (operation == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const Loader::registry_handle_t registry = Loader::sao_plugins_registry_instance();
    if (registry == nullptr)
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    const std::string owned_id(plugin_id);
    const Loader::plugin_handle_t plugin =
        Loader::sao_plugins_registry_find(registry, owned_id.c_str());
    if (plugin == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;
    return map_loader_status(operation(plugin));
}

Operations make_default_operations() noexcept {
    namespace Loader = sao::plugins::loader;
    try {
        Operations operations;
        operations.snapshot = &snapshot_loader;
        operations.enable = [](std::string_view plugin_id) {
            return run_loader_operation(plugin_id, &Loader::sao_plugins_lifecycle_enable);
        };
        operations.disable = [](std::string_view plugin_id) {
            return run_loader_operation(plugin_id, &Loader::sao_plugins_lifecycle_disable);
        };
        operations.reload = [](std::string_view plugin_id) {
            return run_loader_operation(plugin_id, &Loader::sao_plugins_lifecycle_reload);
        };
        return operations;
    } catch (...) {
        return {};
    }
}

#else

Snapshot snapshot_loader() {
    Snapshot snapshot;
    snapshot.error_message =
        "Plugin loader unavailable / 插件加载器不可用。The panel remains available in this build.";
    return snapshot;
}

Operations make_default_operations() noexcept {
    try {
        Operations operations;
        operations.snapshot = &snapshot_loader;
        return operations;
    } catch (...) {
        return {};
    }
}

#endif

Operations make_default_operations_with_reload_all(ReloadAllHandler reload_all_handler) noexcept {
    Operations operations = make_default_operations();
    operations.reload_all = std::move(reload_all_handler);
    return operations;
}

} // namespace

std::string_view plugin_state_label(PluginState state) noexcept {
    static constexpr std::array<std::string_view, 13> labels{
        "Unknown / 未知",
        "Discovered / 已发现",
        "Validating / 校验中",
        "Resolving dependencies / 解析依赖中",
        "Bootstrapping / 初始化依赖中",
        "Loading / 加载中",
        "Enabled / 已启用",
        "Disabled / 已禁用",
        "Unloading / 卸载中",
        "Unloaded / 已卸载",
        "Failed / 失败",
        "Enabling / 启用中",
        "Disabling / 禁用中",
    };
    const auto index = static_cast<std::size_t>(state);
    return index < labels.size() ? labels[index] : labels.front();
}

bool plugin_state_is_transitioning(PluginState state) noexcept {
    return state == PluginState::validating || state == PluginState::resolving_dependencies ||
           state == PluginState::bootstrapping || state == PluginState::loading ||
           state == PluginState::unloading || state == PluginState::enabling ||
           state == PluginState::disabling;
}

bool plugin_state_is_enabled(PluginState state) noexcept {
    return state == PluginState::loaded_active;
}

bool plugin_state_allows_enable(PluginState state) noexcept {
    return state == PluginState::discovered || state == PluginState::loaded_disabled ||
           state == PluginState::unloaded || state == PluginState::failed ||
           state == PluginState::loaded_active;
}

bool plugin_state_allows_reload(PluginState state) noexcept {
    return state != PluginState::unknown && !plugin_state_is_transitioning(state);
}

SaoPanelDescriptor descriptor_for_testing() noexcept {
    SaoPanelDescriptor descriptor{};
    descriptor.struct_size = sizeof(SaoPanelDescriptor);
    descriptor.panel_id_utf8 = kPanelId.data();
    descriptor.title_utf8 = "Plugin Manager";
    descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
    descriptor.default_width_px = 920;
    descriptor.default_height_px = 760;
    descriptor.min_width_px = 620;
    descriptor.min_height_px = 420;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = false;
    descriptor.remember_geometry = true;
    descriptor.modal = false;
    descriptor.overlay_style = false;
    descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
    descriptor.theme_override_json_utf8 = kThemeOverride;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;
    return descriptor;
}

std::string build_spec_for_testing(const Snapshot& snapshot) {
    Json nodes = Json::array();
    nodes.push_back(text_node("Plugin Manager", "title", 30));
    nodes.push_back(text_node(
        "Launcher-native plugin lifecycle control. All actions refresh the canonical loader state.",
        "muted", 36));

    Json status_badges = Json::array();
    status_badges.push_back(badge_node(snapshot.loader_available
                                           ? "Loader available / 加载器可用"
                                           : "Loader unavailable / 加载器不可用",
                                       snapshot.loader_available ? "ok" : "warn"));
    status_badges.push_back(
        badge_node(std::to_string(snapshot.plugins.size()) + " plugins / 个插件", "accent"));
    nodes.push_back(row_node(std::move(status_badges)));

    Json toolbar = Json::array();
    toolbar.push_back(button_node("plugin-manager.refresh", "Refresh / 刷新",
                                  std::string(kActionRefresh), Json(), "primary"));
    toolbar.push_back(button_node("plugin-manager.reload-all", "Reload All / 全部重载",
                                  std::string(kActionReloadAll), Json(), "default",
                                  !snapshot.reload_all_available));
    Json toolbar_card = Json::array();
    toolbar_card.push_back(row_node(std::move(toolbar)));
    if (!snapshot.reload_all_available) {
        toolbar_card.push_back(text_node(
            "Reload All awaits an injected launcher handler / Reload All 等待 launcher 注入回调。",
            "muted", 30));
    }
    nodes.push_back(card_node("Actions / 操作", std::move(toolbar_card), "gold"));

    if (!snapshot.error_message.empty()) {
        Json errors = Json::array();
        errors.push_back(text_node(snapshot.error_message, "bad", 42));
        nodes.push_back(card_node("Error / 错误", std::move(errors), "bad"));
    }

    if (!snapshot.loader_available) {
        Json unavailable = Json::array();
        unavailable.push_back(text_node("Plugin loader unavailable / 插件加载器不可用。This "
                                        "compositor panel can still be opened and closed.",
                                        "warn", 48));
        nodes.push_back(card_node("Unavailable / 不可用", std::move(unavailable), "warn"));
    } else if (snapshot.plugins.empty()) {
        Json empty = Json::array();
        empty.push_back(text_node("No plugins discovered / 未发现插件。", "muted", 34));
        nodes.push_back(card_node("Plugins / 插件", std::move(empty), "cyan"));
    } else {
        for (std::size_t index = 0; index < snapshot.plugins.size(); ++index) {
            const PluginSnapshot& plugin = snapshot.plugins[index];
            const bool transitioning = plugin_state_is_transitioning(plugin.state);
            const bool active = plugin_state_is_enabled(plugin.state);
            Json children = Json::array();

            Json badges = Json::array();
            badges.push_back(badge_node(std::string(plugin_state_label(plugin.state)),
                                        state_style(plugin.state)));
            badges.push_back(badge_node(std::string(source_label(plugin.source)), "muted"));
            badges.push_back(badge_node(plugin.manifest_enabled ? "Configured enabled / 配置启用"
                                                                : "Configured disabled / 配置禁用",
                                        plugin.manifest_enabled ? "accent" : "muted"));
            children.push_back(row_node(std::move(badges)));

            children.push_back(text_node("ID: " + plugin.plugin_id, "mono", 24));
            children.push_back(text_node(
                "Version: " + plugin.version + " · Language: " + plugin.language, "value", 26));
            children.push_back(text_node(plugin.description.empty() ? "No description / 无简介"
                                                                    : plugin.description,
                                         plugin.description.empty() ? "muted" : "value", 36));
            children.push_back(
                text_node("Source: " + (plugin.source_path.empty() ? std::string("unknown")
                                                                   : plugin.source_path),
                          "mono", 34));

            Json actions = Json::array();
            actions.push_back(
                button_node("plugin-manager.toggle." + std::to_string(index),
                            active ? "Disable / 禁用" : "Enable / 启用",
                            active ? std::string(kActionDisable) : std::string(kActionEnable),
                            Json(plugin.plugin_id), active ? "danger" : "primary",
                            transitioning || !plugin_state_allows_enable(plugin.state)));
            actions.push_back(button_node("plugin-manager.reload." + std::to_string(index),
                                          "Reload / 重载", std::string(kActionReload),
                                          Json(plugin.plugin_id), "default",
                                          !plugin_state_allows_reload(plugin.state)));
            children.push_back(row_node(std::move(actions)));

            nodes.push_back(card_node(plugin.name.empty() ? plugin.plugin_id : plugin.name,
                                      std::move(children),
                                      plugin.state == PluginState::failed ? "bad" : "cyan"));
        }
    }

    return Json{{"version", 1}, {"title", ""}, {"surface", "solid"}, {"nodes", std::move(nodes)}}
        .dump();
}

struct Owner::Impl {
    Impl(sao_ui_compositor_handle_t borrowed_compositor, Operations initial_operations) noexcept
        : compositor(borrowed_compositor), operations(std::move(initial_operations)) {}

    ~Impl() = default;

    struct CallbackLease final {
        explicit CallbackLease(Impl* candidate) noexcept : state(candidate) {
            if (state == nullptr)
                return;
            std::lock_guard lock(state->mutex);
            if (!state->accepting || state->retiring)
                return;
            ++state->callbacks_in_flight;
            active = true;
        }

        ~CallbackLease() {
            if (!active)
                return;
            {
                std::lock_guard lock(state->mutex);
                --state->callbacks_in_flight;
            }
        }

        explicit operator bool() const noexcept {
            return active;
        }

        Impl* state{};
        bool active{};
    };

    static void SAO_UI_CALL action_callback(const char* action_id_utf8,
                                            const std::uint8_t* payload_json_utf8,
                                            std::size_t payload_len, void* user_data) {
        auto* state = static_cast<Impl*>(user_data);
        CallbackLease lease(state);
        if (!lease || action_id_utf8 == nullptr ||
            (payload_json_utf8 == nullptr && payload_len != 0U))
            return;
        const std::string_view payload(
            payload_json_utf8 == nullptr ? "" : reinterpret_cast<const char*>(payload_json_utf8),
            payload_json_utf8 == nullptr ? 0U : payload_len);
        (void)state->dispatch(action_id_utf8, payload);
    }

    static void SAO_UI_CALL event_callback(std::int32_t event_kind, void* user_data) {
        auto* state = static_cast<Impl*>(user_data);
        CallbackLease lease(state);
        if (!lease)
            return;
        (void)state->dispatch_event(event_kind);
    }

    sao_status_t require_owner_thread() const noexcept {
        if (compositor == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return sao_ui_compositor_require_owner_thread(compositor);
    }

    sao_status_t set_operations(Operations replacement) noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        try {
            std::lock_guard lock(mutex);
            if (retiring || callbacks_in_flight != 0U)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            operations = std::move(replacement);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t open() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        try {
            sao_ui_panel_handle_t existing = nullptr;
            {
                std::lock_guard lock(mutex);
                if (creating || retiring)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                existing = panel;
                if (existing == nullptr) {
                    creating = true;
                    accepting = false;
                } else if (!action_handler_attached || !event_handler_attached || !accepting) {
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                }
            }

            if (existing == nullptr) {
                const SaoPanelDescriptor descriptor = descriptor_for_testing();
                sao_ui_panel_handle_t created_panel = nullptr;
                sao_ui_panel_body_handle_t created_body = nullptr;
                sao_status_t status =
                    sao_ui_panel_register(compositor, &descriptor, &created_panel, &created_body);
                if (status != SAO_STATUS_OK) {
                    finish_create_failure();
                    return status;
                }
                {
                    std::lock_guard lock(mutex);
                    panel = created_panel;
                    body = created_body;
                }

                status = sao_ui_panel_set_action_handler(created_panel, &action_callback, this);
                if (status == SAO_STATUS_OK) {
                    std::lock_guard lock(mutex);
                    action_handler_attached = true;
                }
                if (status == SAO_STATUS_OK)
                    status = sao_ui_panel_set_event_handler(created_panel, &event_callback, this);
                if (status == SAO_STATUS_OK) {
                    std::lock_guard lock(mutex);
                    event_handler_attached = true;
                    accepting = true;
                }
                if (status == SAO_STATUS_OK)
                    status = refresh_now();
                if (status != SAO_STATUS_OK) {
                    return cleanup_failed_create(status);
                }
                existing = created_panel;
            } else {
                const sao_status_t refresh_status = refresh_now();
                if (refresh_status != SAO_STATUS_OK)
                    return refresh_status;
            }

            sao_status_t status = sao_ui_panel_show(existing);
            if (status == SAO_STATUS_OK)
                status = sao_ui_panel_bring_to_front(existing);
            {
                std::lock_guard lock(mutex);
                creating = false;
            }
            return status;
        } catch (...) {
            finish_create_failure();
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t close() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        sao_ui_panel_handle_t target = nullptr;
        {
            std::lock_guard lock(mutex);
            if (creating || retiring)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            target = panel;
        }
        return target == nullptr ? SAO_STATUS_OK : sao_ui_panel_hide(target);
    }

    sao_status_t refresh() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        try {
            {
                std::lock_guard lock(mutex);
                if (creating || retiring)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                if (panel == nullptr || body == nullptr)
                    return SAO_STATUS_ERR_NOT_INITIALIZED;
            }
            return refresh_now();
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t refresh_now() noexcept {
        try {
            std::function<Snapshot()> snapshot_operation;
            sao_ui_panel_body_handle_t target_body = nullptr;
            std::string pending_error;
            bool reload_all_available = false;
            {
                std::lock_guard lock(mutex);
                target_body = body;
                snapshot_operation = operations.snapshot;
                reload_all_available = static_cast<bool>(operations.reload_all);
                pending_error = operation_error;
            }
            if (target_body == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            Snapshot snapshot;
            if (snapshot_operation) {
                snapshot = snapshot_operation();
            } else {
                snapshot.error_message =
                    "Plugin snapshot operation unavailable / 插件快照操作不可用。";
            }
            snapshot.reload_all_available = reload_all_available;
            if (!pending_error.empty()) {
                if (!snapshot.error_message.empty())
                    snapshot.error_message.append(" ");
                snapshot.error_message.append(pending_error);
            }
            const std::string spec = build_spec_for_testing(snapshot);
            const sao_status_t status = sao_ui_panel_body_set_spec(
                target_body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
            return status;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t dispatch(std::string_view action_id, std::string_view payload_json) noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        if (action_id == kActionRefresh) {
            clear_operation_message();
            return refresh();
        }

        if (action_id == kActionReloadAll) {
            std::function<sao_status_t()> handler;
            try {
                std::lock_guard lock(mutex);
                handler = operations.reload_all;
            } catch (...) {
                set_operation_error("Reload All handler copy failed.");
                (void)refresh();
                return SAO_STATUS_ERR_UNKNOWN;
            }
            sao_status_t operation = SAO_STATUS_ERR_CAPABILITY_MISSING;
            if (handler) {
                try {
                    operation = handler();
                } catch (...) {
                    operation = SAO_STATUS_ERR_UNKNOWN;
                }
            }
            if (operation == SAO_STATUS_OK)
                clear_operation_message();
            else
                set_operation_error(status_message("Reload All", operation));
            const sao_status_t refresh_status = refresh();
            return operation == SAO_STATUS_OK ? refresh_status : operation;
        }

        if (action_id != kActionEnable && action_id != kActionDisable &&
            action_id != kActionReload) {
            set_operation_error("Unknown Plugin Manager action: " + std::string(action_id));
            (void)refresh();
            return SAO_STATUS_ERR_NOT_FOUND;
        }

        const Json payload = Json::parse(payload_json.begin(), payload_json.end(), nullptr, false);
        if (!payload.is_string() || payload.get_ref<const std::string&>().empty()) {
            set_operation_error("Plugin action payload must be the plugin id JSON string.");
            (void)refresh();
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const std::string plugin_id = payload.get<std::string>();
        std::function<sao_status_t(std::string_view)> handler;
        try {
            std::lock_guard lock(mutex);
            if (action_id == kActionEnable)
                handler = operations.enable;
            else if (action_id == kActionDisable)
                handler = operations.disable;
            else
                handler = operations.reload;
        } catch (...) {
            set_operation_error("Plugin operation handler copy failed.");
            (void)refresh();
            return SAO_STATUS_ERR_UNKNOWN;
        }

        sao_status_t operation = SAO_STATUS_ERR_CAPABILITY_MISSING;
        if (handler) {
            try {
                operation = handler(plugin_id);
            } catch (...) {
                operation = SAO_STATUS_ERR_UNKNOWN;
            }
        }
        if (operation == SAO_STATUS_OK)
            clear_operation_message();
        else
            set_operation_error(
                status_message(std::string(action_id) + " for " + plugin_id, operation));
        const sao_status_t refresh_status = refresh();
        return operation == SAO_STATUS_OK ? refresh_status : operation;
    }

    sao_status_t dispatch_event(std::int32_t event_kind) noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        return event_kind == SAO_UI_PANEL_EVENT_CLOSE ? close() : SAO_STATUS_OK;
    }

    sao_status_t dispatch_action_for_testing(std::string_view action_id,
                                             std::string_view payload_json) noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        CallbackLease lease(this);
        return lease ? dispatch(action_id, payload_json) : SAO_UI_PANEL_STATUS_ERR_BUSY;
    }

    sao_status_t dispatch_event_for_testing(std::int32_t event_kind) noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        CallbackLease lease(this);
        return lease ? dispatch_event(event_kind) : SAO_UI_PANEL_STATUS_ERR_BUSY;
    }

    sao_status_t take_offline() noexcept {
        {
            std::lock_guard lock(mutex);
            if (panel == nullptr && !creating && !retiring)
                return SAO_STATUS_OK;
        }
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        sao_ui_panel_handle_t target_panel = nullptr;
        bool had_action = false;
        bool had_event = false;
        bool was_accepting = false;
        {
            std::lock_guard lock(mutex);
            if (creating || retiring || callbacks_in_flight != 0U)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            if (panel == nullptr) {
                return SAO_STATUS_OK;
            }
            retiring = true;
            target_panel = panel;
            had_action = action_handler_attached;
            had_event = event_handler_attached;
            was_accepting = accepting;
            accepting = false;
        }

        bool action_detached = !had_action;
        bool event_detached = !had_event;
        sao_status_t status = SAO_STATUS_OK;
        if (had_action) {
            status = sao_ui_panel_set_action_handler(target_panel, nullptr, nullptr);
            action_detached = status == SAO_STATUS_OK;
        }
        if (status == SAO_STATUS_OK && had_event) {
            status = sao_ui_panel_set_event_handler(target_panel, nullptr, nullptr);
            event_detached = status == SAO_STATUS_OK;
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_unregister(target_panel);

        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex);
            panel = nullptr;
            body = nullptr;
            action_handler_attached = false;
            event_handler_attached = false;
            accepting = false;
            retiring = false;
            return SAO_STATUS_OK;
        }

        bool action_restored = !had_action;
        bool event_restored = !had_event;
        if (had_event && event_detached)
            event_restored = sao_ui_panel_set_event_handler(target_panel, &event_callback, this) ==
                             SAO_STATUS_OK;
        else if (had_event)
            event_restored = true;
        if (had_action && action_detached)
            action_restored = sao_ui_panel_set_action_handler(target_panel, &action_callback,
                                                              this) == SAO_STATUS_OK;
        else if (had_action)
            action_restored = true;

        {
            std::lock_guard lock(mutex);
            action_handler_attached = had_action && action_restored;
            event_handler_attached = had_event && event_restored;
            accepting = was_accepting && action_handler_attached && event_handler_attached;
            retiring = false;
        }
        return action_restored && event_restored ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
    }

    bool is_registered() const noexcept {
        std::lock_guard lock(mutex);
        return panel != nullptr;
    }

    bool is_visible() const noexcept {
        const sao_ui_panel_handle_t target = panel_handle();
        SaoPanelState state{};
        return target != nullptr && sao_ui_panel_get_state(target, &state) == SAO_STATUS_OK &&
               state.visible;
    }

    sao_ui_panel_handle_t panel_handle() const noexcept {
        std::lock_guard lock(mutex);
        return panel;
    }

    void clear_operation_message() noexcept {
        std::lock_guard lock(mutex);
        operation_error.clear();
    }

    void set_operation_error(std::string message) noexcept {
        try {
            std::lock_guard lock(mutex);
            operation_error = std::move(message);
        } catch (...) {
        }
    }

    void finish_create_failure() noexcept {
        std::lock_guard lock(mutex);
        creating = false;
        accepting = panel != nullptr && action_handler_attached && event_handler_attached;
    }

    sao_status_t cleanup_failed_create(sao_status_t primary_status) noexcept {
        {
            std::lock_guard lock(mutex);
            creating = false;
        }
        const sao_status_t teardown_status = take_offline();
        return teardown_status == SAO_STATUS_OK ? primary_status : teardown_status;
    }

    bool shutdown_noexcept() noexcept {
        return take_offline() == SAO_STATUS_OK;
    }

    sao_ui_compositor_handle_t compositor{};
    mutable std::mutex mutex;
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    Operations operations;
    std::string operation_error;
    std::size_t callbacks_in_flight{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool accepting{};
    bool creating{};
    bool retiring{};
};

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor) noexcept
    : Owner(borrowed_compositor, make_default_operations()) {}

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor,
             ReloadAllHandler reload_all_handler) noexcept
    : Owner(borrowed_compositor,
            make_default_operations_with_reload_all(std::move(reload_all_handler))) {}

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor, Operations operations) noexcept
    : impl_(new (std::nothrow) Impl(borrowed_compositor, std::move(operations))) {}

Owner::~Owner() {
    if (impl_ != nullptr && !impl_->shutdown_noexcept())
        (void)impl_.release();
}

Owner::Owner(Owner&& other) noexcept = default;

Owner& Owner::operator=(Owner&& other) noexcept {
    if (this == &other)
        return *this;
    if (impl_ != nullptr && !impl_->shutdown_noexcept())
        (void)impl_.release();
    impl_ = std::move(other.impl_);
    return *this;
}

sao_status_t Owner::set_operations(Operations operations) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                            : impl_->set_operations(std::move(operations));
}

sao_status_t Owner::open() noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->open();
}

sao_status_t Owner::close() noexcept {
    return impl_ == nullptr ? SAO_STATUS_OK : impl_->close();
}

sao_status_t Owner::refresh() noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->refresh();
}

sao_status_t Owner::take_offline() noexcept {
    return impl_ == nullptr ? SAO_STATUS_OK : impl_->take_offline();
}

bool Owner::is_registered() const noexcept {
    return impl_ != nullptr && impl_->is_registered();
}

bool Owner::is_visible() const noexcept {
    return impl_ != nullptr && impl_->is_visible();
}

sao_ui_panel_handle_t Owner::panel_handle() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->panel_handle();
}

sao_status_t Owner::dispatch_action_for_testing(std::string_view action_id,
                                                std::string_view payload_json) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                            : impl_->dispatch_action_for_testing(action_id, payload_json);
}

sao_status_t Owner::dispatch_event_for_testing(std::int32_t event_kind) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                            : impl_->dispatch_event_for_testing(event_kind);
}

} // namespace sao::launcher::plugin_manager_panel
