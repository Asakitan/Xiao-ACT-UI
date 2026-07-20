// lua_host.cpp — Lua 5.4 真 C API 嵌入实装
//
// 通过 CMake gate: SAO_HAS_LUA (find_package(Lua) 成功时定义)。
// 未 gated 时全部 API 返回 SAO_ERR_NOT_IMPLEMENTED (等同 SAO_ERR_NOT_AVAILABLE 语义)。

#if 0
#include "sao/plugins/lua_host/lua_host.h"

#include "sao/plugins/lua_host/lua_sandbox.h"

#if defined(SAO_HAS_LUA)
#include "lua_bridge_internal.h"
#endif

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)

struct lua_host_s {
    lua_State* L = nullptr;
    void (*message_callback)(const char*, int, void*) = nullptr;
    void* callback_user_data = nullptr;
};

// 自定义 print, 走 message_callback (若装了)
static int lua_print_hook(lua_State* L) {
    try {
        auto* host_ptr = static_cast<lua_host_s**>(
            lua_touserdata(L, lua_upvalueindex(1)));
        lua_host_s* host = host_ptr ? *host_ptr : nullptr;
        int n = lua_gettop(L);
        std::string acc;
        for (int i = 1; i <= n; ++i) {
            if (i > 1) acc.push_back('\t');
            size_t len = 0;
            const char* s = luaL_tolstring(L, i, &len);
            if (s) acc.append(s, len);
            lua_pop(L, 1);
        }
        if (host && host->message_callback) {
            host->message_callback(acc.c_str(), 0, host->callback_user_data);
        } else {
            std::fwrite(acc.c_str(), 1, acc.size(), stdout);
            std::fputc('\n', stdout);
        }
        return 0;
    } catch (...) {
        return luaL_error(L, "Lua print callback failed");
    }
}

#endif // SAO_HAS_LUA

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_create(const lua_host_config* cfg, lua_host_handle_t* out_host) {
    if (out_host == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;
#if defined(SAO_HAS_LUA)
    lua_State* L = luaL_newstate();
    if (L == nullptr) return SAO_ERR_OS_CALL_FAILED;
    if (cfg && cfg->install_stdlib) {
        luaL_openlibs(L);
    }
    auto* host = new (std::nothrow) lua_host_s();
    if (host == nullptr) {
        lua_close(L);
        return SAO_ERR_OS_CALL_FAILED;
    }
    host->L = L;
    if (cfg) {
        host->message_callback = cfg->message_callback;
        host->callback_user_data = cfg->callback_user_data;
    }
    // 若装了 stdlib, 用 hook 覆盖 print
    if (cfg && cfg->install_stdlib) {
        auto** slot = static_cast<lua_host_s**>(lua_newuserdatauv(L, sizeof(lua_host_s*), 0));
        *slot = host;
        lua_pushcclosure(L, lua_print_hook, 1);
        lua_setglobal(L, "print");
    }
    *out_host = host;
    return SAO_OK;
#else
        lua_State* state = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
    int print_hook_body(lua_State* state) {
    if (host == nullptr) return SAO_ERR_HANDLE_INVALID;
#if defined(SAO_HAS_LUA)
    if (host->L) {
            lua_host_s* host = host_ptr ? *host_ptr : nullptr;
        if (sao_plugins_luahost_sandbox_is_armed(host->L)) {
            (void)sao_plugins_luahost_sandbox_disarm(host->L);
        }
        lua_close(host->L);
    }
    delete host;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API lua_State* SAO_PLUGINS_CALL
sao_plugins_luahost_state(lua_host_handle_t host) {
#if defined(SAO_HAS_LUA)
    return host ? host->L : nullptr;
#else
    (void)host;
    return nullptr;
#endif
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_luahost_version(void) {
#if defined(SAO_HAS_LUA)
    return LUA_RELEASE;
#else
    return "lua_host: not available (SAO_HAS_LUA not defined)";
#endif
}

// ── 内存源码 execute / call_function 便利入口 (无文件路径) ───────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_execute(lua_host_handle_t host,
                            const char* source_utf8,
                            size_t source_len,
                            char** out_result_utf8,
                            char** out_error_utf8) {
    if (out_result_utf8) *out_result_utf8 = nullptr;
    if (out_error_utf8)  *out_error_utf8  = nullptr;
    if (host == nullptr || source_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    lua_State* L = host->L;
    if (L == nullptr) return SAO_ERR_HANDLE_INVALID;

    int base = lua_gettop(L);
    int rc = luaL_loadbuffer(L, source_utf8, source_len, "=<execute>");
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (msg && out_error_utf8) {
            size_t mlen = std::strlen(msg);
            char* buf = static_cast<char*>(std::malloc(mlen + 1));
            if (buf) { std::memcpy(buf, msg, mlen); buf[mlen] = '\0'; *out_error_utf8 = buf; }
        }
        lua_settop(L, base);
        return SAO_ERR_INVALID_ARGUMENT;
    }
    rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (msg && out_error_utf8) {
            size_t mlen = std::strlen(msg);
            char* buf = static_cast<char*>(std::malloc(mlen + 1));
            if (buf) { std::memcpy(buf, msg, mlen); buf[mlen] = '\0'; *out_error_utf8 = buf; }
        }
        lua_settop(L, base);
        return SAO_ERR_OS_CALL_FAILED;
    }
    // 若有返回值, 把栈顶格式化为字符串
    int nret = lua_gettop(L) - base;
    if (nret > 0 && out_result_utf8) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, -nret, &len);   // 复制拷贝
        if (s) {
            char* buf = static_cast<char*>(std::malloc(len + 1));
            if (buf) { std::memcpy(buf, s, len); buf[len] = '\0'; *out_result_utf8 = buf; }
        }
        lua_pop(L, 1);   // 弹 tostring 结果
    }
    lua_settop(L, base);
    return SAO_OK;
#else
    (void)source_len;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function(lua_host_handle_t host,
                                   const char* fn_name,
                                   char** out_result_utf8,
                                   char** out_error_utf8) {
    if (out_result_utf8) *out_result_utf8 = nullptr;
    if (out_error_utf8)  *out_error_utf8  = nullptr;
    if (host == nullptr || fn_name == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    lua_State* L = host->L;
    if (L == nullptr) return SAO_ERR_HANDLE_INVALID;

    int base = lua_gettop(L);
    lua_getglobal(L, fn_name);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        if (out_error_utf8) {
            const char* m = "function not found";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) { std::strcpy(buf, m); *out_error_utf8 = buf; }
        }
        return SAO_ERR_HANDLE_INVALID;
    }
    int rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (msg && out_error_utf8) {
            size_t mlen = std::strlen(msg);
            char* buf = static_cast<char*>(std::malloc(mlen + 1));
            if (buf) { std::memcpy(buf, msg, mlen); buf[mlen] = '\0'; *out_error_utf8 = buf; }
        }
        lua_settop(L, base);
        return SAO_ERR_OS_CALL_FAILED;
    }
    int nret = lua_gettop(L) - base;
    if (nret > 0 && out_result_utf8) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, -nret, &len);
        if (s) {
            char* buf = static_cast<char*>(std::malloc(len + 1));
            if (buf) { std::memcpy(buf, s, len); buf[len] = '\0'; *out_result_utf8 = buf; }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, base);
    return SAO_OK;
#else
    (void)fn_name;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_luahost_free_string(char* s) {
    if (s != nullptr) std::free(s);
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_is_available(void) {
#if defined(SAO_HAS_LUA)
    return true;
#else
    return false;
#endif
}

} // namespace sao::plugins::lua_host
#endif
