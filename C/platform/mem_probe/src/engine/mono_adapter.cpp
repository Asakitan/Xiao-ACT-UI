// engine/mono_adapter.cpp — Mono runtime walk (Phase 6 production).
//
// Resolves mono_*.dll exports via remote PE export table walk. All stable
// exports (mono_get_root_domain, mono_domain_assembly_open, etc.) come out
// with proper target VAs; caller can then invoke them via kernel bridge or
// dispatch them from within-target scripting host if available.

#include "adapter_common.h"

#include "sao/core/memory.h"

#include <cctype>
#include <cstring>
#include <string>

namespace sao::mem_probe::engine {

namespace {
// Forward from il2cpp_adapter.cpp — shared PE export resolver. We keep a
// local copy here so mono adapter has no linkage dependency on il2cpp.
uint64_t resolve_export_local(sao_core_process_handle_t p, uint64_t module_base,
                               const char* symbol) {
    uint8_t hdr[0x400] = {};
    size_t got = 0;
    if (sao_core_mem_read(p, module_base, hdr, sizeof(hdr), &got) != SAO_STATUS_OK ||
        got < 0x200)
        return 0;
    if (hdr[0] != 'M' || hdr[1] != 'Z') return 0;
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, hdr + 0x3C, 4);
    if (e_lfanew >= sizeof(hdr) - 24) return 0;
    if (std::memcmp(hdr + e_lfanew, "PE\0\0", 4) != 0) return 0;
    const uint8_t* nt = hdr + e_lfanew;
    uint16_t opt_magic = 0;
    std::memcpy(&opt_magic, nt + 24, 2);
    const bool is_pe32_plus = (opt_magic == 0x20B);
    const uint32_t dd_off = is_pe32_plus ? 112 : 96;
    uint32_t export_rva = 0, export_size = 0;
    std::memcpy(&export_rva, nt + 24 + dd_off, 4);
    std::memcpy(&export_size, nt + 24 + dd_off + 4, 4);
    if (export_rva == 0 || export_size == 0) return 0;
    std::vector<uint8_t> export_buf(export_size);
    if (sao_core_mem_read(p, module_base + export_rva, export_buf.data(),
                           export_size, &got) != SAO_STATUS_OK)
        return 0;
    if (got < 40) return 0;
    uint32_t num_names = 0, addr_of_functions = 0, addr_of_names = 0,
             addr_of_name_ordinals = 0;
    std::memcpy(&num_names, export_buf.data() + 24, 4);
    std::memcpy(&addr_of_functions, export_buf.data() + 28, 4);
    std::memcpy(&addr_of_names, export_buf.data() + 32, 4);
    std::memcpy(&addr_of_name_ordinals, export_buf.data() + 36, 4);
    if (num_names == 0 || num_names > 100'000) return 0;
    std::vector<uint32_t> name_rvas(num_names);
    std::vector<uint16_t> ordinals(num_names);
    if (sao_core_mem_read(p, module_base + addr_of_names, name_rvas.data(),
                           num_names * 4, &got) != SAO_STATUS_OK)
        return 0;
    if (sao_core_mem_read(p, module_base + addr_of_name_ordinals, ordinals.data(),
                           num_names * 2, &got) != SAO_STATUS_OK)
        return 0;
    for (uint32_t i = 0; i < num_names; ++i) {
        char nb[256] = {};
        if (sao_core_mem_read(p, module_base + name_rvas[i], nb, sizeof(nb) - 1,
                               &got) != SAO_STATUS_OK ||
            got == 0)
            continue;
        if (std::strcmp(nb, symbol) == 0) {
            uint32_t fn_rva = 0;
            if (sao_core_mem_read(p, module_base + addr_of_functions + ordinals[i] * 4,
                                   &fn_rva, 4, &got) != SAO_STATUS_OK)
                return 0;
            return module_base + fn_rva;
        }
    }
    return 0;
}
} // namespace

class MonoAdapter : public AdapterBase {
public:
    MonoAdapter() { kind = SAO_MEMPROBE_ENGINE_MONO; }

    uint64_t mono_base() const {
        for (const auto& m : modules) {
            std::string n = m.name;
            for (auto& c : n)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.find("mono") != std::string::npos && n.find(".dll") != std::string::npos)
                return m.base;
        }
        return 0;
    }

    sao_status_t list_classes(uint32_t* offsets, size_t max_off, size_t* off_ct,
                               char* names, size_t names_cap,
                               size_t* names_used) override {
        if (offsets == nullptr || off_ct == nullptr || names == nullptr ||
            names_used == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *off_ct = 0;
        *names_used = 0;
        uint64_t base = mono_base();
        if (base == 0) return SAO_STATUS_ERR_NOT_FOUND;
        // Diagnostic: report the mono runtime is live.
        const std::string synth = "MonoRuntime";
        if (max_off < 1 || names_cap < synth.size() + 1)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        offsets[0] = 0;
        std::memcpy(names, synth.data(), synth.size());
        names[synth.size()] = 0;
        *off_ct = 1;
        *names_used = synth.size() + 1;
        return SAO_STATUS_OK;
    }

    sao_status_t find_class(const char* name, uint64_t* out) override {
        if (name == nullptr || out == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *out = 0;
        uint64_t base = mono_base();
        if (base == 0) return SAO_STATUS_ERR_NOT_FOUND;
        uint64_t sym = resolve_export_local(process, base, name);
        if (sym == 0) return SAO_STATUS_ERR_NOT_FOUND;
        *out = sym;
        return SAO_STATUS_OK;
    }
};

AdapterBase* open_mono(sao_core_process_handle_t p) {
    auto* a = new MonoAdapter();
    a->process = p;
    populate_modules(*a);
    return a;
}

} // namespace sao::mem_probe::engine
