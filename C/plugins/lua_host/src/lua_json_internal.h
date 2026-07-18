#pragma once

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lua_state_internal.h"

namespace sao::plugins::lua_host::detail {

using json = nlohmann::json;

inline bool push_json(lua_State* state, const json& value, int depth = 0) {
    if (depth > 32) return false;
    if (value.is_null()) {
        lua_pushnil(state);
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
        lua_pushlstring(state, string_value.data(), string_value.size());
    } else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        for (size_t index = 0; index < value.size(); ++index) {
            if (!push_json(state, value[index], depth + 1)) {
                lua_pop(state, 1);
                return false;
            }
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
    } else if (value.is_object()) {
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (const auto& [key, child] : value.items()) {
            if (!push_json(state, child, depth + 1)) {
                lua_pop(state, 1);
                return false;
            }
            lua_setfield(state, -2, key.c_str());
        }
    } else {
        return false;
    }
    return true;
}

struct push_json_context {
    const json* value = nullptr;
    bool represented = false;
};

inline int push_json_dispatch(lua_State* state) {
    auto* context = static_cast<push_json_context*>(
        lua_touserdata(state, 1));
    if (context == nullptr || context->value == nullptr) {
        return luaL_error(state, "invalid JSON push context");
    }
    context->represented = push_json(state, *context->value);
    return context->represented ? 1 : 0;
}

inline bool protected_push_json(lua_State* state, const json& value,
                                std::string& error) {
    push_json_context context{&value, false};
    if (protected_trampoline(state, push_json_dispatch, &context, 1) !=
        LUA_OK) {
        error = "Lua rejected a JSON value";
        return false;
    }
    if (!context.represented) {
        error = "hook argument is not representable in Lua";
        return false;
    }
    return true;
}

struct table_item {
    bool integer_key = false;
    lua_Integer integer = 0;
    std::string string;
    json value;
};

inline bool stack_to_json(lua_State* state, int index, json& output,
                          std::string& error, int depth = 0) {
    if (depth > 32) {
        error = "Lua value nesting exceeds 32 levels";
        return false;
    }
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
            output = static_cast<double>(lua_tonumber(state, index));
        }
        return true;
    case LUA_TSTRING: {
        size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        output = std::string(value == nullptr ? "" : value, length);
        return true;
    }
    case LUA_TTABLE: {
        std::vector<table_item> items;
        bool array_candidate = true;
        lua_Integer largest_index = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            table_item item;
            if (lua_isinteger(state, -2)) {
                item.integer_key = true;
                item.integer = lua_tointeger(state, -2);
                if (item.integer <= 0) array_candidate = false;
                largest_index = std::max(largest_index, item.integer);
            } else if (lua_type(state, -2) == LUA_TSTRING) {
                size_t length = 0;
                const char* key = lua_tolstring(state, -2, &length);
                item.string.assign(key == nullptr ? "" : key, length);
                array_candidate = false;
            } else {
                lua_pop(state, 2);
                error = "Lua table JSON keys must be strings or positive integers";
                return false;
            }
            if (!stack_to_json(state, -1, item.value, error, depth + 1)) {
                lua_pop(state, 2);
                return false;
            }
            items.push_back(std::move(item));
            lua_pop(state, 1);
        }
        array_candidate = array_candidate && !items.empty() &&
                          largest_index >= 0 &&
                          static_cast<size_t>(largest_index) == items.size();
        if (array_candidate) {
            output = json::array();
            while (output.size() < items.size()) {
                output.push_back(nullptr);
            }
            for (auto& item : items) {
                output[static_cast<size_t>(item.integer - 1)] =
                    std::move(item.value);
            }
        } else {
            output = json::object();
            for (auto& item : items) {
                const std::string key = item.integer_key
                                            ? std::to_string(item.integer)
                                            : std::move(item.string);
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

inline bool push_json_arguments(lua_State* state, const char* json_utf8,
                                int& argument_count, std::string& error) {
    argument_count = 0;
    if (json_utf8 == nullptr || json_utf8[0] == '\0') return true;
    const auto parsed = json::parse(json_utf8, nullptr, false);
    if (parsed.is_discarded()) {
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
        return true;
    }
    if (!protected_push_json(state, parsed, error)) {
        return false;
    }
    argument_count = 1;
    return true;
}

} // namespace sao::plugins::lua_host::detail
#endif
