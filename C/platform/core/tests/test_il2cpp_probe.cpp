// Tests for the platform/core generic IL2CPP runtime probe.
//
// Coverage:
//   * il2cpp_probe_metadata_no_target_skips
//   * il2cpp_scan_klass_pointers_callback_invoked
//   * il2cpp_read_klass_name_metadata_v27_stride
//
// The probe API is deliberately game-agnostic: these tests exercise the
// scan and validate primitives with either a stub target (own process,
// no GameAssembly.dll) or a synthetic memory region rooted at
// allocated heap - never a real IL2CPP target.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/engine_detector.h"
#include "sao/core/il2cpp_probe.h"
#include "sao/core/memory.h"
#include "sao/core/process.h"
#include "sao/core/status.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <cstring>
#include <cstdint>
#include <vector>

namespace {

uint32_t self_pid() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 1u;
#endif
}

// Locate GameAssembly.dll in the current process.  Returns 0 when the
// module is not loaded (the common case for the test host).
uint64_t game_assembly_base_for_current() {
#if defined(_WIN32)
    const HMODULE handle = GetModuleHandleW(L"GameAssembly.dll");
    return reinterpret_cast<uint64_t>(handle);
#else
    return 0;
#endif
}

}  // namespace

TEST_CASE("il2cpp_probe_metadata_no_target_skips",
          "[core][il2cpp_probe][automation]") {
    // Without a live IL2CPP target, the probe must refuse cleanly and
    // never touch the probe_out fields with garbage.
    if (game_assembly_base_for_current() != 0) {
        SKIP("test host somehow ships GameAssembly.dll - "
             "probe would produce live data, defer to integration test.");
    }

    const uint32_t pid = self_pid();
    sao_core_process_handle_t handle = nullptr;
    REQUIRE(sao_core_process_open(pid,
                                  SAO_PROCESS_ACCESS_READ | SAO_PROCESS_ACCESS_INFO,
                                  &handle) == SAO_STATUS_OK);

    // Fake module base - a valid user-mode pointer that we know does
    // NOT point at a PE header of GameAssembly.dll.  The probe must
    // catch this via the DOS/PE header validation and return NOT_FOUND.
    // We use the address of `handle` itself to guarantee it does not
    // start with an "MZ" DOS header.
    const uint64_t bogus_base =
        reinterpret_cast<uint64_t>(&pid) & ~static_cast<uint64_t>(0xFFF);

    SaoIl2CppProbe probe{};
    // Poison the struct so we can verify the probe zeroes it on entry.
    probe.metadata_rva = 0xDEADBEEF;
    probe.metadata_version = 0x99;
    probe.domain_ptr = 0xCAFEBABEull;
    const sao_status_t rc =
        sao_core_il2cpp_probe_metadata(handle, bogus_base, &probe);
    // NOT_FOUND (no il2cpp_domain_get export) or INVALID_ARGUMENT (bogus
    // header) are both acceptable outcomes - the probe must not
    // succeed against this arbitrary user address.
    REQUIRE((rc == SAO_STATUS_ERR_NOT_FOUND ||
             rc == SAO_STATUS_ERR_INVALID_ARGUMENT));
    // After a failure, the struct must be zeroed (never left with the
    // caller's poison values).
    REQUIRE(probe.metadata_rva == 0u);
    REQUIRE(probe.metadata_version == 0u);
    REQUIRE(probe.domain_ptr == 0u);

    sao_core_process_close(handle);
}

