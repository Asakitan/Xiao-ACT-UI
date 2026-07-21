#pragma once

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lua_state_internal.h"

namespace sao::plugins::lua_host::detail {

using json = nlohmann::json;

inline constexpr std::size_t kMaximumJsonNodes = 16384;
inline constexpr std::size_t kMaximumJsonStringBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumJsonTotalStringBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumJsonOutputBytes = 8U * 1024U * 1024U;
inline constexpr int kMaximumJsonNestingDepth = 64;
inline unsigned char kJsonNullSentinel = 0;

struct json_budget final {
    std::size_t nodes = 0;
    std::size_t string_bytes = 0;
};

inline bool valid_utf8(std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char lead = bytes[index];
        if (lead <= 0x7f) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            count = 2;
            codepoint = lead & 0x1fU;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            count = 3;
            codepoint = lead & 0x0fU;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 4;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (count > value.size() - index)
            return false;
        for (std::size_t offset = 1; offset < count; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((count == 3 && codepoint < 0x800U) || (count == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
            return false;
        }
        index += count;
    }
    return true;
}

inline bool consume_json_node(json_budget& budget, std::string& error) {
    if (budget.nodes >= kMaximumJsonNodes) {
        error = "JSON value exceeds its node budget";
        return false;
    }
    ++budget.nodes;
    return true;
}

inline bool consume_json_string(std::string_view value, json_budget& budget, std::string& error) {
    if (!valid_utf8(value)) {
        error = "JSON strings must be valid UTF-8";
        return false;
    }
    if (value.size() > kMaximumJsonStringBytes ||
        budget.string_bytes > kMaximumJsonTotalStringBytes - value.size()) {
        error = "JSON value exceeds its string budget";
        return false;
    }
    budget.string_bytes += value.size();
    return true;
}

inline bool push_json(lua_State* state, const json& value, json_budget& budget, std::string& error,
                      int depth = 0, bool preserve_json_null = false) {
    if (depth > kMaximumJsonNestingDepth) {
        error = "JSON value nesting exceeds 64 levels";
        return false;
    }
    if (!consume_json_node(budget, error))
        return false;
    if (value.is_null()) {
        if (preserve_json_null) {
            lua_pushlightuserdata(state, &kJsonNullSentinel);
        } else {
            lua_pushnil(state);
        }
    } else if (value.is_boolean()) {
        lua_pushboolean(state, value.get<bool>() ? 1 : 0);
    } else if (value.is_number_integer()) {
        lua_pushinteger(state, static_cast<lua_Integer>(value.get<int64_t>()));
    } else if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number <= static_cast<uint64_t>(LUA_MAXINTEGER)) {
            lua_pushinteger(state, static_cast<lua_Integer>(number));
        } else {
            lua_pushnumber(state, static_cast<lua_Number>(number));
        }
    } else if (value.is_number_float()) {
        lua_pushnumber(state, static_cast<lua_Number>(value.get<double>()));
    } else if (value.is_string()) {
        const auto& string_value = value.get_ref<const std::string&>();
        if (!consume_json_string(string_value, budget, error))
            return false;
        lua_pushlstring(state, string_value.data(), string_value.size());
    } else if (value.is_array()) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "JSON array is too large for Lua";
            return false;
        }
        lua_createtable(state, static_cast<int>(value.size()), 0);
        for (size_t index = 0; index < value.size(); ++index) {
            if (!push_json(state, value[index], budget, error, depth + 1,
                           preserve_json_null)) {
                lua_pop(state, 1);
                return false;
            }
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
    } else if (value.is_object()) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "JSON object is too large for Lua";
            return false;
        }
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (const auto& [key, child] : value.items()) {
            if (!consume_json_string(key, budget, error)) {
                lua_pop(state, 1);
                return false;
            }
            lua_pushlstring(state, key.data(), key.size());
            if (!push_json(state, child, budget, error, depth + 1, preserve_json_null)) {
                lua_pop(state, 2);
                return false;
            }
            lua_rawset(state, -3);
        }
    } else {
        error = "JSON value is not representable in Lua";
        return false;
    }
    return true;
}

