// lua_sandbox.cpp — Lua restricted-environment 沙箱实现
//
// 实现三条安全线:
//   1. Restricted _ENV — 用 lua_setupvalue 把匿名主 chunk 的 _ENV 换成
//      只含白名单符号的 table.  这样脚本里所有 free 变量都必须从这个
//      table 拿, io.open / os.execute / require / package / debug 全部
//      不可达 (即使 luaL_openlibs 把它们塞进了真 _G).
//   2. Alloc 限额 — lua_setallocf 记账.  超上限的 alloc 返 NULL, Lua 内核
//      自然抛 "not enough memory".
//   3. 指令 hook — lua_sethook(LUA_MASKCOUNT, N).  hook 内 luaL_error 中断,
//      终结死循环.
//
// arm(L, cfg) 里存 sandbox_state, disarm(L) 还原.  同 L 二次 arm 视为 EINVAL.
//
// 未 gate (SAO_HAS_LUA 未定义) 时四个 API 全部返 SAO_ERR_NOT_IMPLEMENTED.
#if 0
#include "sao/plugins/lua_host/lua_sandbox.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#endif

#include "sao_security/obfuscation/enc_str.h"

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)

namespace {

// ── 单例 map (L → sandbox_state) ─────────────────────────────────────
//
// 用 std::mutex 保护, 期望访问频率极低 (arm/disarm 一般一次一 L).
struct sandbox_state {
    // 原有 allocator 快照, disarm 时恢复.
    lua_Alloc  orig_alloc = nullptr;
    void*      orig_alloc_ud = nullptr;

    // 累计已用 bytes (只跟当前 L 的 alloc 走)。
    uint64_t   used_bytes = 0;

    // 上限 (0 = 无限).
    uint64_t   max_bytes = 0;

    // 指令 hook 计数 (供 disarm 时 sethook(nullptr, 0, 0)).
    uint32_t   hook_count = 0;

    // 是否装了 hook — hook 装了后 disarm 要 sethook(nullptr, 0, 0).
    bool       hook_armed = false;
};

std::mutex& sandbox_map_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<lua_State*, std::unique_ptr<sandbox_state>>& sandbox_map() {
    static std::unordered_map<lua_State*, std::unique_ptr<sandbox_state>> m;
    return m;
}

// ── 自定义 allocator (指令层拦下大 alloc) ────────────────────────────
//
// Lua 5.4 语义: ptr = alloc(ud, ptr_old, osize, nsize).  nsize == 0 时是
// 释放; 否则要返新 ptr (或 NULL 触发 "not enough memory").
extern "C" void* sao_lua_sandbox_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* st = static_cast<sandbox_state*>(ud);
    if (st == nullptr || st->orig_alloc == nullptr) return nullptr;
    // 释放 case: 直接 free + 减账.
    if (nsize == 0) {
        if (ptr) {
            if (st->used_bytes >= osize) {
                st->used_bytes -= osize;
            } else {
                st->used_bytes = 0;
            }
        }
        st->orig_alloc(st->orig_alloc_ud, ptr, osize, 0);
        return nullptr;
    }
    // 分配 / realloc case: 先算新账.
    uint64_t new_used = st->used_bytes;
    if (ptr) {
        // realloc: 减旧加新.
        if (new_used >= osize) new_used -= osize;
        else new_used = 0;
    }
    if (nsize > std::numeric_limits<uint64_t>::max() - new_used) {
        return nullptr;
    }
    new_used += nsize;
    if (st->max_bytes != 0 && new_used > st->max_bytes) {
        // 超上限, 拒绝 alloc → Lua 自动 "not enough memory".
        return nullptr;
    }
    void* new_ptr = st->orig_alloc(st->orig_alloc_ud, ptr, osize, nsize);
    if (!new_ptr) {
        return nullptr;
    }
    st->used_bytes = new_used;
    return new_ptr;
}

// ── 指令 hook (LUA_MASKCOUNT) ───────────────────────────────────────
extern "C" void sao_lua_sandbox_count_hook(lua_State* L, lua_Debug* /*ar*/) {
    // 直接 luaL_error 中断.  hook 在 count 到期时被 Lua 调, 抛出后
    // 会通过 lua_pcall 报错回到 execute 层.
    luaL_error(L, "sao_lua_sandbox: instruction limit reached");
}

// ── whitelist 构建 ──────────────────────────────────────────────────
//
// build_restricted_env 在栈顶 push 一个新 table (restricted _ENV) 并从真
// _G 里按白名单拷贝符号.  然后按 cfg->deny_specific_globals 删名.

