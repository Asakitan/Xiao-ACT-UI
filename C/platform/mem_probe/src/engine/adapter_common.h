// Adapter common — 内部 header, 只给 4 个 adapter cpp 用.
#pragma once

#include "sao/core/memory.h"
#include "sao/core/process.h"
#include "sao/core/status.h"
#include "sao/mem_probe/engine/adapter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sao::mem_probe::engine {

struct AdapterBase {
    sao_memprobe_engine_kind_t kind = SAO_MEMPROBE_ENGINE_UNKNOWN;
    sao_core_process_handle_t process = nullptr;
    // 缓存 module 列表 (bases + names) — adapter open 时一次填, 后续免二次遍历.
    struct Mod {
        std::string name;
        uint64_t base;
        uint64_t size;
    };
    std::vector<Mod> modules;

    virtual ~AdapterBase() = default;

    virtual sao_status_t list_classes(uint32_t* offsets, size_t max_off, size_t* off_ct,
                                       char* names, size_t names_cap, size_t* names_used) = 0;
    virtual sao_status_t find_class(const char* name, uint64_t* out) = 0;

    const Mod* find_module_contains(uint64_t addr) const {
        for (const auto& m : modules) {
            if (addr >= m.base && addr < m.base + m.size) return &m;
        }
        return nullptr;
    }
};

// Populated by each adapter's factory in its own cpp:
AdapterBase* open_il2cpp(sao_core_process_handle_t process);
AdapterBase* open_mono(sao_core_process_handle_t process);
AdapterBase* open_unreal(sao_core_process_handle_t process);
AdapterBase* open_native(sao_core_process_handle_t process);

// Utility: enumerate modules into an AdapterBase.
inline void populate_modules(AdapterBase& a) {
    SaoModuleEntry entries[512];
    char names[65536];
    size_t count = 0;
    size_t used = 0;
    if (sao_core_process_enum_modules(a.process, entries, 512, &count, names,
                                       sizeof(names), &used) != SAO_STATUS_OK)
        return;
    a.modules.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        AdapterBase::Mod m;
        m.name = std::string(names + entries[i].name_offset);
        m.base = entries[i].base_address;
        m.size = entries[i].module_size;
        a.modules.push_back(std::move(m));
    }
}

} // namespace sao::mem_probe::engine
