// Wave 6 - generic engine type detection.  Mirrors the module-name
// classification `mem_probe/engine/detector.py` performs against
// `process.list_modules()`.  Never calls into the target's memory - the
// detector is intentionally cheap so probes on hundreds of pids stay
// snappy.

#include "sao/core/engine_detector.h"

#include "sao/core/error.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

extern void sao_core_set_last_os_error_internal(uint32_t os_error);

namespace {

sao_status_t fail_engine(sao_status_t status,
                         const char* message,
                         const char* file,
                         uint32_t line) {
    sao_core_error_set(status, "core.engine_detector", message, file, line);
    return status;
}

// Case-insensitive substring search.  Returns true if `needle` occurs
// anywhere inside `haystack`.
bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return needle.empty();
    }
    for (size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool matched = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            const char left = static_cast<char>(
                std::tolower(static_cast<unsigned char>(haystack[start + index])));
            const char right = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[index])));
            if (left != right) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

// Enumerate modules for the given pid via the platform's process helper.
// Returns an empty vector on any error (module snapshot failures happen
// legitimately for protected processes or during teardown).
struct ModuleRow {
    uint64_t base;
    uint64_t size;
    std::string name;
};

std::vector<ModuleRow> enumerate_modules(uint32_t pid) {
    std::vector<ModuleRow> out;
    sao_core_process_handle_t handle = nullptr;
    // Modules only need PROCESS_QUERY_LIMITED_INFORMATION - INFO access is
    // sufficient, PROCESS_VM_READ would fail on higher-privilege targets.
    const sao_status_t open_rc =
        sao_core_process_open(pid, SAO_PROCESS_ACCESS_INFO, &handle);
    if (open_rc != SAO_STATUS_OK) {
        // Clear the last-error record: this is a probe, not a failure.
        sao_core_error_clear();
        return out;
    }

    size_t entry_count = 0;
    size_t names_used = 0;
    sao_status_t size_rc = sao_core_process_enum_modules(
        handle, nullptr, 0, &entry_count, nullptr, 0, &names_used);
    if (size_rc != SAO_STATUS_OK || entry_count == 0) {
        sao_core_error_clear();
        sao_core_process_close(handle);
        return out;
    }

    std::vector<SaoModuleEntry> entries(entry_count);
    std::vector<char> names(names_used == 0 ? 1u : names_used);
    const sao_status_t enum_rc = sao_core_process_enum_modules(
        handle, entries.data(), entries.size(), &entry_count,
        names.data(), names.size(), &names_used);
    sao_core_process_close(handle);
    if (enum_rc != SAO_STATUS_OK) {
        sao_core_error_clear();
        return out;
    }

    out.reserve(entry_count);
    for (size_t index = 0; index < entry_count; ++index) {
        const SaoModuleEntry& row = entries[index];
        const char* name_start = names.data() + row.name_offset;
        out.push_back(ModuleRow{
            row.base_address,
            row.module_size,
            std::string(name_start),
        });
    }
    return out;
}

// Test whether a filename looks like an Unreal Engine shipping binary,
// matching the convention `<GameName>-Win64-Shipping.exe` used by every
// packaged UE4/UE5 build.  Case-insensitive.
bool looks_like_unreal_shipping(std::string_view module_name) {
    return contains_ci(module_name, "-Win64-Shipping");
}