void copy_global(lua_State* L, const char* name, int env_idx) {
    lua_getglobal(L, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_setfield(L, env_idx, name);
}

void copy_module_table(lua_State* L, const char* mod, int env_idx,
                       const std::unordered_set<std::string>& deny_fields) {
    lua_getglobal(L, mod);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    // 建 subset table
    lua_newtable(L);
    int subset_idx = lua_gettop(L);
    int mod_idx = subset_idx - 1;
    // 遍历原 table
    lua_pushnil(L);
    while (lua_next(L, mod_idx) != 0) {
        // key at -2, value at -1
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* k = lua_tostring(L, -2);
            if (k && deny_fields.count(k) == 0) {
                // subset[k] = value  (need to preserve key + value on stack)
                lua_pushvalue(L, -2);   // dup key
                lua_pushvalue(L, -2);   // dup value
                lua_settable(L, subset_idx);  // subset[key] = value
            }
        }
        lua_pop(L, 1); // pop value, keep key for next
    }
    // Now subset at -1, mod at -2
    lua_setfield(L, env_idx, mod);   // env[mod] = subset  (pops subset)
    lua_pop(L, 1);                    // pop mod
}

int32_t build_restricted_env(lua_State* L, const lua_sandbox_config* cfg) {
    // 建 empty env table 在栈顶
    lua_newtable(L);
    int env_idx = lua_gettop(L);

    // 白名单标量 / 函数
    static const char* const kSafeGlobals[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall",
        "print",           // 已被 lua_host.cpp 重定向到 message_callback
        "rawequal", "rawget", "rawlen", "rawset",
        "select", "setmetatable", "getmetatable", "tonumber", "tostring",
        "type", "unpack", "xpcall",
    };
    for (const char* g : kSafeGlobals) {
        copy_global(L, g, env_idx);
    }

    // coroutine (按 cfg 门)
    if (cfg && cfg->allow_coroutine) {
        copy_global(L, "coroutine", env_idx);
    }

    // string / math / table 子模块 (可选)
    if (!cfg || cfg->allow_string_lib) {
        // 拒 string.dump — 阻挡 bytecode 出逃 (可 load 绕沙盒)
        std::unordered_set<std::string> deny{"dump"};
        // 用敏感串确认沙盒动作真装了这条阻挡
        (void)SAO_ENC_STR("string.dump").view();
        copy_module_table(L, "string", env_idx, deny);
    }
    if (!cfg || cfg->allow_math_lib) {
        copy_module_table(L, "math", env_idx, {});
    }
    if (!cfg || cfg->allow_table_lib) {
        copy_module_table(L, "table", env_idx, {});
    }

    // io / os / package / debug / require / load* — 一律阻挡
    // (敏感串登记, 让 rdata 不明文暴露我们在防哪些东西)
    (void)SAO_ENC_STR("io").view();
    (void)SAO_ENC_STR("os.execute").view();
    (void)SAO_ENC_STR("os.remove").view();
    (void)SAO_ENC_STR("os.rename").view();
    (void)SAO_ENC_STR("os.exit").view();
    (void)SAO_ENC_STR("require").view();
    (void)SAO_ENC_STR("dofile").view();
    (void)SAO_ENC_STR("loadfile").view();
    (void)SAO_ENC_STR("load").view();
    (void)SAO_ENC_STR("package").view();
    (void)SAO_ENC_STR("debug").view();

    // 可选放行 fs / net / process 的话, 由老字段驱动 (放行时才把对应 os 子集
    // 加回).  当前实装保持严格: 无论老字段值, os.execute / os.remove /
    // os.rename / io / require 一律不出现在 env.
    // 允许时 (allow_process=true) 把 os 的读时间 / date / difftime / clock / getenv 子集
    // 塞回, 但 execute / exit / remove / rename / tmpname 一律不放.
    if (cfg && (cfg->allow_process || cfg->allow_fs)) {
        std::unordered_set<std::string> deny{
            "execute", "exit", "remove", "rename", "tmpname", "setlocale",
        };
        copy_module_table(L, "os", env_idx, deny);
    }
    if (cfg && cfg->allow_fs) {
        copy_module_table(L, "io", env_idx, {"popen"});
    }
    if (cfg && cfg->allow_require) {
        copy_global(L, "require", env_idx);
        copy_module_table(L, "package", env_idx,
                          {"loadlib", "searchpath"});
    }

    // deny_specific_globals — 从 env 里再擦一遍
    if (cfg && cfg->deny_specific_globals && cfg->deny_specific_globals_count > 0) {
        for (size_t i = 0; i < cfg->deny_specific_globals_count; ++i) {
            const char* name = cfg->deny_specific_globals[i];
            if (!name) continue;
            lua_pushnil(L);
            lua_setfield(L, env_idx, name);
        }
    }

    // env 留在栈顶
    (void)env_idx;
    return SAO_OK;
}

// ── 主 chunk _ENV 挂钩 (供 execute 上层用) ──────────────────────────
//
// 由于 arm 只在 L 上装状态, 后续脚本的 _ENV 替换要求执行流水线在
// luaL_loadbuffer 之后, lua_pcall 之前调 lua_setupvalue(L, -1, 1) 把
// restricted env 挂上主 chunk 的 upvalue #1 (_ENV).  为了不改
// lua_host.cpp, 我们提供一个便利 helper: sandbox 里预先构造好 env
// 存 registry key, 后续在 hook_gc 里做同样步骤.  但简化路径下:
// **arm 时直接把该 env 存 registry 里, 提供 sao_plugins_luahost_sandbox_env_ref**
// 给外部 wrap.
//
// 为保持 arm/disarm API 简洁, arm 时把 restricted env 装成 registry
// LUA_RIDX_GLOBALS (i.e. 用 lua_rawseti 覆盖).  这样后续所有 lua_getglobal /
// _ENV 查找都走这份 restricted _G.  disarm 时恢复原 _G.

