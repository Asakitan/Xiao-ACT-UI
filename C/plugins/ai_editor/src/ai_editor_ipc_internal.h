#pragma once

#include <windows.h>

#include "sao/ai_editor/ai_editor_ipc.h"

namespace sao::ai_editor::detail {

int32_t ipc_accept_with_process(sao_ai_editor_ipc_t handle,
                                uint32_t timeout_ms,
                                HANDLE process);

int32_t ipc_recv_with_process(sao_ai_editor_ipc_t handle,
                              void* buffer,
                              uint32_t buffer_cap,
                              uint32_t* out_len,
                              uint32_t timeout_ms,
                              HANDLE process);

}  // namespace sao::ai_editor::detail