// Classify one module row against the known engine signatures.  Returns
// SAO_ENGINE_UNKNOWN when no signature matches so the caller can move on
// to the next module.
sao_engine_type_t classify_module(std::string_view module_name) {
    if (contains_ci(module_name, "gameassembly.dll")) {
        return SAO_ENGINE_IL2CPP;
    }
    if (contains_ci(module_name, "mono-2.0-bdwgc.dll") ||
        contains_ci(module_name, "mono-2.0-boehm.dll") ||
        contains_ci(module_name, "mono.dll")) {
        return SAO_ENGINE_MONO;
    }
    // UE4/UE5: either a UnrealEngine-branded DLL (rare - most builds
    // ship monolithic) or the shipping executable naming convention.
    if (contains_ci(module_name, "unrealengine") ||
        contains_ci(module_name, "ue4-") ||
        contains_ci(module_name, "ue5-") ||
        looks_like_unreal_shipping(module_name)) {
        return SAO_ENGINE_UNREAL;
    }
    return SAO_ENGINE_UNKNOWN;
}

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_engine_detect(
    uint32_t pid, sao_engine_type_t* out_engine) {
    if (out_engine == nullptr) {
        return fail_engine(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           "out_engine is null", __FILE__, __LINE__);
    }
    *out_engine = SAO_ENGINE_NATIVE;
    if (pid == 0) {
        return fail_engine(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           "pid is zero", __FILE__, __LINE__);
    }

    const std::vector<ModuleRow> modules = enumerate_modules(pid);
    if (modules.empty()) {
        // No readable modules => still valid; treat as native.  This
        // keeps the API infallible so callers can iterate arbitrary pids
        // without try/except gymnastics.
        return SAO_STATUS_OK;
    }

    // Precedence: IL2CPP > MONO > UNREAL > NATIVE.  Walk once, keep the
    // strongest match seen so far.
    sao_engine_type_t best = SAO_ENGINE_NATIVE;
    for (const ModuleRow& row : modules) {
        const sao_engine_type_t kind = classify_module(row.name);
        // Custom precedence ranking so we do not have to rely on the
        // numeric values.
        auto rank = [](sao_engine_type_t engine) -> int {
            switch (engine) {
                case SAO_ENGINE_IL2CPP: return 4;
                case SAO_ENGINE_MONO:   return 3;
                case SAO_ENGINE_UNREAL: return 2;
                case SAO_ENGINE_NATIVE: return 1;
                default:                return 0;
            }
        };
        if (rank(kind) > rank(best)) {
            best = kind;
        }
    }
    *out_engine = best;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_engine_get_signature_module(
    uint32_t pid,
    sao_engine_type_t engine,
    SaoEngineModuleInfo* out_module) {
    if (out_module == nullptr) {
        return fail_engine(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           "out_module is null", __FILE__, __LINE__);
    }
    *out_module = SaoEngineModuleInfo{};
    if (pid == 0) {
        return fail_engine(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           "pid is zero", __FILE__, __LINE__);
    }

    const std::vector<ModuleRow> modules = enumerate_modules(pid);
    if (modules.empty()) {
        return fail_engine(SAO_STATUS_ERR_MODULE_NOT_FOUND,
                           "module snapshot empty", __FILE__, __LINE__);
    }

    // Predicate for the signature module of each engine.  For NATIVE we
    // fall through to the first module (main executable).
    auto matches = [engine](std::string_view name) -> bool {
        switch (engine) {
            case SAO_ENGINE_IL2CPP:
                return contains_ci(name, "gameassembly.dll");
            case SAO_ENGINE_MONO:
                return contains_ci(name, "mono-2.0-bdwgc.dll") ||
                       contains_ci(name, "mono-2.0-boehm.dll") ||
                       contains_ci(name, "mono.dll");
            case SAO_ENGINE_UNREAL:
                return contains_ci(name, "unrealengine") ||
                       contains_ci(name, "ue4-") ||
                       contains_ci(name, "ue5-") ||
                       looks_like_unreal_shipping(name);
            default:
                return false;
        }
    };

    for (const ModuleRow& row : modules) {
        if (engine == SAO_ENGINE_NATIVE || matches(row.name)) {
            out_module->base_address = row.base;
            out_module->module_size = row.size;
            const size_t copy_len = std::min<size_t>(
                sizeof(out_module->module_name_utf8) - 1, row.name.size());
            memcpy(out_module->module_name_utf8, row.name.c_str(), copy_len);
            out_module->module_name_utf8[copy_len] = '\0';
            return SAO_STATUS_OK;
        }
    }

    return fail_engine(SAO_STATUS_ERR_MODULE_NOT_FOUND,
                       "signature module not present", __FILE__, __LINE__);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_engine_type_name(
    sao_engine_type_t engine,
    char* out_name_utf8,
    size_t name_capacity) {
    if (out_name_utf8 == nullptr || name_capacity == 0) {
        return fail_engine(SAO_STATUS_ERR_INVALID_ARGUMENT,
                           "output buffer is null", __FILE__, __LINE__);
    }
    const char* value = nullptr;
    switch (engine) {
        case SAO_ENGINE_NATIVE: value = "native"; break;
        case SAO_ENGINE_IL2CPP: value = "il2cpp"; break;
        case SAO_ENGINE_MONO:   value = "unity_mono"; break;
        case SAO_ENGINE_UNREAL: value = "unreal"; break;
        case SAO_ENGINE_UNKNOWN:
        default:
            value = "unknown";
            break;
    }
    const size_t needed = std::strlen(value) + 1;
    if (name_capacity < needed) {
        return fail_engine(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                           "engine name buffer too small",
                           __FILE__, __LINE__);
    }
    memcpy(out_name_utf8, value, needed);
    return SAO_STATUS_OK;
}