struct push_json_context {
    const json* value = nullptr;
    std::string* error = nullptr;
    bool represented = false;
    bool preserve_json_null = false;
};

inline int push_json_dispatch(lua_State* state) noexcept {
    try {
        auto* context = static_cast<push_json_context*>(lua_touserdata(state, 1));
        if (context == nullptr || context->value == nullptr || context->error == nullptr) {
            return luaL_error(state, "invalid JSON push context");
        }
        json_budget budget;
        context->represented = push_json(state, *context->value, budget, *context->error, 0,
                                         context->preserve_json_null);
        return context->represented ? 1 : 0;
    } catch (...) {
        return luaL_error(state, "JSON conversion failed");
    }
}

inline bool protected_push_json(lua_State* state, const json& value, std::string& error,
                                bool preserve_json_null = false) {
    lua_stack_guard stack(state);
    push_json_context context{&value, &error, false, preserve_json_null};
    if (protected_trampoline(state, push_json_dispatch, &context, 1) != LUA_OK) {
        if (error.empty())
            error = "Lua rejected a JSON value";
        return false;
    }
    if (!context.represented) {
        error = "hook argument is not representable in Lua";
        return false;
    }
    stack.dismiss();
    return true;
}

struct table_item {
    bool integer_key = false;
    lua_Integer integer = 0;
    std::string string;
    json value;
};

struct active_table_guard final {
    explicit active_table_guard(std::vector<const void*>& tables) noexcept : tables_(tables) {}
    ~active_table_guard() {
        tables_.pop_back();
    }

    active_table_guard(const active_table_guard&) = delete;
    active_table_guard& operator=(const active_table_guard&) = delete;

  private:
    std::vector<const void*>& tables_;
};

inline bool stack_to_json(lua_State* state, int index, json& output, std::string& error, int depth,
                          json_budget& budget, std::vector<const void*>& active_tables) {
    if (depth > kMaximumJsonNestingDepth) {
        error = "Lua value nesting exceeds 64 levels";
        return false;
    }
    if (!consume_json_node(budget, error))
        return false;
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        output = nullptr;
        return true;
    case LUA_TBOOLEAN:
        output = lua_toboolean(state, index) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) {
            output = static_cast<int64_t>(lua_tointeger(state, index));
        } else {
            const double number = static_cast<double>(lua_tonumber(state, index));
            if (!std::isfinite(number)) {
                error = "Lua JSON numbers must be finite";
                return false;
            }
            output = number;
        }
        return true;
    case LUA_TSTRING: {
        size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        if (!consume_json_string(std::string_view(value == nullptr ? "" : value, length), budget,
                                 error)) {
            return false;
        }
        output = std::string(value == nullptr ? "" : value, length);
        return true;
    }
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(state, index) == &kJsonNullSentinel) {
            output = nullptr;
            return true;
        }
        error = "Lua light userdata is not JSON serializable";
        return false;
    case LUA_TTABLE: {
        const void* identity = lua_topointer(state, index);
        if (std::find(active_tables.begin(), active_tables.end(), identity) !=
            active_tables.end()) {
            error = "cyclic Lua tables are not JSON serializable";
            return false;
        }
        if (lua_checkstack(state, 3) == 0) {
            error = "Lua stack cannot represent the JSON value";
            return false;
        }
        active_tables.push_back(identity);
        active_table_guard active_guard(active_tables);
        std::vector<table_item> items;
        bool array_candidate = true;
        lua_Integer largest_index = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            table_item item;
            if (lua_isinteger(state, -2)) {
                item.integer_key = true;
                item.integer = lua_tointeger(state, -2);
                if (item.integer <= 0)
                    array_candidate = false;
                largest_index = std::max(largest_index, item.integer);
            } else if (lua_type(state, -2) == LUA_TSTRING) {
                size_t length = 0;
                const char* key = lua_tolstring(state, -2, &length);
                item.string.assign(key == nullptr ? "" : key, length);
                if (!consume_json_string(item.string, budget, error)) {
                    lua_pop(state, 2);
                    return false;
                }
                array_candidate = false;
            } else {
                lua_pop(state, 2);
                error = "Lua table JSON keys must be strings or positive integers";
                return false;
            }
            if (!stack_to_json(state, -1, item.value, error, depth + 1, budget,
                               active_tables)) {
                lua_pop(state, 2);
                return false;
            }
            items.push_back(std::move(item));
            lua_pop(state, 1);
        }
        array_candidate = array_candidate && !items.empty() && largest_index >= 0 &&
                          static_cast<size_t>(largest_index) == items.size();
        if (array_candidate) {
            output = json::array();
            while (output.size() < items.size()) {
                output.push_back(nullptr);
            }
            for (auto& item : items) {
                output[static_cast<size_t>(item.integer - 1)] = std::move(item.value);
            }
        } else {
            output = json::object();
            for (auto& item : items) {
                const std::string key =
                    item.integer_key ? std::to_string(item.integer) : std::move(item.string);
                output[key] = std::move(item.value);
            }
        }
        return true;
    }
    default:
        error = std::string("Lua value type '") + luaL_typename(state, index) +
                "' is not JSON serializable";
        return false;
    }
}