TEST_CASE("il2cpp_scan_klass_pointers_callback_invoked",
          "[core][il2cpp_probe][automation]") {
    // Build a synthetic 8-aligned buffer in our own address space.  The
    // scanner reads it via sao_core_mem_read against our own process
    // handle so we get an end-to-end path exercised.
    const uint32_t pid = self_pid();
    sao_core_process_handle_t handle = nullptr;
    REQUIRE(sao_core_process_open(pid,
                                  SAO_PROCESS_ACCESS_READ | SAO_PROCESS_ACCESS_INFO,
                                  &handle) == SAO_STATUS_OK);

    // Layout: 8 pointers.  Half are valid-looking user-mode addresses,
    // half are obvious garbage (below 0x10000).  We expect exactly the
    // valid ones to be reported to the callback.
    alignas(8) std::vector<uint64_t> region(8);
    region[0] = 0x00007FF6'DEAD0000ull;
    region[1] = 0x0000000A'CAFE0000ull;
    region[2] = 0x1234;  // below lower bound - must be filtered
    region[3] = 0;       // null - must be filtered
    region[4] = 0x00000000'01000000ull;
    region[5] = 0xFFFF'FFFF'FFFF'FFFFull;  // above upper bound
    region[6] = 0x0000'0000'00010001ull;   // not 8-aligned (bit 0 set)
    region[7] = 0x00007FF7'BEEF0000ull;

    struct Ctx {
        int seen;
        int max_seen;
    } ctx{0, 5};

    auto cb = [](uint64_t candidate, void* user_data) -> bool {
        auto* c = static_cast<Ctx*>(user_data);
        (void)candidate;
        c->seen += 1;
        // Stop before we hit the artificial cap so we exercise the
        // early-exit code path too.
        return c->seen < c->max_seen;
    };

    size_t visited = 0;
    const sao_status_t rc =
        sao_core_il2cpp_scan_klass_pointers(
            handle,
            reinterpret_cast<uint64_t>(region.data()),
            region.size() * sizeof(uint64_t),
            cb, &ctx, &visited);
    REQUIRE(rc == SAO_STATUS_OK);
    // Three valid candidates: entries 0, 1, 4, 7 (four total).  But
    // early-exit at ctx.max_seen (5) never triggers because we only
    // have 4 valid entries.  visited must equal the number of valid
    // candidates and callback invocations.
    REQUIRE(visited == 4u);
    REQUIRE(ctx.seen == 4);

    // Null callback must be rejected.
    ctx = Ctx{0, 5};
    const sao_status_t null_cb =
        sao_core_il2cpp_scan_klass_pointers(handle, 0x1000, 64, nullptr,
                                            &ctx, &visited);
    REQUIRE(null_cb == SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_core_process_close(handle);
}

TEST_CASE("il2cpp_read_klass_name_metadata_v27_stride",
          "[core][il2cpp_probe][automation]") {
    // Simulate an IL2CPP klass in our own address space so the name
    // reader walks the standard klass+0x10 layout without needing a
    // real IL2CPP process.  Metadata v27/v29/v31 all place the name
    // pointer at offset 0x10 - the header exports this via
    // SAO_IL2CPP_KLASS_NAME_OFFSET.

    // Fake klass: 512 bytes zeroed, name pointer written at +0x10.
    alignas(8) std::vector<uint8_t> klass_backing(512);
    static const char kFakeName[] = "PlayerControllerLike";
    uint64_t name_ptr = reinterpret_cast<uint64_t>(kFakeName);
    std::memcpy(klass_backing.data() + SAO_IL2CPP_KLASS_NAME_OFFSET,
                &name_ptr, sizeof(name_ptr));

    const uint32_t pid = self_pid();
    sao_core_process_handle_t handle = nullptr;
    REQUIRE(sao_core_process_open(pid,
                                  SAO_PROCESS_ACCESS_READ | SAO_PROCESS_ACCESS_INFO,
                                  &handle) == SAO_STATUS_OK);

    char name[64] = {0};
    const uint64_t klass_ptr = reinterpret_cast<uint64_t>(klass_backing.data());
    const sao_status_t rc = sao_core_il2cpp_read_klass_name(
        handle, klass_ptr, name, sizeof(name));
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(std::string(name) == kFakeName);

    // Misaligned klass pointer must be rejected.
    const sao_status_t misaligned =
        sao_core_il2cpp_read_klass_name(handle, klass_ptr + 1,
                                        name, sizeof(name));
    REQUIRE(misaligned == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Below-user-mode klass pointer must be rejected.
    const sao_status_t bogus_ptr =
        sao_core_il2cpp_read_klass_name(handle, 0x100, name, sizeof(name));
    REQUIRE(bogus_ptr == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Tiny output buffer must truncate with BUFFER_TOO_SMALL and still
    // null-terminate.
    char tiny[4] = {0};
    const sao_status_t truncate = sao_core_il2cpp_read_klass_name(
        handle, klass_ptr, tiny, sizeof(tiny));
    REQUIRE(truncate == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(tiny[3] == '\0');

    sao_core_process_close(handle);
}
