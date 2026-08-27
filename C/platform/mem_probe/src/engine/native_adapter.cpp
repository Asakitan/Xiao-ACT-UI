// engine/native_adapter.cpp — non-managed fallback.

#include "adapter_common.h"

namespace sao::mem_probe::engine {

class NativeAdapter : public AdapterBase {
public:
    NativeAdapter() { kind = SAO_MEMPROBE_ENGINE_NATIVE; }

    sao_status_t list_classes(uint32_t* offsets, size_t max_off, size_t* off_ct,
                               char* names, size_t names_cap,
                               size_t* names_used) override {
        if (offsets == nullptr || off_ct == nullptr || names == nullptr ||
            names_used == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *off_ct = 0;
        *names_used = 0;
        if (sao_memprobe_engine_detect(process) != SAO_MEMPROBE_ENGINE_NATIVE)
            return SAO_STATUS_ERR_NOT_FOUND;
        (void)offsets;
        (void)max_off;
        (void)names;
        (void)names_cap;
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }

    sao_status_t find_class(const char* name, uint64_t* out) override {
        if (name == nullptr || out == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (sao_memprobe_engine_detect(process) != SAO_MEMPROBE_ENGINE_NATIVE)
            return SAO_STATUS_ERR_NOT_FOUND;
        *out = 0;
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }
};

AdapterBase* open_native(sao_core_process_handle_t p) {
    auto* a = new NativeAdapter();
    a->process = p;
    populate_modules(*a);
    return a;
}

} // namespace sao::mem_probe::engine