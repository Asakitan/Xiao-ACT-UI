#pragma once

#include "sao/launcher/provider_config.h"

#include <string>
#include <windows.h>

namespace sao::launcher {

// Starts one owned updater worker. The caller owns both returned handles and
// must signal the event before joining and closing the worker handle.
bool startAutoUpdate(UpdateProviderConfiguration configuration, std::wstring base_dir,
                     std::wstring exe_path, DWORD launcher_thread_id, HANDLE* cancel_event_out,
                     HANDLE* worker_handle_out) noexcept;

} // namespace sao::launcher