inline bool stack_to_json(lua_State* state, int index, json& output, std::string& error) {
    lua_stack_guard stack(state);
    json_budget budget;
    std::vector<const void*> active_tables;
    try {
        const bool converted =
            stack_to_json(state, index, output, error, 0, budget, active_tables);
        if (converted)
            stack.dismiss();
        return converted;
    } catch (...) {
        error = "Lua JSON conversion failed";
        return false;
    }
}

inline bool serialize_json(const json& value, std::string& output, std::string& error) {
    try {
        output = value.dump(-1, ' ', false, json::error_handler_t::strict);
        if (output.size() > kMaximumJsonOutputBytes) {
            output.clear();
            error = "JSON output exceeds its byte budget";
            return false;
        }
        return true;
    } catch (...) {
        output.clear();
        error = "JSON output is not valid UTF-8";
        return false;
    }
}

inline bool parse_json(const char* data, std::size_t size, json& output, std::string& error) {
    if (data == nullptr || size > kMaximumJsonOutputBytes || !valid_utf8({data, size})) {
        error = "JSON input is invalid or exceeds its byte budget";
        return false;
    }
    try {
        output = json::parse(data, data + size, nullptr, false);
        if (output.is_discarded()) {
            error = "JSON input is invalid";
            return false;
        }
        return true;
    } catch (...) {
        error = "JSON input conversion failed";
        return false;
    }
}

inline bool parse_json_c_string(const char* data, json& output, std::string& error) {
    if (data == nullptr)
        return parse_json("null", 4, output, error);
    std::size_t size = 0;
    while (size <= kMaximumJsonOutputBytes && data[size] != '\0')
        ++size;
    if (size > kMaximumJsonOutputBytes) {
        error = "JSON input exceeds its byte budget";
        return false;
    }
    return parse_json(data, size, output, error);
}

inline bool push_json_arguments(lua_State* state, const char* json_utf8, int& argument_count,
                                std::string& error) {
    argument_count = 0;
    if (json_utf8 == nullptr || json_utf8[0] == '\0')
        return true;
    lua_stack_guard stack(state);
    json parsed;
    if (!parse_json_c_string(json_utf8, parsed, error)) {
        error = "hook arguments are not valid JSON";
        return false;
    }
    if (parsed.is_array()) {
        for (const auto& argument : parsed) {
            if (!protected_push_json(state, argument, error)) {
                return false;
            }
            ++argument_count;
        }
        stack.dismiss();
        return true;
    }
    if (!protected_push_json(state, parsed, error)) {
        return false;
    }
    argument_count = 1;
    stack.dismiss();
    return true;
}

} // namespace sao::plugins::lua_host::detail
#endif
