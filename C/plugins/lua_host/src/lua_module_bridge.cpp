#include "sao/plugins/lua_host/lua_module_bridge.h"

#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/menu_navigation.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/script_ui.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_engine.h"
#include "sao/sdk/sao_sdk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
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

namespace menu_navigation = sao::plugins::script_ctx::menu_navigation;
using menu_row = menu_navigation::Row;

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
    menu_navigation::State navigation;
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

struct panel_callback_record {
    std::shared_ptr<event_callback_record> render;
    std::shared_ptr<event_callback_record> action;
    bool enable_scoped = false;
    bool pending_enable = false;
};

struct passive_resource {
    passive_resource_kind kind = passive_resource_kind::notification;
    std::string key;
    std::uint64_t sequence = 0;
};

// Dispatch block shared by the four compositor input native callbacks (they
// receive a single common user_data pointer).  `record_ids` holds the
// event_callback_record dispatch ids looked up through g_callback_map.
struct compositor_input_dispatch {
    std::array<uintptr_t, 4> record_ids{};  // cursor_pos, mouse_button, cursor_leave, scroll
    std::string key;                        // layer name
    std::atomic_bool closing{false};
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
    std::unordered_map<std::string, std::shared_ptr<panel_callback_record>> panels;
    std::unordered_map<std::string, int> engines;
    // FULL reflective engine surface: per-bridge SaoSdkContext bound at
    // register_ctx time plus the channel map for ctx.engine.on().  The
    // dedicated mutex keeps provider-thread channel lookups off the state
    // lock so a provider drain inside sao_sdk_context_destroy cannot
    // deadlock against teardown.
    SaoSdkContext sdk_ctx{};
    bool sdk_ctx_bound = false;
    std::mutex engine_callbacks_mutex;
    std::unordered_map<std::string, std::shared_ptr<event_callback_record>> engine_callbacks;
    std::vector<passive_resource> passive_resources;
    std::unordered_map<std::string, std::shared_ptr<compositor_input_dispatch>> compositor_inputs;
    std::unordered_map<uintptr_t, std::shared_ptr<event_callback_record>> compositor_records;
    std::vector<std::shared_ptr<compositor_input_dispatch>> retired_compositor_dispatches;
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

constexpr std::size_t kMaximumMenuRows = menu_navigation::max_nodes;
constexpr std::size_t kMaximumMenuStringBytes = 16U * 1024U;
constexpr std::size_t kMaximumMenuSnapshotBytes = menu_navigation::max_text_bytes;
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

int protected_registry_ref(lua_State* state, int index) {
    lua_pushvalue(state, index);
    const int status = detail::protected_function(state, [](lua_State* current) {
        lua_pushvalue(current, 1);
        const int reference = luaL_ref(current, LUA_REGISTRYINDEX);
        lua_pushinteger(current, reference);
        return 1;
    }, 1, 1);
    if (status != LUA_OK) {
        detail::capture_state_error_locked(state, -1);
        lua_pop(state, 1);
        throw std::bad_alloc{};
    }
    const int reference = static_cast<int>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    return reference;
}

std::shared_ptr<event_callback_record> make_callback(bridge_state& bridge, lua_State* current,
                                                     int function_index, bool one_shot = false) {
    lua_State* state = bridge.state;
    luaL_checktype(current, function_index, LUA_TFUNCTION);
    const int function_ref = protected_registry_ref(current, function_index);
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

struct menu_materialization {
    struct frame {
        int sequence_ref;
        const void* sequence_identity;
        const void* owner_identity;
        std::vector<menu_navigation::Node>* nodes;
        std::size_t count;
        std::size_t next = 0;
        std::string path_identity;
        std::unordered_map<std::string, std::size_t> occurrences;
    };

    lua_State* state;
    std::vector<menu_navigation::Node> tree;
    std::vector<menu_navigation::Node*> destruction_order;
    std::vector<int> references;
    std::vector<frame> frames;
    std::unordered_set<const void*> ancestors;
    std::unordered_map<std::string, int> actions;
    std::unordered_set<std::string> action_ids;
    std::string command_identity;
    std::string explicit_identity;
    std::string identity;
    std::string path_identity;

    explicit menu_materialization(lua_State* value) : state(value) {
        destruction_order.reserve(kMaximumMenuRows);
        references.reserve(3 * kMaximumMenuRows + 1);
        frames.reserve(kMaximumMenuRows + 1);
    }

    ~menu_materialization() {
        release_action_refs(state, actions);
        for (const int reference : references)
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
        // Clear children bottom-up so destruction never follows tree depth on the C++ stack.
        for (auto it = destruction_order.rbegin(); it != destruction_order.rend(); ++it)
            (*it)->children.clear();
    }

    int hold(int index) {
        lua_pushvalue(state, index);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        references.push_back(reference);
        return reference;
    }
};

bool build_menu_snapshot(menu_bridge* bridge, menu_materialization& materialized,
                         const menu_navigation::State* requested_navigation,
                         std::string& error) {
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
    auto& actions = materialized.actions;
    try {
        materialized.tree.reserve(count);
        actions.reserve(count);
        const void* root_identity = lua_topointer(state, -1);
        materialized.ancestors.insert(root_identity);
        materialized.frames.push_back({materialized.hold(-1), root_identity, nullptr,
                                        &materialized.tree, count, 0, {}, {}});
        lua_settop(state, base);
        std::size_t admitted_nodes = count;
        std::size_t total_bytes = 0;
        while (!materialized.frames.empty()) {
            auto& frame = materialized.frames.back();
            if (frame.next == frame.count) {
                materialized.ancestors.erase(frame.sequence_identity);
                if (frame.owner_identity != nullptr)
                    materialized.ancestors.erase(frame.owner_identity);
                materialized.frames.pop_back();
                continue;
            }
            lua_rawgeti(state, LUA_REGISTRYINDEX, frame.sequence_ref);
            lua_rawgeti(state, -1, static_cast<lua_Integer>(++frame.next));
            lua_remove(state, -2);
            if (!lua_istable(state, -1)) {
                error = "menu rows must be tables";
                lua_settop(state, base);
                return false;
            }
            const int row_index = lua_absindex(state, -1);
            const void* row_identity = lua_topointer(state, row_index);
            if (!materialized.ancestors.insert(row_identity).second) {
                error = "menu contains an ancestor cycle";
                lua_settop(state, base);
                return false;
            }
            materialized.hold(row_index);
            frame.nodes->emplace_back();
            auto& node = frame.nodes->back();
            materialized.destruction_order.push_back(&node);
            auto& row = node.row;
            bool disabled = false;
            bool separator = false;
            if (!table_menu_string(state, row_index, "label", true, row.label, error) ||
                !table_menu_string(state, row_index, "icon", false, row.icon, error) ||
                !table_menu_payload(state, row_index, row.payload_json, error) ||
                !table_menu_flag(state, row_index, "keep_menu_open", false, row.keep_open,
                                 error) ||
                !table_menu_flag(state, row_index, "close_menu_before", false,
                                 row.close_before, error) ||
                !table_menu_flag(state, row_index, "disabled", false, disabled, error) ||
                !table_menu_flag(state, row_index, "separator", false, separator, error)) {
                lua_settop(state, base);
                return false;
            }

            int children_ref = LUA_NOREF;
            for (const char* alias : {"children", "items", "submenu"}) {
                raw_get_field(state, row_index, alias);
                if (!lua_isnil(state, -1)) {
                    if (children_ref != LUA_NOREF ||
                        (!lua_istable(state, -1) && !lua_isfunction(state, -1))) {
                        error = "menu children require one sequence or function alias";
                        lua_settop(state, base);
                        return false;
                    }
                    children_ref = materialized.hold(-1);
                }
                lua_pop(state, 1);
            }
            node.submenu = children_ref != LUA_NOREF;
            raw_get_field(state, row_index, "command");
            const bool has_command = !lua_isnil(state, -1);
            if (has_command && (!lua_isfunction(state, -1) || node.submenu)) {
                error = node.submenu ? "menu node cannot combine command and children"
                                     : "menu command must be a function";
                lua_settop(state, base);
                return false;
            }
            const int command_index = lua_absindex(state, -1);
            materialized.command_identity =
                has_command ? callable_identity(state, -1) : std::string{};
            row.can_activate = has_command || node.submenu;
            bool requested_can_activate = row.can_activate;
            if (!table_menu_flag(state, row_index, "can_activate", row.can_activate,
                                 requested_can_activate, error)) {
                lua_settop(state, base);
                return false;
            }
            row.can_activate = row.can_activate && requested_can_activate && !disabled && !separator;

            auto& explicit_identity = materialized.explicit_identity;
            explicit_identity.clear();
            raw_get_field(state, row_index, "action_id");
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                raw_get_field(state, row_index, "id");
            }
            if (!lua_isnil(state, -1) &&
                !stack_menu_string(state, -1, true, explicit_identity, error)) {
                lua_pop(state, 1);
                lua_settop(state, base);
                return false;
            }
            lua_pop(state, 1);

            auto& identity = materialized.identity;
            identity = explicit_identity;
            if (identity.empty()) {
                append_identity_field(identity, materialized.command_identity);
                append_identity_field(identity, row.label);
                append_identity_field(identity, row.icon);
                if (!node.submenu) {
                    append_identity_field(identity, row.payload_json);
                    identity.push_back(row.can_activate ? '1' : '0');
                    identity.push_back(row.keep_open ? '1' : '0');
                    identity.push_back(row.close_before ? '1' : '0');
                }
                const std::size_t occurrence = frame.occurrences[identity]++;
                identity.push_back('#');
                identity.append(std::to_string(occurrence));
            }
            node.key = hash_suffix(identity);
            auto& path_identity = materialized.path_identity;
            path_identity = hash_suffix(
                (frame.path_identity.empty() ? bridge->contribution_id : frame.path_identity) +
                "\n" + identity);
            row.action_id = "menu-action-" + path_identity;
            if (!materialized.action_ids.emplace(row.action_id).second) {
                error = "menu action identities must be unique";
                lua_settop(state, base);
                return false;
            }
            if (has_command) {
                auto [entry, inserted] = actions.emplace(row.action_id, LUA_NOREF);
                (void)inserted;
                lua_pushvalue(state, command_index);
                entry->second = luaL_ref(state, LUA_REGISTRYINDEX);
            }

            const std::size_t row_bytes = node.key.size() + row.label.size() + row.icon.size() +
                                          row.action_id.size() + row.payload_json.size();
            if (total_bytes > kMaximumMenuSnapshotBytes ||
                row_bytes > kMaximumMenuSnapshotBytes - total_bytes) {
                error = "menu builder snapshot exceeds its byte budget";
                lua_settop(state, base);
                return false;
            }
            total_bytes += row_bytes;
            if (node.submenu) {
                lua_rawgeti(state, LUA_REGISTRYINDEX, children_ref);
                if (lua_isfunction(state, -1)) {
                    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
                        detail::capture_state_error_locked(state, -1);
                        lua_settop(state, base);
                        return false;
                    }
                    children_ref = materialized.hold(-1);
                }
                std::size_t child_count = 0;
                if (!sequence_table(state, -1, child_count, error)) {
                    lua_settop(state, base);
                    return false;
                }
                const void* child_identity = lua_topointer(state, -1);
                if (!materialized.ancestors.insert(child_identity).second) {
                    error = "menu contains an ancestor cycle";
                    lua_settop(state, base);
                    return false;
                }
                if (child_count > kMaximumMenuRows - admitted_nodes) {
                    error = "menu node budget exceeded";
                    lua_settop(state, base);
                    return false;
                }
                admitted_nodes += child_count;
                node.children.reserve(child_count);
                materialized.frames.push_back({children_ref, child_identity, row_identity,
                                                &node.children, child_count, 0,
                                                path_identity, {}});
            } else {
                materialized.ancestors.erase(row_identity);
            }
            lua_settop(state, base);
        }

        auto navigation = requested_navigation == nullptr ? bridge->navigation : *requested_navigation;
        if (!navigation.replace(materialized.tree, error)) {
            lua_settop(state, base);
            return false;
        }
        auto rows = navigation.rows();

        const bool rows_changed = bridge->revision == 0 || bridge->rows != rows;
        if (rows_changed && bridge->revision == std::numeric_limits<std::uint64_t>::max()) {
            error = "menu snapshot revision exhausted";
            release_action_refs(state, actions);
            lua_settop(state, base);
            return false;
        }

        bridge->navigation = std::move(navigation);
        bridge->rows.swap(rows);
        bridge->actions.swap(actions);
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
    menu_materialization materialized;
    const menu_navigation::State* requested_navigation = nullptr;

    menu_snapshot_build_context(menu_bridge* value, std::string* failure,
                                const menu_navigation::State* navigation = nullptr)
        : bridge(value), error(failure), materialized(value->state),
          requested_navigation(navigation) {}
};

int build_menu_snapshot_body(lua_State* state) {
    auto* context = static_cast<menu_snapshot_build_context*>(lua_touserdata(state, 1));
    context->succeeded = build_menu_snapshot(context->bridge, context->materialized,
                                            context->requested_navigation, *context->error);
    return 0;
}

int32_t SAO_PLUGINS_CALL native_menu_snapshot_v2(
    void* rows, std::uint32_t capacity, std::uint32_t row_stride_bytes,
    std::uint32_t* out_count, std::uint64_t* out_revision,
    sao::plugins::loader::entity_snapshot_content_token_t* out_content_token,
    std::uint32_t* out_row_stride_bytes, void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || out_content_token == nullptr ||
        out_row_stride_bytes == nullptr || user_data == nullptr ||
        (capacity > 0 && rows == nullptr) || (rows == nullptr && row_stride_bytes != 0)) {
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
            menu_snapshot_build_context context{bridge, &error};
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
        *out_content_token =
            bridge->revision == sao::plugins::loader::kInvalidEntitySnapshotContentToken
                ? 1
                : bridge->revision;
        *out_row_stride_bytes =
            bridge->rows.empty()
                ? 0
                : static_cast<std::uint32_t>(
                      sizeof(sao::plugins::loader::entity_menu_row_v2));
        if (capacity < bridge->rows.size()) {
            return SAO_ERR_BUFFER_TOO_SMALL;
        }
        if (!bridge->rows.empty() &&
            row_stride_bytes < sizeof(sao::plugins::loader::entity_menu_row_v2)) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ABI_MISMATCH;
        }
        if (!bridge->rows.empty() &&
            row_stride_bytes % alignof(sao::plugins::loader::entity_menu_row_v2) != 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        for (std::size_t index = 0; index < bridge->rows.size(); ++index) {
            const auto& source = bridge->rows[index];
            const sao::plugins::loader::entity_menu_row_v2 row{
                sizeof(sao::plugins::loader::entity_menu_row_v2),
                bridge->contribution_id.c_str(),
                bridge->name.c_str(),
                bridge->icon.c_str(),
                bridge->priority,
                source.label.c_str(),
                source.icon.c_str(),
                source.action_id.c_str(),
                source.payload_json.c_str(),
                static_cast<std::uint8_t>(source.can_activate),
                static_cast<std::uint8_t>(source.keep_open),
                static_cast<std::uint8_t>(source.close_before),
                {},
            };
            std::memcpy(static_cast<std::byte*>(rows) + index * row_stride_bytes, &row,
                        sizeof(row));
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t submit_action_result(
    sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
    void* result_sink_user_data, bool handled, const char* result_json_utf8) noexcept {
    if (result_sink == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const sao::plugins::loader::entity_action_result_v2 result{
        sizeof(sao::plugins::loader::entity_action_result_v2),
        sao::plugins::loader::kEntityActionAbiVersion2,
        static_cast<std::uint8_t>(handled),
        {},
        result_json_utf8,
    };
    return result_sink(&result, result_sink_user_data);
}

int32_t submit_lua_action_result(
    lua_State* state, int index, bool nil_is_decline,
    sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
    void* result_sink_user_data, std::string& conversion_error) {
    if (lua_isnil(state, index))
        return submit_action_result(result_sink, result_sink_user_data, !nil_is_decline, nullptr);

    detail::json result_value;
    if (!detail::stack_to_json(state, index, result_value, conversion_error))
        return SAO_ERR_INVALID_ARGUMENT;
    std::string serialized;
    if (!detail::serialize_json(result_value, serialized, conversion_error))
        return SAO_ERR_INVALID_ARGUMENT;
    return submit_action_result(result_sink, result_sink_user_data, true, serialized.c_str());
}

int32_t SAO_PLUGINS_CALL native_menu_action_v2(
    const char* action_id_utf8, const char* payload_json_utf8,
    sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
    void* result_sink_user_data, void* user_data) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr || result_sink == nullptr ||
        user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* bridge = static_cast<menu_bridge*>(user_data);
    try {
        detail::state_operation operation;
        const int32_t status = detail::acquire_state_operation(bridge->state, operation);
        if (status != SAO_OK)
            return status;
        lua_State* state = operation.state();
        if (bridge->closing)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        detail::lua_stack_guard stack(state);
        auto navigation = bridge->navigation;
        const auto navigation_result = navigation.activate(action_id_utf8);
        if (navigation_result != menu_navigation::NavigationResult::not_navigation) {
            if (navigation_result == menu_navigation::NavigationResult::stale)
                return submit_action_result(result_sink, result_sink_user_data, false, nullptr);
            detail::clear_state_error_locked(state);
            std::string error;
            menu_snapshot_build_context context{bridge, &error, &navigation};
            if (detail::protected_trampoline(state, build_menu_snapshot_body, &context, 0) != LUA_OK) {
                detail::capture_state_error_locked(state, -1);
                lua_pop(state, 1);
                return SAO_ERR_OS_CALL_FAILED;
            }
            if (!context.succeeded) {
                if (!error.empty())
                    detail::set_state_error_locked(state, std::move(error));
                return SAO_ERR_OS_CALL_FAILED;
            }
            return submit_action_result(result_sink, result_sink_user_data, true, nullptr);
        }
        const auto visible = std::find_if(bridge->rows.begin(), bridge->rows.end(),
            [action_id_utf8](const menu_row& row) {
                return row.action_id == action_id_utf8 && row.can_activate;
            });
        if (visible == bridge->rows.end())
            return submit_action_result(result_sink, result_sink_user_data, false, nullptr);
        const auto found = bridge->actions.find(action_id_utf8);
        if (found == bridge->actions.end())
            return submit_action_result(result_sink, result_sink_user_data, false, nullptr);

        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
        if (!lua_isfunction(state, -1)) {
            lua_settop(state, base);
            return SAO_ERR_HANDLE_INVALID;
        }
        detail::clear_state_error_locked(state);
        if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }
        std::string conversion_error;
        const int32_t result_status = submit_lua_action_result(
            state, -1, false, result_sink, result_sink_user_data, conversion_error);
        if (!conversion_error.empty())
            detail::set_state_error_locked(state, std::move(conversion_error));
        lua_settop(state, base);
        return result_status;
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
        detail::lua_stack_guard stack(state);
        if (std::string_view(action_id_utf8).starts_with(menu_navigation::navigation_prefix))
            return submit_action_result(result_sink, result_sink_user_data, false, nullptr);
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
        detail::json payload;
        std::string conversion_error;
        if (!detail::protected_push_json(state, detail::json(action_id_utf8), conversion_error) ||
            !detail::parse_json_c_string(payload_json_utf8, payload, conversion_error) ||
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

        const int32_t sink_status = submit_lua_action_result(
            state, -1, true, result_sink, result_sink_user_data, conversion_error);
        if (!conversion_error.empty()) {
            detail::set_state_error_locked(state, std::move(conversion_error));
        }
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

        sao::plugins::loader::context_entity_provider_descriptor_v3 provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = menu.provider_id.c_str();
        provider.snapshot = native_menu_snapshot_v2;
        provider.action_handler = nullptr;
        provider.user_data = &menu;
        provider.root_contribution = &root;
        provider.action_handler_v2 = native_menu_action_v2;
        provider.action_user_data = &menu;
        return sao::plugins::loader::sao_plugins_ctx_register_entity_provider_v3(
            bridge.context, &provider);
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
        struct stack_guard final {
            lua_State* state;
            int base;
            ~stack_guard() { lua_settop(state, base); }
        } stack{state, base};
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback->function_ref);
        std::string conversion_error;
        detail::json payload;
        if ((surface_utf8 != nullptr &&
             !detail::protected_push_json(state, detail::json(surface_utf8), conversion_error)) ||
            !detail::parse_json_c_string(payload_json_utf8, payload, conversion_error) ||
            !detail::protected_push_json(state, payload, conversion_error)) {
            detail::set_state_error_locked(state, std::move(conversion_error));
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (lua_pcall(state, surface_utf8 == nullptr ? 1 : 2, 1, 0) != LUA_OK) {
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

int32_t panel_render_callback(const char* payload, char** output, void* user_data) noexcept {
    auto* panel = static_cast<panel_callback_record*>(user_data);
    if (panel == nullptr || !panel->render)
        return SAO_ERR_HANDLE_INVALID;
    return render_hook_callback(nullptr, payload, output,
                                 reinterpret_cast<void*>(panel->render->dispatch_id));
}

int32_t panel_action_callback(const char* action, const char* payload, char** output,
                              void* user_data) noexcept {
    auto* panel = static_cast<panel_callback_record*>(user_data);
    if (panel == nullptr || !panel->action)
        return SAO_ERR_HANDLE_INVALID;
    return render_hook_callback(action == nullptr ? "" : action, payload, output,
                                 reinterpret_cast<void*>(panel->action->dispatch_id));
}

int32_t release_panel_callbacks(bridge_state& bridge, lua_State* state,
                                bool pending_only, bool enable_only) {
    for (auto entry = bridge.panels.begin(); entry != bridge.panels.end();) {
        auto& panel = *entry->second;
        if ((pending_only && !panel.pending_enable) || (enable_only && !panel.enable_scoped)) {
            ++entry;
            continue;
        }
        if (panel.render) {
            const int32_t status = stop_callback(*panel.render);
            if (status != SAO_OK)
                return status;
        }
        if (panel.action) {
            const int32_t status = stop_callback(*panel.action);
            if (status != SAO_OK) {
                if (panel.render)
                    resume_callback(*panel.render);
                return status;
            }
        }
        const int32_t status = sao::plugins::loader::sao_plugins_ctx_unregister_ui_panel(
            bridge.context, entry->first.c_str());
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
            if (panel.render)
                resume_callback(*panel.render);
            if (panel.action)
                resume_callback(*panel.action);
            return status;
        }
        if (panel.render)
            release_callback_ref(state, *panel.render);
        if (panel.action)
            release_callback_ref(state, *panel.action);
        entry = bridge.panels.erase(entry);
    }
    return SAO_OK;
}

// ── ctx → lua_State map (runtime_bridge provider lookup) ─────────────
std::mutex g_ctx_state_mutex;
std::unordered_map<loader_context_t*, lua_State*> g_ctx_state_map;

lua_State* find_state_for_context(loader_context_t* context) noexcept {
    if (context == nullptr)
        return nullptr;
    try {
        std::lock_guard lock(g_ctx_state_mutex);
        const auto found = g_ctx_state_map.find(context);
        return found == g_ctx_state_map.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

// ── UTF-8 / UTF-16 helpers (filesystem codecs keep this TU windows-free) ──
std::string wide_to_utf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0')
        return {};
    try {
        const auto u8 = std::filesystem::path(std::wstring(value)).u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (...) {
        return {};
    }
}

std::wstring utf8_to_wide_string(const char* value) {
    if (value == nullptr || *value == '\0')
        return {};
    try {
        const std::string_view view(value);
        return std::filesystem::path(
                   std::u8string_view(reinterpret_cast<const char8_t*>(value),
                                      view.size()))
            .wstring();
    } catch (...) {
        return {};
    }
}

// ── compositor input dispatch (shared user_data for the 4 native fns) ──
enum compositor_input_slot : std::size_t {
    compositor_input_cursor_pos = 0,
    compositor_input_mouse_button = 1,
    compositor_input_cursor_leave = 2,
    compositor_input_scroll = 3,
};

std::shared_ptr<event_callback_record> find_compositor_record(
    compositor_input_dispatch* dispatch, std::size_t slot) noexcept {
    if (dispatch == nullptr || slot >= dispatch->record_ids.size() ||
        dispatch->closing.load(std::memory_order_acquire) ||
        dispatch->record_ids[slot] == 0) {
        return nullptr;
    }
    return find_callback(reinterpret_cast<void*>(dispatch->record_ids[slot]));
}

void compositor_invoke(void* user_data, std::size_t slot, double arg_a, double arg_b,
                       bool arg_b_is_bool, int argument_count) noexcept {
    try {
        auto* dispatch = static_cast<compositor_input_dispatch*>(user_data);
        auto record = find_compositor_record(dispatch, slot);
        if (record == nullptr || !enter_callback(*record))
            return;
        struct callback_guard final {
            event_callback_record& callback;
            ~callback_guard() {
                leave_callback(callback);
            }
        } guard{*record};
        lua_State* state = record->state.load(std::memory_order_acquire);
        if (state == nullptr)
            return;
        detail::state_operation operation;
        if (detail::acquire_state_operation(state, operation) != SAO_OK ||
            record->closing.load(std::memory_order_acquire)) {
            return;
        }
        state = operation.state();
        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, record->function_ref);
        if (!lua_isfunction(state, -1)) {
            lua_settop(state, base);
            return;
        }
        int pushed = 0;
        if (argument_count >= 1) {
            lua_pushnumber(state, static_cast<lua_Number>(arg_a));
            pushed = 1;
        }
        if (argument_count >= 2) {
            if (arg_b_is_bool) {
                lua_pushboolean(state, arg_b != 0.0 ? 1 : 0);
            } else {
                lua_pushnumber(state, static_cast<lua_Number>(arg_b));
            }
            pushed = 2;
        }
        if (lua_pcall(state, pushed, 0, 0) != LUA_OK) {
            if (lua_gettop(state) > base) {
                detail::capture_state_error_locked(state, -1);
            }
        }
        lua_settop(state, base);
    } catch (...) {
    }
}

void SAO_PLUGINS_CALL compositor_cursor_pos_dispatch(float x, float y,
                                                     void* user_data) noexcept {
    compositor_invoke(user_data, compositor_input_cursor_pos, static_cast<double>(x),
                      static_cast<double>(y), false, 2);
}

void SAO_PLUGINS_CALL compositor_mouse_button_dispatch(uint32_t button, bool pressed,
                                                       void* user_data) noexcept {
    compositor_invoke(user_data, compositor_input_mouse_button,
                      static_cast<double>(button), pressed ? 1.0 : 0.0, true, 2);
}

void SAO_PLUGINS_CALL compositor_cursor_leave_dispatch(void* user_data) noexcept {
    compositor_invoke(user_data, compositor_input_cursor_leave, 0.0, 0.0, false, 0);
}

void SAO_PLUGINS_CALL compositor_scroll_dispatch(float dx, float dy,
                                                 void* user_data) noexcept {
    compositor_invoke(user_data, compositor_input_scroll, static_cast<double>(dx),
                      static_cast<double>(dy), false, 2);
}

// Release the lua callback records bound to one compositor layer name.
// Returns SAO_PLUGINS_ERR_BUSY if a callback is currently in flight.
int32_t release_compositor_input_locked(bridge_state& bridge, lua_State* state,
                                        const std::string& name) noexcept {
    const auto found = bridge.compositor_inputs.find(name);
    if (found == bridge.compositor_inputs.end())
        return SAO_OK;
    auto& dispatch = found->second;
    dispatch->closing.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<event_callback_record>> records;
    records.reserve(dispatch->record_ids.size());
    for (const uintptr_t id : dispatch->record_ids) {
        if (id == 0)
            continue;
        const auto rec = bridge.compositor_records.find(id);
        if (rec != bridge.compositor_records.end())
            records.push_back(rec->second);
    }
    std::vector<std::shared_ptr<event_callback_record>> stopped;
    stopped.reserve(records.size());
    for (const auto& record : records) {
        const int32_t status = stop_callback(*record);
        if (status != SAO_OK) {
            for (const auto& item : stopped)
                resume_callback(*item);
            dispatch->closing.store(false, std::memory_order_release);
            return status;
        }
        stopped.push_back(record);
    }
    for (const auto& record : records) {
        bridge.compositor_records.erase(record->dispatch_id);
        release_callback_ref(state, *record);
    }
    bridge.compositor_inputs.erase(found);
    try {
        bridge.retired_compositor_dispatches.push_back(std::move(dispatch));
    } catch (...) {
    }
    return SAO_OK;
}

// ── script_value ↔ lua marshalling ───────────────────────────────────────
constexpr int kMaximumScriptValueDepth = 64;
constexpr std::size_t kMaximumScriptValueItems = 16384;

struct script_value_box {
    script_ctx::script_value_ptr value;
};

struct script_module_box {
    std::shared_ptr<script_ctx::script_module> module;
};

int script_value_dispatch(lua_State* state) noexcept;   // forward
int script_module_index(lua_State* state) noexcept;     // forward
int script_module_member_call(lua_State* state) noexcept;  // forward

int script_value_gc(lua_State* state) noexcept {
    auto* box = static_cast<script_value_box*>(lua_touserdata(state, 1));
    if (box != nullptr)
        box->~script_value_box();
    return 0;
}

int script_module_gc(lua_State* state) noexcept {
    auto* box = static_cast<script_module_box*>(lua_touserdata(state, 1));
    if (box != nullptr)
        box->~script_module_box();
    return 0;
}

void ensure_script_value_metatable(lua_State* state) {
    if (luaL_newmetatable(state, "SaoScriptValue") == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushcfunction(state, script_value_dispatch);
    lua_setfield(state, -2, "__call");
    lua_pushcfunction(state, script_value_gc);
    lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "locked");
    lua_setfield(state, -2, "__metatable");
}

void ensure_script_module_metatable(lua_State* state) {
    if (luaL_newmetatable(state, "SaoScriptModule") == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushcfunction(state, script_module_index);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, script_module_gc);
    lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "locked");
    lua_setfield(state, -2, "__metatable");
}

bool push_script_box(lua_State* state, script_ctx::script_value_ptr value) {
    auto* box = static_cast<script_value_box*>(lua_newuserdatauv(
        state, sizeof(script_value_box), 0));
    if (box == nullptr)
        return false;
    try {
        new (box) script_value_box{std::move(value)};
    } catch (...) {
        lua_pop(state, 1);
        return false;
    }
    ensure_script_value_metatable(state);
    lua_setmetatable(state, -2);
    return true;
}

bool push_script_module_box(lua_State* state,
                            std::shared_ptr<script_ctx::script_module> module) {
    auto* box = static_cast<script_module_box*>(lua_newuserdatauv(
        state, sizeof(script_module_box), 0));
    if (box == nullptr)
        return false;
    try {
        new (box) script_module_box{std::move(module)};
    } catch (...) {
        lua_pop(state, 1);
        return false;
    }
    ensure_script_module_metatable(state);
    lua_setmetatable(state, -2);
    return true;
}

// script_value → lua.  `error` is optional diagnostics for callers.
bool push_script_value(lua_State* state, const script_ctx::script_value_ptr& value,
                       std::string& error, int depth = 0) {
    if (depth > kMaximumScriptValueDepth) {
        error = "script value nesting exceeds its depth budget";
        return false;
    }
    if (value == nullptr) {
        lua_pushnil(state);
        return true;
    }
    using kind = script_ctx::script_value::kind;
    switch (value->k) {
    case kind::null:
        lua_pushnil(state);
        return true;
    case kind::boolean:
        lua_pushboolean(state, value->boolean ? 1 : 0);
        return true;
    case kind::integer:
        lua_pushinteger(state, static_cast<lua_Integer>(value->integer));
        return true;
    case kind::number:
        lua_pushnumber(state, static_cast<lua_Number>(value->number));
        return true;
    case kind::string:
    case kind::bytes:
        lua_pushlstring(state, value->text.data(), value->text.size());
        return true;
    case kind::list: {
        if (value->items.size() > kMaximumScriptValueItems ||
            value->items.size() > static_cast<std::size_t>(
                                      std::numeric_limits<int>::max())) {
            error = "script list value is too large for Lua";
            return false;
        }
        lua_createtable(state, static_cast<int>(value->items.size()), 0);
        for (std::size_t index = 0; index < value->items.size(); ++index) {
            if (!push_script_value(state, value->items[index], error, depth + 1)) {
                lua_pop(state, 1);
                return false;
            }
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return true;
    }
    case kind::map: {
        if (value->object.size() > kMaximumScriptValueItems) {
            error = "script map value is too large for Lua";
            return false;
        }
        lua_createtable(state, 0, static_cast<int>(value->object.size()));
        for (const auto& [key, item] : value->object) {
            lua_pushlstring(state, key.data(), key.size());
            if (!push_script_value(state, item, error, depth + 1)) {
                lua_pop(state, 2);
                return false;
            }
            lua_rawset(state, -3);
        }
        return true;
    }
    case kind::function:
        return push_script_box(state, value);
    }
    error = "script value kind is not representable in Lua";
    return false;
}

// lua → script_value (positional args for module->call).
bool lua_to_script_value(lua_State* state, int index, script_ctx::script_value_ptr& out,
                         std::string& error, int depth,
                         std::vector<const void*>& active_tables) {
    if (depth > kMaximumScriptValueDepth) {
        error = "Lua value nesting exceeds its depth budget";
        return false;
    }
    index = lua_absindex(state, index);
    using kind = script_ctx::script_value::kind;
    switch (lua_type(state, index)) {
    case LUA_TNONE:
    case LUA_TNIL:
        out = script_ctx::script_value::null_value();
        return true;
    case LUA_TBOOLEAN:
        out = script_ctx::script_value::make_boolean(lua_toboolean(state, index) != 0);
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) {
            out = script_ctx::script_value::make_integer(
                static_cast<int64_t>(lua_tointeger(state, index)));
        } else {
            const double number = static_cast<double>(lua_tonumber(state, index));
            if (!std::isfinite(number)) {
                error = "Lua numbers passed to modules must be finite";
                return false;
            }
            out = script_ctx::script_value::make_number(number);
        }
        return true;
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        out = script_ctx::script_value::make_string(
            std::string(text == nullptr ? "" : text, length));
        return true;
    }
    case LUA_TUSERDATA: {
        auto* box = static_cast<script_value_box*>(luaL_testudata(state, index,
                                                                "SaoScriptValue"));
        if (box != nullptr && box->value != nullptr &&
            box->value->k == kind::function) {
            out = box->value;
            return true;
        }
        error = "Lua userdata cannot cross the script module boundary";
        return false;
    }
    case LUA_TTABLE: {
        const void* identity = lua_topointer(state, index);
        if (std::find(active_tables.begin(), active_tables.end(), identity) !=
            active_tables.end()) {
            error = "cyclic Lua tables cannot cross the script module boundary";
            return false;
        }
        if (lua_checkstack(state, 3) == 0) {
            error = "Lua stack cannot represent the script value";
            return false;
        }
        active_tables.push_back(identity);
        detail::active_table_guard guard(active_tables);
        struct entry {
            bool integer_key = false;
            lua_Integer integer = 0;
            std::string string_key;
            script_ctx::script_value_ptr value;
        };
        std::vector<entry> items;
        bool array_candidate = true;
        lua_Integer largest_index = 0;
        std::size_t count = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            if (++count > kMaximumScriptValueItems) {
                lua_pop(state, 2);
                error = "Lua table exceeds the script value item budget";
                return false;
            }
            entry item;
            if (lua_isinteger(state, -2)) {
                item.integer_key = true;
                item.integer = lua_tointeger(state, -2);
                if (item.integer <= 0)
                    array_candidate = false;
                largest_index = std::max(largest_index, item.integer);
            } else if (lua_type(state, -2) == LUA_TSTRING) {
                std::size_t length = 0;
                const char* key = lua_tolstring(state, -2, &length);
                item.string_key.assign(key == nullptr ? "" : key, length);
                array_candidate = false;
            } else {
                lua_pop(state, 2);
                error = "Lua table script-value keys must be strings or positive integers";
                return false;
            }
            if (!lua_to_script_value(state, -1, item.value, error, depth + 1,
                                     active_tables)) {
                lua_pop(state, 2);
                return false;
            }
            items.push_back(std::move(item));
            lua_pop(state, 1);
        }
        array_candidate = array_candidate && !items.empty() && largest_index >= 0 &&
                          static_cast<std::size_t>(largest_index) == items.size();
        if (array_candidate) {
            std::vector<script_ctx::script_value_ptr> list(items.size());
            for (auto& item : items) {
                list[static_cast<std::size_t>(item.integer - 1)] = std::move(item.value);
            }
            out = script_ctx::script_value::make_list(std::move(list));
        } else {
            std::vector<std::pair<std::string, script_ctx::script_value_ptr>> object;
            object.reserve(items.size());
            for (auto& item : items) {
                const std::string key = item.integer_key
                                            ? std::to_string(item.integer)
                                            : std::move(item.string_key);
                object.emplace_back(std::move(key), std::move(item.value));
            }
            out = script_ctx::script_value::make_map(std::move(object));
        }
        return true;
    }
    default:
        error = std::string("Lua value type '") + luaL_typename(state, index) +
                "' cannot cross the script module boundary";
        return false;
    }
}

bool lua_to_script_value(lua_State* state, int index, script_ctx::script_value_ptr& out,
                         std::string& error) {
    std::vector<const void*> active_tables;
    try {
        return lua_to_script_value(state, index, out, error, 0, active_tables);
    } catch (...) {
        error = "Lua script-value conversion failed";
        return false;
    }
}

// ── lua-backed script_module facade (ctx.load_local for .lua) ────────────
// Executes a bundled .lua file inside the plugin's own lua_State under a fresh
// _ENV and exposes the result table (or the env table when the chunk returns
// nil) through the shared script_module ABI.
class lua_local_module final
    : public script_ctx::script_module,
      public std::enable_shared_from_this<lua_local_module> {
  public:
    lua_local_module(lua_State* state, int table_ref, std::string id)
        : state_(detail::main_thread(state)), table_ref_(table_ref), id_(std::move(id)) {}

    ~lua_local_module() override {
        if (state_ == nullptr || table_ref_ == LUA_NOREF || table_ref_ == LUA_REFNIL)
            return;
        // Skip the unref once the bridge is gone — during lua_close the
        // registry dies with the state and luaL_unref would touch dead memory.
        if (!detail::has_ctx_bridge_locked(state_))
            return;
        detail::state_operation operation;
        if (detail::acquire_state_operation(state_, operation) == SAO_OK &&
            operation.state() != nullptr) {
            luaL_unref(operation.state(), LUA_REGISTRYINDEX, table_ref_);
        }
        table_ref_ = LUA_NOREF;
    }

    const std::string& module_id() const noexcept override {
        return id_;
    }

    std::vector<std::string> member_names() const override {
        std::vector<std::string> names;
        if (state_ == nullptr)
            return names;
        detail::state_operation operation;
        if (detail::acquire_state_operation(state_, operation) != SAO_OK)
            return names;
        lua_State* state = operation.state();
        if (state == nullptr)
            return names;
        const int base = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, table_ref_);
        if (lua_istable(state, -1) && lua_checkstack(state, 3) != 0) {
            lua_pushnil(state);
            while (lua_next(state, -2) != 0) {
                if (lua_type(state, -2) == LUA_TSTRING) {
                    std::size_t length = 0;
                    const char* key = lua_tolstring(state, -2, &length);
                    if (key != nullptr && std::strlen(key) == length)
                        names.emplace_back(key, length);
                }
                lua_pop(state, 1);
            }
        }
        lua_settop(state, base);
        return names;
    }

    int32_t get(const std::string& name, script_ctx::script_value_ptr* out_value,
                std::string* out_error) override {
        if (out_value == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        out_value->reset();
        if (state_ == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua module state is closed";
            return SAO_ERR_HANDLE_INVALID;
        }
        detail::state_operation operation;
        const int32_t acquire = detail::acquire_state_operation(state_, operation);
        if (acquire != SAO_OK || operation.state() == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua module state is unavailable";
            return acquire != SAO_OK ? acquire : SAO_ERR_HANDLE_INVALID;
        }
        lua_State* state = operation.state();
        const int base = lua_gettop(state);
        int32_t result = SAO_OK;
        lua_rawgeti(state, LUA_REGISTRYINDEX, table_ref_);
        if (!lua_istable(state, -1)) {
            if (out_error != nullptr)
                *out_error = "lua module table is unavailable";
            result = SAO_ERR_HANDLE_INVALID;
        } else {
            lua_pushlstring(state, name.data(), name.size());
            lua_rawget(state, -2);
            if (lua_isfunction(state, -1)) {
                auto self = shared_from_this();
                const std::string member = name;
                *out_value = script_ctx::script_value::make_function(
                    [self, member](const std::vector<script_ctx::script_value_ptr>& args,
                                   script_ctx::script_value_ptr* inner_out,
                                   std::string* inner_error) -> int32_t {
                        return self->call(member, args, inner_out, inner_error);
                    });
            } else {
                script_ctx::script_value_ptr converted;
                std::string error;
                if (!lua_to_script_value(state, -1, converted, error)) {
                    if (out_error != nullptr)
                        *out_error = std::move(error);
                    result = SAO_ERR_INVALID_ARGUMENT;
                } else {
                    *out_value = std::move(converted);
                }
            }
        }
        lua_settop(state, base);
        return result;
    }

    int32_t call(const std::string& name,
                 const std::vector<script_ctx::script_value_ptr>& args,
                 script_ctx::script_value_ptr* out_value,
                 std::string* out_error) override {
        if (state_ == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua module state is closed";
            return SAO_ERR_HANDLE_INVALID;
        }
        detail::state_operation operation;
        const int32_t acquire = detail::acquire_state_operation(state_, operation);
        if (acquire != SAO_OK || operation.state() == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua module state is unavailable";
            return acquire != SAO_OK ? acquire : SAO_ERR_HANDLE_INVALID;
        }
        lua_State* state = operation.state();
        const int base = lua_gettop(state);
        int32_t result = SAO_OK;
        lua_rawgeti(state, LUA_REGISTRYINDEX, table_ref_);
        if (!lua_istable(state, -1)) {
            if (out_error != nullptr)
                *out_error = "lua module table is unavailable";
            result = SAO_ERR_HANDLE_INVALID;
        } else {
            lua_pushlstring(state, name.data(), name.size());
            lua_rawget(state, -2);
            if (!lua_isfunction(state, -1)) {
                if (out_error != nullptr)
                    *out_error = "lua module member is not callable: " + name;
                result = SAO_ERR_INVALID_ARGUMENT;
            } else {
                int argument_count = 0;
                bool args_ok = true;
                std::string push_error;
                for (const auto& argument : args) {
                    if (!push_script_value(state, argument, push_error)) {
                        args_ok = false;
                        break;
                    }
                    ++argument_count;
                }
                if (!args_ok) {
                    if (out_error != nullptr)
                        *out_error = std::move(push_error);
                    result = SAO_ERR_INVALID_ARGUMENT;
                } else if (lua_pcall(state, argument_count, 1, 0) != LUA_OK) {
                    const char* message = lua_tostring(state, -1);
                    if (out_error != nullptr) {
                        *out_error = message == nullptr
                                         ? "lua module member call failed"
                                         : std::string(message);
                    }
                    detail::capture_state_error_locked(state, -1);
                    result = SAO_ERR_OS_CALL_FAILED;
                } else {
                    script_ctx::script_value_ptr converted;
                    std::string error;
                    if (!lua_to_script_value(state, -1, converted, error)) {
                        if (out_error != nullptr)
                            *out_error = std::move(error);
                        result = SAO_ERR_INVALID_ARGUMENT;
                    } else if (out_value != nullptr) {
                        *out_value = std::move(converted);
                    }
                }
            }
        }
        lua_settop(state, base);
        return result;
    }

  private:
    lua_State* state_ = nullptr;
    int table_ref_ = LUA_NOREF;
    std::string id_;
};

// ── runtime_bridge provider (lua engine, priority 50) ────────────────────
bool lua_engine_probe(loader_context_t*, const wchar_t*, std::string&,
                      void* /*user_data*/) noexcept {
    return true;
}

int32_t SAO_PLUGINS_CALL lua_engine_load_module(
    loader_context_t* ctx, const wchar_t* abs_path, const std::string& logical_name,
    std::shared_ptr<script_ctx::script_module>* out_module, std::string* out_error,
    void* /*user_data*/) noexcept {
    try {
        if (out_module == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        out_module->reset();
        if (ctx == nullptr || abs_path == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua load_module: invalid argument";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        lua_State* state = find_state_for_context(ctx);
        if (state == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua engine has no live state for this context";
            return SAO_ERR_HANDLE_INVALID;
        }
        detail::state_operation operation;
        const int32_t acquire = detail::acquire_state_operation(state, operation);
        if (acquire != SAO_OK || operation.state() == nullptr) {
            if (out_error != nullptr)
                *out_error = "lua state is closing";
            return acquire != SAO_OK ? acquire : SAO_ERR_HANDLE_INVALID;
        }
        state = operation.state();

        std::ifstream input(std::filesystem::path(abs_path), std::ios::binary);
        if (!input) {
            if (out_error != nullptr)
                *out_error = "lua load_module: cannot open file";
            return SAO_ERR_HANDLE_INVALID;
        }
        std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
        if (!input.eof() && input.fail()) {
            if (out_error != nullptr)
                *out_error = "lua load_module: read failed";
            return SAO_ERR_OS_CALL_FAILED;
        }

        const int base = lua_gettop(state);
        const std::string chunk_name = "@" + wide_to_utf8(abs_path);
        if (luaL_loadbufferx(state, source.data(), source.size(), chunk_name.c_str(),
                            "t") != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            if (out_error != nullptr) {
                *out_error = message == nullptr ? "lua chunk load failed"
                                              : std::string(message);
            }
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        // stack: [chunk]; add isolated _ENV = setmetatable({}, {__index=_G})
        lua_newtable(state);                                   // chunk, env
        lua_newtable(state);                                   // chunk, env, env_mt
        lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, -2);
        lua_pushvalue(state, -1);                              // chunk, env, env
        lua_setupvalue(state, -3, 1);                          // chunk._ENV=env → chunk, env
        if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            if (out_error != nullptr) {
                *out_error = message == nullptr ? "lua chunk exec failed"
                                              : std::string(message);
            }
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            return SAO_ERR_OS_CALL_FAILED;
        }
        // stack: env, result.  Module table = returned table, else env.
        int module_ref = LUA_NOREF;
        if (lua_istable(state, -1)) {
            module_ref = luaL_ref(state, LUA_REGISTRYINDEX);  // refs result table
        } else {
            lua_pop(state, 1);                    // drop non-table result
            module_ref = luaL_ref(state, LUA_REGISTRYINDEX);  // refs env table
        }
        lua_settop(state, base);
        auto module = std::make_shared<lua_local_module>(state, module_ref,
                                                         logical_name);
        *out_module = std::move(module);
        return SAO_OK;
    } catch (...) {
        if (out_error != nullptr)
            *out_error = "lua load_module: internal error";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

const char* const kLuaScriptExtensions[] = {"lua", nullptr};

const script_ctx::script_engine_ops kLuaEngineOps = {
    "lua",
    50,   // priority: below python hosts — lowest-priority probing provider wins
    kLuaScriptExtensions,
    &lua_engine_probe,
    &lua_engine_load_module,
    nullptr,
};

int script_value_dispatch(lua_State* state) noexcept {
    auto* box = static_cast<script_value_box*>(
        luaL_checkudata(state, 1, "SaoScriptValue"));
    if (box == nullptr || box->value == nullptr ||
        box->value->k != script_ctx::script_value::kind::function ||
        !box->value->call) {
        return luaL_error(state, "script callable is not invokable");
    }
    const int top = lua_gettop(state);
    std::vector<script_ctx::script_value_ptr> args;
    try {
        args.reserve(static_cast<std::size_t>(top > 1 ? top - 1 : 0));
    } catch (...) {
        return luaL_error(state, "script call argument allocation failed");
    }
    for (int index = 2; index <= top; ++index) {
        script_ctx::script_value_ptr value;
        std::string error;
        if (!lua_to_script_value(state, index, value, error)) {
            return luaL_error(state, "script call argument is not marshalable: %s",
                              error.c_str());
        }
        args.push_back(std::move(value));
    }
    script_ctx::script_value_ptr result;
    std::string error;
    const int32_t status = box->value->call(args, &result, &error);
    if (status != SAO_OK) {
        return luaL_error(state, "script call failed with status %d: %s",
                          static_cast<int>(status), error.c_str());
    }
    if (!push_script_value(state, result, error)) {
        return luaL_error(state, "script call result is not marshalable: %s",
                          error.c_str());
    }
    return 1;
}

int script_module_index(lua_State* state) noexcept {
    auto* box = static_cast<script_module_box*>(
        luaL_checkudata(state, 1, "SaoScriptModule"));
    const char* name = luaL_checkstring(state, 2);
    if (box != nullptr && box->module != nullptr && name != nullptr && name[0] != '\0') {
        script_ctx::script_value_ptr value;
        std::string error;
        if (box->module->get(name, &value, &error) == SAO_OK && value != nullptr) {
            std::string push_error;
            if (push_script_value(state, value, push_error))
                return 1;
        }
        // get() may decline callable-only members — if the name exists at all,
        // return a closure that forwards to module->call.
        try {
            for (const auto& member : box->module->member_names()) {
                if (member == name) {
                    lua_pushvalue(state, 1);  // box udata keeps module alive
                    lua_pushvalue(state, 2);  // member name
                    lua_pushcclosure(state, script_module_member_call, 2);
                    return 1;
                }
            }
        } catch (...) {
        }
    }
    lua_pushnil(state);
    return 1;
}

int script_module_member_call(lua_State* state) noexcept {
    auto* box = static_cast<script_module_box*>(
        lua_touserdata(state, lua_upvalueindex(1)));
    const char* name = lua_tostring(state, lua_upvalueindex(2));
    if (box == nullptr || box->module == nullptr || name == nullptr) {
        return luaL_error(state, "script module handle is closed");
    }
    const int top = lua_gettop(state);
    std::vector<script_ctx::script_value_ptr> args;
    try {
        args.reserve(static_cast<std::size_t>(top));
    } catch (...) {
        return luaL_error(state, "script call argument allocation failed");
    }
    for (int index = 1; index <= top; ++index) {
        script_ctx::script_value_ptr value;
        std::string error;
        if (!lua_to_script_value(state, index, value, error)) {
            return luaL_error(state, "script call argument is not marshalable: %s",
                              error.c_str());
        }
        args.push_back(std::move(value));
    }
    script_ctx::script_value_ptr result;
    std::string error;
    const int32_t status = box->module->call(name, args, &result, &error);
    if (status != SAO_OK) {
        return luaL_error(state, "module member '%s' failed with status %d: %s", name,
                          static_cast<int>(status), error.c_str());
    }
    std::string push_error;
    if (!push_script_value(state, result, push_error)) {
        return luaL_error(state, "module result is not marshalable: %s",
                          push_error.c_str());
    }
    return 1;
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
    const bool has_render = !lua_isnoneornil(state, 4);
    const bool has_action = !lua_isnoneornil(state, 5);
    if ((has_render && !lua_isfunction(state, 4)) || (has_action && !lua_isfunction(state, 5)))
        return push_status_error(state, "register_ui_panel callbacks", SAO_ERR_INVALID_ARGUMENT);
    if (bridge->panels.contains(panel_id))
        return push_status_error(state, "register_ui_panel", sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    auto panel = std::make_shared<panel_callback_record>();
    panel->pending_enable = bridge->enable_checkpoint_active;
    try {
        if (has_render)
            panel->render = make_callback(*bridge, state, 4);
        if (has_action)
            panel->action = make_callback(*bridge, state, 5);
        bridge->panels.emplace(panel_id, panel);
    } catch (...) {
        if (panel->render)
            release_callback_ref(state, *panel->render);
        if (panel->action)
            release_callback_ref(state, *panel->action);
        throw;
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_ui_panel(
        bridge->context, panel_id, metadata.c_str(), has_render ? panel_render_callback : nullptr,
        has_action ? panel_action_callback : nullptr, panel.get());
    if (status != SAO_OK) {
        const int32_t cleanup = status == sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS ?
            SAO_OK : sao::plugins::loader::sao_plugins_ctx_unregister_ui_panel(bridge->context, panel_id);
        if (cleanup == SAO_OK || cleanup == SAO_ERR_HANDLE_INVALID) {
            if (panel->render)
                release_callback_ref(state, *panel->render);
            if (panel->action)
                release_callback_ref(state, *panel->action);
            bridge->panels.erase(panel_id);
        }
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

    const int candidate_ref = protected_registry_ref(state, 2);
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
    menu->builder_ref = protected_registry_ref(state, 4);

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

int ctx_register_menu_surface(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* surface_id = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TTABLE);
    const double priority = luaL_optnumber(state, 4, 0.0);
    if (surface_id == nullptr || surface_id[0] == '\0' || !std::isfinite(priority)) {
        return push_status_error(state, "register_menu_surface", SAO_ERR_INVALID_ARGUMENT);
    }
    // Flatten the descriptor: scalar/structured values marshal to JSON and
    // function fields are recorded by name under "hooks" (they cannot cross
    // the C ABI but must be visible in the record).
    detail::json metadata = detail::json::object();
    detail::json hooks = detail::json::array();
    std::string error;
    const int descriptor = lua_absindex(state, 3);
    lua_pushnil(state);
    while (lua_next(state, descriptor) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            return push_status_error(state, "register_menu_surface descriptor",
                                     SAO_ERR_INVALID_ARGUMENT);
        }
        std::size_t key_length = 0;
        const char* key = lua_tolstring(state, -2, &key_length);
        detail::json_budget key_budget;
        if (key == nullptr || std::strlen(key) != key_length || key_length == 0 ||
            !detail::consume_json_string({key, key_length}, key_budget, error)) {
            lua_pop(state, 2);
            return push_status_error(state, "register_menu_surface descriptor",
                                     SAO_ERR_INVALID_ARGUMENT);
        }
        std::string key_copy(key, key_length);
        if (lua_isfunction(state, -1)) {
            try {
                hooks.push_back(std::move(key_copy));
            } catch (...) {
                lua_pop(state, 2);
                throw method_failure{"register_menu_surface", SAO_ERR_OS_CALL_FAILED};
            }
        } else {
            detail::json value;
            if (!detail::stack_to_json(state, -1, value, error)) {
                lua_pop(state, 2);
                return push_status_error(state, "register_menu_surface descriptor",
                                         SAO_ERR_INVALID_ARGUMENT);
            }
            if (!value.is_null()) {
                try {
                    metadata[key_copy] = std::move(value);
                } catch (...) {
                    lua_pop(state, 2);
                    throw method_failure{"register_menu_surface", SAO_ERR_OS_CALL_FAILED};
                }
            }
        }
        lua_pop(state, 1);
    }
    if (!hooks.empty())
        metadata["hooks"] = std::move(hooks);
    std::string serialized;
    if (!detail::serialize_json(metadata, serialized, error)) {
        return push_status_error(state, "register_menu_surface", SAO_ERR_INVALID_ARGUMENT);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_menu_surface(
        bridge->context, surface_id, serialized.c_str(), static_cast<float>(priority));
    if (status != SAO_OK) {
        return push_status_error(state, "register_menu_surface", status);
    }
    lua_pushstring(state, surface_id);
    return 1;
}

// open_file(filters, title, initial_dir, hwnd_owner)
// filters: nil | string | table.  Strings in pipe form "label|pattern|..."
// are converted to the v1 dict-list JSON the provider's tokenizer pairs up;
// tables marshal through the standard JSON path.
int ctx_open_file(lua_State* state) {
    auto* bridge = checked_bridge(state);
    detail::json filters = detail::json(nullptr);
    std::string error;
    if (!lua_isnoneornil(state, 2)) {
        if (lua_istable(state, 2)) {
            if (!detail::stack_to_json(state, 2, filters, error)) {
                return push_status_error(state, "open_file filters",
                                         SAO_ERR_INVALID_ARGUMENT);
            }
        } else {
            const char* text = luaL_checkstring(state, 2);
            if (text == nullptr) {
                return push_status_error(state, "open_file filters",
                                         SAO_ERR_INVALID_ARGUMENT);
            }
            const std::string_view raw(text);
            if (raw.find('|') != std::string_view::npos) {
                filters = detail::json::array();
                std::vector<std::string> tokens;
                std::size_t cursor = 0;
                while (cursor <= raw.size()) {
                    const std::size_t bar = raw.find('|', cursor);
                    if (bar == std::string_view::npos) {
                        tokens.emplace_back(raw.substr(cursor));
                        break;
                    }
                    tokens.emplace_back(raw.substr(cursor, bar - cursor));
                    cursor = bar + 1;
                }
                for (std::size_t index = 0; index + 1 < tokens.size(); index += 2) {
                    filters.push_back({{"name", tokens[index]},
                                       {"spec", tokens[index + 1]}});
                }
                if (filters.empty())
                    filters = nullptr;
            } else {
                filters = detail::json::array(
                    {{{"name", raw}, {"spec", raw.find('*') != std::string_view::npos
                                                     ? std::string(raw)
                                                     : std::string("*.*")}}});
            }
        }
    }
    const char* title = luaL_optstring(state, 3, "");
    const char* initial = luaL_optstring(state, 4, nullptr);
    const auto hwnd_owner = static_cast<intptr_t>(luaL_optinteger(state, 5, 0));
    const std::wstring initial_dir = utf8_to_wide_string(initial);
    const std::string filters_json = filters.is_null() ? std::string{} : filters.dump();
    wchar_t* selected = nullptr;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_open_file(
        bridge->context, filters_json.empty() ? nullptr : filters_json.c_str(), title,
        initial_dir.empty() ? nullptr : initial_dir.c_str(), hwnd_owner, &selected);
    if (status != SAO_OK) {
        sao::plugins::loader::sao_plugins_ctx_free_wstring(selected);
        return push_status_error(state, "open_file", status);
    }
    if (selected == nullptr || selected[0] == L'\0') {
        sao::plugins::loader::sao_plugins_ctx_free_wstring(selected);
        lua_pushnil(state);
        return 1;
    }
    const std::string utf8 = wide_to_utf8(selected);
    sao::plugins::loader::sao_plugins_ctx_free_wstring(selected);
    lua_pushlstring(state, utf8.data(), utf8.size());
    return 1;
}

int ctx_open_window(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* panel_id = luaL_optstring(state, 2, "");
    const auto width = static_cast<uint32_t>(luaL_optinteger(state, 3, 0));
    const auto height = static_cast<uint32_t>(luaL_optinteger(state, 4, 0));
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_open_window(
        bridge->context, panel_id, width, height);
    if (status != SAO_OK) {
        return push_status_error(state, "open_window", status);
    }
    lua_pushstring(state, panel_id == nullptr ? "" : panel_id);
    return 1;
}

int ctx_ensure_requirements(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const bool install = lua_isnoneornil(state, 2) || lua_toboolean(state, 2) != 0;
    char* report = nullptr;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_ensure_requirements(
        bridge->context, install, &report);
    return push_owned_json(state, status, report, "ensure_requirements");
}

int ctx_load_local(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* rel = luaL_checkstring(state, 2);
    if (rel == nullptr || rel[0] == '\0') {
        return push_status_error(state, "load_local", SAO_ERR_INVALID_ARGUMENT);
    }
    const char* plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(bridge->context);
    const wchar_t* root_dir = sao::plugins::loader::sao_plugins_ctx_path(bridge->context);
    script_ctx::load_local_result kind = script_ctx::load_local_result::missing;
    std::shared_ptr<script_ctx::script_module> module;
    std::wstring abs_path;
    std::string diag;
    const int32_t status = script_ctx::runtime_bridge_load_local(
        bridge->context, plugin_id == nullptr ? "" : plugin_id, root_dir, rel, &kind,
        &module, &abs_path, &diag);
    if (status != SAO_OK) {
        return push_status_error(state, "load_local", status);
    }
    switch (kind) {
    case script_ctx::load_local_result::module:
        if (module != nullptr && push_script_module_box(state, std::move(module))) {
            return 1;
        }
        lua_pushnil(state);
        return 1;
    case script_ctx::load_local_result::path_only: {
        const std::string utf8 = wide_to_utf8(abs_path.c_str());
        lua_pushlstring(state, utf8.data(), utf8.size());
        return 1;
    }
    default:
        // missing / unsupported — plugins nil-guard; the diagnostic mirrors
        // the legacy graceful path through ctx.log.
        if (!diag.empty()) {
            sao::plugins::loader::sao_plugins_ctx_log(bridge->context, diag.c_str());
        }
        lua_pushnil(state);
        return 1;
    }
}

// create_compositor_layer(name, w, h, x=0, y=0, z=140, click_through=true,
//                          high_fps=false, target_fps=0)
int ctx_create_compositor_layer(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* name = luaL_checkstring(state, 2);
    const auto width = static_cast<uint32_t>(luaL_checkinteger(state, 3));
    const auto height = static_cast<uint32_t>(luaL_checkinteger(state, 4));
    const auto x = static_cast<int32_t>(luaL_optinteger(state, 5, 0));
    const auto y = static_cast<int32_t>(luaL_optinteger(state, 6, 0));
    const auto z = static_cast<int32_t>(luaL_optinteger(state, 7, 140));
    const bool click_through =
        lua_isnoneornil(state, 8) || lua_toboolean(state, 8) != 0;
    const bool high_fps = lua_toboolean(state, 9) != 0;
    const auto target_fps = static_cast<uint32_t>(luaL_optinteger(state, 10, 0));
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
        bridge->context, name, width, height, x, y, z, click_through, high_fps,
        target_fps);
    if (status != SAO_OK) {
        return push_status_error(state, "create_compositor_layer", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

// upload_compositor_frame(name, bgra_string, w, h[, x, y]) — x/y accepted for
// v1 signature parity but the current ctx export has no position override.
int ctx_upload_compositor_frame(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* name = luaL_checkstring(state, 2);
    std::size_t bytes_length = 0;
    const char* bytes = luaL_checklstring(state, 3, &bytes_length);
    const auto width = static_cast<uint32_t>(luaL_checkinteger(state, 4));
    const auto height = static_cast<uint32_t>(luaL_checkinteger(state, 5));
    if (bytes == nullptr || bytes_length == 0) {
        return push_status_error(state, "upload_compositor_frame",
                                 SAO_ERR_INVALID_ARGUMENT);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_upload_compositor_frame(
        bridge->context, name, reinterpret_cast<const uint8_t*>(bytes), bytes_length,
        width, height);
    if (status != SAO_OK) {
        return push_status_error(state, "upload_compositor_frame", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

const char* compositor_source_string(lua_State* state, int index, bool allow_empty,
                                     const char* operation) {
    if (lua_type(state, index) != LUA_TSTRING)
        push_status_error(state, operation, SAO_ERR_INVALID_ARGUMENT);
    std::size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    if ((!allow_empty && length == 0) ||
        length > SAO_PLUGIN_CONTEXT_COMPOSITOR_LAYER_NAME_MAX_BYTES ||
        std::memchr(value, '\0', length) != nullptr)
        push_status_error(state, operation, SAO_ERR_INVALID_ARGUMENT);
    return value;
}

int ctx_set_compositor_layer_mmf_source(lua_State* state) {
    auto* bridge = checked_bridge(state);
    constexpr const char* operation = "set_compositor_layer_mmf_source";
    const char* name = compositor_source_string(state, 2, false, operation);
    const char* mmf = lua_isnoneornil(state, 3)
                          ? "" : compositor_source_string(state, 3, true, operation);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_mmf_source(
        bridge->context, name, mmf);
    if (status != SAO_OK)
        return push_status_error(state, operation, status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_set_compositor_layer_shared_texture_source(lua_State* state) {
    static_assert(sizeof(lua_Integer) == sizeof(std::uint64_t));
    auto* bridge = checked_bridge(state);
    constexpr const char* operation = "set_compositor_layer_shared_texture_source";
    const char* name = compositor_source_string(state, 2, false, operation);
    if (!lua_isinteger(state, 3) || !lua_isinteger(state, 4) || !lua_isinteger(state, 5))
        return push_status_error(state, operation, SAO_ERR_INVALID_ARGUMENT);
    // Negative Lua integers preserve the high bit of an opaque uint64 handle.
    const auto handle = static_cast<std::uint64_t>(lua_tointeger(state, 3));
    const lua_Integer width_value = lua_tointeger(state, 4);
    const lua_Integer height_value = lua_tointeger(state, 5);
    if (width_value < 0 || height_value < 0 ||
        static_cast<std::uint64_t>(width_value) > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(height_value) > std::numeric_limits<std::uint32_t>::max())
        return push_status_error(state, operation, SAO_ERR_INVALID_ARGUMENT);
    const auto width = static_cast<std::uint32_t>(width_value);
    const auto height = static_cast<std::uint32_t>(height_value);
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_shared_texture_source(
            bridge->context, name, handle, width, height);
    if (status != SAO_OK)
        return push_status_error(state, operation, status);
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_compositor_gpu_interop_available(lua_State* state) {
    auto* bridge = checked_bridge(state);
    bool available = false;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_compositor_gpu_interop_available(
        bridge->context, &available);
    if (status != SAO_OK)
        return push_status_error(state, "compositor_gpu_interop_available", status);
    lua_pushboolean(state, available);
    return 1;
}

int ctx_compositor_layer_shared_texture_active(lua_State* state) {
    auto* bridge = checked_bridge(state);
    constexpr const char* operation = "compositor_layer_shared_texture_active";
    const char* name = compositor_source_string(state, 2, false, operation);
    bool active = false;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_compositor_layer_shared_texture_active(
            bridge->context, name, &active);
    if (status != SAO_OK)
        return push_status_error(state, operation, status);
    lua_pushboolean(state, active);
    return 1;
}

int ctx_set_compositor_layer_position(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* name = luaL_checkstring(state, 2);
    const auto x = static_cast<int32_t>(luaL_checkinteger(state, 3));
    const auto y = static_cast<int32_t>(luaL_checkinteger(state, 4));
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_position(
            bridge->context, name, x, y);
    if (status != SAO_OK) {
        return push_status_error(state, "set_compositor_layer_position", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_set_compositor_layer_visible(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* name = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TBOOLEAN);
    const bool visible = lua_toboolean(state, 3) != 0;
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
            bridge->context, name, visible);
    if (status != SAO_OK) {
        return push_status_error(state, "set_compositor_layer_visible", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

// set_compositor_layer_input(name, handlers|nil)
// handlers: {cursor_pos=fn, mouse_button=fn, cursor_leave=fn, scroll=fn} or the
// same four functions passed positionally.  nil clears the layer's callbacks.
int ctx_set_compositor_layer_input(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* name = luaL_checkstring(state, 2);
    if (name == nullptr || name[0] == '\0') {
        return push_status_error(state, "set_compositor_layer_input",
                                 SAO_ERR_INVALID_ARGUMENT);
    }
    if (lua_isnoneornil(state, 3)) {
        const int32_t status = release_compositor_input_locked(*bridge, state, name);
        if (status != SAO_OK) {
            return push_status_error(state, "set_compositor_layer_input", status);
        }
        const int32_t clear_status =
            sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
                bridge->context, name, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (clear_status != SAO_OK && clear_status != SAO_ERR_HANDLE_INVALID) {
            return push_status_error(state, "set_compositor_layer_input", clear_status);
        }
        lua_pushboolean(state, 1);
        return 1;
    }

    constexpr const char* kInputFields[4] = {"cursor_pos", "mouse_button",
                                             "cursor_leave", "scroll"};
    auto dispatch = std::make_shared<compositor_input_dispatch>();
    dispatch->key = name;
    dispatch->sequence = bridge->next_resource_sequence++;
    std::unordered_map<uintptr_t, std::shared_ptr<event_callback_record>> staged_records;
    const bool table_form = lua_istable(state, 3);
    for (std::size_t slot = 0; slot < 4; ++slot) {
        if (table_form) {
            raw_get_field(state, 3, kInputFields[slot]);
        } else {
            lua_pushvalue(state, static_cast<int>(3 + slot));
        }
        const int value_index = lua_absindex(state, -1);
        if (lua_isnoneornil(state, value_index)) {
            lua_pop(state, 1);
            continue;
        }
        if (!lua_isfunction(state, value_index)) {
            lua_pop(state, 1);
            for (const auto& [_, record] : staged_records) {
                release_callback_ref(state, *record);
            }
            return push_status_error(state, "set_compositor_layer_input handlers",
                                     SAO_ERR_INVALID_ARGUMENT);
        }
        auto record = make_callback(*bridge, state, value_index);
        lua_pop(state, 1);
        dispatch->record_ids[slot] = record->dispatch_id;
        staged_records.emplace(record->dispatch_id, record);
    }
    if (staged_records.empty()) {
        return push_status_error(state, "set_compositor_layer_input",
                                 SAO_ERR_INVALID_ARGUMENT);
    }
    // Swap out any existing dispatch first so a BUSY old record fails before
    // the native registration changes hands.
    const int32_t release_status =
        release_compositor_input_locked(*bridge, state, name);
    if (release_status != SAO_OK) {
        for (const auto& [_, record] : staged_records) {
            release_callback_ref(state, *record);
        }
        return push_status_error(state, "set_compositor_layer_input", release_status);
    }
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
            bridge->context, name, compositor_cursor_pos_dispatch,
            compositor_mouse_button_dispatch, compositor_cursor_leave_dispatch,
            compositor_scroll_dispatch, dispatch.get());
    if (status != SAO_OK) {
        for (const auto& [_, record] : staged_records) {
            release_callback_ref(state, *record);
        }
        return push_status_error(state, "set_compositor_layer_input", status);
    }
    try {
        bridge->compositor_records.insert(staged_records.begin(),
                                          staged_records.end());
        bridge->compositor_inputs.emplace(name, std::move(dispatch));
    } catch (...) {
        for (const auto& [_, record] : staged_records) {
            bridge->compositor_records.erase(record->dispatch_id);
            release_callback_ref(state, *record);
        }
        throw;
    }
    lua_pushboolean(state, 1);
    return 1;
}

int ctx_destroy_compositor_layer(lua_State* state) {
    auto* bridge = checked_bridge(state);
    const char* name = luaL_checkstring(state, 2);
    const int32_t release_status =
        release_compositor_input_locked(*bridge, state, name == nullptr ? "" : name);
    if (release_status != SAO_OK) {
        return push_status_error(state, "destroy_compositor_layer", release_status);
    }
    const int32_t status =
        sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(bridge->context,
                                                                       name);
    if (status != SAO_OK) {
        return push_status_error(state, "destroy_compositor_layer", status);
    }
    lua_pushboolean(state, 1);
    return 1;
}

// ── ctx.ui.* dispatch (single C closure per method via upvalue) ──────────
int ctx_ui_dispatch(lua_State* state) noexcept {
    const char* method = lua_tostring(state, lua_upvalueindex(1));
    const int top = lua_gettop(state);
    int first = 1;
    // colon-call `ctx.ui:text(...)` puts the ui table at arg1.
    if (top >= 1 && lua_rawequal(state, 1, lua_upvalueindex(2))) {
        first = 2;
    }
    detail::json args;
    std::string error;
    const int count = top - first + 1;
    if (count == 1 && lua_istable(state, first)) {
        // One dict-like table reads as kwargs; an array-like table stays a
        // single positional argument.
        detail::json converted;
        if (!detail::stack_to_json(state, first, converted, error)) {
            return luaL_error(state, "ctx.ui.%s argument is not JSON serializable: %s",
                              method == nullptr ? "?" : method, error.c_str());
        }
        args = converted.is_object() ? std::move(converted)
                                   : detail::json::array({std::move(converted)});
    } else {
        args = detail::json::array();
        for (int index = first; index <= top; ++index) {
            detail::json value;
            if (!detail::stack_to_json(state, index, value, error)) {
                return luaL_error(state,
                                  "ctx.ui.%s argument is not JSON serializable: %s",
                                  method == nullptr ? "?" : method, error.c_str());
            }
            args.push_back(std::move(value));
        }
    }
    detail::json node;
    std::string build_error;
    if (!sao::plugins::script_ctx::script_ui_build(
            method == nullptr ? "" : method, args, node, build_error)) {
        return luaL_error(state, "ctx.ui.%s failed: %s",
                          method == nullptr ? "?" : method, build_error.c_str());
    }
    if (!detail::protected_push_json(state, node, error)) {
        return luaL_error(state, "ctx.ui.%s result is not representable in Lua",
                          method == nullptr ? "?" : method);
    }
    return 1;
}

// ── ctx.engine reflective surface ────────────────────────────────────────
// Named functions bound from the sdk_binding engine catalog.  Each call is a
// sdk_method_id::method_engine_call JSON request against the bridge-owned
// SaoSdkContext; a nonzero transport or envelope status raises
// "engine call <name> failed: <status>".

std::string engine_lua_name(std::string_view name) {
    std::string result(name);
    std::replace(result.begin(), result.end(), '.', '_');
    return result;
}

struct engine_call_failure final {
    std::string message;
};

[[noreturn]] void raise_engine_call_failed(const char* name, int32_t status) {
    throw engine_call_failure{std::string("engine call ") +
                              (name == nullptr ? "?" : name) +
                              " failed: " + std::to_string(status)};
}

// sdk_context_engine_callback_fn trampoline: user_data is the bridge_state*
// (stable while the bridge object lives, live or retired).  Channel names are
// resolved through bridge->engine_callbacks under the dedicated mutex; the
// record's gate/state checks then mirror event_callback.
void SAO_PLUGINS_CALL engine_channel_dispatch(const char* channel_utf8,
                                              const uint8_t* payload_json_utf8,
                                              size_t payload_size,
                                              void* user_data) noexcept {
    try {
        auto* bridge = static_cast<bridge_state*>(user_data);
        if (bridge == nullptr || channel_utf8 == nullptr || channel_utf8[0] == '\0')
            return;
        std::shared_ptr<event_callback_record> record;
        {
            // Short critical section: provider threads only copy the ref here,
            // so sdk_ctx teardown never blocks behind an in-flight emission.
            std::lock_guard lock(bridge->engine_callbacks_mutex);
            const auto found = bridge->engine_callbacks.find(channel_utf8);
            if (found == bridge->engine_callbacks.end())
                return;
            record = found->second;
        }
        if (record == nullptr || !enter_callback(*record))
            return;
        struct callback_guard final {
            event_callback_record& callback;
            ~callback_guard() {
                leave_callback(callback);
            }
        } guard{*record};
        lua_State* callback_state = record->state.load(std::memory_order_acquire);
        if (callback_state == nullptr)
            return;
        detail::state_operation operation;
        if (detail::acquire_state_operation(callback_state, operation) != SAO_OK)
            return;
        if (record->closing.load(std::memory_order_acquire) ||
            record->state.load(std::memory_order_acquire) != callback_state ||
            record->function_ref == LUA_NOREF) {
            return;
        }
        callback_state = operation.state();
        if (callback_state == nullptr)
            return;
        const int base = lua_gettop(callback_state);
        lua_rawgeti(callback_state, LUA_REGISTRYINDEX, record->function_ref);
        if (!lua_isfunction(callback_state, -1)) {
            lua_settop(callback_state, base);
            return;
        }
        lua_pushstring(callback_state, channel_utf8);
        detail::json payload;
        std::string conversion_error;
        const char* payload_data = payload_json_utf8 == nullptr
                                       ? "null"
                                       : reinterpret_cast<const char*>(payload_json_utf8);
        const std::size_t payload_length =
            payload_json_utf8 == nullptr ? 4 : payload_size;
        if (!detail::parse_json(payload_data, payload_length, payload, conversion_error) ||
            !detail::protected_push_json(callback_state, payload, conversion_error, true)) {
            lua_settop(callback_state, base);
            return;
        }
        if (lua_pcall(callback_state, 2, 0, 0) != LUA_OK) {
            detail::capture_state_error_locked(callback_state, -1);
        }
        lua_settop(callback_state, base);
    } catch (...) {
    }
}

// Serialize `request_json`, run the two-phase method_engine_call /
// method_engine_list dispatch, and decode the {"status","result"} envelope.
// Returns the `result` member; raises engine_call_failure on any nonzero
// transport or envelope status and method_failure on decode errors.
detail::json engine_run_call(
    bridge_state* bridge, sao::plugins::sdk_binding::sdk_method_id method,
    const char* display_name, detail::json&& request_json) {
    if (bridge == nullptr || bridge->closing || bridge->context == nullptr) {
        raise_engine_call_failed(display_name, SAO_ERR_HANDLE_INVALID);
    }
    if (!bridge->sdk_ctx_bound) {
        // Lazy bind (csmini-style): register_ctx can run before the SDK
        // runtime/plugin map entry is ready, so retry here at first use.
        // sdk_ctx writes race-safe because engine calls serialize on the
        // script mutex held by the caller's lifecycle slot.
        try {
            const char* plugin_id =
                sao::plugins::loader::sao_plugins_ctx_plugin_id(bridge->context);
            if (plugin_id != nullptr && plugin_id[0] != '\0' &&
                sao_sdk_bind_context(plugin_id, nullptr, &bridge->sdk_ctx) == SAO_SDK_OK) {
                bridge->sdk_ctx_bound = true;
                if (sao_sdk_context_bind_platform_services(&bridge->sdk_ctx) != SAO_SDK_OK) {
                    sao_sdk_context_destroy(&bridge->sdk_ctx);
                    bridge->sdk_ctx = SaoSdkContext{};
                    bridge->sdk_ctx_bound = false;
                }
            }
        } catch (...) {
            bridge->sdk_ctx = SaoSdkContext{};
            bridge->sdk_ctx_bound = false;
        }
        if (!bridge->sdk_ctx_bound) {
            raise_engine_call_failed(display_name, SAO_ERR_HANDLE_INVALID);
        }
    }
    std::string serialized;
    std::string error;
    if (!detail::serialize_json(request_json, serialized, error)) {
        raise_engine_call_failed(display_name, SAO_ERR_INVALID_ARGUMENT);
    }
    std::vector<char> buffer;
    std::size_t required = 0;
    int32_t status = SAO_OK;
    for (std::size_t attempt = 0; attempt < 8; ++attempt) {
        sao::plugins::sdk_binding::sdk_context_call_request request{};
        request.args_json_utf8 = serialized.c_str();
        request.args_size = serialized.size();
        // Callback-capable slots share this generic channel trampoline; the
        // user_data anchor is the bridge and channel records sit in
        // bridge->engine_callbacks keyed by channel name.
        request.engine_callback = &engine_channel_dispatch;
        request.callback_user_data = bridge;
        request.out_result_json_utf8 = buffer.empty() ? nullptr : buffer.data();
        request.out_capacity = buffer.size();
        required = 0;
        request.out_required = &required;
        status = sao::plugins::sdk_binding::sao_plugins_sdk_context_dispatch(
            &bridge->sdk_ctx, method, &request);
        if (status == SAO_OK) {
            if (buffer.empty() && required != 0 &&
                required <= sao::plugins::sdk_binding::kMaximumBindingJsonBytes) {
                buffer.assign(required, '\0');
                continue;  // probe pass completed — run the fill pass
            }
            break;
        }
        if (status == SAO_ERR_BUFFER_TOO_SMALL && required > buffer.size() &&
            required <= sao::plugins::sdk_binding::kMaximumBindingJsonBytes) {
            buffer.assign(required, '\0');
            continue;
        }
        break;
    }
    if (status != SAO_OK) {
        raise_engine_call_failed(display_name, status);
    }
    detail::json envelope;
    const std::size_t length =
        buffer.empty()
            ? 0
            : (required != 0 && required <= buffer.size() ? required - 1
                                                          : std::strlen(buffer.data()));
    if (length == 0 ||
        !detail::parse_json(buffer.data(), length, envelope, error)) {
        throw method_failure{"engine call result", SAO_ERR_INVALID_ARGUMENT};
    }
    int64_t inner_status = 0;
    if (envelope.is_object()) {
        const auto found_status = envelope.find("status");
        if (found_status != envelope.end() && found_status->is_number()) {
            inner_status = found_status->get<int64_t>();
        }
    }
    if (inner_status != 0) {
        raise_engine_call_failed(display_name, static_cast<int32_t>(inner_status));
    }
    if (envelope.is_object()) {
        const auto found_result = envelope.find("result");
        if (found_result != envelope.end()) {
            return *found_result;
        }
    }
    return detail::json(nullptr);
}

int engine_push_result(lua_State* state, bridge_state* bridge,
                       sao::plugins::sdk_binding::sdk_method_id method,
                       const char* display_name, detail::json&& request_json) {
    detail::json result =
        engine_run_call(bridge, method, display_name, std::move(request_json));
    std::string push_error;
    if (!detail::protected_push_json(state, result, push_error, true)) {
        throw method_failure{"engine call result", SAO_ERR_INVALID_ARGUMENT};
    }
    return 1;
}

// Fallback key for a catalog entry whose arg_names slot is null.
std::string engine_arg_key(const sao::plugins::sdk_binding::sdk_engine_function_desc* desc,
                           std::size_t slot) {
    const char* name =
        desc->arg_names != nullptr && slot < desc->arg_count ? desc->arg_names[slot] : nullptr;
    if (name != nullptr && name[0] != '\0')
        return std::string(name);
    return "arg" + std::to_string(slot + 1);
}

// Convert the lua argument list into the engine "args" object: a single
// dict-like table is kwargs, an array-like table or the remaining values are
// positional and map onto desc->arg_names in order.
detail::json engine_collect_args(lua_State* state, int first, int top,
                                 const sao::plugins::sdk_binding::sdk_engine_function_desc* desc) {
    const int count = top - first + 1;
    detail::json args = detail::json::object();
    std::string error;
    if (count == 1 && lua_istable(state, first)) {
        detail::json converted;
        if (!detail::stack_to_json(state, first, converted, error)) {
            throw method_failure{"engine call arguments", SAO_ERR_INVALID_ARGUMENT};
        }
        if (converted.is_object()) {
            return converted;
        }
        if (converted.is_array()) {
            if (converted.size() > desc->arg_count) {
                throw method_failure{"engine call arguments", SAO_ERR_INVALID_ARGUMENT};
            }
            std::size_t index = 0;
            for (const auto& element : converted) {
                args[engine_arg_key(desc, index)] = element;
                ++index;
            }
            return args;
        }
        return args;
    }
    for (int index = first; index <= top; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index - first);
        if (slot >= desc->arg_count) {
            throw method_failure{"engine call arguments", SAO_ERR_INVALID_ARGUMENT};
        }
        detail::json value;
        if (!detail::stack_to_json(state, index, value, error)) {
            throw method_failure{"engine call arguments", SAO_ERR_INVALID_ARGUMENT};
        }
        args[engine_arg_key(desc, slot)] = std::move(value);
    }
    return args;
}

// Wraps the {"name","args"} envelope; a kwargs-style "callback_channel" is
// also hoisted to the top-level protocol field.
int engine_call_push_result(lua_State* state, bridge_state* bridge, const char* name,
                            detail::json&& args) {
    detail::json request_json = detail::json::object();
    request_json["name"] = name == nullptr ? "" : name;
    if (args.is_object()) {
        const auto found_channel = args.find("callback_channel");
        if (found_channel != args.end() && found_channel->is_string()) {
            request_json["callback_channel"] = *found_channel;
        }
    }
    request_json["args"] = std::move(args);
    return engine_push_result(
        state, bridge, sao::plugins::sdk_binding::sdk_method_id::method_engine_call, name,
        std::move(request_json));
}

int engine_invoke_named_impl(lua_State* state) {
    auto* desc = static_cast<const sao::plugins::sdk_binding::sdk_engine_function_desc*>(
        lua_touserdata(state, lua_upvalueindex(1)));
    auto* bridge =
        static_cast<bridge_state*>(lua_touserdata(state, lua_upvalueindex(2)));
    if (desc == nullptr || desc->name == nullptr || bridge == nullptr) {
        return push_status_error(state, "engine call", SAO_ERR_HANDLE_INVALID);
    }
    require_permission(state, detail::permission_engine_access, desc->name);
    const int top = lua_gettop(state);
    int first = 1;
    // colon-call `ctx.engine:name(...)` puts the engine table at arg1.
    if (top >= 1 && lua_rawequal(state, 1, lua_upvalueindex(3))) {
        first = 2;
    }
    detail::json args = engine_collect_args(state, first, top, desc);
    return engine_call_push_result(state, bridge, desc->name, std::move(args));
}

int engine_invoke_named(lua_State* state) noexcept {
    try {
        return engine_invoke_named_impl(state);
    } catch (const engine_call_failure& failure) {
        return luaL_error(state, "%s", failure.message.c_str());
    } catch (const method_failure& failure) {
        return luaL_error(state, "%s failed with status %d", failure.operation,
                          failure.status);
    } catch (...) {
        return luaL_error(state, "engine call failed with status %d",
                          static_cast<int>(SAO_ERR_OS_CALL_FAILED));
    }
}

int engine_list_impl(lua_State* state) {
    auto* bridge =
        static_cast<bridge_state*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (bridge == nullptr) {
        return push_status_error(state, "engine list", SAO_ERR_HANDLE_INVALID);
    }
    require_permission(state, detail::permission_engine_access, "engine list");
    return engine_push_result(state, bridge,
                              sao::plugins::sdk_binding::sdk_method_id::method_engine_list,
                              "engine list", detail::json::object());
}

int engine_list_fn(lua_State* state) noexcept {
    try {
        return engine_list_impl(state);
    } catch (const engine_call_failure& failure) {
        return luaL_error(state, "%s", failure.message.c_str());
    } catch (const method_failure& failure) {
        return luaL_error(state, "%s failed with status %d", failure.operation,
                          failure.status);
    } catch (...) {
        return luaL_error(state, "engine list failed with status %d",
                          static_cast<int>(SAO_ERR_OS_CALL_FAILED));
    }
}

int engine_on_impl(lua_State* state) {
    auto* bridge =
        static_cast<bridge_state*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (bridge == nullptr || bridge->closing || bridge->context == nullptr) {
        return push_status_error(state, "engine on", SAO_ERR_HANDLE_INVALID);
    }
    require_permission(state, detail::permission_engine_access, "engine on");
    int index = 1;
    // colon-call `ctx.engine:on(channel, fn)` puts the engine table at arg1.
    if (lua_gettop(state) >= 1 && lua_rawequal(state, 1, lua_upvalueindex(2))) {
        index = 2;
    }
    const char* channel = luaL_checkstring(state, index);
    if (channel == nullptr || channel[0] == '\0') {
        return push_status_error(state, "engine on", SAO_ERR_INVALID_ARGUMENT);
    }
    luaL_checktype(state, index + 1, LUA_TFUNCTION);
    auto record = make_callback(*bridge, state, index + 1);
    record->key = channel;
    std::shared_ptr<event_callback_record> replaced;
    {
        std::lock_guard lock(bridge->engine_callbacks_mutex);
        const auto found = bridge->engine_callbacks.find(record->key);
        if (found != bridge->engine_callbacks.end()) {
            replaced = found->second;
            bridge->engine_callbacks.erase(found);
        }
        bridge->engine_callbacks.emplace(record->key, record);
    }
    if (replaced != nullptr) {
        // The map entry is already replaced; a busy old record restores it and
        // drops the new registration instead of killing a live channel.
        const int32_t stop = stop_callback(*replaced);
        if (stop != SAO_OK) {
            {
                std::lock_guard lock(bridge->engine_callbacks_mutex);
                bridge->engine_callbacks[record->key] = replaced;
            }
            release_callback_ref(state, *record);
            return push_status_error(state, "engine on", stop);
        }
        release_callback_ref(state, *replaced);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int engine_on_fn(lua_State* state) noexcept {
    try {
        return engine_on_impl(state);
    } catch (const method_failure& failure) {
        return luaL_error(state, "%s failed with status %d", failure.operation,
                          failure.status);
    } catch (...) {
        return luaL_error(state, "engine on failed with status %d",
                          static_cast<int>(SAO_ERR_OS_CALL_FAILED));
    }
}

// ctx.engine_call(name, args_table_or_nil) — raw passthrough for callers that
// prefer the dotted name over the bound function.
int ctx_engine_call(lua_State* state) {
    auto* bridge = checked_bridge(state);
    require_permission(state, detail::permission_engine_access, "engine_call");
    const char* name = luaL_checkstring(state, 2);
    if (name == nullptr || name[0] == '\0') {
        return push_status_error(state, "engine_call", SAO_ERR_INVALID_ARGUMENT);
    }
    detail::json args = detail::json::object();
    if (!lua_isnoneornil(state, 3)) {
        if (!lua_istable(state, 3)) {
            return push_status_error(state, "engine_call", SAO_ERR_INVALID_ARGUMENT);
        }
        std::string error;
        detail::json converted;
        if (!detail::stack_to_json(state, 3, converted, error) ||
            !converted.is_object()) {
            return push_status_error(state, "engine_call", SAO_ERR_INVALID_ARGUMENT);
        }
        args = std::move(converted);
    }
    return engine_call_push_result(state, bridge, name, std::move(args));
}

int ctx_engine_call_wrapped(lua_State* state) noexcept {
    try {
        return ctx_engine_call(state);
    } catch (const engine_call_failure& failure) {
        return luaL_error(state, "%s", failure.message.c_str());
    } catch (const method_failure& failure) {
        return luaL_error(state, "%s failed with status %d", failure.operation,
                          failure.status);
    } catch (...) {
        return luaL_error(state, "engine_call failed with status %d",
                          static_cast<int>(SAO_ERR_OS_CALL_FAILED));
    }
}

// __index metamethod: methods table first, then live properties
// (plugin_id / path / should_stop).
int ctx_index(lua_State* state) noexcept {
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (!lua_isnil(state, -1))
        return 1;
    lua_pop(state, 2);
    const char* key = lua_tostring(state, 2);
    auto* bridge = static_cast<bridge_state*>(nullptr);
    if (key != nullptr && lua_isuserdata(state, 1)) {
        auto** slot = static_cast<bridge_state**>(
            luaL_testudata(state, 1, "SaoPluginContext"));
        if (slot != nullptr)
            bridge = *slot;
    }
    if (bridge == nullptr || bridge->context == nullptr || key == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (std::strcmp(key, "plugin_id") == 0) {
        const char* value =
            sao::plugins::loader::sao_plugins_ctx_plugin_id(bridge->context);
        if (value == nullptr) {
            lua_pushnil(state);
        } else {
            lua_pushstring(state, value);
        }
    } else if (std::strcmp(key, "path") == 0) {
        const wchar_t* value =
            sao::plugins::loader::sao_plugins_ctx_path(bridge->context);
        const std::string utf8 = wide_to_utf8(value);
        lua_pushlstring(state, utf8.data(), utf8.size());
    } else if (std::strcmp(key, "should_stop") == 0) {
        lua_pushboolean(
            state,
            sao::plugins::loader::sao_plugins_ctx_should_stop(bridge->context) ? 1 : 0);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

// Names noted for ctx_surface (canonical Python surface spelling).
const char* const kCtxSurfaceNames[] = {
    "log", "time", "set_defaults", "get_setting", "setting", "set_setting",
    "snapshot_value", "register_ui_panel", "register_menu_category",
    "register_menu_surface", "register_action_handler", "subscribe",
    "subscribe_once", "unsubscribe", "emit", "publish", "get_snapshot",
    "recent_events", "register_render_hook", "unregister_render_hook",
    "set_overlay", "clear_overlay", "request_redraw", "register_hotkey",
    "unregister_hotkey", "set_interval", "set_timeout", "clear_timer", "notify",
    "toast", "dismiss_notify", "register_engine", "get_engine", "open_file",
    "open_window", "load_local", "ensure_requirements",
    "create_compositor_layer", "upload_compositor_frame",
    "set_compositor_layer_mmf_source", "set_compositor_layer_shared_texture_source",
    "compositor_gpu_interop_available", "compositor_layer_shared_texture_active",
    "set_compositor_layer_position", "set_compositor_layer_visible",
    "set_compositor_layer_input", "destroy_compositor_layer", "engine",
    "engine_call", "engine.list", "engine.on", "plugin_id",
    "path", "should_stop", "ui", "ui.badge", "ui.bar", "ui.button", "ui.canvas",
    "ui.card", "ui.ctext", "ui.divider", "ui.group", "ui.input", "ui.kv",
    "ui.line", "ui.oval", "ui.panel", "ui.rect", "ui.rgba_frame", "ui.row",
    "ui.section", "ui.spacer", "ui.slider", "ui.table", "ui.text", "ui.title",
    nullptr,
};

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

// Legacy compat: scripts written for the Python-style surface call
// ctx.name(args) (dot call) while the C methods expect ctx:name(...)
// (self as arg 1).  Wrap every method so that when arg 1 is not the
// SaoPluginContext userdata the bound global ctx is injected first.
int ctx_method_dispatch(lua_State* state) {
    if (luaL_testudata(state, 1, "SaoPluginContext") == nullptr) {
        lua_getglobal(state, "ctx");
        if (luaL_testudata(state, -1, "SaoPluginContext") == nullptr) {
            lua_pop(state, 1);
            return luaL_error(state, "ctx method called without a plugin context");
        }
        lua_insert(state, 1);
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
    return lua_gettop(state);
}

void set_method(lua_State* state, const char* name, lua_CFunction function) {
    lua_pushcfunction(state, function);
    lua_pushcclosure(state, ctx_method_dispatch, 1);
    lua_setfield(state, -2, name);
}

int register_ctx_body(lua_State* state) {
    auto* bridge = static_cast<bridge_state*>(lua_touserdata(state, 1));
    if (bridge == nullptr)
        return luaL_error(state, "invalid ctx bridge");
    sao::plugins::script_ctx::ctx_surface_note_all(loader::engine_kind::lua,
                                                 kCtxSurfaceNames);
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
        set_method(state, "register_menu_surface", safe_method<ctx_register_menu_surface>);
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
        set_method(state, "open_file", safe_method<ctx_open_file>);
        set_method(state, "open_window", safe_method<ctx_open_window>);
        set_method(state, "load_local", safe_method<ctx_load_local>);
        set_method(state, "ensure_requirements", safe_method<ctx_ensure_requirements>);
        set_method(state, "create_compositor_layer",
                   safe_method<ctx_create_compositor_layer>);
        set_method(state, "upload_compositor_frame",
                   safe_method<ctx_upload_compositor_frame>);
        set_method(state, "set_compositor_layer_mmf_source",
                 safe_method<ctx_set_compositor_layer_mmf_source>);
        set_method(state, "set_compositor_layer_shared_texture_source",
                 safe_method<ctx_set_compositor_layer_shared_texture_source>);
        set_method(state, "compositor_gpu_interop_available",
                 safe_method<ctx_compositor_gpu_interop_available>);
        set_method(state, "compositor_layer_shared_texture_active",
                 safe_method<ctx_compositor_layer_shared_texture_active>);
        set_method(state, "set_compositor_layer_position",
                   safe_method<ctx_set_compositor_layer_position>);
        set_method(state, "set_compositor_layer_visible",
                   safe_method<ctx_set_compositor_layer_visible>);
        set_method(state, "set_compositor_layer_input",
                   safe_method<ctx_set_compositor_layer_input>);
        set_method(state, "destroy_compositor_layer",
                   safe_method<ctx_destroy_compositor_layer>);
        set_method(state, "engine_call", ctx_engine_call_wrapped);
        // ctx.engine — named functions bound from the FULL reflective engine
        // catalog (sdk_engine_catalog_at); '.' maps to '_'
        // ("mem.read_u64" → mem_read_u64).  Availability probing stays lazy:
        // the dispatch resolves it per call so missing providers fail closed
        // instead of hiding catalog entries at bind time.
        lua_newtable(state);
        const int engine_table = lua_absindex(state, -1);
        const std::size_t engine_count =
            sao::plugins::sdk_binding::sdk_engine_catalog_size();
        for (std::size_t index = 0; index < engine_count; ++index) {
            const auto* desc = sao::plugins::sdk_binding::sdk_engine_catalog_at(index);
            if (desc == nullptr || desc->name == nullptr)
                continue;
            lua_pushlightuserdata(
                state, const_cast<sao::plugins::sdk_binding::sdk_engine_function_desc*>(desc));
            lua_pushlightuserdata(state, bridge);
            lua_pushvalue(state, engine_table);
            lua_pushcclosure(state, engine_invoke_named, 3);
            lua_setfield(state, engine_table, engine_lua_name(desc->name).c_str());
        }
        lua_pushlightuserdata(state, bridge);
        lua_pushvalue(state, engine_table);
        lua_pushcclosure(state, engine_list_fn, 2);
        lua_setfield(state, engine_table, "list");
        lua_pushlightuserdata(state, bridge);
        lua_pushvalue(state, engine_table);
        lua_pushcclosure(state, engine_on_fn, 2);
        lua_setfield(state, engine_table, "on");
        lua_setfield(state, -2, "engine");
        // ctx.ui — sub-table of spec builders served by script_ui_build.
        lua_newtable(state);
        const int ui_table = lua_absindex(state, -1);
        std::size_t ui_count = 0;
        for (const char* const* name =
                 sao::plugins::script_ctx::script_ui_methods(&ui_count);
             ui_count != 0 && name != nullptr && *name != nullptr;
             ++name, --ui_count) {
            lua_pushstring(state, *name);
            lua_pushvalue(state, ui_table);
            lua_pushcclosure(state, ctx_ui_dispatch, 2);
            lua_setfield(state, ui_table, *name);
        }
        lua_setfield(state, -2, "ui");
        // __index = ctx_index(methods) — live props resolve through it.
        lua_pushvalue(state, -1);
        lua_pushcclosure(state, ctx_index, 1);
        lua_setfield(state, -3, "__index");
        lua_pushliteral(state, "locked");
        lua_setfield(state, -3, "__metatable");
        lua_pop(state, 1);
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
        for (const auto& [_, panel] : found->second->panels) {
            if (panel->pending_enable) {
                panel->pending_enable = false;
                panel->enable_scoped = true;
            }
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
        const int32_t panel_status = release_panel_callbacks(*bridge, state, true, false);
        if (panel_status != SAO_OK)
            return panel_status;
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
            menu->navigation = {};
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
        const int32_t panel_status = release_panel_callbacks(*bridge, state, false, true);
        if (panel_status != SAO_OK)
            return panel_status;
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
            menu->navigation = {};
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
        const int32_t panel_status = release_panel_callbacks(*bridge, state, false, false);
        if (panel_status != SAO_OK)
            return panel_status;
        // Compositor input dispatches hold one shared user_data each; drop
        // them before the loader frees the ctx's layer resources and retire
        // their callbacks with the rest of the bridge records.
        for (const auto& [layer_name, dispatch] : bridge->compositor_inputs) {
            dispatch->closing.store(true, std::memory_order_release);
        }
        bridge->retired_compositor_dispatches.reserve(
            bridge->retired_compositor_dispatches.size() +
            bridge->compositor_inputs.size());
        for (const auto& [layer_name, dispatch] : bridge->compositor_inputs) {
            bridge->retired_compositor_dispatches.push_back(dispatch);
        }
        bridge->compositor_inputs.clear();
        std::vector<std::shared_ptr<event_callback_record>> callbacks;
        callbacks.reserve(bridge->callbacks.size() + bridge->hotkeys.size() +
                          bridge->timers.size() + bridge->render_hooks.size() +
                          bridge->compositor_records.size() +
                          bridge->engine_callbacks.size());
        for (const auto& [_, callback] : bridge->callbacks) {
            callbacks.push_back(callback);
        }
        for (const auto& [_, callback] : bridge->compositor_records) {
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
        {
            // Engine channel records are pure lua callbacks — they join the
            // stop/release waves but never the loader resource unwind.
            std::lock_guard engine_lock(bridge->engine_callbacks_mutex);
            for (const auto& [_, callback] : bridge->engine_callbacks) {
                callbacks.push_back(callback);
            }
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
        bridge->compositor_records.clear();
        bridge->passive_resources.clear();
        {
            // Emissions in flight past this point find an empty channel map
            // and drop without taking the state lock.
            std::lock_guard engine_lock(bridge->engine_callbacks_mutex);
            bridge->engine_callbacks.clear();
        }
        // The bridge-owned SaoSdkContext dies before the loader context:
        // engine callback records are already stopped and released, so a
        // provider drain inside destroy cannot reach a live channel.
        if (bridge->sdk_ctx_bound) {
            sao_sdk_context_destroy(&bridge->sdk_ctx);
            bridge->sdk_ctx = SaoSdkContext{};
            bridge->sdk_ctx_bound = false;
        }
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
            menu->navigation = {};
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
        if (bridge->context != nullptr) {
            std::lock_guard ctx_lock(g_ctx_state_mutex);
            const auto mapped = g_ctx_state_map.find(bridge->context);
            if (mapped != g_ctx_state_map.end() && mapped->second == state)
                g_ctx_state_map.erase(mapped);
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

int32_t register_runtime_bridge_provider() noexcept {
    return sao::plugins::script_ctx::runtime_bridge_register(&kLuaEngineOps);
}

int32_t unregister_runtime_bridge_provider() noexcept {
    return sao::plugins::script_ctx::runtime_bridge_unregister(&kLuaEngineOps);
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
        {
            std::lock_guard ctx_lock(g_ctx_state_mutex);
            g_ctx_state_map[context] = state;
        }
        context_retained = false;
        // Bridge-owned SaoSdkContext for the ctx.engine reflective surface.
        // Binding is non-fatal: engine calls fail closed per call site when
        // the context stays unbound.  The inner try/catch keeps any provider-
        // side throw from failing the whole ctx registration (the script would
        // then die in load_script before on_load ever runs).
        try {
            const char* plugin_id =
                sao::plugins::loader::sao_plugins_ctx_plugin_id(context);
            if (plugin_id != nullptr && plugin_id[0] != '\0' &&
                sao_sdk_bind_context(plugin_id, nullptr, &bridge_ptr->sdk_ctx) == SAO_SDK_OK) {
                bridge_ptr->sdk_ctx_bound = true;
                if (sao_sdk_context_bind_platform_services(&bridge_ptr->sdk_ctx) != SAO_SDK_OK) {
                    sao_sdk_context_destroy(&bridge_ptr->sdk_ctx);
                    bridge_ptr->sdk_ctx = SaoSdkContext{};
                    bridge_ptr->sdk_ctx_bound = false;
                }
            }
        } catch (...) {
            bridge_ptr->sdk_ctx = SaoSdkContext{};
            bridge_ptr->sdk_ctx_bound = false;
        }
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
