// .NET Core removed Code Access Security. The current host has no collectible
// ALC bootstrap and no enforceable API sandbox, so arm() fails closed with
// SAO_ERR_NOT_IMPLEMENTED when hostfxr is present. Recording a namespace deny
// list alone is not a security boundary.
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
    bool allow_reflection_emit = false; // 拒 Reflection.Emit → 禁运行时 gen code
    bool allow_unsafe_pinvoke = false;  // 拒任意 [DllImport], 只允许 SAO 平台 SDK
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_arm(cs_domain_handle_t domain, const cs_sandbox_config* cfg);

// ── Wave 18 / Agent a 测试 & 内省 hook ─────────────────────
// mode: 0=真实探测(默认); 1=强制 hostfxr 可用; 2=强制不可用.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_set_hostfxr_mock(int mode);

// Reserved diagnostics. Outputs are zero and the API returns NOT_IMPLEMENTED.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_sandbox_get_policy_snapshot(
    cs_domain_handle_t domain, uint64_t* out_alc_id, size_t* out_deny_count);

// Reserved enforcement probe. out_denied is false and the API returns
// NOT_IMPLEMENTED until a managed bootstrap enforces the policy.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_sandbox_is_api_denied(
    cs_domain_handle_t domain, const char* api_utf8, bool* out_denied);

// Reserved release operation; returns NOT_IMPLEMENTED for non-null domains.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_release(cs_domain_handle_t domain);

// Compatibility teardown; no records are created and the return value is zero.
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL sao_plugins_cshost_sandbox_clear_all(void);

} // namespace sao::plugins::csharp_host
