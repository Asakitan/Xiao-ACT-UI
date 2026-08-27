#include "sao/plugins/lua_host/lua_module_bridge.h"

#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "lua_bridge_internal.h"
#include "lua_json_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)

namespace {

using loader_context_t = sao::plugins::loader::plugin_context_t;

struct bridge_state;

struct menu_row {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const menu_row&) const = default;
};

struct menu_bridge {
    lua_State* state = nullptr;
    int builder_ref = LUA_NOREF;
    bool enable_scoped = false;
    std::string provider_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::uint64_t revision = 0;
    bool closing = false;
    std::vector<menu_row> rows;
    std::unordered_map<std::string, int> actions;
};

struct action_handler_bridge {
    lua_State* state = nullptr;
    int persistent_ref = LUA_NOREF;
    int enable_ref = LUA_NOREF;
    bool closing = false;
};

struct event_callback_record {
    std::atomic<lua_State*> state{nullptr};
    std::shared_ptr<std::recursive_mutex> state_mutex;
    struct callback_gate {
        enum class registration_state : std::uint8_t { registering, armed, fired_pending, closed };
        std::mutex mutex;
        bool accepting = true;
        size_t in_flight = 0;
        registration_state registration = registration_state::armed;
        std::string pending_event_json;
    } gate;
    bridge_state* owner = nullptr;
    int function_ref = LUA_NOREF;
    uint32_t token = 0;
    uintptr_t dispatch_id = 0;
    std::atomic_bool closing{false};
    bool one_shot = false;
    std::string key;
    std::string loader_token;
    std::uint64_t sequence = 0;
};

enum class passive_resource_kind : std::uint8_t { notification, overlay };

struct passive_resource {
    passive_resource_kind kind = passive_resource_kind::notification;
    std::string key;
    std::uint64_t sequence = 0;
};

struct bridge_state {
    lua_State* state = nullptr;
    loader_context_t* context = nullptr;
    int context_ref = LUA_NOREF;
    bool context_lease = false;
    std::shared_ptr<std::recursive_mutex> mutex = std::make_shared<std::recursive_mutex>();
    bool closing = false;
    std::unordered_map<uint32_t, std::shared_ptr<event_callback_record>> callbacks;
    std::unordered_map<std::string, std::shared_ptr<event_callback_record>> hotkeys;
    std::unordered_map<std::string, std::shared_ptr<event_callback_record>> timers;
    std::unordered_map<uint32_t, std::shared_ptr<event_callback_record>> render_hooks;
    std::unordered_map<std::string, int> engines;
    std::vector<passive_resource> passive_resources;
    std::vector<std::unique_ptr<menu_bridge>> menus;
    action_handler_bridge action_handler;
    bool enable_checkpoint_active = false;
    bool enable_checkpoint_had_action = false;
    std::uint64_t next_resource_sequence = 1;
};

std::mutex g_bridge_map_mutex;
std::unordered_map<lua_State*, std::unique_ptr<bridge_state>> g_bridge_map;
std::unordered_map<lua_State*, std::vector<std::unique_ptr<bridge_state>>> g_retired_bridges;
std::mutex g_callback_map_mutex;
std::unordered_map<uintptr_t, std::shared_ptr<event_callback_record>> g_callback_map;
std::atomic_uintptr_t g_next_callback_id{1};

constexpr size_t kMaximumCallbackNesting = 64;
thread_local std::array<event_callback_record*, kMaximumCallbackNesting> g_active_callbacks{};
thread_local size_t g_active_callback_depth = 0;

constexpr std::size_t kMaximumMenuRows = 4096;
constexpr std::size_t kMaximumMenuStringBytes = 16U * 1024U;
constexpr std::size_t kMaximumMenuSnapshotBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumRememberedActions = 4096;
constexpr std::string_view kActionProviderId = "action-handler";

bool callback_active_on_current_thread(const event_callback_record& callback) noexcept {
    return std::find(g_active_callbacks.begin(),
                     g_active_callbacks.begin() + g_active_callback_depth,
                     &callback) != g_active_callbacks.begin() + g_active_callback_depth;
}

bool enter_callback(event_callback_record& callback,
                    const char* pending_event_json = nullptr) noexcept {
    if (g_active_callback_depth == kMaximumCallbackNesting)
        return false;
    try {
        std::lock_guard lock(callback.gate.mutex);
        if (!callback.gate.accepting || callback.closing.load())
            return false;
        if (callback.one_shot &&
            callback.gate.registration ==
                event_callback_record::callback_gate::registration_state::registering) {
            callback.gate.registration =
                event_callback_record::callback_gate::registration_state::fired_pending;
            if (pending_event_json != nullptr)
                callback.gate.pending_event_json = pending_event_json;
            return false;
        }
        if (callback.one_shot &&
            callback.gate.registration !=
                event_callback_record::callback_gate::registration_state::armed) {
            return false;
        }
        if (callback.one_shot) {
            callback.gate.registration =
                event_callback_record::callback_gate::registration_state::closed;
            callback.gate.accepting = false;
        }
        ++callback.gate.in_flight;
        g_active_callbacks[g_active_callback_depth++] = &callback;
        return true;
    } catch (...) {
        return false;
    }
}

bool arm_one_shot(event_callback_record& callback, std::string* pending_event_json = nullptr) {
    std::lock_guard lock(callback.gate.mutex);
    using registration_state = event_callback_record::callback_gate::registration_state;
    if (!callback.one_shot || callback.gate.registration == registration_state::closed)
        return false;
    const bool fired = callback.gate.registration == registration_state::fired_pending;
    callback.gate.registration = registration_state::armed;
    if (pending_event_json != nullptr)
        pending_event_json->swap(callback.gate.pending_event_json);
    return fired;
}

void leave_callback(event_callback_record& callback) noexcept {
    if (g_active_callback_depth > 0 &&
        g_active_callbacks[g_active_callback_depth - 1] == &callback) {
        g_active_callbacks[--g_active_callback_depth] = nullptr;
    }
    try {
        std::lock_guard lock(callback.gate.mutex);
        if (callback.gate.in_flight > 0)
            --callback.gate.in_flight;
    } catch (...) {
    }
}

