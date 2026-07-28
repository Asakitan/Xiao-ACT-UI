// engine/native_adapter.cpp — non-managed fallback: enumerate PE exports of
// executable module + list well-known symbols. Phase 6 skeleton.

#include "adapter_common.h"

#include <cstring>

namespace sao::mem_probe::engine {

class NativeAdapter : public AdapterBase {
public:
    NativeAdapter() { kind = SAO_MEMPROBE_ENGINE_NATIVE; }
    sao_status_t list_classes(uint32_t* offsets, size_t max_off, size_t* off_ct,
                               char* names, size_t names_cap,
                               size_t* names_used) override {
        // 对 native process, "class list" 意义有限。返回 module 列表作 fallback。
        if (offsets == nullptr || off_ct == nullptr || names == nullptr ||
            names_used == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        size_t written = 0;
        size_t used = 0;
        for (const auto& m : modules) {
            if (written >= max_off) break;
            if (used + m.name.size() + 1 > names_cap) break;
            offsets[written++] = static_cast<uint32_t>(used);
            std::memcpy(names + used, m.name.data(), m.name.size());
            used += m.name.size();
            names[used++] = '\0';
        }
        *off_ct = written;
        *names_used = used;
        return SAO_STATUS_OK;
    }
    sao_status_t find_class(const char* name, uint64_t* out) override {
        if (name == nullptr || out == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        for (const auto& m : modules) {
            if (m.name == name) {
                *out = m.base;
                return SAO_STATUS_OK;
            }
        }
        *out = 0;
        return SAO_STATUS_ERR_NOT_FOUND;
    }
};

AdapterBase* open_native(sao_core_process_handle_t p) {
    auto* a = new NativeAdapter();
    a->process = p;
    populate_modules(*a);
    return a;
}

} // namespace sao::mem_probe::engine
