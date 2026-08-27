// engine/detector.cpp — engine kind detection from module names + C API wrappers.
//
// Phase 6.

#include "adapter_common.h"
#include "sao/core/il2cpp_probe.h"

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
std::string basename_lower(std::string value) {
    const size_t slash = value.find_last_of("\\/");
    if (slash != std::string::npos)
        value.erase(0, slash + 1);
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

bool ends_with(const std::string& value, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

bool il2cpp_basic_available(sao_core_process_handle_t process, uint64_t base) {
    SaoIl2CppProbeV2 probe{};
    sao_il2cpp_discovery_budget_t budget{};
    sao_core_il2cpp_discovery_budget_init(&budget, 64ull * 1024ull * 1024ull, 8192u, 750u);
    const sao_status_t status =
        sao_core_il2cpp_probe_metadata_v2_with_budget(process, base, &budget, &probe);
    if (status != SAO_STATUS_OK || probe.metadata_version_valid != 1u ||
        (probe.metadata_binding != SAO_IL2CPP_METADATA_BINDING_MAPPED_FILE &&
         probe.metadata_binding != SAO_IL2CPP_METADATA_BINDING_RUNTIME_REGISTRATION) ||
        (probe.metadata_version != SAO_IL2CPP_METADATA_VERSION_24 &&
         probe.metadata_version != SAO_IL2CPP_METADATA_VERSION_27 &&
         probe.metadata_version != SAO_IL2CPP_METADATA_VERSION_29 &&
         probe.metadata_version != SAO_IL2CPP_METADATA_VERSION_31) ||
        probe.domain_ptr < 0x10000ull || probe.domain_ptr > 0x00007FFFFFFFFFFFull ||
        (probe.domain_ptr & 7ull) != 0)
        return false;
    return true;
}
} // namespace

extern "C" sao_memprobe_engine_kind_t SAO_CORE_CALL
sao_memprobe_engine_detect(sao_core_process_handle_t process) {
    try {
        if (process == nullptr)
            return SAO_MEMPROBE_ENGINE_UNKNOWN;
        SaoModuleEntry entries[256];
        char names[65536];
        size_t entry_count = 0;
        size_t names_used = 0;
        if (sao_core_process_enum_modules(process, entries, 256, &entry_count, names, sizeof(names),
                                          &names_used) != SAO_STATUS_OK)
            return SAO_MEMPROBE_ENGINE_UNKNOWN;
        for (size_t i = 0; i < entry_count; ++i) {
            if (entries[i].name_offset >= names_used)
                continue;
            const size_t remaining = names_used - entries[i].name_offset;
            const char* raw = names + entries[i].name_offset;
            if (std::memchr(raw, '\0', remaining) == nullptr)
                continue;
            const std::string name = basename_lower(raw);
            if ((name == "gameassembly.dll" || name == "il2cpp.dll") &&
                il2cpp_basic_available(process, entries[i].base_address))
                return SAO_MEMPROBE_ENGINE_IL2CPP;
        }
        for (size_t i = 0; i < entry_count; ++i) {
            if (entries[i].name_offset >= names_used)
                continue;
            const size_t remaining = names_used - entries[i].name_offset;
            const char* raw = names + entries[i].name_offset;
            if (std::memchr(raw, '\0', remaining) == nullptr)
                continue;
            const std::string name = basename_lower(raw);
            if (name == "mono.dll" || name == "mono-2.0.dll" || name == "mono-2.0-bdwgc.dll" ||
                name == "mono-2.0-sgen.dll" || name == "monobdwgc.dll")
                return SAO_MEMPROBE_ENGINE_MONO;
        }
        for (size_t i = 0; i < entry_count; ++i) {
            if (entries[i].name_offset >= names_used)
                continue;
            const size_t remaining = names_used - entries[i].name_offset;
            const char* raw = names + entries[i].name_offset;
            if (std::memchr(raw, '\0', remaining) == nullptr)
                continue;
            const std::string name = basename_lower(raw);
            if (name == "ue4editor.exe" || name == "ue5editor.exe" || name == "unrealengine.dll" ||
                name == "ue4game.dll" || name == "ue5game.dll" ||
                ends_with(name, "-win64-shipping.exe"))
                return SAO_MEMPROBE_ENGINE_UNREAL;
        }
        return SAO_MEMPROBE_ENGINE_NATIVE;
    } catch (...) {
        return SAO_MEMPROBE_ENGINE_UNKNOWN;
    }
}

// ── C API wrappers ─────────────────────────────────────────

extern "C" sao_memprobe_engine_adapter_t* SAO_CORE_CALL
sao_memprobe_engine_open(sao_core_process_handle_t process, sao_memprobe_engine_kind_t kind) {
    try {
        if (process == nullptr || kind == SAO_MEMPROBE_ENGINE_UNKNOWN)
            return nullptr;
        if (sao_memprobe_engine_detect(process) != kind)
            return nullptr;
        AdapterBase* a = nullptr;
        switch (kind) {
        case SAO_MEMPROBE_ENGINE_IL2CPP:
            a = open_il2cpp(process);
            break;
        case SAO_MEMPROBE_ENGINE_MONO:
            a = open_mono(process);
            break;
        case SAO_MEMPROBE_ENGINE_UNREAL:
            a = open_unreal(process);
            break;
        case SAO_MEMPROBE_ENGINE_NATIVE:
            a = open_native(process);
            break;
        default:
            return nullptr;
        }
        return reinterpret_cast<sao_memprobe_engine_adapter_t*>(a);
    } catch (...) {
        return nullptr;
    }
}

extern "C" void SAO_CORE_CALL sao_memprobe_engine_close(sao_memprobe_engine_adapter_t* adapter) {
    try {
        delete reinterpret_cast<AdapterBase*>(adapter);
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_engine_list_classes(
    sao_memprobe_engine_adapter_t* adapter, uint32_t* out_offsets, size_t max_offsets,
    size_t* out_offset_count, char* out_names_utf8, size_t names_capacity, size_t* out_names_used) {
    if (out_offset_count != nullptr)
        *out_offset_count = 0;
    if (out_names_used != nullptr)
        *out_names_used = 0;
    try {
        if (adapter == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return reinterpret_cast<AdapterBase*>(adapter)->list_classes(
            out_offsets, max_offsets, out_offset_count, out_names_utf8, names_capacity,
            out_names_used);
    } catch (...) {
        if (out_offset_count != nullptr)
            *out_offset_count = 0;
        if (out_names_used != nullptr)
            *out_names_used = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_engine_find_class(
    sao_memprobe_engine_adapter_t* adapter, const char* class_name_utf8, uint64_t* out_class_ptr) {
    if (out_class_ptr != nullptr)
        *out_class_ptr = 0;
    try {
        if (adapter == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return reinterpret_cast<AdapterBase*>(adapter)->find_class(class_name_utf8, out_class_ptr);
    } catch (...) {
        if (out_class_ptr != nullptr)
            *out_class_ptr = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
