// SAO Auto - generic engine type detection.
//
// Mirrors the responsibilities of `mem_probe/engine/detector.py` and
// `mem_probe/engine/adapter.py`.  The detector answers a single question:
// "which runtime family owns this process's managed heap?"
//
// The API is deliberately game-agnostic.  It classifies by module presence
// only - no klass names, no metadata parsing, no protobuf schema.  That
// lives one layer up in a plugin (e.g. star_resonance_plugin).
//
// Contract:
//   * Input:  a live pid (does not require an open handle).
//   * Output: one of the SAO_ENGINE_* enum values.
//   * A native process with no known managed runtime resolves to
//     SAO_ENGINE_NATIVE (never fails, never returns "unknown").
//
// The header lives alongside sao/core/process.h because runtime detection
// piggybacks on `sao_core_process_enum_modules` for module discovery.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/process.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Engine family taxonomy.  Kept intentionally small - subtypes (Unity
// 2018/2019/... under IL2CPP; UE4 vs UE5 under UNREAL) are the responsibility
// of the per-engine probe layer, not the detector.
enum sao_engine_type_e : int32_t {
    SAO_ENGINE_UNKNOWN = 0,
    SAO_ENGINE_NATIVE  = 1,  // C/C++/Rust game, no managed runtime
    SAO_ENGINE_IL2CPP  = 2,  // Unity IL2CPP (GameAssembly.dll)
    SAO_ENGINE_MONO    = 3,  // Unity Mono (mono-2.0-bdwgc.dll / mono.dll)
    SAO_ENGINE_UNREAL  = 4,  // Unreal Engine (*-Win64-Shipping.exe or UE*.dll)
};

typedef int32_t sao_engine_type_t;

// Classify the given pid by walking its loaded module list.  Precedence
// matches the Python detector: IL2CPP > Mono > Unreal > Native.  A pid
// with no readable module list still resolves (falls back to
// SAO_ENGINE_UNKNOWN) rather than returning an error status; that keeps
// the API usable from probes that only have PROCESS_QUERY_LIMITED_INFORMATION.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_engine_detect(
    uint32_t pid, sao_engine_type_t* out_engine);

// Compact module descriptor - avoids the caller having to re-enumerate to
// get the "signature" module base+size.  base_address is 0 when the engine
// exposes no single authoritative module (e.g. native binaries return the
// main .exe module).  For IL2CPP this points at GameAssembly.dll.
struct SaoEngineModuleInfo {
    uint64_t base_address;
    uint64_t module_size;
    char     module_name_utf8[128];
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_engine_get_signature_module(
    uint32_t pid,
    sao_engine_type_t engine,
    SaoEngineModuleInfo* out_module);

// Human-readable engine name for logs / telemetry.  Never null; returns
// "unknown" for out-of-range values.  The buffer is at most 32 bytes
// including terminator so callers can size a stack array safely.
enum : size_t { SAO_ENGINE_TYPE_NAME_MAX = 32 };

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_engine_type_name(
    sao_engine_type_t engine,
    char* out_name_utf8,
    size_t name_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif
