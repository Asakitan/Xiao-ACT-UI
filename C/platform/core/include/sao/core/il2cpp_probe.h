// SAO Auto - generic IL2CPP runtime probe.
//
// The probe is the game-agnostic half of `mem_probe/engine/il2cpp_adapter.py`:
// it can locate the IL2CPP metadata region, sweep a memory range for
// candidate klass pointers, and read a klass' name field.  It **never**
// hard-codes a klass name, namespace, or field offset that belongs to a
// specific title.  Selecting which klass matters is the plugin's job -
// this header just gives the plugin the primitives.
//
// Runtime offsets are usable only after a versioned metadata header has been
// validated; a readable pointer alone is not layout evidence.
// Struct offsets follow the mainstream x64 IL2CPP layout observed across
// Unity 2020-2024 (metadata v27 / v29 / v31), matching the values used by
// the authoritative Python `il2cpp_adapter.py`:
//
//     Il2CppClass:
//       +0x00  Il2CppImage*    image
//       +0x08  void*           gc_desc
//       +0x10  const char*     name
//       +0x18  const char*     namespaze
//       +0x40  Il2CppClass*    element_class
//       +0x48  Il2CppClass*    cast_class
//       +0x58  Il2CppClass*    parent
//       +0x80  FieldInfo*      fields
//       +0x10C uint16_t        field_count
//
// Any of these can drift with new Unity revisions. V2 publishes a version
// only after parsing and validating a live metadata header; otherwise it
// returns SAO_STATUS_ERR_CAPABILITY_MISSING and clears the result.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/process.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum : uint32_t {
    SAO_IL2CPP_METADATA_VERSION_NOT_READY = 0u,
    SAO_IL2CPP_METADATA_VERSION_24 = 24u,
    SAO_IL2CPP_METADATA_VERSION_27 = 27u,
    SAO_IL2CPP_METADATA_VERSION_29 = 29u,
    SAO_IL2CPP_METADATA_VERSION_31 = 31u,
    SAO_IL2CPP_METADATA_VERSION_V1_MIN = 20u,
    SAO_IL2CPP_METADATA_VERSION_V1_MAX = 40u,
};

// Standard IL2CPP klass member offsets used by this probe.  Exported as
// constants so plugins can share the same "canonical" values instead of
// re-declaring them.  Any deviation is game-specific and lives in the
// plugin.
enum : uint32_t {
    SAO_IL2CPP_KLASS_NAME_OFFSET = 0x10,
    SAO_IL2CPP_KLASS_NAMESPACE_OFFSET = 0x18,
    SAO_IL2CPP_KLASS_PARENT_OFFSET = 0x58,
    SAO_IL2CPP_KLASS_FIELDS_OFFSET = 0x80,
    SAO_IL2CPP_KLASS_FIELD_COUNT_OFFSET = 0x10C,
    SAO_IL2CPP_FIELDINFO_NAME_OFFSET = 0x00,
    SAO_IL2CPP_FIELDINFO_OFFSET_OFFSET = 0x18,
    SAO_IL2CPP_FIELDINFO_STRIDE = 0x20,
};

// V1 probe result. Version recovery is best-effort across metadata versions 20..40.
// This layout is retained for existing callers and must not
// receive appended fields. `metadata_rva` is the module-relative RVA of the
// resolved `il2cpp_domain_get` export anchor.
struct SaoIl2CppProbe {
    uint64_t metadata_rva;
    uint32_t metadata_version;
    uint32_t klass_stride_hint;  // usually 0 - reserved for future use
    uint64_t domain_ptr;         // resolved from il2cpp_domain_get if decodable
    uint64_t game_assembly_base; // convenience echo of the module base
};

enum : uint32_t {
    SAO_IL2CPP_PROBE_V2_ABI_VERSION = 2u,
    SAO_IL2CPP_PROBE_ABI_VERSION = SAO_IL2CPP_PROBE_V2_ABI_VERSION,
};

// Independent size/versioned extension. The V1 fields remain a prefix so an
// internal result can be projected back without writing past a V1 object.
struct SaoIl2CppProbeV2 {
    uint64_t metadata_rva;
    uint32_t metadata_version;
    uint32_t klass_stride_hint;
    uint64_t domain_ptr;
    uint64_t game_assembly_base;
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t metadata_version_valid; // 0 = not-ready, 1 = parsed/validated
    uint32_t _reserved;
    uint64_t metadata_header_rva; // module-relative only when in-module
    uint32_t metadata_binding;
    uint32_t _reserved2;
};

