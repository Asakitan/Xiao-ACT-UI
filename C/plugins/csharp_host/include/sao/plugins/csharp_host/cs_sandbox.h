// cs_sandbox.h — AssemblyLoadContext 隔离 + code trust 策略
//
// .NET Core 起 CAS (Code Access Security) 移除了, 沙箱靠隔离 + AOT reflection
// 白名单 + Roslyn 时 CSharpParseOptions.LanguageVersion。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_domain_s* cs_domain_handle_t;

struct cs_sandbox_config {
    bool allow_fs = false;
    bool allow_net = false;
    bool allow_process = false;
    bool allow_reflection_emit = false;   // 拒 Reflection.Emit → 禁运行时 gen code
    bool allow_unsafe_pinvoke = false;    // 拒任意 [DllImport], 只允许 SAO 平台 SDK
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_arm(cs_domain_handle_t domain,
                               const cs_sandbox_config* cfg);

} // namespace sao::plugins::csharp_host
