// mem_probe/engine/adapter.h — game engine SDK probe abstraction.
//
// Phase 6 (Python parity closure) — port of python/mem_probe/engine/*.py.
// Detects and walks IL2CPP / Mono / Unreal / Native runtimes purely via
// user-mode ReadProcessMemory. Zero injection.

#pragma once

#include "sao/core/process.h"
#include "sao/core/status.h"

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sao_memprobe_engine_kind_e {
    SAO_MEMPROBE_ENGINE_UNKNOWN = 0,
    SAO_MEMPROBE_ENGINE_IL2CPP,
    SAO_MEMPROBE_ENGINE_MONO,
    SAO_MEMPROBE_ENGINE_UNREAL,
    SAO_MEMPROBE_ENGINE_NATIVE,
} sao_memprobe_engine_kind_t;

typedef struct sao_memprobe_engine_adapter_s sao_memprobe_engine_adapter_t;

// Detect engine kind from process (checks module names + exported symbols).
SAO_CORE_API sao_memprobe_engine_kind_t SAO_CORE_CALL
sao_memprobe_engine_detect(sao_core_process_handle_t process);

// Open a suitable adapter (returns NULL if engine unavailable).
SAO_CORE_API sao_memprobe_engine_adapter_t* SAO_CORE_CALL
sao_memprobe_engine_open(sao_core_process_handle_t process,
                         sao_memprobe_engine_kind_t kind);

SAO_CORE_API void SAO_CORE_CALL
sao_memprobe_engine_close(sao_memprobe_engine_adapter_t* adapter);

// Enumerate classes (writes UTF-8 names back-to-back into out_names_utf8).
// Names are null-terminated; out_offsets[i] indexes the i-th name.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_engine_list_classes(
    sao_memprobe_engine_adapter_t* adapter,
    uint32_t* out_offsets,
    size_t max_offsets,
    size_t* out_offset_count,
    char* out_names_utf8,
    size_t names_capacity,
    size_t* out_names_used);

// Get the runtime address of a named class (l2CPP klass* / MonoClass* / UClass*).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_engine_find_class(
    sao_memprobe_engine_adapter_t* adapter, const char* class_name_utf8,
    uint64_t* out_class_ptr);

#ifdef __cplusplus
} // extern "C"
#endif