enum : uint32_t {
    SAO_IL2CPP_METADATA_BINDING_NONE = 0u,
    SAO_IL2CPP_METADATA_BINDING_MAPPED_FILE = 1u,
    SAO_IL2CPP_METADATA_BINDING_RUNTIME_REGISTRATION = 2u,
};

typedef struct sao_il2cpp_discovery_budget_s {
    uint64_t bytes_remaining;
    uint32_t reads_remaining;
    uint32_t _reserved;
    uint64_t deadline_ms;
} sao_il2cpp_discovery_budget_t;

SAO_CORE_API void SAO_CORE_CALL
sao_core_il2cpp_discovery_budget_init(sao_il2cpp_discovery_budget_t* budget, uint64_t max_bytes,
                                      uint32_t max_reads, uint32_t max_duration_ms);

// Probe the IL2CPP metadata inside the given (already opened) process
// handle.  `module_base` must be the base address of GameAssembly.dll -
// callers use `sao_core_engine_get_signature_module` to obtain it.  On
// success the probe populates `*probe_out`; on failure it clears the struct
// and returns a non-OK status. V2 returns SAO_STATUS_ERR_CAPABILITY_MISSING
// when a validated metadata/runtime layout is unavailable.
//
// The probe is intentionally passive: it reads the PE export directory
// and (when the domain export decodes cleanly) dereferences the domain
// pointer.  It does NOT walk the class tree, and it never writes into
// the target.  Cost: at most a handful of NtReadVirtualMemory calls.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata(
    sao_core_process_handle_t handle, uint64_t module_base, SaoIl2CppProbe* probe_out);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata_v2(
    sao_core_process_handle_t handle, uint64_t module_base, SaoIl2CppProbeV2* probe_out);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata_v2_with_budget(
    sao_core_process_handle_t handle, uint64_t module_base, sao_il2cpp_discovery_budget_t* budget,
    SaoIl2CppProbeV2* probe_out);

// Resolve a named PE export as a module-relative RVA. This is the general
// export-anchor capability; `metadata_rva` remains the legacy
// `il2cpp_domain_get` anchor returned by the metadata probe.
SAO_CORE_API sao_status_t SAO_CORE_CALL
sao_core_il2cpp_resolve_export_anchor(sao_core_process_handle_t handle, uint64_t module_base,
                                      const char* export_name, uint64_t* out_rva);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_il2cpp_resolve_export_anchor_with_budget(
    sao_core_process_handle_t handle, uint64_t module_base, const char* export_name,
    sao_il2cpp_discovery_budget_t* budget, uint64_t* out_rva);

// Callback for `sao_core_il2cpp_scan_klass_pointers`.  Fired for every
// 8-byte aligned candidate pointer inside the scan region that satisfies
// the "looks like a klass" heuristic (non-null, above the module base
// and below 0x00007FFF'FFFFFFFF).  Return false to stop the scan; return
// true to keep going.  The candidate is passed as an *address to
// validate* - the callback typically follows +0x10 to read a name and
// decides from there.
typedef bool(SAO_CORE_CALL* sao_il2cpp_klass_callback_t)(uint64_t candidate_klass_ptr,
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
    sao_core_process_handle_t handle, uint64_t region_base, size_t region_size,
    sao_il2cpp_klass_callback_t callback, void* user_data, size_t* visited_out);

// Read the ASCII name string at `klass_ptr + 0x10`. Always clears the
// output on failure, null-terminates successful output when
// `name_capacity > 0`, and returns SAO_STATUS_ERR_INVALID_ARGUMENT when
// the pointer is obviously bogus (< 0x10000 or misaligned).  The read
// tolerates short strings but caps at 256 characters - anything longer
// is a strong signal that the klass pointer is invalid.
SAO_CORE_API sao_status_t SAO_CORE_CALL
sao_core_il2cpp_read_klass_name(sao_core_process_handle_t handle, uint64_t klass_ptr,
                                char* out_name_utf8, size_t name_capacity);

#ifdef __cplusplus
} // extern "C"
#endif