int32_t stop_callback(event_callback_record& callback) noexcept {
    try {
        std::lock_guard lock(callback.gate.mutex);
        if (callback_active_on_current_thread(callback) || callback.gate.in_flight != 0) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        callback.gate.accepting = false;
        callback.gate.registration =
            event_callback_record::callback_gate::registration_state::closed;
        callback.closing.store(true, std::memory_order_release);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void resume_callback(event_callback_record& callback) noexcept {
    try {
        std::lock_guard lock(callback.gate.mutex);
        callback.closing.store(false, std::memory_order_release);
        callback.gate.accepting = true;
        if (callback.one_shot) {
            callback.gate.registration =
                event_callback_record::callback_gate::registration_state::armed;
        }
    } catch (...) {
    }
}

std::shared_ptr<event_callback_record> find_callback(void* user_data) noexcept {
    try {
        const uintptr_t dispatch_id = reinterpret_cast<uintptr_t>(user_data);
        std::lock_guard map_lock(g_callback_map_mutex);
        const auto found = g_callback_map.find(dispatch_id);
        return found == g_callback_map.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

void forget_callback(const event_callback_record& callback) noexcept {
    try {
        std::lock_guard map_lock(g_callback_map_mutex);
        g_callback_map.erase(callback.dispatch_id);
    } catch (...) {
    }
}

std::shared_ptr<event_callback_record> make_callback(bridge_state& bridge, lua_State* current,
                                                     int function_index, bool one_shot = false) {
    lua_State* state = bridge.state;
    luaL_checktype(current, function_index, LUA_TFUNCTION);
    lua_pushvalue(current, function_index);
    if (current != state)
        lua_xmove(current, state, 1);
    const int function_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    std::shared_ptr<event_callback_record> callback;
    try {
        callback = std::make_shared<event_callback_record>();
        callback->state = state;
        callback->state_mutex = detail::state_mutex(state);
        callback->owner = &bridge;
        callback->function_ref = function_ref;
        callback->one_shot = one_shot;
        callback->gate.registration =
            one_shot ? event_callback_record::callback_gate::registration_state::registering
                     : event_callback_record::callback_gate::registration_state::armed;
        callback->sequence = bridge.next_resource_sequence++;
        std::lock_guard map_lock(g_callback_map_mutex);
        do {
            callback->dispatch_id = g_next_callback_id.fetch_add(1);
        } while (callback->dispatch_id == 0 || g_callback_map.contains(callback->dispatch_id));
        g_callback_map.emplace(callback->dispatch_id, callback);
        return callback;
    } catch (...) {
        luaL_unref(state, LUA_REGISTRYINDEX, function_ref);
        throw;
    }
}

void release_callback_ref(lua_State* state, event_callback_record& callback) noexcept {
    callback.closing.store(true, std::memory_order_release);
    callback.state.store(nullptr, std::memory_order_release);
    forget_callback(callback);
    if (callback.function_ref != LUA_NOREF && callback.function_ref != LUA_REFNIL) {
        luaL_unref(state, LUA_REGISTRYINDEX, callback.function_ref);
        callback.function_ref = LUA_NOREF;
    }
}

std::uint64_t stable_hash(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_suffix(std::string_view value) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(stable_hash(value)));
    return buffer;
}

bool valid_menu_string(std::string_view value, bool required) noexcept {
    return (!required || !value.empty()) && value.size() <= kMaximumMenuStringBytes &&
           value.find('\0') == std::string_view::npos;
}

bool stack_menu_string(lua_State* state, int index, bool required, std::string& output,
                       std::string& error) {
    output.clear();
    if (lua_isnoneornil(state, index)) {
        if (required)
            error = "required menu text field is missing";
        return !required;
    }
    if (lua_type(state, index) != LUA_TSTRING) {
        error = "menu text fields must be strings";
        return false;
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    if (value == nullptr || !valid_menu_string(std::string_view(value, length), required)) {
        error = "menu text field is invalid";
        return false;
    }
    output.assign(value, length);
    return true;
}

void raw_get_field(lua_State* state, int table_index, const char* key) {
    table_index = lua_absindex(state, table_index);
    lua_pushstring(state, key);
    lua_rawget(state, table_index);
}

bool table_menu_string(lua_State* state, int table_index, const char* key, bool required,
                       std::string& output, std::string& error) {
    table_index = lua_absindex(state, table_index);
    raw_get_field(state, table_index, key);
    const bool result = stack_menu_string(state, -1, required, output, error);
    lua_pop(state, 1);
    return result;
}

bool table_menu_flag(lua_State* state, int table_index, const char* key, bool fallback,
                     bool& output, std::string& error) {
    table_index = lua_absindex(state, table_index);
    raw_get_field(state, table_index, key);
    if (lua_isnil(state, -1)) {
        output = fallback;
    } else if (!lua_isboolean(state, -1)) {
        lua_pop(state, 1);
        error = "menu flag fields must be booleans";
        return false;
    } else {
        output = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    return true;
}

bool table_menu_payload(lua_State* state, int table_index, std::string& output,
                        std::string& error) {
    table_index = lua_absindex(state, table_index);
    raw_get_field(state, table_index, "payload_json");
    if (!lua_isnil(state, -1)) {
        const bool result = stack_menu_string(state, -1, false, output, error);
        lua_pop(state, 1);
        if (!result)
            return false;
        detail::json payload;
        if (!detail::parse_json(output.data(), output.size(), payload, error)) {
            error = "menu payload_json must be valid JSON";
            return false;
        }
        return true;
    }
    lua_pop(state, 1);

    raw_get_field(state, table_index, "payload");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        output = "{}";
        return true;
    }
    detail::json payload;
    if (!detail::stack_to_json(state, -1, payload, error)) {
        lua_pop(state, 1);
        return false;
    }
    if (!detail::serialize_json(payload, output, error)) {
        lua_pop(state, 1);
        return false;
    }
    lua_pop(state, 1);
    if (!valid_menu_string(output, false)) {
        error = "menu payload exceeds its field budget";
        return false;
    }
    return true;
}

std::string callable_identity(lua_State* state, int index) {
    index = lua_absindex(state, index);
    lua_Debug debug{};
    lua_pushvalue(state, index);
    if (lua_getinfo(state, ">S", &debug) == 0) {
        return "lua-callable";
    }
    std::string result = debug.source == nullptr ? "lua-callable" : debug.source;
    result.push_back(':');
    result.append(std::to_string(debug.linedefined));
    result.push_back(':');
    result.append(std::to_string(debug.lastlinedefined));
    result.push_back(':');
    result.append(debug.what == nullptr ? "" : debug.what);
    return result;
}

void append_identity_field(std::string& identity, std::string_view value) {
    identity.append(std::to_string(value.size()));
    identity.push_back(':');
    identity.append(value);
    identity.push_back('\n');
}

void release_action_refs(lua_State* state, std::unordered_map<std::string, int>& actions) noexcept {
    for (const auto& [_, reference] : actions) {
        if (reference != LUA_NOREF && reference != LUA_REFNIL) {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
        }
    }
    actions.clear();
}

bool valid_registry_ref(int reference) noexcept {
    return reference != LUA_NOREF && reference != LUA_REFNIL;
}

int current_action_handler_ref(const action_handler_bridge& handler) noexcept {
    return valid_registry_ref(handler.enable_ref) ? handler.enable_ref : handler.persistent_ref;
}

void release_registry_ref(lua_State* state, int& reference) noexcept {
    if (valid_registry_ref(reference))
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
    reference = LUA_NOREF;
}

void release_action_handler_refs(action_handler_bridge& handler) noexcept {
    if (handler.state == nullptr)
        return;
    release_registry_ref(handler.state, handler.enable_ref);
    release_registry_ref(handler.state, handler.persistent_ref);
}

int32_t unregister_entity_provider_ids(
    bridge_state& bridge, const std::vector<std::string_view>& local_provider_ids) noexcept {
    if (bridge.context == nullptr || local_provider_ids.empty())
        return SAO_OK;
    try {
        const char* plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(bridge.context);
        if (plugin_id == nullptr || plugin_id[0] == '\0')
            return SAO_ERR_HANDLE_INVALID;
        std::vector<std::string> qualified_ids;
        qualified_ids.reserve(local_provider_ids.size());
        std::vector<const char*> provider_ids;
        provider_ids.reserve(local_provider_ids.size());
        for (const auto provider_id : local_provider_ids) {
            qualified_ids.push_back(std::string(plugin_id) + "/" + std::string(provider_id));
        }
        for (const auto& provider_id : qualified_ids)
            provider_ids.push_back(provider_id.c_str());
        return sao::plugins::loader::plugin_context_unregister_entity_providers(
            bridge.context, provider_ids.data(), provider_ids.size());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_menu_providers(bridge_state& bridge,
                                  const std::vector<menu_bridge*>& menus) noexcept {
    if (menus.empty()) {
        return SAO_OK;
    }
    try {
        std::vector<std::string_view> provider_ids;
        provider_ids.reserve(menus.size());
        for (const auto* menu : menus)
            provider_ids.push_back(menu->provider_id);
        return unregister_entity_provider_ids(bridge, provider_ids);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_action_provider(bridge_state& bridge) noexcept {
    try {
        return unregister_entity_provider_ids(bridge, {kActionProviderId});
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_all_entity_providers(bridge_state& bridge) noexcept {
    try {
        std::vector<std::string_view> provider_ids;
        provider_ids.reserve(bridge.menus.size() + 1);
        for (const auto& menu : bridge.menus)
            provider_ids.push_back(menu->provider_id);
        if (valid_registry_ref(current_action_handler_ref(bridge.action_handler)))
            provider_ids.push_back(kActionProviderId);
        return unregister_entity_provider_ids(bridge, provider_ids);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_menu_providers(bridge_state& bridge, std::size_t checkpoint) noexcept {
    if (checkpoint >= bridge.menus.size())
        return SAO_OK;
    try {
        std::vector<menu_bridge*> menus;
        menus.reserve(bridge.menus.size() - checkpoint);
        for (std::size_t index = checkpoint; index < bridge.menus.size(); ++index) {
            menus.push_back(bridge.menus[index].get());
        }
        return unregister_menu_providers(bridge, menus);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

class registry_ref_guard {
  public:
    registry_ref_guard(lua_State* state, int reference) noexcept
        : state_(state), reference_(reference) {}

    ~registry_ref_guard() {
        if (reference_ != LUA_NOREF && reference_ != LUA_REFNIL) {
            luaL_unref(state_, LUA_REGISTRYINDEX, reference_);
        }
    }

    registry_ref_guard(const registry_ref_guard&) = delete;
    registry_ref_guard& operator=(const registry_ref_guard&) = delete;

    int get() const noexcept {
        return reference_;
    }

    void release() noexcept {
        reference_ = LUA_NOREF;
    }

  private:
    lua_State* state_ = nullptr;
    int reference_ = LUA_NOREF;
};

bool sequence_table(lua_State* state, int index, std::size_t& count, std::string& error) {
    index = lua_absindex(state, index);
    if (!lua_istable(state, index)) {
        error = "menu builder must return a sequence table";
        return false;
    }
    count = lua_rawlen(state, index);
    if (count > kMaximumMenuRows) {
        error = "menu builder returned too many rows";
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        const bool valid_key = lua_isinteger(state, -2) && lua_tointeger(state, -2) > 0 &&
                               static_cast<std::size_t>(lua_tointeger(state, -2)) <= count;
        lua_pop(state, 1);
        if (!valid_key) {
            lua_pop(state, 1);
            error = "menu builder result must be a sequence table";
            return false;
        }
    }
    return true;
}

bool build_menu_snapshot(menu_bridge* bridge, std::string& error) {
    lua_State* state = bridge->state;
    const int base = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, bridge->builder_ref);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, base);
        error = "menu builder is no longer callable";
        return false;
    }
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        detail::capture_state_error_locked(state, -1);
        lua_settop(state, base);
        return false;
    }

    std::size_t count = 0;
    if (!sequence_table(state, -1, count, error)) {
        lua_settop(state, base);
        return false;
    }
    const int sequence_index = lua_absindex(state, -1);
    std::vector<menu_row> rows;
    std::unordered_map<std::string, int> actions;
    std::unordered_map<std::string, std::size_t> identity_occurrences;
    std::unordered_set<std::string> action_ids;
    try {
        rows.reserve(count);
        actions.reserve(count);
        identity_occurrences.reserve(count);
        action_ids.reserve(count);
        std::size_t total_bytes = 0;
        for (std::size_t index = 0; index < count; ++index) {
            lua_rawgeti(state, sequence_index, static_cast<lua_Integer>(index + 1));
            if (!lua_istable(state, -1)) {
                error = "menu rows must be tables";
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }
            const int row_index = lua_absindex(state, -1);
            menu_row row;
            if (!table_menu_string(state, row_index, "label", true, row.label, error) ||
                !table_menu_string(state, row_index, "icon", false, row.icon, error) ||
                !table_menu_payload(state, row_index, row.payload_json, error) ||
                !table_menu_flag(state, row_index, "keep_menu_open", false, row.keep_menu_open,
                                 error) ||
                !table_menu_flag(state, row_index, "close_menu_before", false,
                                 row.close_menu_before, error)) {
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }

            raw_get_field(state, row_index, "command");
            const bool has_command = !lua_isnil(state, -1);
            if (has_command && !lua_isfunction(state, -1)) {
                lua_pop(state, 1);
                error = "menu command must be a function";
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }
            const std::string command_identity =
                has_command ? callable_identity(state, -1) : std::string{};
            int command_ref = LUA_NOREF;
            if (has_command) {
                lua_pushvalue(state, -1);
                command_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            }
            registry_ref_guard command_guard(state, command_ref);
            lua_pop(state, 1);
            row.can_activate = has_command;
            bool requested_can_activate = row.can_activate;
            if (!table_menu_flag(state, row_index, "can_activate", row.can_activate,
                                 requested_can_activate, error)) {
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }
            row.can_activate = has_command && requested_can_activate;

            std::string explicit_identity;
            raw_get_field(state, row_index, "action_id");
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                raw_get_field(state, row_index, "id");
            }
            if (!lua_isnil(state, -1) &&
                !stack_menu_string(state, -1, true, explicit_identity, error)) {
                lua_pop(state, 1);
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }
            lua_pop(state, 1);

            std::string identity = explicit_identity;
            if (identity.empty()) {
                append_identity_field(identity, command_identity);
                append_identity_field(identity, row.label);
                append_identity_field(identity, row.icon);
                append_identity_field(identity, row.payload_json);
                identity.push_back(row.can_activate ? '1' : '0');
                identity.push_back(row.keep_menu_open ? '1' : '0');
                identity.push_back(row.close_menu_before ? '1' : '0');
                const std::size_t occurrence = identity_occurrences[identity]++;
                identity.push_back('#');
                identity.append(std::to_string(occurrence));
            }
            row.action_id = "menu-action-" + hash_suffix(bridge->contribution_id + "\n" + identity);
            if (!action_ids.emplace(row.action_id).second) {
                error = "menu action identities must be unique";
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }
            if (command_ref != LUA_NOREF) {
                actions.emplace(row.action_id, command_ref);
                command_guard.release();
            }

            const std::size_t row_bytes = row.label.size() + row.icon.size() +
                                          row.action_id.size() + row.payload_json.size() +
                                          bridge->contribution_id.size() + bridge->name.size() +
                                          bridge->icon.size();
            if (total_bytes > kMaximumMenuSnapshotBytes ||
                row_bytes > kMaximumMenuSnapshotBytes - total_bytes) {
                error = "menu builder snapshot exceeds its byte budget";
                release_action_refs(state, actions);
                lua_settop(state, base);
                return false;
            }
            total_bytes += row_bytes;
            rows.push_back(std::move(row));
            lua_pop(state, 1);
        }

        std::size_t new_action_count = 0;
        for (const auto& [action_id, _] : actions) {
            if (!bridge->actions.contains(action_id))
                ++new_action_count;
        }
        if (bridge->actions.size() > kMaximumRememberedActions ||
            new_action_count > kMaximumRememberedActions - bridge->actions.size()) {
            error = "menu action history exceeds its budget";
            release_action_refs(state, actions);
            lua_settop(state, base);
            return false;
        }

        const bool rows_changed = bridge->revision == 0 || bridge->rows != rows;
        if (rows_changed && bridge->revision == std::numeric_limits<std::uint64_t>::max()) {
            error = "menu snapshot revision exhausted";
            release_action_refs(state, actions);
            lua_settop(state, base);
            return false;
        }

        auto next_actions = bridge->actions;
        for (const auto& [action_id, reference] : actions) {
            next_actions.insert_or_assign(action_id, reference);
        }
        std::vector<int> replaced_refs;
        replaced_refs.reserve(actions.size());
        for (const auto& [action_id, _] : actions) {
            const auto existing = bridge->actions.find(action_id);
            if (existing != bridge->actions.end()) {
                replaced_refs.push_back(existing->second);
            }
        }
        bridge->actions.swap(next_actions);
        for (auto& [_, reference] : actions)
            reference = LUA_NOREF;
        for (const int reference : replaced_refs) {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
        }
        bridge->rows = std::move(rows);
        if (rows_changed)
            ++bridge->revision;
        lua_settop(state, base);
        return true;
    } catch (...) {
        release_action_refs(state, actions);
        lua_settop(state, base);
        error = "menu snapshot conversion failed";
        return false;
    }
}

struct menu_snapshot_build_context {
    menu_bridge* bridge = nullptr;
    std::string* error = nullptr;
    bool succeeded = false;
};

int build_menu_snapshot_body(lua_State* state) {
    auto* context = static_cast<menu_snapshot_build_context*>(lua_touserdata(state, 1));
    context->succeeded = build_menu_snapshot(context->bridge, *context->error);
    return 0;
}

int32_t SAO_PLUGINS_CALL native_menu_snapshot(sao::plugins::loader::entity_menu_row* rows,
                                              std::uint32_t capacity, std::uint32_t* out_count,
                                              std::uint64_t* out_revision, void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr ||
        (capacity > 0 && rows == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* bridge = static_cast<menu_bridge*>(user_data);
    try {
        detail::state_operation operation;
        const int32_t status = detail::acquire_state_operation(bridge->state, operation);
        if (status != SAO_OK)
            return status;
        if (bridge->closing) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        if (rows == nullptr) {
            detail::clear_state_error_locked(bridge->state);
            std::string error;
            menu_snapshot_build_context context{bridge, &error, false};
            if (detail::protected_trampoline(bridge->state, build_menu_snapshot_body, &context,
                                             0) != LUA_OK) {
                detail::capture_state_error_locked(bridge->state, -1);
                lua_pop(bridge->state, 1);
                return SAO_ERR_OS_CALL_FAILED;
            }
            if (!context.succeeded) {
                if (!error.empty()) {
                    detail::set_state_error_locked(bridge->state, std::move(error));
                }
                return SAO_ERR_OS_CALL_FAILED;
            }
        }
        *out_count = static_cast<std::uint32_t>(bridge->rows.size());
        *out_revision = bridge->revision;
        if (capacity < bridge->rows.size()) {
            return SAO_ERR_BUFFER_TOO_SMALL;
        }
        for (std::size_t index = 0; index < bridge->rows.size(); ++index) {
            const auto& source = bridge->rows[index];
            rows[index] = {
                sizeof(sao::plugins::loader::entity_menu_row),
                bridge->contribution_id.c_str(),
                bridge->name.c_str(),
                bridge->icon.c_str(),
                bridge->priority,
                source.label.c_str(),
                source.icon.c_str(),
                source.action_id.c_str(),
                source.payload_json.c_str(),
                static_cast<std::uint8_t>(source.can_activate),
                static_cast<std::uint8_t>(source.keep_menu_open),
                static_cast<std::uint8_t>(source.close_menu_before),
                {},
            };
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL native_menu_action(const char* action_id_utf8, const char*,
                                            void* user_data) {
    if (action_id_utf8 == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* bridge = static_cast<menu_bridge*>(user_data);
    try {
        detail::state_operation operation;
        const int32_t status = detail::acquire_state_operation(bridge->state, operation);
        if (status != SAO_OK)
            return status;
        if (bridge->closing) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        const auto found = bridge->actions.find(action_id_utf8);
        if (found == bridge->actions.end())
            return SAO_ERR_HANDLE_INVALID;
        lua_rawgeti(bridge->state, LUA_REGISTRYINDEX, found->second);
        if (!lua_isfunction(bridge->state, -1)) {
            lua_pop(bridge->state, 1);
            return SAO_ERR_HANDLE_INVALID;
        }
        detail::clear_state_error_locked(bridge->state);
        if (lua_pcall(bridge->state, 0, 0, 0) != LUA_OK) {
            detail::capture_state_error_locked(bridge->state, -1);
            lua_pop(bridge->state, 1);
            return SAO_ERR_OS_CALL_FAILED;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL native_action_handler_v2(
    const char* action_id_utf8, const char* payload_json_utf8,
    sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
    void* result_sink_user_data, void* user_data) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr || result_sink == nullptr ||
        user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* handler = static_cast<action_handler_bridge*>(user_data);
    try {
        detail::state_operation operation;
        const int32_t status = detail::acquire_state_operation(handler->state, operation);
        if (status != SAO_OK)
            return status;
        lua_State* state = operation.state();
        if (handler->closing)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        const int reference = current_action_handler_ref(*handler);
        if (!valid_registry_ref(reference))
            return SAO_ERR_HANDLE_INVALID;

        const int base = lua_gettop(state);
        detail::clear_state_error_locked(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
        if (!lua_isfunction(state, -1)) {
            lua_settop(state, base);
            return SAO_ERR_HANDLE_INVALID;
        }
        lua_pushstring(state, action_id_utf8);
        detail::json payload;
        std::string conversion_error;
        if (!detail::parse_json_c_string(payload_json_utf8, payload, conversion_error) ||
            !detail::protected_push_json(state, payload, conversion_error, true)) {
            detail::set_state_error_locked(state, std::move(conversion_error));
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }

        sao::plugins::loader::entity_action_result_v2 result{};
        result.struct_size = sizeof(result);
        result.abi_version = sao::plugins::loader::kEntityActionAbiVersion2;
        if (lua_isnil(state, -1)) {
            result.handled = 0;
            const int32_t sink_status = result_sink(&result, result_sink_user_data);
            lua_settop(state, base);
            return sink_status;
        }

        detail::json result_value;
        if (!detail::stack_to_json(state, -1, result_value, conversion_error)) {
            detail::set_state_error_locked(state, std::move(conversion_error));
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::string serialized;
        if (!detail::serialize_json(result_value, serialized, conversion_error)) {
            detail::set_state_error_locked(state, std::move(conversion_error));
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        result.handled = 1;
        result.result_json_utf8 = serialized.c_str();
        const int32_t sink_status = result_sink(&result, result_sink_user_data);
        lua_settop(state, base);
        return sink_status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_action_provider(bridge_state& bridge) noexcept {
    if (bridge.context == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        sao::plugins::loader::context_entity_provider_descriptor_v3 provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = kActionProviderId.data();
        provider.action_handler_v2 = native_action_handler_v2;
        provider.action_user_data = &bridge.action_handler;
        provider.flags = sao::plugins::loader::kContextEntityProviderV3ActionOnly;
        return sao::plugins::loader::sao_plugins_ctx_register_entity_provider_v3(bridge.context,
                                                                                 &provider);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_menu_provider(bridge_state& bridge, menu_bridge& menu) noexcept {
    if (bridge.context == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        sao::plugins::loader::entity_root_contribution_descriptor root{};
        root.struct_size = sizeof(root);
        root.contribution_id_utf8 = menu.contribution_id.c_str();
        root.root_id_utf8 = menu.root_id.c_str();
        root.name_utf8 = menu.name.c_str();
        root.icon_utf8 = menu.icon.c_str();
        root.priority = menu.priority;

        sao::plugins::loader::context_entity_provider_descriptor provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = menu.provider_id.c_str();
        provider.snapshot = native_menu_snapshot;
        provider.action_handler = native_menu_action;
        provider.user_data = &menu;
        provider.root_contribution = &root;
        return sao::plugins::loader::sao_plugins_ctx_register_entity_provider(bridge.context,
                                                                              &provider);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_menu_providers(bridge_state& bridge) noexcept {
    try {
        std::vector<menu_bridge*> registered;
        registered.reserve(bridge.menus.size());
        for (const auto& menu : bridge.menus) {
            const int32_t status = register_menu_provider(bridge, *menu);
            if (status != SAO_OK) {
                const int32_t rollback_status = unregister_menu_providers(bridge, registered);
                return rollback_status == SAO_OK ? status : rollback_status;
            }
            registered.push_back(menu.get());
        }
        if (valid_registry_ref(current_action_handler_ref(bridge.action_handler))) {
            const int32_t status = register_action_provider(bridge);
            if (status != SAO_OK) {
                const int32_t rollback_status = unregister_menu_providers(bridge, registered);
                return rollback_status == SAO_OK ? status : rollback_status;
            }
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bridge_state* checked_bridge(lua_State* state) {
    auto** slot = static_cast<bridge_state**>(luaL_checkudata(state, 1, "SaoPluginContext"));
    if (slot == nullptr || *slot == nullptr || (*slot)->closing || (*slot)->context == nullptr) {
        luaL_error(state, "plugin context is no longer available");
        return nullptr;
    }
    return *slot;
}

struct method_failure final {
    const char* operation = "ctx method";
    int32_t status = SAO_ERR_OS_CALL_FAILED;
};

[[noreturn]] int push_status_error(lua_State*, const char* operation, int32_t status) {
    throw method_failure{operation, status};
}

bool stack_json(lua_State* state, int index, std::string& serialized) {
    detail::json value;
    std::string error;
    if (!detail::stack_to_json(state, index, value, error)) {
        throw method_failure{"JSON conversion", SAO_ERR_INVALID_ARGUMENT};
    }
    if (!detail::serialize_json(value, serialized, error))
        throw method_failure{"JSON conversion", SAO_ERR_INVALID_ARGUMENT};
    return true;
}

int push_owned_json(lua_State* state, int32_t status, char* value, const char* operation) {
    if (status != SAO_OK) {
        sao::plugins::loader::sao_plugins_ctx_free_string(value);
        return push_status_error(state, operation, status);
    }
    std::string conversion_error;
    detail::json parsed;
    const int32_t conversion = detail::parse_json_c_string(value, parsed, conversion_error) &&
                                       detail::protected_push_json(state, parsed, conversion_error)
                                   ? SAO_OK
                                   : SAO_ERR_INVALID_ARGUMENT;
    sao::plugins::loader::sao_plugins_ctx_free_string(value);
    if (conversion != SAO_OK) {
        return push_status_error(state, operation, conversion);
    }
    return 1;
}

void event_callback(const char*, const char* event_json_utf8, void* user_data) noexcept {
    try {
        auto callback = find_callback(user_data);
        if (callback == nullptr ||
            !enter_callback(*callback, event_json_utf8 == nullptr ? "null" : event_json_utf8))
            return;
        struct callback_guard final {
            event_callback_record& callback;
            ~callback_guard() {
                leave_callback(callback);
            }
        } guard{*callback};
        lua_State* state = callback->state.load(std::memory_order_acquire);
        if (state == nullptr)
            return;
        detail::state_operation operation;
        if (detail::acquire_state_operation(state, operation) != SAO_OK) {
            return;
        }
        if (callback->closing.load(std::memory_order_acquire) ||
            callback->state.load(std::memory_order_acquire) != state ||
            callback->function_ref == LUA_NOREF) {
            return;
        }
        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback->function_ref);
        std::string conversion_error;
        detail::json event;
        if (!detail::parse_json_c_string(event_json_utf8, event, conversion_error) ||
            !detail::protected_push_json(state, event, conversion_error)) {
            lua_settop(state, base);
            return;
        }
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
        }
        if (callback->one_shot && callback->owner != nullptr) {
            callback->owner->callbacks.erase(callback->token);
            release_callback_ref(state, *callback);
        }
    } catch (...) {
    }
}

void invoke_void_callback(void* user_data, bool timer) noexcept {
    try {
        auto callback = find_callback(user_data);
        if (callback == nullptr || !enter_callback(*callback))
            return;
        struct callback_guard final {
            event_callback_record& callback;
            ~callback_guard() {
                leave_callback(callback);
            }
        } guard{*callback};
        lua_State* state = callback->state.load(std::memory_order_acquire);
        if (state == nullptr)
            return;
        detail::state_operation operation;
        if (detail::acquire_state_operation(state, operation) != SAO_OK ||
            callback->closing.load(std::memory_order_acquire)) {
            return;
        }
        if (timer && callback->one_shot && callback->owner != nullptr &&
            callback->owner->context != nullptr && !callback->loader_token.empty()) {
            (void)sao::plugins::loader::sao_plugins_ctx_complete_timer(
                callback->owner->context, callback->loader_token.c_str());
        }
        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback->function_ref);
        if (!lua_isfunction(state, -1) || lua_pcall(state, 0, 0, 0) != LUA_OK) {
            if (lua_gettop(state) > base) {
                detail::capture_state_error_locked(state, -1);
            }
            lua_settop(state, base);
        }
        if (timer && callback->one_shot && callback->owner != nullptr) {
            callback->owner->timers.erase(callback->key);
            release_callback_ref(state, *callback);
        }
    } catch (...) {
    }
}

void hotkey_callback(void* user_data) noexcept {
    invoke_void_callback(user_data, false);
}

void timer_callback(void* user_data) noexcept {
    invoke_void_callback(user_data, true);
}

int32_t render_hook_callback(const char* surface_utf8, const char* payload_json_utf8,
                             char** out_spec_json_utf8, void* user_data) noexcept {
    if (out_spec_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_spec_json_utf8 = nullptr;
    try {
        auto callback = find_callback(user_data);
        if (callback == nullptr || !enter_callback(*callback)) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        struct callback_guard final {
            event_callback_record& callback;
            ~callback_guard() {
                leave_callback(callback);
            }
        } guard{*callback};
        lua_State* state = callback->state.load(std::memory_order_acquire);
        if (state == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        detail::state_operation operation;
        int32_t status = detail::acquire_state_operation(state, operation);
        if (status != SAO_OK)
            return status;
        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback->function_ref);
        lua_pushstring(state, surface_utf8 == nullptr ? "" : surface_utf8);
        std::string conversion_error;
        detail::json payload;
        if (!detail::parse_json_c_string(payload_json_utf8, payload, conversion_error) ||
            !detail::protected_push_json(state, payload, conversion_error)) {
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }
        detail::json spec;
        if (!detail::stack_to_json(state, -1, spec, conversion_error)) {
            detail::set_state_error_locked(state, std::move(conversion_error));
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::string serialized;
        if (!detail::serialize_json(spec, serialized, conversion_error)) {
            detail::set_state_error_locked(state, std::move(conversion_error));
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        auto output = std::make_unique<char[]>(serialized.size() + 1);
        std::copy(serialized.begin(), serialized.end(), output.get());
        output[serialized.size()] = '\0';
        *out_spec_json_utf8 = output.release();
        lua_settop(state, base);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int ctx_log(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* message = luaL_checkstring(state, 2);
    sao::plugins::loader::sao_plugins_ctx_log(bridge->context, message);
    return 0;
}

int ctx_time(lua_State* state) {
    (void)checked_bridge(state);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    lua_pushnumber(state, static_cast<lua_Number>(std::chrono::duration<double>(now).count()));
    return 1;
}

int ctx_set_defaults(lua_State* state) {
    auto* bridge = checked_bridge(state);
    std::string defaults;
    if (!stack_json(state, 2, defaults))
        return 0;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_defaults(bridge->context, defaults.c_str());
    if (status != SAO_OK)
        return push_status_error(state, "set_defaults", status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_get_setting(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* key = luaL_checkstring(state, 2);
    char* json = nullptr;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_get_setting(bridge->context, key, &json);
    if (status == SAO_ERR_HANDLE_INVALID && lua_gettop(state) >= 3) {
        lua_pushvalue(state, 3);
        return 1;
    }
    return push_owned_json(state, status, json, "get_setting");
}

int ctx_set_setting(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* key = luaL_checkstring(state, 2);
    std::string value;
    if (!stack_json(state, 3, value))
        return 0;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_setting(bridge->context, key, value.c_str());
    if (status != SAO_OK)
        return push_status_error(state, "set_setting", status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_snapshot_value(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* path = luaL_checkstring(state, 2);
    char* value = nullptr;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_snapshot_value(bridge->context, path, &value);
    if (status == SAO_ERR_HANDLE_INVALID && lua_gettop(state) >= 3) {
        sao::plugins::loader::sao_plugins_ctx_free_string(value);
        lua_pushvalue(state, 3);
        return 1;
    }
    return push_owned_json(state, status, value, "snapshot_value");
}

void require_permission(lua_State* state, detail::lua_permission permission,
                        const char* operation) {
    if (!detail::state_has_permission_locked(state, permission)) {
        push_status_error(state, operation,
                          sao::plugins::loader::SAO_PLUGINS_ERR_CAPABILITY_MISMATCH);
    }
}

int ctx_register_hotkey(lua_State* state) {
    auto* bridge = checked_bridge(state);
    require_permission(state, detail::permission_hotkey, "register_hotkey");
    const char* hotkey_id = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TFUNCTION);
    const char* default_key = luaL_checkstring(state, 4);
    const char* label = luaL_optstring(state, 5, "");
    if (bridge->hotkeys.contains(hotkey_id)) {
        return push_status_error(state, "register_hotkey",
                                 sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    }
    auto callback = make_callback(*bridge, state, 3);
    callback->key = hotkey_id;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_hotkey(
        bridge->context, hotkey_id, default_key, label, hotkey_callback,
        reinterpret_cast<void*>(callback->dispatch_id));
    if (status != SAO_OK) {
        release_callback_ref(state, *callback);
        return push_status_error(state, "register_hotkey", status);
    }
    try {
        bridge->hotkeys.emplace(callback->key, callback);
    } catch (...) {
        (void)sao::plugins::loader::sao_plugins_ctx_unregister_hotkey(bridge->context, hotkey_id);
        release_callback_ref(state, *callback);
        throw;
    }
    lua_pushstring(state, hotkey_id);
    return 1;
}

int ctx_unregister_hotkey(lua_State* state) {
    auto* bridge = checked_bridge(state);
    require_permission(state, detail::permission_hotkey, "unregister_hotkey");
    const char* hotkey_id = luaL_checkstring(state, 2);
    const auto found = bridge->hotkeys.find(hotkey_id);
    if (found == bridge->hotkeys.end()) {
        return push_status_error(state, "unregister_hotkey", SAO_ERR_HANDLE_INVALID);
    }
    int32_t status = stop_callback(*found->second);
    if (status != SAO_OK) {
        return push_status_error(state, "unregister_hotkey", status);
    }
    status = sao::plugins::loader::sao_plugins_ctx_unregister_hotkey(bridge->context, hotkey_id);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
        resume_callback(*found->second);
        return push_status_error(state, "unregister_hotkey", status);
    }
    release_callback_ref(state, *found->second);
    bridge->hotkeys.erase(found);
    lua_pushboolean(state, 1);
    return 1;
}

int register_timer(lua_State* state, bool one_shot) {
    auto* bridge = checked_bridge(state);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    const double seconds = luaL_checknumber(state, 3);
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return push_status_error(state, one_shot ? "set_timeout" : "set_interval",
                                 SAO_ERR_INVALID_ARGUMENT);
    }
    auto callback = make_callback(*bridge, state, 2, one_shot);
    char* loader_token = nullptr;
    const int32_t status = one_shot
                               ? sao::plugins::loader::sao_plugins_ctx_set_timeout(
                                     bridge->context, timer_callback, seconds,
                                     reinterpret_cast<void*>(callback->dispatch_id), &loader_token)
                               : sao::plugins::loader::sao_plugins_ctx_set_interval(
                                     bridge->context, timer_callback, seconds,
                                     reinterpret_cast<void*>(callback->dispatch_id), &loader_token);
    if (status != SAO_OK || loader_token == nullptr || loader_token[0] == '\0') {
        sao::plugins::loader::sao_plugins_ctx_free_string(loader_token);
        release_callback_ref(state, *callback);
        return push_status_error(state, one_shot ? "set_timeout" : "set_interval",
                                 status == SAO_OK ? SAO_ERR_HANDLE_INVALID : status);
    }
    callback->key = loader_token;
    callback->loader_token = loader_token;
    sao::plugins::loader::sao_plugins_ctx_free_string(loader_token);
    try {
        bridge->timers.emplace(callback->key, callback);
    } catch (...) {
        (void)sao::plugins::loader::sao_plugins_ctx_clear_timer(bridge->context,
                                                                callback->loader_token.c_str());
        release_callback_ref(state, *callback);
        throw;
    }
    if (one_shot && arm_one_shot(*callback)) {
        invoke_void_callback(reinterpret_cast<void*>(callback->dispatch_id), true);
    }
    lua_pushlstring(state, callback->key.data(), callback->key.size());
    return 1;
}

int ctx_set_interval(lua_State* state) {
    return register_timer(state, false);
}
int ctx_set_timeout(lua_State* state) {
    return register_timer(state, true);
}

int ctx_clear_timer(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* token = luaL_checkstring(state, 2);
    const auto found = bridge->timers.find(token);
    if (found == bridge->timers.end()) {
        return push_status_error(state, "clear_timer", SAO_ERR_HANDLE_INVALID);
    }
    int32_t status = stop_callback(*found->second);
    if (status != SAO_OK) {
        return push_status_error(state, "clear_timer", status);
    }
    status = sao::plugins::loader::sao_plugins_ctx_clear_timer(bridge->context,
                                                               found->second->loader_token.c_str());
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
        resume_callback(*found->second);
        return push_status_error(state, "clear_timer", status);
    }
    release_callback_ref(state, *found->second);
    bridge->timers.erase(found);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_notify(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* title = luaL_optstring(state, 2, "");
    const char* message = luaL_checkstring(state, 3);
    const double duration = luaL_optnumber(state, 4, 60.0);
    const char* kind = luaL_optstring(state, 5, "plugin");
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_notify(bridge->context, title,
                                                                        message, duration, kind);
    if (status != SAO_OK)
        return push_status_error(state, "notify", status);
    bridge->passive_resources.push_back(
        {passive_resource_kind::notification, {}, bridge->next_resource_sequence++});
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_toast(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* message = luaL_checkstring(state, 2);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_toast(bridge->context, message);
    if (status != SAO_OK)
        return push_status_error(state, "toast", status);
    bridge->passive_resources.push_back(
        {passive_resource_kind::notification, {}, bridge->next_resource_sequence++});
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_dismiss_notify(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_dismiss_notify(bridge->context);
    if (status != SAO_OK) {
        return push_status_error(state, "dismiss_notify", status);
    }
    std::erase_if(bridge->passive_resources, [](const passive_resource& item) {
        return item.kind == passive_resource_kind::notification;
    });
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_register_render_hook(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* surface = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TFUNCTION);
    const double priority = luaL_optnumber(state, 4, 0.0);
    if (!std::isfinite(priority) ||
        priority < -static_cast<double>(std::numeric_limits<float>::max()) ||
        priority > static_cast<double>(std::numeric_limits<float>::max())) {
        return push_status_error(state, "register_render_hook", SAO_ERR_INVALID_ARGUMENT);
    }
    auto callback = make_callback(*bridge, state, 3);
    uint32_t token = 0;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_render_hook(
        bridge->context, surface, static_cast<float>(priority), render_hook_callback,
        reinterpret_cast<void*>(callback->dispatch_id), &token);
    if (status != SAO_OK) {
        release_callback_ref(state, *callback);
        return push_status_error(state, "register_render_hook", status);
    }
    callback->token = token;
    try {
        bridge->render_hooks.emplace(token, callback);
    } catch (...) {
        (void)sao::plugins::loader::sao_plugins_ctx_unregister_render_hook(bridge->context, token);
        release_callback_ref(state, *callback);
        throw;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(token));
    return 1;
}

int ctx_unregister_render_hook(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const auto token = static_cast<uint32_t>(luaL_checkinteger(state, 2));
    const auto found = bridge->render_hooks.find(token);
    if (found == bridge->render_hooks.end()) {
        return push_status_error(state, "unregister_render_hook", SAO_ERR_HANDLE_INVALID);
    }
    int32_t status = stop_callback(*found->second);
    if (status != SAO_OK) {
        return push_status_error(state, "unregister_render_hook", status);
    }
    status = sao::plugins::loader::sao_plugins_ctx_unregister_render_hook(bridge->context, token);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
        resume_callback(*found->second);
        return push_status_error(state, "unregister_render_hook", status);
    }
    release_callback_ref(state, *found->second);
    bridge->render_hooks.erase(found);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_set_overlay(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* surface = luaL_checkstring(state, 2);
    std::string spec;
    if (!stack_json(state, 3, spec))
        return 0;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_overlay(bridge->context, surface, spec.c_str());
    if (status != SAO_OK)
        return push_status_error(state, "set_overlay", status);
    std::erase_if(bridge->passive_resources, [surface](const passive_resource& item) {
        return item.kind == passive_resource_kind::overlay && item.key == surface;
    });
    bridge->passive_resources.push_back(
        {passive_resource_kind::overlay, surface, bridge->next_resource_sequence++});
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_clear_overlay(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* surface = luaL_checkstring(state, 2);
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_clear_overlay(bridge->context, surface);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
        return push_status_error(state, "clear_overlay", status);
    }
    std::erase_if(bridge->passive_resources, [surface](const passive_resource& item) {
        return item.kind == passive_resource_kind::overlay && item.key == surface;
    });
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_request_redraw(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* surface = luaL_optstring(state, 2, "");
    const char* reason = luaL_optstring(state, 3, "");
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_request_redraw(bridge->context, surface, reason);
    if (status != SAO_OK) {
        return push_status_error(state, "request_redraw", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

std::string normalize_engine_name(const char* name) {
    std::string result;
    if (name == nullptr)
        return result;
    for (const unsigned char value : std::string(name)) {
        if (std::isspace(value) != 0)
            continue;
        result.push_back(value == '-' ? '_' : static_cast<char>(std::tolower(value)));
    }
    return result;
}

int ctx_register_engine(lua_State* state) {
    auto* bridge = checked_bridge(state);
    require_permission(state, detail::permission_engine_access, "register_engine");
    const std::string name = normalize_engine_name(luaL_checkstring(state, 2));
    if (name.empty() || lua_isnoneornil(state, 3)) {
        return push_status_error(state, "register_engine", SAO_ERR_INVALID_ARGUMENT);
    }
    if (bridge->engines.contains(name)) {
        return push_status_error(state, "register_engine",
                                 sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    }
    lua_pushvalue(state, 3);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_engine(
        bridge->context, name.c_str(), reinterpret_cast<void*>(static_cast<intptr_t>(reference)));
    if (status != SAO_OK) {
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        return push_status_error(state, "register_engine", status);
    }
    try {
        bridge->engines.emplace(name, reference);
    } catch (...) {
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        throw;
    }
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_get_engine(lua_State* state) {
    auto* bridge = checked_bridge(state);
    require_permission(state, detail::permission_engine_access, "get_engine");
    const std::string name = normalize_engine_name(luaL_checkstring(state, 2));
    void* native = sao::plugins::loader::sao_plugins_ctx_get_engine(bridge->context, name.c_str());
    const auto found = bridge->engines.find(name);
    if (found != bridge->engines.end() &&
        native == reinterpret_cast<void*>(static_cast<intptr_t>(found->second))) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
        return 1;
    }
    if (lua_gettop(state) >= 3) {
        lua_pushvalue(state, 3);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

int ctx_register_ui_panel(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* panel_id = luaL_checkstring(state, 2);
    std::string metadata;
    if (!stack_json(state, 3, metadata))
        return 0;
    if ((lua_gettop(state) >= 4 && !lua_isnoneornil(state, 4)) ||
        (lua_gettop(state) >= 5 && !lua_isnoneornil(state, 5))) {
        return push_status_error(state, "register_ui_panel callbacks",
                                 sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_ui_panel(
        bridge->context, panel_id, metadata.c_str(), nullptr, nullptr, nullptr);
    if (status != SAO_OK) {
        return push_status_error(state, "register_ui_panel", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_register_action_handler(lua_State* state) {
    auto* bridge = checked_bridge(state);
    if (!lua_isfunction(state, 2))
        return push_status_error(state, "register_action_handler", SAO_ERR_INVALID_ARGUMENT);
    const char* plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(bridge->context);
    if (plugin_id == nullptr || plugin_id[0] == '\0')
        return push_status_error(state, "register_action_handler context",
                                 SAO_ERR_HANDLE_INVALID);

    lua_pushvalue(state, 2);
    if (state != bridge->state)
        lua_xmove(state, bridge->state, 1);
    const int candidate_ref = luaL_ref(bridge->state, LUA_REGISTRYINDEX);
    registry_ref_guard candidate_guard(bridge->state, candidate_ref);
    const int32_t status = register_action_provider(*bridge);
    if (status != SAO_OK)
        return push_status_error(state, "action handler provider registration", status);

    int& committed_ref = bridge->enable_checkpoint_active
                             ? bridge->action_handler.enable_ref
                             : bridge->action_handler.persistent_ref;
    const int replaced_ref = committed_ref;
    committed_ref = candidate_ref;
    candidate_guard.release();
    if (valid_registry_ref(replaced_ref))
        luaL_unref(bridge->state, LUA_REGISTRYINDEX, replaced_ref);
    lua_pushstring(state, plugin_id);
    return 1;
}

int ctx_register_menu_category(lua_State* state) {
    auto* bridge = checked_bridge(state);
    std::string name;
    std::string icon;
    std::string error;
    if (!stack_menu_string(state, 2, true, name, error) ||
        !stack_menu_string(state, 3, false, icon, error)) {
        return push_status_error(state, "register_menu_category", SAO_ERR_INVALID_ARGUMENT);
    }
    if (!lua_isfunction(state, 4)) {
        return push_status_error(state, "register_menu_category builder", SAO_ERR_INVALID_ARGUMENT);
    }
    double priority = 0.0;
    if (!lua_isnoneornil(state, 5)) {
        int is_number = 0;
        priority = static_cast<double>(lua_tonumberx(state, 5, &is_number));
        if (is_number == 0 || !std::isfinite(priority)) {
            return push_status_error(state, "register_menu_category priority",
                                     SAO_ERR_INVALID_ARGUMENT);
        }
    }

    const char* plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(bridge->context);
    if (plugin_id == nullptr || plugin_id[0] == '\0') {
        return push_status_error(state, "register_menu_category context", SAO_ERR_HANDLE_INVALID);
    }
    auto menu = std::make_unique<menu_bridge>();
    menu->state = bridge->state;
    menu->provider_id = "menu-" + hash_suffix(name);
    menu->contribution_id = menu->provider_id;
    menu->root_id = "plugin:" + hash_suffix(std::string(plugin_id) + "\n" + name);
    menu->name = std::move(name);
    menu->icon = std::move(icon);
    menu->priority = priority;
    lua_pushvalue(state, 4);
    if (state != bridge->state)
        lua_xmove(state, bridge->state, 1);
    menu->builder_ref = luaL_ref(bridge->state, LUA_REGISTRYINDEX);

    menu_bridge* menu_ptr = menu.get();
    try {
        bridge->menus.push_back(std::move(menu));
    } catch (...) {
        luaL_unref(state, LUA_REGISTRYINDEX, menu_ptr->builder_ref);
        throw;
    }

    const int32_t status = register_menu_provider(*bridge, *menu_ptr);
    if (status != SAO_OK) {
        luaL_unref(state, LUA_REGISTRYINDEX, menu_ptr->builder_ref);
        bridge->menus.pop_back();
        return push_status_error(state, "dynamic menu provider registration", status);
    }
    lua_pushlstring(state, menu_ptr->provider_id.data(), menu_ptr->provider_id.size());
    return 1;
}

int subscribe_impl(lua_State* state, bool once) {
    auto* bridge = checked_bridge(state);
    const char* topic = luaL_checkstring(state, 2);
    auto callback = make_callback(*bridge, state, 3, once);
    const auto rollback = [&](bool unsubscribe) noexcept {
        if (unsubscribe && bridge->context != nullptr) {
            (void)sao::plugins::loader::sao_plugins_ctx_unsubscribe(bridge->context,
                                                                    callback->token);
        }
        release_callback_ref(state, *callback);
    };
    uint32_t token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = once ? sao::plugins::loader::sao_plugins_ctx_subscribe_once(
                            bridge->context, topic, event_callback,
                            reinterpret_cast<void*>(callback->dispatch_id), &token)
                      : sao::plugins::loader::sao_plugins_ctx_subscribe(
                            bridge->context, topic, event_callback,
                            reinterpret_cast<void*>(callback->dispatch_id), &token);
    } catch (...) {
        rollback(false);
        throw;
    }
    if (status != SAO_OK) {
        rollback(false);
        return push_status_error(state, once ? "subscribe_once" : "subscribe", status);
    }
    callback->token = token;
    bool inserted = false;
    try {
        inserted = bridge->callbacks.emplace(token, callback).second;
    } catch (...) {
        rollback(true);
        throw;
    }
    if (!inserted) {
        rollback(true);
        return push_status_error(state, once ? "subscribe_once" : "subscribe",
                                 sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    }
    std::string pending_event;
    if (once && arm_one_shot(*callback, &pending_event)) {
        event_callback(nullptr, pending_event.c_str(),
                       reinterpret_cast<void*>(callback->dispatch_id));
    }
    lua_pushinteger(state, static_cast<lua_Integer>(token));
    return 1;
}

int ctx_subscribe(lua_State* state) {
    return subscribe_impl(state, false);
}
int ctx_subscribe_once(lua_State* state) {
    return subscribe_impl(state, true);
}

int ctx_unsubscribe(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const auto token = static_cast<uint32_t>(luaL_checkinteger(state, 2));
    const auto found = bridge->callbacks.find(token);
    if (found == bridge->callbacks.end()) {
        return push_status_error(state, "unsubscribe", SAO_ERR_HANDLE_INVALID);
    }
    int32_t status = stop_callback(*found->second);
    if (status != SAO_OK) {
        return push_status_error(state, "unsubscribe", status);
    }
    status = sao::plugins::loader::sao_plugins_ctx_unsubscribe(bridge->context, token);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
        resume_callback(*found->second);
        return push_status_error(state, "unsubscribe", status);
    }
    release_callback_ref(state, *found->second);
    bridge->callbacks.erase(found);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_emit(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* topic = luaL_checkstring(state, 2);
    std::string payload = "null";
    if (lua_gettop(state) >= 3 && !lua_isnil(state, 3) && !stack_json(state, 3, payload)) {
        return 0;
    }
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_emit(bridge->context, topic, payload.c_str());
    if (status != SAO_OK)
        return push_status_error(state, "emit", status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_get_snapshot(lua_State* state) {
    auto* bridge = checked_bridge(state);
    char* value = nullptr;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_get_snapshot(bridge->context, &value);
    return push_owned_json(state, status, value, "get_snapshot");
}

int ctx_recent_events(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const auto limit = static_cast<uint32_t>(luaL_optinteger(state, 2, 64));
    const char* topic = luaL_optstring(state, 3, nullptr);
    char* value = nullptr;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_recent_events(bridge->context, limit, topic, &value);
    return push_owned_json(state, status, value, "recent_events");
}

int unsupported(lua_State* state) {
    const char* operation = lua_tostring(state, lua_upvalueindex(1));
    return luaL_error(state, "%s failed with status %d",
                      operation == nullptr ? "ctx operation" : operation,
                      sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
}

template <int (*Function)(lua_State*)> int safe_method(lua_State* state) noexcept {
    const char* operation = nullptr;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        return Function(state);
    } catch (const method_failure& failure) {
        operation = failure.operation;
        status = failure.status;
    } catch (...) {
        operation = "ctx method crossed C++ exception boundary";
    }
    return luaL_error(state, "%s failed with status %d", operation, status);
}

void set_method(lua_State* state, const char* name, lua_CFunction function) {
    lua_pushcfunction(state, function);
    lua_setfield(state, -2, name);
}

void set_unsupported(lua_State* state, const char* name) {
    lua_pushstring(state, name);
    lua_pushcclosure(state, unsupported, 1);
    lua_setfield(state, -2, name);
}

int register_ctx_body(lua_State* state) {
    auto* bridge = static_cast<bridge_state*>(lua_touserdata(state, 1));
    if (bridge == nullptr)
        return luaL_error(state, "invalid ctx bridge");
    auto** slot = static_cast<bridge_state**>(lua_newuserdatauv(state, sizeof(bridge_state*), 0));
    *slot = bridge;
    if (luaL_newmetatable(state, "SaoPluginContext") != 0) {
        lua_newtable(state);
        set_method(state, "log", safe_method<ctx_log>);
        set_method(state, "time", safe_method<ctx_time>);
        set_method(state, "set_defaults", safe_method<ctx_set_defaults>);
        set_method(state, "get_setting", safe_method<ctx_get_setting>);
        set_method(state, "setting", safe_method<ctx_get_setting>);
        set_method(state, "set_setting", safe_method<ctx_set_setting>);
        set_method(state, "snapshot_value", safe_method<ctx_snapshot_value>);
        set_method(state, "register_ui_panel", safe_method<ctx_register_ui_panel>);
        set_method(state, "register_menu_category", safe_method<ctx_register_menu_category>);
        set_method(state, "register_action_handler", safe_method<ctx_register_action_handler>);
        set_method(state, "subscribe", safe_method<ctx_subscribe>);
        set_method(state, "subscribe_once", safe_method<ctx_subscribe_once>);
        set_method(state, "unsubscribe", safe_method<ctx_unsubscribe>);
        set_method(state, "emit", safe_method<ctx_emit>);
        set_method(state, "publish", safe_method<ctx_emit>);
        set_method(state, "get_snapshot", safe_method<ctx_get_snapshot>);
        set_method(state, "recent_events", safe_method<ctx_recent_events>);
        set_method(state, "register_render_hook", safe_method<ctx_register_render_hook>);
        set_method(state, "unregister_render_hook", safe_method<ctx_unregister_render_hook>);
        set_method(state, "set_overlay", safe_method<ctx_set_overlay>);
        set_method(state, "clear_overlay", safe_method<ctx_clear_overlay>);
        set_method(state, "request_redraw", safe_method<ctx_request_redraw>);
        set_method(state, "register_hotkey", safe_method<ctx_register_hotkey>);
        set_method(state, "unregister_hotkey", safe_method<ctx_unregister_hotkey>);
        set_method(state, "set_interval", safe_method<ctx_set_interval>);
        set_method(state, "set_timeout", safe_method<ctx_set_timeout>);
        set_method(state, "clear_timer", safe_method<ctx_clear_timer>);
        set_method(state, "notify", safe_method<ctx_notify>);
        set_method(state, "toast", safe_method<ctx_toast>);
        set_method(state, "dismiss_notify", safe_method<ctx_dismiss_notify>);
        set_method(state, "register_engine", safe_method<ctx_register_engine>);
        set_method(state, "get_engine", safe_method<ctx_get_engine>);
        for (const char* name : {"register_menu_surface", "open_file", "open_window",
                                 "load_local", "ensure_requirements"}) {
            set_unsupported(state, name);
        }
        lua_setfield(state, -2, "__index");
        lua_pushliteral(state, "locked");
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
    lua_pushvalue(state, -1);
    bridge->context_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_setglobal(state, "ctx");
    return 0;
}

int clear_ctx_body(lua_State* state) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    lua_pushliteral(state, "ctx");
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
    return 0;
}

int restore_ctx_body(lua_State* state) {
    auto* bridge = static_cast<bridge_state*>(lua_touserdata(state, 1));
    if (bridge == nullptr || bridge->context_ref == LUA_NOREF ||
        bridge->context_ref == LUA_REFNIL) {
        return luaL_error(state, "saved ctx bridge is unavailable");
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return luaL_error(state, "global table is unavailable");
    }
    lua_pushliteral(state, "ctx");
    lua_rawgeti(state, LUA_REGISTRYINDEX, bridge->context_ref);
    lua_rawset(state, -3);
    lua_pop(state, 1);
    return 0;
}

} // namespace

namespace detail {

std::shared_ptr<std::recursive_mutex> bridge_mutex(lua_State* state) noexcept {
    return state_mutex(state);
}

std::size_t ctx_menu_checkpoint_locked(lua_State* state) noexcept {
    if (state == nullptr)
        return 0;
    try {
        state = main_thread(state);
        std::lock_guard lock(g_bridge_map_mutex);
        const auto found = g_bridge_map.find(state);
        if (found == g_bridge_map.end())
            return 0;
        found->second->enable_checkpoint_active = true;
        found->second->enable_checkpoint_had_action =
            valid_registry_ref(current_action_handler_ref(found->second->action_handler));
        return found->second->menus.size();
    } catch (...) {
        return 0;
    }
}

int32_t commit_ctx_enable_menus_locked(lua_State* state, std::size_t checkpoint) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        std::lock_guard lock(g_bridge_map_mutex);
        const auto found = g_bridge_map.find(state);
        if (found == g_bridge_map.end())
            return SAO_OK;
        if (checkpoint > found->second->menus.size())
            return SAO_ERR_INVALID_ARGUMENT;
        for (std::size_t index = checkpoint; index < found->second->menus.size(); ++index) {
            found->second->menus[index]->enable_scoped = true;
        }
        found->second->enable_checkpoint_active = false;
        found->second->enable_checkpoint_had_action = false;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t rollback_ctx_menus_locked(lua_State* state, std::size_t checkpoint) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        bridge_state* bridge = nullptr;
        {
            std::lock_guard lock(g_bridge_map_mutex);
            const auto found = g_bridge_map.find(state);
            if (found == g_bridge_map.end())
                return SAO_OK;
            bridge = found->second.get();
        }
        if (checkpoint > bridge->menus.size())
            return SAO_ERR_INVALID_ARGUMENT;
        const int32_t status = unregister_menu_providers(*bridge, checkpoint);
        if (status != SAO_OK)
            return status;
        for (std::size_t index = checkpoint; index < bridge->menus.size(); ++index) {
            auto& menu = bridge->menus[index];
            menu->closing = true;
            if (menu->builder_ref != LUA_NOREF && menu->builder_ref != LUA_REFNIL) {
                luaL_unref(state, LUA_REGISTRYINDEX, menu->builder_ref);
                menu->builder_ref = LUA_NOREF;
            }
            release_action_refs(state, menu->actions);
            menu->rows.clear();
        }
        bridge->menus.erase(bridge->menus.begin() + static_cast<std::ptrdiff_t>(checkpoint),
                            bridge->menus.end());
        if (bridge->enable_checkpoint_active) {
            auto& action_handler = bridge->action_handler;
            if (valid_registry_ref(action_handler.enable_ref)) {
                if (!bridge->enable_checkpoint_had_action) {
                    const int32_t action_status = unregister_action_provider(*bridge);
                    if (action_status != SAO_OK)
                        return action_status;
                }
                release_registry_ref(state, action_handler.enable_ref);
            }
            bridge->enable_checkpoint_active = false;
            bridge->enable_checkpoint_had_action = false;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t remove_ctx_enable_menus_locked(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        bridge_state* bridge = nullptr;
        {
            std::lock_guard lock(g_bridge_map_mutex);
            const auto found = g_bridge_map.find(state);
            if (found == g_bridge_map.end())
                return SAO_OK;
            bridge = found->second.get();
        }
        std::vector<menu_bridge*> menus;
        for (const auto& menu : bridge->menus) {
            if (menu->enable_scoped)
                menus.push_back(menu.get());
        }
        const int32_t status = unregister_menu_providers(*bridge, menus);
        if (status != SAO_OK)
            return status;
        std::erase_if(bridge->menus, [state](const auto& menu) {
            if (!menu->enable_scoped)
                return false;
            menu->closing = true;
            if (menu->builder_ref != LUA_NOREF && menu->builder_ref != LUA_REFNIL) {
                luaL_unref(state, LUA_REGISTRYINDEX, menu->builder_ref);
                menu->builder_ref = LUA_NOREF;
            }
            release_action_refs(state, menu->actions);
            menu->rows.clear();
            return true;
        });
        auto& action_handler = bridge->action_handler;
        if (valid_registry_ref(action_handler.enable_ref)) {
            if (!valid_registry_ref(action_handler.persistent_ref)) {
                const int32_t action_status = unregister_action_provider(*bridge);
                if (action_status != SAO_OK)
                    return action_status;
            }
            release_registry_ref(state, action_handler.enable_ref);
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t quiesce_ctx_menu_providers(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        bridge_state* bridge = nullptr;
        loader_context_t* context = nullptr;
        std::vector<std::string> provider_ids;
        {
            state_operation operation;
            const int32_t state_status = acquire_state_operation_for_close(state, operation);
            if (state_status != SAO_OK)
                return state_status;
            {
                std::lock_guard lock(g_bridge_map_mutex);
                const auto found = g_bridge_map.find(state);
                if (found == g_bridge_map.end())
                    return SAO_OK;
                bridge = found->second.get();
            }
            bridge->closing = true;
            bridge->action_handler.closing = true;
            for (const auto& menu : bridge->menus)
                menu->closing = true;
            context = bridge->context;
            const char* plugin_id = context == nullptr
                                        ? nullptr
                                        : sao::plugins::loader::sao_plugins_ctx_plugin_id(context);
            provider_ids.reserve(bridge->menus.size() + 1);
            for (const auto& menu : bridge->menus) {
                provider_ids.push_back(plugin_id == nullptr
                                           ? menu->provider_id
                                           : std::string(plugin_id) + "/" + menu->provider_id);
            }
            if (valid_registry_ref(current_action_handler_ref(bridge->action_handler))) {
                provider_ids.push_back(plugin_id == nullptr
                                           ? std::string(kActionProviderId)
                                           : std::string(plugin_id) + "/" +
                                                 std::string(kActionProviderId));
            }
        }
        if (context == nullptr || provider_ids.empty())
            return SAO_OK;
        std::vector<const char*> provider_id_views;
        provider_id_views.reserve(provider_ids.size());
        for (const auto& provider_id : provider_ids) {
            provider_id_views.push_back(provider_id.c_str());
        }
        const int32_t status = sao::plugins::loader::plugin_context_unregister_entity_providers(
            context, provider_id_views.data(), provider_id_views.size());
        if (status != SAO_OK) {
            state_operation rollback_operation;
            if (acquire_state_operation_for_close(state, rollback_operation) == SAO_OK) {
                std::lock_guard lock(g_bridge_map_mutex);
                const auto found = g_bridge_map.find(state);
                if (found != g_bridge_map.end() && found->second.get() == bridge) {
                    bridge->closing = false;
                    bridge->action_handler.closing = false;
                    for (const auto& menu : bridge->menus)
                        menu->closing = false;
                }
            }
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool has_ctx_bridge_locked(lua_State* state) noexcept {
    if (state == nullptr)
        return false;
    try {
        state = main_thread(state);
        std::lock_guard lock(g_bridge_map_mutex);
        return g_bridge_map.contains(state);
    } catch (...) {
        return true;
    }
}

int32_t resume_ctx_menu_providers_locked(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        std::lock_guard lock(g_bridge_map_mutex);
        const auto found = g_bridge_map.find(state);
        if (found == g_bridge_map.end())
            return SAO_OK;
        if (protected_trampoline(state, restore_ctx_body, found->second.get(), 0) != LUA_OK) {
            capture_state_error_locked(state, -1);
            lua_pop(state, 1);
            return SAO_ERR_OS_CALL_FAILED;
        }
        const int32_t status = register_menu_providers(*found->second);
        if (status != SAO_OK)
            return status;
        for (const auto& menu : found->second->menus)
            menu->closing = false;
        found->second->action_handler.closing = false;
        found->second->closing = false;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t push_canonical_ctx_locked(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        lua_State* main = main_thread(state);
        std::lock_guard lock(g_bridge_map_mutex);
        const auto found = g_bridge_map.find(main);
        if (found == g_bridge_map.end() || found->second->context_ref == LUA_NOREF ||
            found->second->context_ref == LUA_REFNIL || found->second->closing) {
            return SAO_ERR_HANDLE_INVALID;
        }
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second->context_ref);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t teardown_ctx_bridge_locked(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        state = main_thread(state);
        bridge_state* bridge = nullptr;
        {
            std::lock_guard lock(g_bridge_map_mutex);
            const auto found = g_bridge_map.find(state);
            if (found == g_bridge_map.end())
                return SAO_OK;
            bridge = found->second.get();
        }
        if (protected_function(state, clear_ctx_body, 0, 0) != LUA_OK) {
            capture_state_error_locked(state, -1);
            lua_pop(state, 1);
            return SAO_ERR_OS_CALL_FAILED;
        }
        const int32_t provider_status = unregister_all_entity_providers(*bridge);
        if (provider_status != SAO_OK)
            return provider_status;
        std::vector<std::shared_ptr<event_callback_record>> callbacks;
        callbacks.reserve(bridge->callbacks.size() + bridge->hotkeys.size() +
                          bridge->timers.size() + bridge->render_hooks.size());
        for (const auto& [_, callback] : bridge->callbacks) {
            callbacks.push_back(callback);
        }
        for (const auto& [_, callback] : bridge->hotkeys) {
            callbacks.push_back(callback);
        }
        for (const auto& [_, callback] : bridge->timers) {
            callbacks.push_back(callback);
        }
        for (const auto& [_, callback] : bridge->render_hooks) {
            callbacks.push_back(callback);
        }
        std::sort(callbacks.begin(), callbacks.end(), [](const auto& left, const auto& right) {
            return left->sequence > right->sequence;
        });
        std::vector<std::shared_ptr<event_callback_record>> stopped;
        stopped.reserve(callbacks.size());
        for (const auto& callback : callbacks) {
            const int32_t status = stop_callback(*callback);
            if (status != SAO_OK) {
                for (const auto& item : stopped)
                    resume_callback(*item);
                return status;
            }
            stopped.push_back(callback);
        }
        bridge->closing = true;
        struct resource final {
            enum class kind : std::uint8_t {
                event,
                hotkey,
                timer,
                render_hook,
                notification,
                overlay,
            } type;
            std::uint64_t sequence = 0;
            std::shared_ptr<event_callback_record> callback;
            std::string key;
        };
        std::vector<resource> resources;
        resources.reserve(callbacks.size() + bridge->passive_resources.size());
        for (const auto& [_, callback] : bridge->callbacks) {
            resources.push_back({resource::kind::event, callback->sequence, callback, {}});
        }
        for (const auto& [_, callback] : bridge->hotkeys) {
            resources.push_back({resource::kind::hotkey, callback->sequence, callback, {}});
        }
        for (const auto& [_, callback] : bridge->timers) {
            resources.push_back({resource::kind::timer, callback->sequence, callback, {}});
        }
        for (const auto& [_, callback] : bridge->render_hooks) {
            resources.push_back({resource::kind::render_hook, callback->sequence, callback, {}});
        }
        for (const auto& passive : bridge->passive_resources) {
            resources.push_back({passive.kind == passive_resource_kind::notification
                                     ? resource::kind::notification
                                     : resource::kind::overlay,
                                 passive.sequence, nullptr, passive.key});
        }
        std::sort(resources.begin(), resources.end(),
                  [](const resource& left, const resource& right) {
                      return left.sequence > right.sequence;
                  });
        int32_t status = SAO_OK;
        bool notifications_dismissed = false;
        for (const auto& item : resources) {
            int32_t release_status = SAO_OK;
            if (bridge->context == nullptr)
                break;
            switch (item.type) {
            case resource::kind::event:
                release_status = sao::plugins::loader::sao_plugins_ctx_unsubscribe(
                    bridge->context, item.callback->token);
                break;
            case resource::kind::hotkey:
                release_status = sao::plugins::loader::sao_plugins_ctx_unregister_hotkey(
                    bridge->context, item.callback->key.c_str());
                break;
            case resource::kind::timer:
                release_status = sao::plugins::loader::sao_plugins_ctx_clear_timer(
                    bridge->context, item.callback->loader_token.c_str());
                break;
            case resource::kind::render_hook:
                release_status = sao::plugins::loader::sao_plugins_ctx_unregister_render_hook(
                    bridge->context, item.callback->token);
                break;
            case resource::kind::notification:
                if (!notifications_dismissed) {
                    release_status =
                        sao::plugins::loader::sao_plugins_ctx_dismiss_notify(bridge->context);
                    notifications_dismissed = true;
                }
                break;
            case resource::kind::overlay:
                release_status = sao::plugins::loader::sao_plugins_ctx_clear_overlay(
                    bridge->context, item.key.c_str());
                break;
            }
            if (status == SAO_OK && release_status != SAO_OK &&
                release_status != SAO_ERR_HANDLE_INVALID) {
                status = release_status;
            }
        }
        if (status != SAO_OK) {
            for (const auto& callback : stopped)
                resume_callback(*callback);
            bridge->closing = false;
            return status;
        }
        for (const auto& callback : callbacks) {
            release_callback_ref(state, *callback);
        }
        bridge->callbacks.clear();
        bridge->hotkeys.clear();
        bridge->timers.clear();
        bridge->render_hooks.clear();
        bridge->passive_resources.clear();
        for (const auto& [_, reference] : bridge->engines) {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
        }
        bridge->engines.clear();
        for (auto& menu : bridge->menus) {
            menu->closing = true;
            if (menu->builder_ref != LUA_NOREF && menu->builder_ref != LUA_REFNIL) {
                luaL_unref(state, LUA_REGISTRYINDEX, menu->builder_ref);
                menu->builder_ref = LUA_NOREF;
            }
            release_action_refs(state, menu->actions);
            menu->rows.clear();
        }
        bridge->menus.clear();
        bridge->action_handler.closing = true;
        release_action_handler_refs(bridge->action_handler);
        bridge->action_handler.state = nullptr;
        bridge->enable_checkpoint_active = false;
        bridge->enable_checkpoint_had_action = false;
        if (bridge->context_ref != LUA_NOREF && bridge->context_ref != LUA_REFNIL) {
            luaL_unref(state, LUA_REGISTRYINDEX, bridge->context_ref);
            bridge->context_ref = LUA_NOREF;
        }
        if (bridge->context_lease && bridge->context != nullptr) {
            sao::plugins::loader::plugin_context_release_host_lease(bridge->context);
            bridge->context_lease = false;
        }
        bridge->context = nullptr;
        bridge->state = nullptr;
        {
            std::lock_guard lock(g_bridge_map_mutex);
            const auto found = g_bridge_map.find(state);
            if (found != g_bridge_map.end() && found->second.get() == bridge) {
                auto& retired = g_retired_bridges[state];
                retired.reserve(retired.size() + 1);
                retired.push_back(std::move(found->second));
                g_bridge_map.erase(found);
            }
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t teardown_ctx_bridge(lua_State* state) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    state = main_thread(state);
    const int32_t quiesce_status = quiesce_ctx_menu_providers(state);
    if (quiesce_status != SAO_OK)
        return quiesce_status;
    state_operation operation;
    const int32_t status = acquire_state_operation(state, operation);
    return status == SAO_OK ? teardown_ctx_bridge_locked(state) : status;
}

void release_ctx_bridges_locked(lua_State* state) noexcept {
    try {
        state = main_thread(state);
        std::lock_guard lock(g_bridge_map_mutex);
        g_retired_bridges.erase(state);
    } catch (...) {
    }
}

} // namespace detail

#endif

extern "C" SAO_PLUGINS_API
    int32_t SAO_PLUGINS_CALL sao_plugins_luahost_register_ctx(lua_State* state, void* ctx_handle) {
    if (state == nullptr || ctx_handle == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    bool context_retained = false;
    try {
        detail::state_operation operation;
        int32_t status = detail::acquire_state_operation(state, operation);
        if (status != SAO_OK)
            return status;
        state = operation.state();
        auto* context = static_cast<sao::plugins::loader::plugin_context_t*>(ctx_handle);
        status = sao::plugins::loader::plugin_context_retain_host_lease(context);
        if (status != SAO_OK)
            return status;
        context_retained = true;
        auto bridge = std::make_unique<bridge_state>();
        bridge->state = state;
        bridge->context = context;
        bridge->context_lease = true;
        bridge->action_handler.state = state;
        bridge->mutex = detail::state_mutex(state);
        if (!bridge->mutex) {
            sao::plugins::loader::plugin_context_release_host_lease(context);
            context_retained = false;
            return SAO_ERR_HANDLE_INVALID;
        }
        bridge_state* bridge_ptr = bridge.get();
        {
            std::lock_guard lock(g_bridge_map_mutex);
            if (g_bridge_map.contains(state)) {
                sao::plugins::loader::plugin_context_release_host_lease(context);
                context_retained = false;
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            g_bridge_map.emplace(state, std::move(bridge));
        }
        context_retained = false;
        const int base = lua_gettop(state);
        if (detail::protected_trampoline(state, register_ctx_body, bridge_ptr, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            (void)detail::teardown_ctx_bridge_locked(state);
            return SAO_ERR_OS_CALL_FAILED;
        }
        lua_settop(state, base);
        return SAO_OK;
    } catch (...) {
        if (context_retained) {
            sao::plugins::loader::plugin_context_release_host_lease(
                static_cast<sao::plugins::loader::plugin_context_t*>(ctx_handle));
        }
        (void)detail::teardown_ctx_bridge(state);
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_ui(lua_State* state) {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_mem(lua_State* state, void* ctx_handle) {
    if (state == nullptr || ctx_handle == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_engine(lua_State* state, void* ctx_handle) {
    if (state == nullptr || ctx_handle == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_python_compat(lua_State* state, bool enable) {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return enable ? sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED : SAO_OK;
}

} // namespace sao::plugins::lua_host
