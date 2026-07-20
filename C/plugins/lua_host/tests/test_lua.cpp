// Lightweight Lua host smoke test without Catch2.

#include "sao/plugins/lua_host/lua_call.h"
#include "sao/plugins/lua_host/lua_host.h"
#include "sao/plugins/lua_host/lua_module_bridge.h"
#include "sao/plugins/lua_host/lua_sandbox.h"
#include "sao/plugins/lua_host/lua_stdlib.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace sao::plugins::lua_host;

    const char* version = sao_plugins_luahost_version();
    assert(version != nullptr);
    assert(std::strlen(version) > 0);

    if (sao_plugins_luahost_is_available()) {
        lua_host_config config{};
        config.install_stdlib = true;
        lua_host_handle_t host = nullptr;
        const int32_t rc = sao_plugins_luahost_create(&config, &host);
        assert(rc == SAO_OK);
        assert(host != nullptr);
        assert(sao_plugins_luahost_state(host) != nullptr);
        sao_plugins_luahost_destroy(host);
        std::printf("lua_host smoke test passed (real Lua %s)\n", version);
    } else {
        lua_host_config config{};
        lua_host_handle_t host = nullptr;
        int32_t rc = sao_plugins_luahost_create(&config, &host);
        assert(rc == SAO_ERR_NOT_IMPLEMENTED);
        assert(host == nullptr);

        auto* unavailable_state = reinterpret_cast<lua_State*>(1);
        assert(sao_plugins_luahost_install_sao_stdlib(unavailable_state) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(sao_plugins_luahost_register_ctx(unavailable_state, reinterpret_cast<void*>(1)) ==
               SAO_ERR_NOT_IMPLEMENTED);
        lua_stdlib_config stdlib{};
        assert(sao_plugins_luahost_install_stdlib(unavailable_state, &stdlib) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(sao_plugins_luahost_install_one_stdlib(unavailable_state, "base") ==
               SAO_ERR_NOT_IMPLEMENTED);
        lua_sandbox_config sandbox{};
        assert(sao_plugins_luahost_sandbox_arm(unavailable_state, &sandbox) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(sao_plugins_luahost_sandbox_disarm(unavailable_state) == SAO_ERR_NOT_IMPLEMENTED);
        assert(sao_plugins_luahost_sandbox_bytes_used(unavailable_state) == 0);
        assert(!sao_plugins_luahost_sandbox_is_armed(unavailable_state));

        auto* unavailable_host = reinterpret_cast<lua_host_handle_t>(1);
        char* result = reinterpret_cast<char*>(1);
        char* error = reinterpret_cast<char*>(1);
        assert(sao_plugins_luahost_execute(unavailable_host, "return true", 11, &result, &error) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(result == nullptr);
        assert(error == nullptr);
        result = reinterpret_cast<char*>(1);
        error = reinterpret_cast<char*>(1);
        assert(sao_plugins_luahost_call_function(unavailable_host, "probe", &result, &error) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(result == nullptr);
        assert(error == nullptr);
        assert(sao_plugins_luahost_state(unavailable_host) == nullptr);
        assert(sao_plugins_luahost_destroy(unavailable_host) == SAO_ERR_NOT_IMPLEMENTED);

        lua_loader_adapter_owner_t adapter_owner = reinterpret_cast<lua_loader_adapter_owner_t>(1);
        assert(sao_plugins_luahost_register_loader_adapter(&config, &adapter_owner) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(adapter_owner == nullptr);
        assert(sao_plugins_luahost_unregister_loader_adapter(
                   reinterpret_cast<lua_loader_adapter_owner_t>(1)) == SAO_ERR_NOT_IMPLEMENTED);

        lua_plugin_handle_t plugin = reinterpret_cast<lua_plugin_handle_t>(1);
        assert(sao_plugins_luahost_load_script(unavailable_state, L".", "plugin.lua", "stub",
                                               nullptr, &plugin) == SAO_ERR_NOT_IMPLEMENTED);
        assert(plugin == nullptr);
        auto* unavailable_plugin = reinterpret_cast<lua_plugin_handle_t>(1);
        bool allow_unload = false;
        assert(sao_plugins_luahost_call_on_load(unavailable_plugin) == SAO_ERR_NOT_IMPLEMENTED);
        assert(sao_plugins_luahost_call_on_unload(unavailable_plugin, &allow_unload) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(allow_unload);
        result = reinterpret_cast<char*>(1);
        assert(sao_plugins_luahost_call_hook(unavailable_plugin, "probe", nullptr, &result) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(result == nullptr);
        assert(!sao_plugins_luahost_has_hook(unavailable_plugin, "probe"));
        assert(sao_plugins_luahost_unload_script(unavailable_plugin) == SAO_ERR_NOT_IMPLEMENTED);
        result = reinterpret_cast<char*>(1);
        assert(sao_plugins_luahost_call_function_by_ref(unavailable_state, 1, nullptr, &result) ==
               SAO_ERR_NOT_IMPLEMENTED);
        assert(result == nullptr);
        std::printf("lua_host smoke test passed (stub: %s)\n", version);
    }
    return 0;
}
