// Wave 6 - generic IL2CPP runtime probe.  Delivers the primitives the
// Python `il2cpp_adapter.py` uses (PE export walk to locate
// `il2cpp_domain_get`, name pointer read at klass+0x10, safe pointer
// scan) without embedding any title-specific klass name.

#include "sao/core/il2cpp_probe.h"

#include "sao/core/error.h"
#include "sao/core/memory.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {

sao_status_t fail_probe(sao_status_t status,
                        const char* message,
                        const char* file,
                        uint32_t line) {
    sao_core_error_set(status, "core.il2cpp_probe", message, file, line);
    return status;
}

// Convenience: try to read `count` bytes from the target; returns true
// only when the whole read succeeded.
bool read_exact(sao_core_process_handle_t handle,
                uint64_t address,
                void* buffer,
                size_t count) {
    size_t got = 0;
    if (sao_core_mem_read(handle, address, buffer, count, &got) != SAO_STATUS_OK) {
        return false;
    }
    return got == count;
}

// Read the u64 at `address`.  Returns 0 on failure.  A raw zero return is
// still meaningful downstream (null pointers exist), so callers check the
// second `ok` output.
uint64_t read_u64(sao_core_process_handle_t handle,
                  uint64_t address,
                  bool* ok) {
    uint64_t value = 0;
    *ok = read_exact(handle, address, &value, sizeof(value));
    return value;
}

uint32_t read_u32(sao_core_process_handle_t handle,
                  uint64_t address,
                  bool* ok) {
    uint32_t value = 0;
    *ok = read_exact(handle, address, &value, sizeof(value));
    return value;
}

// Locate a named export inside a Windows PE loaded at `module_base`.
// Returns 0 on any failure.  This is a 1:1 port of the Python
// `_resolve_export` in `il2cpp_adapter.py` and handles PE32+ only (x64).
uint64_t resolve_export(sao_core_process_handle_t handle,
                        uint64_t module_base,
                        const char* export_name) {
    if (!handle || module_base == 0 || export_name == nullptr) {
        return 0;
    }

    std::array<uint8_t, 0x40> dos{};
    if (!read_exact(handle, module_base, dos.data(), dos.size())) {
        return 0;
    }
    if (dos[0] != 'M' || dos[1] != 'Z') {
        return 0;
    }
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, dos.data() + 0x3C, sizeof(e_lfanew));

    std::array<uint8_t, 0x88> pe_hdr{};
    if (!read_exact(handle, module_base + e_lfanew, pe_hdr.data(), pe_hdr.size())) {
        return 0;
    }
    if (pe_hdr[0] != 'P' || pe_hdr[1] != 'E' || pe_hdr[2] != 0 || pe_hdr[3] != 0) {
        return 0;
    }
    // Optional header magic - 0x20B for PE32+, 0x10B for PE32.  IL2CPP
    // ships x64, so we require PE32+.
    uint16_t magic = 0;
    std::memcpy(&magic, pe_hdr.data() + 0x18, sizeof(magic));
    if (magic != 0x20B) {
        return 0;
    }
    // Export table entry sits at optional_header + 0x70 (data directory 0).
    uint32_t export_rva = 0;
    std::memcpy(&export_rva, pe_hdr.data() + 0x18 + 0x70, sizeof(export_rva));
    if (export_rva == 0) {
        return 0;
    }

    // IMAGE_EXPORT_DIRECTORY layout (40 bytes).
    std::array<uint8_t, 40> ed{};
    if (!read_exact(handle, module_base + export_rva, ed.data(), ed.size())) {
        return 0;
    }
    uint32_t nfuncs = 0;
    uint32_t nnames = 0;
    uint32_t addr_rva = 0;
    uint32_t name_rva = 0;
    uint32_t ord_rva  = 0;
    std::memcpy(&nfuncs,   ed.data() + 0x14, sizeof(nfuncs));
    std::memcpy(&nnames,   ed.data() + 0x18, sizeof(nnames));
    std::memcpy(&addr_rva, ed.data() + 0x1C, sizeof(addr_rva));
    std::memcpy(&name_rva, ed.data() + 0x20, sizeof(name_rva));
    std::memcpy(&ord_rva,  ed.data() + 0x24, sizeof(ord_rva));
    if (nnames == 0 || nnames > 50000 || nfuncs == 0 || nfuncs > 50000) {
        return 0;
    }

    const size_t name_len = std::strlen(export_name);
    if (name_len == 0 || name_len > 128) {
        return 0;
    }

    std::vector<uint8_t> name_ptrs(static_cast<size_t>(nnames) * 4);
    std::vector<uint8_t> ordinals(static_cast<size_t>(nnames) * 2);
    if (!read_exact(handle, module_base + name_rva,
                    name_ptrs.data(), name_ptrs.size())) {
        return 0;
    }
    if (!read_exact(handle, module_base + ord_rva,
                    ordinals.data(), ordinals.size())) {
        return 0;
    }

    std::vector<uint8_t> raw(name_len + 1);
    for (uint32_t index = 0; index < nnames; ++index) {
        uint32_t str_rva = 0;
        std::memcpy(&str_rva, name_ptrs.data() + index * 4, sizeof(str_rva));
        if (str_rva == 0) {
            continue;
        }
        if (!read_exact(handle, module_base + str_rva, raw.data(), raw.size())) {
            continue;
        }
        if (raw[name_len] == 0 &&
            std::memcmp(raw.data(), export_name, name_len) == 0) {
            uint16_t ordinal = 0;
            std::memcpy(&ordinal, ordinals.data() + index * 2, sizeof(ordinal));
            if (ordinal >= nfuncs) {
                return 0;
            }
            std::vector<uint8_t> functions(static_cast<size_t>(nfuncs) * 4);
            if (!read_exact(handle, module_base + addr_rva,
                            functions.data(), functions.size())) {
                return 0;
            }
            uint32_t fn_rva = 0;
            std::memcpy(&fn_rva, functions.data() + ordinal * 4, sizeof(fn_rva));
            return module_base + fn_rva;
        }
    }
    return 0;
}