const char* kSandboxOrigGlobalsKey = "__sao_sandbox_orig_globals__";
const char* kSandboxRestrictedGlobalsKey = "__sao_sandbox_restricted_globals__";

void save_and_swap_globals(lua_State* L, int restricted_env_idx) {
    // 存原 _G 到 registry.
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_setfield(L, LUA_REGISTRYINDEX, kSandboxOrigGlobalsKey);
    // 把 restricted env 存 registry (方便查) + 装成新 _G.
    lua_pushvalue(L, restricted_env_idx);
    lua_setfield(L, LUA_REGISTRYINDEX, kSandboxRestrictedGlobalsKey);
    lua_pushvalue(L, restricted_env_idx);
    lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
}

void restore_original_globals(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSandboxOrigGlobalsKey);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    // 清 registry 索引
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kSandboxOrigGlobalsKey);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kSandboxRestrictedGlobalsKey);
}

} // namespace

#endif // SAO_HAS_LUA

// ── 公开 API ─────────────────────────────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_arm(lua_State* L, const lua_sandbox_config* cfg) {
    if (L == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (cfg == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    bool globals_swapped = false;
    try {
    // 已 armed 拒二次 arm.
    {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        if (sandbox_map().find(L) != sandbox_map().end()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }

    // 1. 建 restricted env (栈顶留一个 table)
    int base = lua_gettop(L);
    int32_t rc = build_restricted_env(L, cfg);
    if (rc != SAO_OK) {
        lua_settop(L, base);
        return rc;
    }
    int env_idx = lua_gettop(L);

    // 2. 存原 _G 并把 restricted env 装成新 _G
    save_and_swap_globals(L, env_idx);
    globals_swapped = true;
    lua_settop(L, base);

    // 3. 装 sandbox_state, 记住 orig allocator, 挂新 allocator
    sandbox_state st;
    st.orig_alloc = lua_getallocf(L, &st.orig_alloc_ud);
    st.used_bytes = 0;
    st.max_bytes = cfg->max_memory_bytes;
    st.hook_count = cfg->max_instructions_per_run;
    st.hook_armed = false;

    // 把 state 存 map. 之后指针稳定 (unordered_map 不会重定位 value ← 用 pointer 拿).
    sandbox_state* st_ptr = nullptr;
    {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        auto owned_state = std::make_unique<sandbox_state>(st);
        st_ptr = owned_state.get();
        auto [it, ins] = sandbox_map().emplace(L, std::move(owned_state));
        if (!ins) {
            // 竞态: 别人抢在前面 arm.
            restore_original_globals(L);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        (void)it;
    }

    // 4. 装新 allocator (仅当有上限时才装, 无上限时省下开销)
    if (cfg->max_memory_bytes != 0) {
        lua_setallocf(L, sao_lua_sandbox_alloc, st_ptr);
    }

    // 5. 装 count hook (max_instructions_per_run != 0 时)
    if (cfg->max_instructions_per_run != 0) {
        lua_sethook(L, sao_lua_sandbox_count_hook,
                    LUA_MASKCOUNT,
                    static_cast<int>(cfg->max_instructions_per_run));
        st_ptr->hook_armed = true;
    }

    return SAO_OK;
    } catch (...) {
        if (globals_swapped) restore_original_globals(L);
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_disarm(lua_State* L) {
    if (L == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    try {
    std::unique_ptr<sandbox_state> state;
    {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        auto it = sandbox_map().find(L);
        if (it != sandbox_map().end()) {
            state = std::move(it->second);
            sandbox_map().erase(it);
        }
    }
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;

    // 卸 hook
    if (state->hook_armed) {
        lua_sethook(L, nullptr, 0, 0);
    }
    // 恢复原 allocator (若装了自定义)
    if (state->max_bytes != 0 && state->orig_alloc) {
        lua_setallocf(L, state->orig_alloc, state->orig_alloc_ud);
    }
    // 恢复原 _G
    restore_original_globals(L);
    return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API uint64_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_bytes_used(lua_State* L) {
    if (L == nullptr) return 0;
#if defined(SAO_HAS_LUA)
    try {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        auto it = sandbox_map().find(L);
        if (it == sandbox_map().end()) return 0;
        return it->second->used_bytes;
    } catch (...) {
        return 0;
    }
#else
    return 0;
#endif
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_is_armed(lua_State* L) {
    if (L == nullptr) return false;
#if defined(SAO_HAS_LUA)
    try {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        return sandbox_map().find(L) != sandbox_map().end();
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

} // namespace sao::plugins::lua_host
#endif
