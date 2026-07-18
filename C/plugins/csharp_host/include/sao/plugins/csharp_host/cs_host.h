// cs_host.h — .NET hostfxr 生命周期
//
// **升级说明**: Python 侧走 pythonnet (Python 加载 CLR), C++ 侧直接走
// hostfxr —— .NET 8 的官方原生嵌入 API。免 pythonnet 双跳, 免 Python 依赖。
//
// Generic component loading uses hostfxr's runtimeconfig path and precompiled DLLs.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_host_s* cs_host_handle_t;
typedef struct cs_domain_s* cs_domain_handle_t;

enum class cs_assembly_unload_mode : uint32_t {
    process_resident = 0,
    collectible = 1,
};

struct cs_host_config {
    // hostfxr.dll 的路径 (随包 vendor 或 .NET runtime 目录)
    const wchar_t* hostfxr_path;
    // .NET runtime config JSON (runtimeconfig.json, 由本模块生成或随包)
    const wchar_t* runtime_config_json_path;
    // 消息回调
    void (*message_callback)(const char* utf8, int is_error, void* ud);
    void* callback_user_data = nullptr;
};

// 初始化 hostfxr + coreclr。进程内单例。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_init(const cs_host_config* cfg, cs_host_handle_t* out_host);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_shutdown(cs_host_handle_t host);

// Collectible AssemblyLoadContext bootstrap is not available in this host yet.
// Domain creation therefore fails instead of claiming isolation.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_create_domain(
    cs_host_handle_t host, const char* domain_name_utf8, cs_domain_handle_t* out_domain);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_destroy_domain(cs_domain_handle_t domain);

// Compatibility snapshot backed by thread-local storage. Prefer the buffer API.
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_cshost_runtime_version(cs_host_handle_t host);

// ── Wave 4 新增 API (旧签名不动, 只追加) ─────────────────────

// 静态查询 hostfxr 是否可用 (无需先 init)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_is_available(bool* out_available);

// 获取 runtime 版本到调用方 buffer。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_runtime_version(cs_host_handle_t host, char* buf, size_t buf_size);

// Reports whether managed assemblies can be physically unloaded. The current
// default-ALC implementation returns process_resident; logical plugin unload
// does not unload assembly code and the same assembly path cannot be reloaded.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_get_assembly_unload_mode(
    cs_host_handle_t host, cs_assembly_unload_mode* out_mode);

// Copies the last host/runtime cleanup diagnostic and leaves the host valid so
// a failed shutdown or component close can be retried.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_last_error(cs_host_handle_t host, char* buf, size_t buf_size);

} // namespace sao::plugins::csharp_host
