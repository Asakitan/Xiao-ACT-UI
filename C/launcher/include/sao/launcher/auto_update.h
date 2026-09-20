#pragma once

#include "sao/launcher/provider_config.h"

#include <string>
#include <windows.h>

namespace sao::launcher {

// Reconciles a staged helper result before platform bring-up. Ordinary
// launches do no network work unless a bound staging candidate exists.
bool acknowledgeCompletedAutoUpdate(const UpdateProviderConfiguration& configuration,
                                    const std::wstring& base_dir,
                                    const std::wstring& exe_path) noexcept;

// Starts one owned updater worker. The caller owns both returned handles and
// must signal the event before joining and closing the worker handle.
bool startAutoUpdate(UpdateProviderConfiguration configuration, std::wstring base_dir,
                     std::wstring exe_path, DWORD launcher_thread_id, HANDLE* cancel_event_out,
                     HANDLE* worker_handle_out) noexcept;

} // namespace sao::launcher