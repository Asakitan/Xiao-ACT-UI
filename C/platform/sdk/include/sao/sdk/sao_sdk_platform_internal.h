#pragma once

#include "sao/sdk/sao_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_bind_ui_compositor(void* compositor);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_unbind_ui_compositor(void);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_get_ui_compositor(void** out_compositor);

/* Streaming-mode transaction hook.  The launcher registers a callback that
 * applies the full anti-screencap transaction (security worker flow +
 * capture-mode rollback) so plugin/settings call sites share the exact same
 * semantics as the entity shell's built-in toggle instead of only flipping a
 * diagnostic bit.  When no hook is bound the launcher falls back to the
 * offline flow-bit behavior. */
typedef int32_t(SAO_SDK_CALL* SaoSdkStreamingModeApplyFn)(int32_t enabled);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_bind_streaming_mode_apply(SaoSdkStreamingModeApplyFn callback);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_apply_streaming_mode(int32_t enabled, bool* out_applied);

#ifdef __cplusplus
}
#endif
