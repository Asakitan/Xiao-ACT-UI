#include "plugin_manager_panel_internal.h"

#include "sao/ui/panel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
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

constexpr std::string_view kActionRefresh = "plugin_manager.refresh";
constexpr std::string_view kActionReloadAll = "plugin_manager.reload_all";
constexpr std::string_view kActionEnable = "plugin_manager.enable";
constexpr std::string_view kActionDisable = "plugin_manager.disable";
constexpr std::string_view kActionReload = "plugin_manager.reload";
constexpr std::size_t kMaximumActionPayloadBytes = 4096U;
constexpr std::size_t kMaximumPluginIdBytes = 256U;
constexpr std::size_t kMaximumPanelSpecBytes = 256U * 1024U;
constexpr std::size_t kMaximumTaskQueue = 32U;

bool valid_utf8(std::string_view value) noexcept {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size())
            return false;
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        offset += continuation_count + 1U;
    }
    return true;
}

bool valid_text(std::string_view value, std::size_t maximum, bool required) noexcept {
    return (!required || !value.empty()) && value.size() <= maximum &&
           value.find('\0') == std::string_view::npos && valid_utf8(value);
}

std::optional<std::string_view> bounded_c_text(const char* value,
                                               std::size_t maximum) noexcept {
    if (value == nullptr)
        return std::nullopt;
    const void* terminator = std::memchr(value, '\0', maximum + 1U);
    if (terminator == nullptr)
        return std::nullopt;
    const auto length =
        static_cast<std::size_t>(static_cast<const char*>(terminator) - value);
    const std::string_view result(value, length);
    return valid_text(result, maximum, true) ? std::optional<std::string_view>(result)
                                              : std::nullopt;
}

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
    return Json{{"type", "card"}, {"title", clamp_utf8(std::move(title), 512U)},
                {"accent", accent}, {"children", std::move(children)}};
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
        "Unknown / 未知来源",
        "Built-in / 内置",
        "User / 用户",
    };
    const auto index = static_cast<std::size_t>(source);
    return index < labels.size() ? labels[index] : labels.front();
}

enum class ManagerState : std::uint8_t { initializing, loading, ready, empty, unavailable, error };

ManagerState manager_state(const Snapshot& snapshot) {
    if (snapshot.busy)
        return ManagerState::loading;
    if (!snapshot.loader_available) {
        if (snapshot.error_message.find("Loading") != std::string::npos)
            return ManagerState::initializing;
        if (snapshot.error_message.find("loading") != std::string::npos)
            return ManagerState::loading;
        return ManagerState::unavailable;
    }
    const bool has_operation_error = !snapshot.error_message.empty();
    if (has_operation_error)
        return ManagerState::error;
    return snapshot.plugins.empty() ? ManagerState::empty : ManagerState::ready;
}

std::string_view manager_state_label(ManagerState state) {
    switch (state) {
    case ManagerState::initializing: return "Initializing / 初始化";
    case ManagerState::loading: return "Loading / 加载中";
    case ManagerState::ready: return "Ready / 就绪";
    case ManagerState::empty: return "Empty / 空";
    case ManagerState::unavailable: return "Unavailable / 不可用";
    case ManagerState::error: return "Error / 错误";
    }
    return "Error / 错误";
}

std::string status_message(std::string_view operation, sao_status_t status);

std::string classified_status_message(std::string_view operation, sao_status_t status) {
    switch (status) {
    case SAO_STATUS_ERR_ABI_MISMATCH:
        return std::string(operation) + ": ABI mismatch";
    case SAO_STATUS_ERR_NOT_FOUND:
        return std::string(operation) + ": dependency missing";
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return std::string(operation) + ": permission denied";
    case SAO_STATUS_ERR_TIMEOUT:
    case SAO_UI_PANEL_STATUS_ERR_BUSY:
        return std::string(operation) + ": busy";
    default:
        return status_message(operation, status);
    }
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
        snapshot.error_message = "Plugin loader registry unavailable.";
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
        "Plugin loader unavailable. The panel remains available in this build.";
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
        "Unknown / 未知", "Discovered / 已发现", "Validating / 校验中", "Resolving dependencies / 解析依赖",
        "Bootstrapping / 启动中", "Loading / 加载中", "Enabled / 已启用", "Disabled / 已禁用",
        "Unloading / 卸载中", "Unloaded / 已卸载", "Failed / 失败", "Enabling / 启用中", "Disabling / 禁用中",
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
    descriptor.theme_override_json_utf8 = nullptr;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;
    return descriptor;
}

