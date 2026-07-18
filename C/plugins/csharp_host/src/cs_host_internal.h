#pragma once

#include "sao/plugins/csharp_host/cs_host.h"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sao::plugins::csharp_host {

#if defined(_WIN32)
#define SAO_CSHOST_HOSTFXR_CALLTYPE __cdecl
#define SAO_CSHOST_CORECLR_DELEGATE_CALLTYPE __stdcall
#else
#define SAO_CSHOST_HOSTFXR_CALLTYPE
#define SAO_CSHOST_CORECLR_DELEGATE_CALLTYPE
#endif

using hostfxr_handle_t = void*;
using hostfxr_initialize_for_runtime_config_fn = int32_t(SAO_CSHOST_HOSTFXR_CALLTYPE*)(
    const wchar_t* runtime_config_path, const void* parameters,
    hostfxr_handle_t* host_context_handle);
using hostfxr_get_runtime_delegate_fn = int32_t(SAO_CSHOST_HOSTFXR_CALLTYPE*)(
    hostfxr_handle_t host_context_handle, int32_t type, void** delegate);
using hostfxr_close_fn =
    int32_t(SAO_CSHOST_HOSTFXR_CALLTYPE*)(hostfxr_handle_t host_context_handle);
using load_assembly_and_get_function_pointer_fn = int32_t(SAO_CSHOST_CORECLR_DELEGATE_CALLTYPE*)(
    const wchar_t* assembly_path, const wchar_t* type_name, const wchar_t* method_name,
    const wchar_t* delegate_type_name, void* reserved, void** delegate_out);
using component_entry_point_fn =
    int32_t(SAO_CSHOST_CORECLR_DELEGATE_CALLTYPE*)(void* argument, int32_t argument_size);

struct cshost_runtime_api {
#if defined(_WIN32)
    HMODULE module = nullptr;
#endif
    hostfxr_initialize_for_runtime_config_fn initialize_for_runtime_config = nullptr;
    hostfxr_get_runtime_delegate_fn get_runtime_delegate = nullptr;
    hostfxr_close_fn close = nullptr;
    cs_host_handle_t owner = nullptr;
};

int32_t cshost_acquire_runtime_api(cs_host_handle_t host, cshost_runtime_api& out_api) noexcept;
void cshost_release_runtime_api(cshost_runtime_api& api) noexcept;

} // namespace sao::plugins::csharp_host
