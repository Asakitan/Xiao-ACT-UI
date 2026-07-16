// cs_compile.h — Roslyn 编译 .cs 源
//
// 用随包 Roslyn (Microsoft.CodeAnalysis.CSharp.dll) 在内存里编译 .cs → .dll,
// LoadContext 加载。免用户装 .NET SDK。
//
// 对齐 Python 源: csharp_runtime.py 的 fetch_roslyn + 编译逻辑 (只是 C++ 侧
// 直接调 Roslyn 托管 API 而非通过 pythonnet 反射)。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_domain_s* cs_domain_handle_t;
typedef struct cs_assembly_s* cs_assembly_handle_t;

struct cs_compile_options {
    // 编译输出程序集名 (不带扩展名)
    const char* assembly_name_utf8;
    // 目标框架 (net8.0 / net9.0)
    const char* target_framework_utf8;
    // 引用程序集路径列表 (每插件可指定)
    const wchar_t* const* reference_paths;
    uint32_t reference_paths_count;
    // 是否是 debug 编译 (加 pdb)
    bool debug = false;
};

// 编译 .cs 文件 (单文件或目录) → cs_assembly_handle_t。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_compile_source(cs_domain_handle_t domain,
                                  const wchar_t* source_dir,
                                  const wchar_t* entry_relative,
                                  const cs_compile_options* opts,
                                  cs_assembly_handle_t* out_assembly);

// 直接加载已有 .dll (无需编译)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_assembly(cs_domain_handle_t domain,
                                 const wchar_t* assembly_path,
                                 cs_assembly_handle_t* out_assembly);

// 卸载 (LoadContext.Unload)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unload_assembly(cs_assembly_handle_t assembly);

} // namespace sao::plugins::csharp_host