Json section_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "section"}, {"title", clamp_utf8(std::move(title), 512U)},
                {"accent", accent}, {"children", std::move(children)}};
}

Json dock_document(Json nodes, std::string content_id, int min_width = 560) {
    if (!nodes.is_array() || nodes.empty())
        return Json{{"version", 1}, {"title", ""}, {"layout", "dock"}, {"nodes", std::move(nodes)}};
    Json top = std::move(nodes.front());
    nodes.erase(nodes.begin());
    top["dock"] = "top";
    Json content{{"type", "section"},
                 {"id", std::move(content_id)},
                 {"container", true},
                 {"layout", "vertical"},
                 {"width", 0},
                 {"min_width", min_width},
                 {"weight", 1.0F},
                 {"dock", "fill"},
                 {"scroll", {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}}},
                 {"children", std::move(nodes)}};
    return Json{{"version", 1},
                {"title", ""},
                {"layout", "dock"},
                {"nodes", Json::array({std::move(top), std::move(content)})}};
}

std::string_view manager_accent(ManagerState state) noexcept {
    if (state == ManagerState::ready) return "ok";
    if (state == ManagerState::initializing || state == ManagerState::loading) return "gold";
    if (state == ManagerState::error || state == ManagerState::unavailable) return "danger";
    return "cyan";
}

std::string_view plugin_accent(PluginState state) noexcept {
    if (plugin_state_is_enabled(state)) return "ok";
    if (plugin_state_is_transitioning(state)) return "gold";
    if (state == PluginState::failed) return "danger";
    if (state == PluginState::unknown) return "muted";
    return "cyan";
}

