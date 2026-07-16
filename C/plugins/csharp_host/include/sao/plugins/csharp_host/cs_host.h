// cs_host.h — .NET hostfxr 生命周期
//
// **升级说明**: Python 侧走 pythonnet (Python 加载 CLR), C++ 侧直接走
// hostfxr —— .NET 8 的官方原生嵌入 API。免 pythonnet 双跳, 免 Python 依赖。
//
// 对齐 Python 源: csharp_runtime.py (fetch_roslyn / assembly 加载语义参考)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_host_s* cs_host_handle_t;
typedef struct cs_domain_s* cs_domain_handle_t;    // 每插件独立 AssemblyLoadContext

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

// 每插件独立 AssemblyLoadContext (隔离)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_create_domain(cs_host_handle_t host,
                                 const char* domain_name_utf8,
                                 cs_domain_handle_t* out_domain);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_destroy_domain(cs_domain_handle_t domain);

// .NET runtime 版本 (X.Y.Z)。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_cshost_runtime_version(cs_host_handle_t host);

// ── Wave 4 新增 API (旧签名不动, 只追加) ─────────────────────

// 静态查询 hostfxr 是否可用 (无需先 init)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_is_available(bool* out_available);

// 获取 runtime 版本到调用方 buffer。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_runtime_version(cs_host_handle_t host,
                                       char* buf,
                                       size_t buf_size);

} // namespace sao::plugins::csharp_host
