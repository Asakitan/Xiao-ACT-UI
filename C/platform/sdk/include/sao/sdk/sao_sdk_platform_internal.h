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

#ifdef __cplusplus
}
#endif
