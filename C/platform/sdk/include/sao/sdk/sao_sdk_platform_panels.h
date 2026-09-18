#pragma once

#include "sao/sdk/sao_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel-open binding.  The launcher registers one callback that resolves a
 * semantic panel name — "settings", "hotkeys", "workshop", "plugins",
 * "process", "memory", or "license" — to the matching launcher panel.  The
 * caller's user_data pointer is passed straight through on every invoke so
 * the launcher keeps its own routing context without globals leaking into
 * the SDK.
 *
 * sao_sdk_platform_open_panel walks the binding and forwards the launcher's
 * return code.  Unknown names report SAO_SDK_ERR_NOT_FOUND without invoking
 * the callback; when no callback is bound the call reports
 * SAO_SDK_ERR_NOT_INITIALIZED. */
typedef int32_t(SAO_SDK_CALL* SaoSdkPanelOpenFn)(const char* panel_name,
                                                void* user_data);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_bind_panel_open(SaoSdkPanelOpenFn callback, void* user_data);

SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_platform_open_panel(
    const char* panel_name);

#ifdef __cplusplus
}
#endif