Json status_strip_node(const Snapshot& snapshot, ManagerState manager) {
    const std::string_view accent = manager_accent(manager);
    return card_node(
        "状态",
        Json::array({row_node(Json::array(
            {badge_node(std::string(manager_state_label(manager)), accent),
             badge_node(std::to_string(snapshot.plugins.size()) + " 个插件", "cyan")}))}),
        accent);
}
std::string build_spec(const Snapshot& snapshot, bool reload_all_busy,
                       const std::unordered_set<std::string>& busy_plugins) {
    const ManagerState manager = manager_state(snapshot);
    Json nodes = Json::array();
    nodes.push_back(status_strip_node(snapshot, manager));
    Json overview = Json::array();
    overview.push_back(
        text_node("Refresh discovers the current plugin catalog; actions run in the background. / "
                  "刷新当前插件目录；操作在后台执行。",
                  "muted", 34));
    Json toolbar = Json::array();
    toolbar.push_back(button_node("plugin-manager.refresh",
                                  snapshot.loader_available ? "刷新" : "重试",
                                  std::string(kActionRefresh), Json(), "primary",
                                  snapshot.busy || manager == ManagerState::loading));
    toolbar.push_back(button_node("plugin-manager.reload-all", "全部重载",
                                  std::string(kActionReloadAll), Json(), "default",
                                  !snapshot.reload_all_available || snapshot.busy));
    overview.push_back(row_node(std::move(toolbar)));
    nodes.push_back(card_node("插件库 / Library", std::move(overview), manager_accent(manager)));
    if (manager == ManagerState::initializing || manager == ManagerState::loading) {
        Json loader = Json::array();
        loader.push_back(text_node("正在加载插件目录…", "warn", 42));
        nodes.push_back(section_node("Loader / 加载器", std::move(loader), "gold"));
    } else if (manager == ManagerState::unavailable) {
        Json loader = Json::array();
        loader.push_back(text_node(snapshot.error_message.empty()
                                       ? "Plugin loader unavailable / 插件加载器不可用。"
                                       : snapshot.error_message,
                                   "warn", 42));
        loader.push_back(button_node("plugin-manager.retry", "重试", std::string(kActionRefresh),
                                     Json(), "primary", snapshot.busy));
        nodes.push_back(section_node("Loader / 加载器", std::move(loader), "danger"));
    } else if (!snapshot.error_message.empty()) {
        Json errors = Json::array();
        errors.push_back(text_node(snapshot.error_message, "bad", 42));
        errors.push_back(button_node("plugin-manager.error-retry", "重试",
                                     std::string(kActionRefresh), Json(), "primary",
                                     snapshot.busy));
        nodes.push_back(section_node("Error / 错误", std::move(errors), "danger"));
    } else if (snapshot.plugins.empty()) {
        Json empty = Json::array();
        empty.push_back(text_node("No plugins discovered / 未发现插件。", "muted", 34));
        empty.push_back(button_node("plugin-manager.empty-retry", "刷新",
                                    std::string(kActionRefresh), Json(), "primary", snapshot.busy));
        nodes.push_back(section_node("Plugins / 插件", std::move(empty), "cyan"));
    } else {
        Json plugin_cards = Json::array();
        for (std::size_t index = 0; index < snapshot.plugins.size(); ++index) {
            const PluginSnapshot& plugin = snapshot.plugins[index];
            const bool transitioning = plugin_state_is_transitioning(plugin.state);
            const bool active = plugin_state_is_enabled(plugin.state);
            const bool row_busy = reload_all_busy || snapshot.busy ||
                                  busy_plugins.find(plugin.plugin_id) != busy_plugins.end();
            const std::string widget_suffix =
                valid_text(plugin.plugin_id, kMaximumPluginIdBytes, true)
                    ? plugin.plugin_id
                    : std::to_string(index);
            Json details = Json::array();
            Json metadata = Json::array();
            metadata.push_back(badge_node("v" + plugin.version, "cyan"));
            metadata.push_back(badge_node(std::string(plugin_state_label(plugin.state)),
                                          state_style(plugin.state)));
            details.push_back(row_node(std::move(metadata)));
            details.push_back(text_node("Language / 语言: " + (plugin.language.empty() ? "Unknown / 未知" : plugin.language) +
                                           " · Source / 来源: " + std::string(source_label(plugin.source)), "muted", 24));
            if (!plugin.description.empty())
                details.push_back(text_node(clamp_utf8(plugin.description, 320U), "value", 42));
            details.push_back(text_node("ID: " + plugin.plugin_id, "muted", 24));
            if (!plugin.source_path.empty())
                details.push_back(text_node("Path / 路径: " + clamp_utf8(plugin.source_path, 320U),
                                           "mono", 30));
            Json actions = Json::array();
            actions.push_back(button_node(
                "plugin-manager.toggle." + widget_suffix, active ? "禁用" : "启用",
                active ? std::string(kActionDisable) : std::string(kActionEnable),
                Json(plugin.plugin_id), active ? "danger" : "primary",
                row_busy || transitioning || !plugin_state_allows_enable(plugin.state)));
            actions.push_back(button_node("plugin-manager.reload." + widget_suffix, "重载",
                                          std::string(kActionReload), Json(plugin.plugin_id),
                                          "default",
                                          row_busy || !plugin_state_allows_reload(plugin.state)));
            details.push_back(row_node(std::move(actions)));
            plugin_cards.push_back(card_node(plugin.name.empty() ? plugin.plugin_id : plugin.name,
                                              std::move(details), plugin_accent(plugin.state)));
        }
        nodes.push_back(section_node("Plugins / 插件", std::move(plugin_cards), "cyan"));
    }
    std::string spec = dock_document(std::move(nodes), "plugin-manager-content").dump();
    if (spec.size() <= kMaximumPanelSpecBytes)
        return spec;
    Json compact = Json::array();
    compact.push_back(text_node("Plugin Manager content exceeded the panel budget.", "bad", 44));
    compact.push_back(button_node("plugin-manager.budget-retry", "重试",
                                  std::string(kActionRefresh), Json(), "primary", snapshot.busy));
    return Json{{"version", 1}, {"title", ""}, {"nodes", std::move(compact)}}.dump();
}

