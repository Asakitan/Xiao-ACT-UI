// test_lua.cpp — smoke test (无 Catch2)
//
// 只做 version 字符串 + gate 存在性检测; 真功能测走 test_lua_wave3.cpp。

#include "sao/plugins/lua_host/lua_host.h"

#include <cassert>
#include <cstdio>
#include <cstring>

// wave3 便利函数 (在 lua_host.cpp)
extern "C" {
    SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_luahost_is_available(void);
}

int main() {
    using namespace sao::plugins::lua_host;

    const char* v = sao_plugins_luahost_version();
    assert(v != nullptr);
    assert(std::strlen(v) > 0);

    bool avail = sao_plugins_luahost_is_available();
    if (avail) {
        lua_host_config cfg{};
        cfg.install_stdlib = true;
        lua_host_handle_t h = nullptr;
        int32_t rc = sao_plugins_luahost_create(&cfg, &h);
        assert(rc == SAO_OK);
        assert(h != nullptr);
        assert(sao_plugins_luahost_state(h) != nullptr);
        sao_plugins_luahost_destroy(h);
        std::printf("lua_host smoke test passed (real Lua %s)\n", v);
    } else {
        // 无 lua: create 应返 NOT_IMPLEMENTED
        lua_host_config cfg{};
        lua_host_handle_t h = nullptr;
        int32_t rc = sao_plugins_luahost_create(&cfg, &h);
        assert(rc == SAO_ERR_NOT_IMPLEMENTED);
        assert(h == nullptr);
        std::printf("lua_host smoke test passed (stub: %s)\n", v);
    }
    return 0;
}
