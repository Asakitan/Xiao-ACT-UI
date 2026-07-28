// mem_probe/anchor_store.h — per-plugin JSON persistence of pointer chains.
//
// Path: %APPDATA%/SAOAuto/plugins/<plugin_id>/anchors.json
//   { "chains": [ { "name": "hp", "chain": <sao_memprobe_ptr_chain_t> } ] }

#pragma once

#include "sao/core/status.h"
#include "sao/mem_probe/pointer_chain.h"

#ifdef __cplusplus
extern "C" {
#endif

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_anchor_save(
    const char* plugin_id_utf8, const char* anchor_name_utf8,
    const sao_memprobe_ptr_chain_t* chain);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_anchor_load(
    const char* plugin_id_utf8, const char* anchor_name_utf8,
    sao_memprobe_ptr_chain_t* out_chain);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_anchor_delete(
    const char* plugin_id_utf8, const char* anchor_name_utf8);

#ifdef __cplusplus
} // extern "C"
#endif
