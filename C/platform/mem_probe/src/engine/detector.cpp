// engine/detector.cpp — engine kind detection from module names + C API wrappers.
//
// Phase 6.

#include "adapter_common.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using sao::mem_probe::engine::AdapterBase;
using sao::mem_probe::engine::open_il2cpp;
using sao::mem_probe::engine::open_mono;
using sao::mem_probe::engine::open_native;
using sao::mem_probe::engine::open_unreal;

namespace {
bool contains_ci(const std::string& hay, const char* needle) {
    std::string a = hay, b = needle;
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}
} // namespace

extern "C" sao_memprobe_engine_kind_t SAO_CORE_CALL
sao_memprobe_engine_detect(sao_core_process_handle_t process) {
    if (process == nullptr) return SAO_MEMPROBE_ENGINE_UNKNOWN;
    SaoModuleEntry entries[256];
    char names[65536];
    size_t entry_count = 0;
    size_t names_used = 0;
    if (sao_core_process_enum_modules(process, entries, 256, &entry_count, names,
                                       sizeof(names), &names_used) != SAO_STATUS_OK)
        return SAO_MEMPROBE_ENGINE_UNKNOWN;
    for (size_t i = 0; i < entry_count; ++i) {
        std::string n(names + entries[i].name_offset);
        if (contains_ci(n, "gameassembly") ||
            contains_ci(n, "il2cpp"))
            return SAO_MEMPROBE_ENGINE_IL2CPP;
    }
    for (size_t i = 0; i < entry_count; ++i) {
        std::string n(names + entries[i].name_offset);
        if (contains_ci(n, "mono-2.0") || contains_ci(n, "mono.dll") ||
            contains_ci(n, "monobdwgc"))
            return SAO_MEMPROBE_ENGINE_MONO;
    }
    for (size_t i = 0; i < entry_count; ++i) {
        std::string n(names + entries[i].name_offset);
        if (contains_ci(n, "ue4editor") || contains_ci(n, "ue5editor") ||
            contains_ci(n, "unrealengine") || contains_ci(n, "-win64-shipping"))
            return SAO_MEMPROBE_ENGINE_UNREAL;
    }
    return SAO_MEMPROBE_ENGINE_NATIVE;
}

// ── C API wrappers ─────────────────────────────────────────

extern "C" sao_memprobe_engine_adapter_t* SAO_CORE_CALL
sao_memprobe_engine_open(sao_core_process_handle_t process,
                         sao_memprobe_engine_kind_t kind) {
    if (process == nullptr) return nullptr;
    AdapterBase* a = nullptr;
    switch (kind) {
    case SAO_MEMPROBE_ENGINE_IL2CPP: a = open_il2cpp(process); break;
    case SAO_MEMPROBE_ENGINE_MONO:   a = open_mono(process); break;
    case SAO_MEMPROBE_ENGINE_UNREAL: a = open_unreal(process); break;
    case SAO_MEMPROBE_ENGINE_NATIVE: a = open_native(process); break;
    default: return nullptr;
    }
    return reinterpret_cast<sao_memprobe_engine_adapter_t*>(a);
}

extern "C" void SAO_CORE_CALL
sao_memprobe_engine_close(sao_memprobe_engine_adapter_t* adapter) {
    delete reinterpret_cast<AdapterBase*>(adapter);
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_engine_list_classes(
    sao_memprobe_engine_adapter_t* adapter, uint32_t* out_offsets,
    size_t max_offsets, size_t* out_offset_count, char* out_names_utf8,
    size_t names_capacity, size_t* out_names_used) {
    if (adapter == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return reinterpret_cast<AdapterBase*>(adapter)->list_classes(
        out_offsets, max_offsets, out_offset_count, out_names_utf8,
        names_capacity, out_names_used);
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_engine_find_class(
    sao_memprobe_engine_adapter_t* adapter, const char* class_name_utf8,
    uint64_t* out_class_ptr) {
    if (adapter == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return reinterpret_cast<AdapterBase*>(adapter)->find_class(class_name_utf8,
                                                                out_class_ptr);
}
