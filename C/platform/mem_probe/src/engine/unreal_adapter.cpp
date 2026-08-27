// engine/unreal_adapter.cpp — engine capability probe.
// Class enumeration and class lookup remain capability-gated until a real
// runtime class-walk contract is available.

#include "adapter_common.h"

#include "sao/core/memory.h"

#include <cctype>
#include <cstring>
#include <string>

namespace sao::mem_probe::engine {

class UnrealAdapter : public AdapterBase {
public:
    UnrealAdapter() { kind = SAO_MEMPROBE_ENGINE_UNREAL; }

    uint64_t game_module_base() const {
        for (const auto& m : modules) {
            std::string n = m.name;
            for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.find("-shipping") != std::string::npos ||
                n.find("ue4editor") != std::string::npos ||
                n.find("ue5editor") != std::string::npos ||
                n.find("unrealengine") != std::string::npos)
                return m.base;
        }
        return modules.empty() ? 0 : modules.front().base;
    }

    sao_status_t list_classes(uint32_t* offsets, size_t max_off, size_t* off_ct,
                               char* names, size_t names_cap,
                               size_t* names_used) override {
        if (offsets == nullptr || off_ct == nullptr || names == nullptr ||
            names_used == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *off_ct = 0;
        *names_used = 0;
        if (sao_memprobe_engine_detect(process) != SAO_MEMPROBE_ENGINE_UNREAL)
            return SAO_STATUS_ERR_NOT_FOUND;
        uint64_t base = game_module_base();
        if (base == 0) return SAO_STATUS_ERR_NOT_FOUND;
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }
    sao_status_t find_class(const char* name, uint64_t* out) override {
        if (out != nullptr) *out = 0;
        if (name == nullptr || out == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (sao_memprobe_engine_detect(process) != SAO_MEMPROBE_ENGINE_UNREAL)
            return SAO_STATUS_ERR_NOT_FOUND;
        if (game_module_base() == 0) return SAO_STATUS_ERR_NOT_FOUND;
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }
};

AdapterBase* open_unreal(sao_core_process_handle_t p) {
    auto* a = new UnrealAdapter();
    a->process = p;
    populate_modules(*a);
    return a;
}

} // namespace sao::mem_probe::engine