// Decode either `mov rax, [rip+disp32]` (48 8B 05 xx xx xx xx) or
// `lea rax, [rip+disp32]` (48 8D 05 xx xx xx xx) starting at `code_addr`
// and return the absolute target address.  Returns 0 on shape mismatch.
uint64_t decode_domain_get_prologue(sao_core_process_handle_t handle,
                                    uint64_t code_addr) {
    std::array<uint8_t, 16> code{};
    if (!read_exact(handle, code_addr, code.data(), code.size())) {
        return 0;
    }
    if (code[0] != 0x48 || code[2] != 0x05) {
        return 0;
    }
    if (code[1] != 0x8B && code[1] != 0x8D) {
        return 0;
    }
    int32_t disp = 0;
    std::memcpy(&disp, code.data() + 3, sizeof(disp));
    // rip is code_addr + 7 (length of the mov/lea instruction).
    return static_cast<uint64_t>(static_cast<int64_t>(code_addr) + 7 +
                                 static_cast<int64_t>(disp));
}

// Heuristic upper bound for user-mode pointers on x64.  Anything above
// this is either kernel or garbage.
constexpr uint64_t kUserModeUpperBound = 0x00007FFFFFFFFFFFull;
constexpr uint64_t kUserModeLowerBound = 0x0000000000010000ull;

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_probe_metadata(
    sao_core_process_handle_t handle,
    uint64_t module_base,
    SaoIl2CppProbe* probe_out) {
    if (probe_out == nullptr) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "probe_out is null", __FILE__, __LINE__);
    }
    *probe_out = SaoIl2CppProbe{};
    if (handle == nullptr) {
        return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID,
                          "handle is null", __FILE__, __LINE__);
    }
    if (module_base < kUserModeLowerBound) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "module_base looks bogus", __FILE__, __LINE__);
    }

    probe_out->game_assembly_base = module_base;

    // 1) Resolve il2cpp_domain_get.  Failure here means IL2CPP is not
    //    present at this address, so we surface NOT_FOUND rather than
    //    OK-with-empty-values.
    const uint64_t domain_get_addr =
        resolve_export(handle, module_base, "il2cpp_domain_get");
    if (domain_get_addr == 0) {
        return fail_probe(SAO_STATUS_ERR_NOT_FOUND,
                          "il2cpp_domain_get export missing",
                          __FILE__, __LINE__);
    }

    // 2) Decode the mov/lea prologue to get the address of the global
    //    domain pointer.  Modern IL2CPP always uses one of these two
    //    instructions - if decoding fails, treat as best-effort probe.
    const uint64_t domain_slot = decode_domain_get_prologue(handle, domain_get_addr);
    if (domain_slot != 0) {
        bool ok = false;
        const uint64_t domain_value = read_u64(handle, domain_slot, &ok);
        if (ok && domain_value >= kUserModeLowerBound &&
            domain_value < kUserModeUpperBound) {
            probe_out->domain_ptr = domain_value;
        }
    }

    // 3) Metadata version and RVA - probe the well-known
    //    il2cpp_get_corlib / il2cpp_runtime_get_options exports for their
    //    RVA; we do NOT parse the metadata registration table itself
    //    (that is a per-plugin decision).  We record the RVA of
    //    il2cpp_domain_get as the "metadata anchor" which the plugin
    //    can use as a starting point when it needs to sig-scan for
    //    Il2CppMetadataRegistration.
    probe_out->metadata_rva = domain_get_addr - module_base;

    // 4) Try to read the metadata version tag.  IL2CPP exports
    //    `s_GlobalMetadataHeader` in some versions, but the export is
    //    absent on release builds; a best-effort probe uses the version
    //    field IL2CPP writes at the start of the domain object, which
    //    Il2CppDomain->version_number resides at +0x18 on most 2019+
    //    builds.  When absent we leave the field zero.
    if (probe_out->domain_ptr != 0) {
        bool ok = false;
        const uint32_t maybe_version =
            read_u32(handle, probe_out->domain_ptr + 0x18, &ok);
        // The observed values across Unity 2019-2024 fall in the
        // 20..40 range.  Anything outside this window is treated as
        // "unknown" so downstream plugins do not build on garbage.
        if (ok && maybe_version >= 20 && maybe_version <= 40) {
            probe_out->metadata_version = maybe_version;
        }
    }

    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_scan_klass_pointers(
    sao_core_process_handle_t handle,
    uint64_t region_base,
    size_t region_size,
    sao_il2cpp_klass_callback_t callback,
    void* user_data,
    size_t* visited_out) {
    size_t visited = 0;
    if (visited_out != nullptr) {
        *visited_out = 0;
    }
    if (callback == nullptr) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "callback is null", __FILE__, __LINE__);
    }
    if (handle == nullptr) {
        return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID,
                          "handle is null", __FILE__, __LINE__);
    }
    if (region_size < sizeof(uint64_t)) {
        return SAO_STATUS_OK;  // Empty region is a no-op success.
    }

    // Align region_base up to 8 bytes.  We deliberately mirror the
    // Cython scanner's alignment discipline: klass pointers are always
    // 8-aligned in IL2CPP.
    const uint64_t aligned_base = (region_base + 7ull) & ~7ull;
    if (aligned_base < region_base) {
        return SAO_STATUS_OK;  // Wrap - refuse to scan.
    }
    const uint64_t start_shift = aligned_base - region_base;
    if (start_shift >= region_size) {
        return SAO_STATUS_OK;
    }
    const size_t usable = region_size - static_cast<size_t>(start_shift);
    const size_t pointer_count = usable / sizeof(uint64_t);
    if (pointer_count == 0) {
        return SAO_STATUS_OK;
    }

    // Read in chunks so we do not have to allocate the whole region.
    constexpr size_t kChunkPointers = 4096;  // 32 KiB per chunk
    std::vector<uint64_t> chunk(kChunkPointers);
    size_t remaining = pointer_count;
    uint64_t cursor = aligned_base;
    while (remaining > 0) {
        const size_t take = std::min(kChunkPointers, remaining);
        const size_t bytes = take * sizeof(uint64_t);
        size_t got = 0;
        const sao_status_t rc =
            sao_core_mem_read(handle, cursor, chunk.data(), bytes, &got);
        // Even on short reads (page unmapped mid-chunk) we still process
        // whatever bytes we got - IL2CPP heaps are famously sparse.
        const size_t got_pointers = got / sizeof(uint64_t);
        for (size_t index = 0; index < got_pointers; ++index) {
            const uint64_t candidate = chunk[index];
            if (candidate < kUserModeLowerBound ||
                candidate > kUserModeUpperBound) {
                continue;
            }
            if ((candidate & 7ull) != 0) {
                continue;  // Klass pointers themselves are 8-aligned.
            }
            ++visited;
            if (!callback(candidate, user_data)) {
                if (visited_out != nullptr) {
                    *visited_out = visited;
                }
                return SAO_STATUS_OK;
            }
        }
        if (rc != SAO_STATUS_OK) {
            // Best-effort: skip the failing chunk and keep scanning at
            // the next 4 KiB page boundary.
            constexpr uint64_t kPage = 0x1000;
            const uint64_t next = (cursor + bytes + kPage - 1) & ~(kPage - 1);
            const uint64_t hop = next - cursor;
            const size_t hop_pointers = static_cast<size_t>(hop / sizeof(uint64_t));
            if (hop_pointers >= remaining) {
                break;
            }
            cursor += hop;
            remaining -= hop_pointers;
            continue;
        }
        cursor += bytes;
        remaining -= take;
    }
    if (visited_out != nullptr) {
        *visited_out = visited;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_il2cpp_read_klass_name(
    sao_core_process_handle_t handle,
    uint64_t klass_ptr,
    char* out_name_utf8,
    size_t name_capacity) {
    if (out_name_utf8 == nullptr || name_capacity == 0) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "output buffer is null", __FILE__, __LINE__);
    }
    out_name_utf8[0] = '\0';
    if (handle == nullptr) {
        return fail_probe(SAO_STATUS_ERR_HANDLE_INVALID,
                          "handle is null", __FILE__, __LINE__);
    }
    if (klass_ptr < kUserModeLowerBound || (klass_ptr & 7ull) != 0) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "klass pointer misaligned or bogus",
                          __FILE__, __LINE__);
    }

    bool ok = false;
    const uint64_t name_ptr =
        read_u64(handle, klass_ptr + SAO_IL2CPP_KLASS_NAME_OFFSET, &ok);
    if (!ok) {
        return fail_probe(SAO_STATUS_ERR_READ_FAULT,
                          "klass name pointer unreadable",
                          __FILE__, __LINE__);
    }
    if (name_ptr < kUserModeLowerBound || name_ptr > kUserModeUpperBound) {
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "name pointer looks bogus",
                          __FILE__, __LINE__);
    }

    // Read up to 256 bytes; strings longer than 255 are treated as
    // "invalid klass" per the same convention the Python reader uses.
    constexpr size_t kNameLimit = 256;
    std::array<uint8_t, kNameLimit + 1> buf{};
    size_t got = 0;
    const sao_status_t rc =
        sao_core_mem_read(handle, name_ptr, buf.data(), buf.size(), &got);
    if (rc != SAO_STATUS_OK && got == 0) {
        return fail_probe(SAO_STATUS_ERR_READ_FAULT,
                          "name string unreadable",
                          __FILE__, __LINE__);
    }
    // Find NUL terminator inside what we got.
    size_t length = 0;
    while (length < got && buf[length] != 0) {
        ++length;
    }
    if (length == got) {
        // No terminator found in the window - not a valid klass name.
        return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                          "name string not null-terminated",
                          __FILE__, __LINE__);
    }
    // Sanity: names must be printable ASCII/Latin-1.
    for (size_t index = 0; index < length; ++index) {
        const uint8_t byte = buf[index];
        if (byte < 0x20 || byte > 0x7E) {
            // Allow underscore/dot/backtick already in 0x20..0x7E, so
            // anything outside is a strong "not a klass name" signal.
            return fail_probe(SAO_STATUS_ERR_INVALID_ARGUMENT,
                              "name string not ASCII",
                              __FILE__, __LINE__);
        }
    }

    const size_t needed = length + 1;
    if (name_capacity < needed) {
        // Emit truncated result plus BUFFER_TOO_SMALL so callers can
        // decide whether to grow.
        const size_t copy_len = name_capacity - 1;
        memcpy(out_name_utf8, buf.data(), copy_len);
        out_name_utf8[copy_len] = '\0';
        return fail_probe(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                          "name buffer too small",
                          __FILE__, __LINE__);
    }
    memcpy(out_name_utf8, buf.data(), length);
    out_name_utf8[length] = '\0';
    return SAO_STATUS_OK;
}