std::string build_spec_for_testing(const Snapshot& snapshot) {
    return build_spec(snapshot, snapshot.busy, {});
}

struct Owner::Impl {
    static std::mutex deferred_mutex;
    static std::vector<std::unique_ptr<Impl>> deferred_cleanup;
    enum class TaskKind : std::uint8_t {
        refresh,
        reload_all,
        enable,
        disable,
        reload,
    };

    struct Task {
        TaskKind kind{TaskKind::refresh};
        std::string plugin_id;
    };

    Impl(sao_ui_compositor_handle_t borrowed_compositor, Operations initial_operations) noexcept
        : compositor(borrowed_compositor), operations(std::move(initial_operations)) {
        try {
            worker = std::jthread([this](std::stop_token stop) { worker_main(stop); });
            worker_online = true;
        } catch (...) {
            worker_accepting = false;
        }
    }

    ~Impl() {
        stop_worker();
    }

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
            std::lock_guard lock(state->mutex);
            --state->callbacks_in_flight;
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
        const auto action = bounded_c_text(action_id_utf8, 64U);
        CallbackLease lease(state);
        if (!lease || !action.has_value() ||
            (payload_json_utf8 == nullptr && payload_len != 0U)) {
            return;
        }
        const std::string_view payload(
            payload_json_utf8 == nullptr ? "" : reinterpret_cast<const char*>(payload_json_utf8),
            payload_json_utf8 == nullptr ? 0U : payload_len);
        (void)state->dispatch(*action, payload);
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
            if (retiring || callbacks_in_flight != 0U || worker_active || refresh_running ||
                worker_publishing || owner_publishing || !tasks.empty()) {
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            }
            operations = std::move(replacement);
            last_snapshot = {};
            snapshot_available = false;
            operation_error.clear();
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
                if (creating || retiring || owner_publishing)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                existing = panel;
                if (existing == nullptr) {
                    if (worker_active || refresh_running || worker_publishing || !tasks.empty())
                        return SAO_UI_PANEL_STATUS_ERR_BUSY;
                    creating = true;
                    accepting = false;
                } else if (!action_handler_attached || !event_handler_attached || !accepting) {
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                }
            }

            const bool reopening = existing != nullptr;
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
                    last_published_body = nullptr;
                    last_published_spec.clear();
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
                    status = enqueue_task(Task{TaskKind::refresh, {}});
                if (status != SAO_STATUS_OK)
                    return cleanup_failed_create(status);
                existing = created_panel;
            }

            sao_status_t status = sao_ui_panel_show(existing);
            if (status == SAO_STATUS_OK)
                status = sao_ui_panel_bring_to_front(existing);
            {
                std::lock_guard lock(mutex);
                creating = false;
            }
            if (status == SAO_STATUS_OK && reopening)
                status = enqueue_task(Task{TaskKind::refresh, {}});
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
        return enqueue_task(Task{TaskKind::refresh, {}});
    }

    sao_status_t service_ui() noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        return publish_cached_state(false);
    }

    sao_status_t publish_snapshot(Snapshot snapshot) noexcept {
        try {
            sao_ui_panel_body_handle_t target_body = nullptr;
            std::string pending_error;
            std::unordered_set<std::string> busy_plugins;
            bool reload_all_busy = false;
            bool reload_all_available = false;
            {
                std::lock_guard lock(mutex);
                target_body = body;
                reload_all_available = static_cast<bool>(operations.reload_all);
                pending_error = operation_error;
                busy_plugins = busy_plugin_ids;
                reload_all_busy = reload_all_pending;
                snapshot.busy = worker_active || refresh_running || !tasks.empty();
            }
            if (target_body == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            snapshot.reload_all_available = reload_all_available;
            if (!pending_error.empty()) {
                if (!snapshot.error_message.empty())
                    snapshot.error_message.append(" ");
                snapshot.error_message.append(pending_error);
            }
            const std::string spec = build_spec(snapshot, reload_all_busy, busy_plugins);
            {
                std::lock_guard lock(mutex);
                if (body == target_body && last_published_body == target_body &&
                    last_published_spec == spec)
                    return SAO_STATUS_OK;
            }
            const sao_status_t status = sao_ui_panel_body_set_spec(
                target_body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
            if (status != SAO_STATUS_OK)
                return status;
            {
                std::lock_guard lock(mutex);
                if (body != target_body)
                    return SAO_STATUS_ERR_HANDLE_INVALID;
                last_published_body = target_body;
                last_published_spec = spec;
            }
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t publish_cached_state(bool force) noexcept {
        try {
            Snapshot snapshot;
            {
                std::lock_guard lock(mutex);
                if (!force && !cache_dirty)
                    return SAO_STATUS_OK;
                if (snapshot_available)
                    snapshot = last_snapshot;
                else
                    snapshot.error_message = "Loading plugin catalog...";
                cache_dirty = false;
            }
            const sao_status_t status = publish_snapshot(std::move(snapshot));
            if (status != SAO_STATUS_OK) {
                std::lock_guard lock(mutex);
                cache_dirty = true;
            }
            return status;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t refresh_now() noexcept {
        try {
            std::function<Snapshot()> snapshot_operation;
            {
                std::lock_guard lock(mutex);
                snapshot_operation = operations.snapshot;
            }
            Snapshot snapshot;
            if (snapshot_operation)
                snapshot = snapshot_operation();
            else
                snapshot.error_message = "Plugin snapshot operation unavailable.";
            {
                std::lock_guard lock(mutex);
                last_snapshot = std::move(snapshot);
                snapshot_available = true;
                cache_dirty = true;
            }
            return SAO_STATUS_OK;
        } catch (...) {
            std::lock_guard lock(mutex);
            cache_dirty = true;
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t enqueue_task(Task task) noexcept {
        {
            std::lock_guard lock(mutex);
            if (!worker_online || !worker_accepting || !accepting || retiring ||
                panel == nullptr) {
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            }
            if (task.kind == TaskKind::refresh) {
                if (refresh_queued || refresh_running)
                    return SAO_STATUS_OK;
                if (tasks.size() >= kMaximumTaskQueue)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                refresh_queued = true;
            } else {
                if (tasks.size() >= kMaximumTaskQueue)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                if (task.kind == TaskKind::reload_all) {
                    if (reload_all_pending || !busy_plugin_ids.empty())
                        return SAO_UI_PANEL_STATUS_ERR_BUSY;
                    reload_all_pending = true;
                } else if (reload_all_pending ||
                           busy_plugin_ids.find(task.plugin_id) != busy_plugin_ids.end()) {
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                } else {
                    busy_plugin_ids.insert(task.plugin_id);
                }
            }
            if (task.kind != TaskKind::refresh)
                operation_error.clear();
            tasks.push_back(std::move(task));
            owner_publishing = true;
        }
        const sao_status_t publish_status = publish_cached_state(true);
        {
            std::lock_guard lock(mutex);
            owner_publishing = false;
            if (publish_status != SAO_STATUS_OK) cache_dirty = true;
        }
        worker_cv.notify_all();
        return SAO_STATUS_OK;
    }

    void worker_main(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            Task task;
            std::function<sao_status_t()> reload_all_handler;
            std::function<sao_status_t(std::string_view)> plugin_handler;
            try {
                std::unique_lock lock(mutex);
                if (!worker_cv.wait(lock, stop, [this] {
                        return (!tasks.empty() && !owner_publishing) || !worker_accepting;
                    })) {
                    break;
                }
                if (stop.stop_requested() || !worker_accepting)
                    break;
                task = std::move(tasks.front());
                tasks.pop_front();
                if (task.kind == TaskKind::refresh) {
                    refresh_queued = false;
                    refresh_running = true;
                }
                worker_active = true;
                if (task.kind == TaskKind::reload_all) {
                    reload_all_handler = operations.reload_all;
                } else if (task.kind == TaskKind::enable) {
                    plugin_handler = operations.enable;
                } else if (task.kind == TaskKind::disable) {
                    plugin_handler = operations.disable;
                } else if (task.kind == TaskKind::reload) {
                    plugin_handler = operations.reload;
                }
            } catch (...) {
                std::lock_guard lock(mutex);
                if (task.kind == TaskKind::reload_all)
                    reload_all_pending = false;
                else if (task.kind == TaskKind::refresh)
                    refresh_running = false;
                else
                    busy_plugin_ids.erase(task.plugin_id);
                worker_active = false;
                operation_error = "Plugin operation handler copy failed.";
                cache_dirty = true;
                continue;
            }

            sao_status_t operation_status = task.kind == TaskKind::refresh
                                                ? SAO_STATUS_OK
                                                : SAO_STATUS_ERR_CAPABILITY_MISSING;
            try {
                if (task.kind == TaskKind::reload_all) {
                    if (reload_all_handler)
                        operation_status = reload_all_handler();
                } else if (task.kind != TaskKind::refresh && plugin_handler) {
                    operation_status = plugin_handler(task.plugin_id);
                }
            } catch (...) {
                operation_status = SAO_STATUS_ERR_UNKNOWN;
            }

            {
                std::lock_guard lock(mutex);
                if (operation_status == SAO_STATUS_OK) {
                    if (task.kind != TaskKind::refresh)
                        operation_error.clear();
                } else if (task.kind == TaskKind::reload_all) {
                    operation_error = classified_status_message("Reload All", operation_status);
                } else {
                    operation_error = classified_status_message(
                        std::string(task.kind == TaskKind::enable
                                        ? kActionEnable
                                        : task.kind == TaskKind::disable ? kActionDisable
                                                                         : kActionReload) +
                            " for " + task.plugin_id,
                        operation_status);
                }
                if (task.kind == TaskKind::reload_all)
                    reload_all_pending = false;
                else if (task.kind != TaskKind::refresh)
                    busy_plugin_ids.erase(task.plugin_id);
                refresh_running = true;
            }
            const sao_status_t refresh_status = refresh_now();
            {
                std::lock_guard lock(mutex);
                refresh_running = false;
                worker_active = false;
                cache_dirty = true;
                if (refresh_status != SAO_STATUS_OK) {
                    if (operation_error.empty())
                        operation_error = status_message("Plugin Manager refresh", refresh_status);
                    cache_dirty = true;
                } else if (task.kind != TaskKind::refresh) {
                    for (auto it = tasks.begin(); it != tasks.end();) {
                        if (it->kind == TaskKind::refresh)
                            it = tasks.erase(it);
                        else
                            ++it;
                    }
                    refresh_queued = false;
                }
            }
        }
    }

    sao_status_t dispatch(std::string_view action_id, std::string_view payload_json) noexcept {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        if (!valid_text(action_id, 64U, true) ||
            payload_json.size() > kMaximumActionPayloadBytes ||
            payload_json.find('\0') != std::string_view::npos || !valid_utf8(payload_json)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (action_id == kActionRefresh) {
            clear_operation_message();
            return refresh();
        }
        if (action_id == kActionReloadAll)
            return enqueue_task(Task{TaskKind::reload_all, {}});

        TaskKind kind{};
        if (action_id == kActionEnable)
            kind = TaskKind::enable;
        else if (action_id == kActionDisable)
            kind = TaskKind::disable;
        else if (action_id == kActionReload)
            kind = TaskKind::reload;
        else
            return SAO_STATUS_ERR_NOT_FOUND;

        const Json payload =
            Json::parse(payload_json.begin(), payload_json.end(), nullptr, false, false);
        if (!payload.is_string())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::string plugin_id = payload.get<std::string>();
        if (!valid_text(plugin_id, kMaximumPluginIdBytes, true))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return enqueue_task(Task{kind, std::move(plugin_id)});
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

    void fail_next_unregister_for_testing(sao_status_t status) noexcept {
        std::lock_guard lock(mutex);
        fail_next_unregister_status = status;
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
            if (creating || retiring || callbacks_in_flight != 0U || worker_active ||
                refresh_running || worker_publishing || owner_publishing || !tasks.empty()) {
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            }
            if (panel == nullptr)
                return SAO_STATUS_OK;
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
        if (status == SAO_STATUS_OK) {
            sao_status_t injected = SAO_STATUS_OK;
            {
                std::lock_guard lock(mutex);
                injected = std::exchange(fail_next_unregister_status, SAO_STATUS_OK);
            }
            status = injected == SAO_STATUS_OK ? sao_ui_panel_unregister(target_panel) : injected;
        }

        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex);
            panel = nullptr;
            body = nullptr;
            action_handler_attached = false;
            event_handler_attached = false;
            accepting = false;
            last_snapshot = {};
            snapshot_available = false;
            operation_error.clear();
            last_published_body = nullptr;
            last_published_spec.clear();
            refresh_queued = false;
            refresh_running = false;
            retiring = false;
            return SAO_STATUS_OK;
        }

        bool action_restored = !had_action;
        bool event_restored = !had_event;
        if (had_event && event_detached) {
            const sao_status_t injected = std::exchange(fail_next_event_restore_status, SAO_STATUS_OK);
            event_restored = injected == SAO_STATUS_OK &&
                             sao_ui_panel_set_event_handler(target_panel, &event_callback, this) ==
                             SAO_STATUS_OK;
        } else if (had_event) {
            event_restored = true;
        }
        if (had_action && action_detached) {
            const sao_status_t injected = std::exchange(fail_next_action_restore_status, SAO_STATUS_OK);
            action_restored = injected == SAO_STATUS_OK &&
                sao_ui_panel_set_action_handler(target_panel, &action_callback, this) ==
                SAO_STATUS_OK;
        } else if (had_action) {
            action_restored = true;
        }

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

    void request_shutdown() noexcept {
        {
            std::lock_guard lock(mutex);
            worker_accepting = false;
            accepting = false;
            tasks.clear();
            busy_plugin_ids.clear();
            reload_all_pending = false;
            refresh_queued = false;
            refresh_running = false;
        }
        worker_cv.notify_all();
        if (worker.joinable())
            worker.request_stop();
    }

    void stop_worker() noexcept {
        {
            std::lock_guard lock(mutex);
            worker_accepting = false;
            tasks.clear();
        }
        worker_cv.notify_all();
        if (worker.joinable()) {
            worker.request_stop();
            worker_cv.notify_all();
            worker.join();
        }
        std::lock_guard lock(mutex);
        worker_active = false;
        worker_publishing = false;
        owner_publishing = false;
        busy_plugin_ids.clear();
        reload_all_pending = false;
        refresh_queued = false;
        refresh_running = false;
        worker_online = false;
    }

    bool shutdown_noexcept() noexcept {
        stop_worker();
        if (panel_handle() == nullptr)
            return true;
        if (require_owner_thread() != SAO_STATUS_OK)
            return false;
        for (int attempt = 0; attempt < 1 && panel_handle() != nullptr; ++attempt) {
            if (take_offline() == SAO_STATUS_OK)
                break;
        }
        return panel_handle() == nullptr;
    }

    static void defer_cleanup(std::unique_ptr<Impl> state) noexcept;
    static void drain_deferred_cleanup() noexcept;

    sao_ui_compositor_handle_t compositor{};
    mutable std::mutex mutex;
    std::condition_variable_any worker_cv;
    std::deque<Task> tasks;
    std::jthread worker;
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    sao_ui_panel_body_handle_t last_published_body{};
    std::string last_published_spec;
    Operations operations;
    Snapshot last_snapshot;
    std::string operation_error;
    std::unordered_set<std::string> busy_plugin_ids;
    bool reload_all_pending{};
    bool refresh_queued{};
    bool refresh_running{};
    sao_status_t fail_next_unregister_status{SAO_STATUS_OK};
    sao_status_t fail_next_action_restore_status{SAO_STATUS_OK};
    sao_status_t fail_next_event_restore_status{SAO_STATUS_OK};
    std::size_t callbacks_in_flight{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool accepting{};
    bool creating{};
    bool retiring{};
    bool worker_accepting{true};
    bool worker_online{};
    bool worker_active{};
    bool worker_publishing{};
    bool owner_publishing{};
    bool snapshot_available{};
    bool cache_dirty{true};
};

std::mutex Owner::Impl::deferred_mutex;
std::vector<std::unique_ptr<Owner::Impl>> Owner::Impl::deferred_cleanup;

void Owner::Impl::defer_cleanup(std::unique_ptr<Impl> state) noexcept { if (state == nullptr) return; { std::lock_guard lock(state->mutex); state->accepting = false; } std::lock_guard lock(deferred_mutex); deferred_cleanup.push_back(std::move(state)); }
void Owner::Impl::drain_deferred_cleanup() noexcept { std::vector<std::unique_ptr<Impl>> pending; { std::lock_guard lock(deferred_mutex); pending.swap(deferred_cleanup); } std::vector<std::unique_ptr<Impl>> retry; for (auto& state : pending) { const sao_status_t status = state->take_offline(); if (status != SAO_STATUS_OK) retry.push_back(std::move(state)); } if (!retry.empty()) { std::lock_guard lock(deferred_mutex); for (auto& state : retry) deferred_cleanup.push_back(std::move(state)); } }

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor) noexcept
    : Owner(borrowed_compositor, make_default_operations()) {}

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor,
             ReloadAllHandler reload_all_handler) noexcept
    : Owner(borrowed_compositor,
            make_default_operations_with_reload_all(std::move(reload_all_handler))) {}

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor, Operations operations) noexcept
    : impl_(new (std::nothrow) Impl(borrowed_compositor, std::move(operations))) {}

Owner::~Owner() {
    if (impl_ == nullptr)
        return;
    auto state = std::move(impl_);
    if (!state->shutdown_noexcept())
        Impl::defer_cleanup(std::move(state));
}

Owner::Owner(Owner&& other) noexcept = default;

Owner& Owner::operator=(Owner&& other) noexcept {
    if (this == &other)
        return *this;
    if (impl_ != nullptr) {
        auto state = std::move(impl_);
        if (!state->shutdown_noexcept())
            Impl::defer_cleanup(std::move(state));
    }
    impl_ = std::move(other.impl_);
    return *this;
}

void Owner::request_shutdown() noexcept {
    if (impl_ != nullptr)
        impl_->request_shutdown();
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

sao_status_t Owner::service_ui() noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->service_ui();
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

void Owner::fail_next_handler_restore_for_testing(sao_status_t action_status, sao_status_t event_status) noexcept {
    if (impl_ != nullptr) {
        std::lock_guard lock(impl_->mutex);
        impl_->fail_next_action_restore_status = action_status;
        impl_->fail_next_event_restore_status = event_status;
    }
}

void Owner::fail_next_unregister_for_testing(sao_status_t status) noexcept {
    if (impl_ != nullptr)
        impl_->fail_next_unregister_for_testing(status);
}

void Owner::drain_deferred_cleanup_for_owner() noexcept { Impl::drain_deferred_cleanup(); }

void Owner::drain_deferred_cleanup_for_testing() noexcept { drain_deferred_cleanup_for_owner(); }

} // namespace sao::launcher::plugin_manager_panel
