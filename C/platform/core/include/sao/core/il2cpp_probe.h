// SAO Auto - Wave 6 - generic IL2CPP runtime probe.
//
// The probe is the game-agnostic half of `mem_probe/engine/il2cpp_adapter.py`:
// it can locate the IL2CPP metadata region, sweep a memory range for
// candidate klass pointers, and read a klass' name field.  It **never**
// hard-codes a klass name, namespace, or field offset that belongs to a
// specific title.  Selecting which klass matters is the plugin's job -
// this header just gives the plugin the primitives.
//
// Struct offsets follow the mainstream x64 IL2CPP layout observed across
// Unity 2020-2024 (metadata v27 / v29 / v31), matching the values used by
// the authoritative Python `il2cpp_adapter.py`:
//
//     Il2CppClass:
//       +0x00  Il2CppImage*    image
//       +0x08  void*           gc_desc
//       +0x10  const char*     name
//       +0x18  const char*     namespaze
//       +0x40  Il2CppClass*    parent
//       +0x58  Il2CppClass*    element_class / cast_class
//       +0x80  FieldInfo*      fields
//       +0x10C uint16_t        field_count
//
// Any of these can drift with new Unity revisions; the *_METADATA_VERSION
// APIs on this probe let callers detect that.  The probe never fails
// silently - it returns SAO_STATUS_ERR_NOT_FOUND when the metadata cannot
// be located, and callers are free to fall back to their own signature
// scans.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/process.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Standard IL2CPP klass member offsets used by this probe.  Exported as
// constants so plugins can share the same "canonical" values instead of
// re-declaring them.  Any deviation is game-specific and lives in the
// plugin.
enum : uint32_t {
    SAO_IL2CPP_KLASS_NAME_OFFSET       = 0x10,
    SAO_IL2CPP_KLASS_NAMESPACE_OFFSET  = 0x18,
    SAO_IL2CPP_KLASS_PARENT_OFFSET     = 0x40,
    SAO_IL2CPP_KLASS_FIELDS_OFFSET     = 0x80,
    SAO_IL2CPP_KLASS_FIELD_COUNT_OFFSET = 0x10C,
    SAO_IL2CPP_FIELDINFO_NAME_OFFSET   = 0x00,
    SAO_IL2CPP_FIELDINFO_OFFSET_OFFSET = 0x18,
    SAO_IL2CPP_FIELDINFO_STRIDE        = 0x20,
};

// Metadata probe result.  `metadata_rva` is the offset of the earliest
// candidate metadata blob within GameAssembly.dll.  `metadata_version`
// is the parsed magic-number version tag (22/24/27/29/31 in the wild);
// zero if the version bytes could not be decoded.  Fields default to 0
// on any error.
struct SaoIl2CppProbe {
    uint64_t metadata_rva;
    uint32_t metadata_version;
    uint32_t klass_stride_hint;  // usually 0 - reserved for future use
    uint64_t domain_ptr;         // resolved from il2cpp_domain_get if decodable
    uint64_t game_assembly_base; // convenience echo of the module base
};

// Probe the IL2CPP metadata inside the given (already opened) process
// handle.  `module_base` must be the base address of GameAssembly.dll -
// callers use `sao_core_engine_get_signature_module` to obtain it.  On
// success the probe populates `*probe_out`; on failure it clears the
// struct and returns a non-OK status.
//
// The probe is intentionally passive: it reads the PE export directory
// and (when the domain export decodes cleanly) dereferences the domain
// pointer.  It does NOT walk the class tree, and it never writes into
// the target.  Cost: at most a handful of NtReadVirtualMemory calls.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata(
    sao_core_process_handle_t handle,
    uint64_t module_base,
    SaoIl2CppProbe* probe_out);

// Callback for `sao_core_il2cpp_scan_klass_pointers`.  Fired for every
// 8-byte aligned candidate pointer inside the scan region that satisfies
// the "looks like a klass" heuristic (non-null, above the module base
// and below 0x00007FFF'FFFFFFFF).  Return false to stop the scan; return
// true to keep going.  The candidate is passed as an *address to
// validate* - the callback typically follows +0x10 to read a name and
// decides from there.
typedef bool (SAO_CORE_CALL* sao_il2cpp_klass_callback_t)(
    uint64_t candidate_klass_ptr,
    void* user_data);

// Sweep a byte range at 8-byte alignment, calling `callback` for each
// pointer that clears a minimal sanity filter (non-null, plausibly
// user-mode).  This is the generic version of the "self anchor" scans
// the Python side runs in Cython (`_sao_cy_memscan`) - callers control
// the validate step so the probe stays game-agnostic.
//
// Returns SAO_STATUS_OK when the whole region has been swept OR when the
// callback returned false (early stop is a normal outcome, not an error).
// `*visited_out` records how many candidates were reported to the
// callback (useful for progress diagnostics).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_scan_klass_pointers(
    sao_core_process_handle_t handle,
    uint64_t region_base,
    size_t region_size,
    sao_il2cpp_klass_callback_t callback,
    void* user_data,
    size_t* visited_out);

// Read the ASCII name string at `klass_ptr + 0x10`.  Truncates safely
// on buffers smaller than the C string, always null-terminates when
// `name_capacity > 0`, and returns SAO_STATUS_ERR_INVALID_ARGUMENT when
// the pointer is obviously bogus (< 0x10000 or misaligned).  The read
// tolerates short strings but caps at 256 characters - anything longer
// is a strong signal that the klass pointer is invalid.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_read_klass_name(
    sao_core_process_handle_t handle,
    uint64_t klass_ptr,
    char* out_name_utf8,
    size_t name_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif
