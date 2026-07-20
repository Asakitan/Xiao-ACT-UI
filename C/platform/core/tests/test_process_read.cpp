// Tests for platform/core process + memory RPM/WPM covering G1.9.
//
// Coverage:
//   * process_open_current_pid_succeeds
//   * process_read_own_memory_roundtrip
//   * process_enum_modules_includes_kernel32
//   * process_enum_regions_returns_committed
//   * process_read_multiple_requests_return_independent_status
//
// The Python authoritative source is `mem_probe/process.py`.  Every test
// runs against the current process (self-attach) so no external target is
// needed and the tests are hermetic on CI.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/process.h"
#include "sao/core/memory.h"
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
#include <string>
#include <vector>

namespace {

uint32_t self_pid() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 1u;
#endif
}

}  // namespace

TEST_CASE("process_open_current_pid_succeeds",
          "[core][process][runtime]") {
    const uint32_t pid = self_pid();
    REQUIRE(pid > 0u);

    sao_core_process_handle_t p = nullptr;
    const sao_status_t rc = sao_core_process_open(
        pid,
        SAO_PROCESS_ACCESS_INFO | SAO_PROCESS_ACCESS_READ,
        &p);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(p != nullptr);

    sao_core_process_close(p);
}

TEST_CASE("process_read_own_memory_roundtrip",
          "[core][process][memory][runtime]") {
    const uint32_t pid = self_pid();
    sao_core_process_handle_t p = nullptr;
    REQUIRE(sao_core_process_open(pid, SAO_PROCESS_ACCESS_READ, &p) == SAO_STATUS_OK);

    // Prepare a signature buffer that lives on our own stack/heap and
    // is guaranteed to be committed + readable across the ABI boundary.
    const char signature[] = "SAO-PROCESS-READ-TEST-XYZ12345";
    const size_t sig_len = sizeof(signature);
    std::vector<char> src(sig_len);
    std::memcpy(src.data(), signature, sig_len);

    std::vector<char> dst(sig_len, '\0');
    size_t bytes_read = 0;
    const sao_status_t rc = sao_core_mem_read(
        p,
        reinterpret_cast<uint64_t>(src.data()),
        dst.data(),
        sig_len,
        &bytes_read);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(bytes_read == sig_len);
    CHECK(std::memcmp(dst.data(), signature, sig_len) == 0);

    // u32 typed read using the same source (first 4 bytes).
    uint32_t u = 0;
    const uint32_t expected_u = *reinterpret_cast<const uint32_t*>(signature);
    REQUIRE(sao_core_mem_read_u32(p, reinterpret_cast<uint64_t>(src.data()), &u) == SAO_STATUS_OK);
    CHECK(u == expected_u);

    sao_core_process_close(p);
}

TEST_CASE("process_enum_modules_includes_kernel32",
          "[core][process][modules][runtime]") {
#if !defined(_WIN32)
    SUCCEED("Windows-only test");
    return;
#else
    const uint32_t pid = self_pid();
    sao_core_process_handle_t p = nullptr;
    REQUIRE(sao_core_process_open(pid, SAO_PROCESS_ACCESS_INFO, &p) == SAO_STATUS_OK);

    // Size query.
    size_t entry_count = 0;
    size_t names_used = 0;
    REQUIRE(sao_core_process_enum_modules(
        p, nullptr, 0, &entry_count, nullptr, 0, &names_used) == SAO_STATUS_OK);
    REQUIRE(entry_count > 0);
    REQUIRE(names_used > 0);

    // Real enumeration.
    std::vector<SaoModuleEntry> entries(entry_count);
    std::vector<char> names(names_used + 1, '\0');
    REQUIRE(sao_core_process_enum_modules(
        p,
        entries.data(),
        entries.size(),
        &entry_count,
        names.data(),
        names.size(),
        &names_used) == SAO_STATUS_OK);
    REQUIRE(entry_count == entries.size());

    // KERNEL32.DLL is guaranteed loaded in every Windows process.
    bool found_kernel32 = false;
    for (const auto& e : entries) {
        const char* name = names.data() + e.name_offset;
        if (_stricmp(name, "kernel32.dll") == 0) {
            found_kernel32 = true;
            CHECK(e.base_address != 0u);
            CHECK(e.module_size > 0u);
            break;
        }
    }
    CHECK(found_kernel32);

    sao_core_process_close(p);
#endif
}

TEST_CASE("process_enum_regions_returns_committed",
          "[core][process][memory][runtime]") {
#if !defined(_WIN32)
    SUCCEED("Windows-only test");
    return;
#else
    const uint32_t pid = self_pid();
    sao_core_process_handle_t p = nullptr;
    REQUIRE(sao_core_process_open(pid, SAO_PROCESS_ACCESS_READ, &p) == SAO_STATUS_OK);

    // Size query.
    size_t region_count = 0;
    REQUIRE(sao_core_mem_enum_regions(p, nullptr, 0, &region_count) == SAO_STATUS_OK);
    REQUIRE(region_count > 0);

    std::vector<SaoMemRegion> regions(region_count);
    REQUIRE(sao_core_mem_enum_regions(
        p, regions.data(), regions.size(), &region_count) == SAO_STATUS_OK);
    REQUIRE(region_count > 0);
    regions.resize(region_count);

    // Every returned region is committed + readable.  is_readable_protection
    // in memory.cpp already screens out GUARD/NOACCESS so we just spot check.
    bool any_committed = false;
    for (const auto& r : regions) {
        CHECK(r.state == MEM_COMMIT);
        CHECK(r.region_size > 0u);
        if (r.state == MEM_COMMIT) any_committed = true;
    }
    CHECK(any_committed);

    // The address of a live local buffer must fall inside one of the
    // returned regions.
    std::vector<char> probe(4096, 'X');
    const uint64_t probe_addr = reinterpret_cast<uint64_t>(probe.data());
    bool probe_in_range = false;
    for (const auto& r : regions) {
        if (probe_addr >= r.base_address &&
            probe_addr < r.base_address + r.region_size) {
            probe_in_range = true;
            break;
        }
    }
    CHECK(probe_in_range);

    sao_core_process_close(p);
#endif
}

TEST_CASE("process_read_multiple_requests_return_independent_status",
          "[core][process][memory][multiple][runtime]") {
    const uint32_t pid = self_pid();
    sao_core_process_handle_t p = nullptr;
    REQUIRE(sao_core_process_open(pid, SAO_PROCESS_ACCESS_READ, &p) == SAO_STATUS_OK);

    // Two known-good reads with one deliberately bogus address between them.
    // The failed request must not poison the valid request that follows it.
    const char text_a[] = "read-request-A";
    const char text_b[] = "read-request-B";
    char dst_a[32] = {};
    char dst_b[32] = {};
    char dst_bogus[16] = {};

    size_t bytes_read_a = 0;
    const sao_status_t status_a = sao_core_mem_read(
        p, reinterpret_cast<uint64_t>(text_a), dst_a, sizeof(text_a), &bytes_read_a);

    size_t bytes_read_bogus = 0;
    const sao_status_t status_bogus = sao_core_mem_read(
        p, 0xFFFFFFFFFFFF0000ULL, dst_bogus, sizeof(dst_bogus), &bytes_read_bogus);

    size_t bytes_read_b = 0;
    const sao_status_t status_b = sao_core_mem_read(
        p, reinterpret_cast<uint64_t>(text_b), dst_b, sizeof(text_b), &bytes_read_b);

    CHECK(status_a == SAO_STATUS_OK);
    CHECK(bytes_read_a == sizeof(text_a));
    CHECK(std::memcmp(dst_a, text_a, sizeof(text_a)) == 0);

    CHECK(status_bogus != SAO_STATUS_OK);

    CHECK(status_b == SAO_STATUS_OK);
    CHECK(bytes_read_b == sizeof(text_b));
    CHECK(std::memcmp(dst_b, text_b, sizeof(text_b)) == 0);

    sao_core_process_close(p);
}

TEST_CASE("process_read_pointer_chain_zero_offset_is_base",
          "[core][process][memory][chain][runtime]") {
    const uint32_t pid = self_pid();
    sao_core_process_handle_t p = nullptr;
    REQUIRE(sao_core_process_open(pid, SAO_PROCESS_ACCESS_READ, &p) == SAO_STATUS_OK);

    uint64_t out = 0;
    REQUIRE(sao_core_mem_read_pointer_chain(
        p, 0xDEADBEEFULL, nullptr, 0, &out) == SAO_STATUS_OK);
    CHECK(out == 0xDEADBEEFULL);

    // One-hop: pointer stored inside a local variable, offset 0.
    uint64_t hidden = 0x1234567890ABCDEFULL;
    int32_t offsets[1] = {0};
    REQUIRE(sao_core_mem_read_pointer_chain(
        p, reinterpret_cast<uint64_t>(&hidden), offsets, 1, &out) == SAO_STATUS_OK);
    CHECK(out == 0x1234567890ABCDEFULL);

    sao_core_process_close(p);
}
