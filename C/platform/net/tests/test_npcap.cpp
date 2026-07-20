// SAO Auto — Wave 6 tests for the low-level Npcap wrapper.
//
// The wrapper is designed to compile & link even without the Npcap SDK
// (wpcap.dll is loaded lazily via GetProcAddress).  These tests reflect
// that: when Npcap is not installed on the runner, every case SKIP()s
// with a clear message instead of failing.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "sao/net/npcap_capture.h"
#include "sao/net/packet_pipeline.h"

namespace {
bool npcap_installed() {
    bool available = false;
    sao_status_t st = sao_net_npcap_available(&available);
    return st == SAO_STATUS_OK && available;
}
}  // namespace

TEST_CASE("npcap_available_matches_installed", "[net][automation][npcap]") {
    // Contract: sao_net_npcap_available NEVER returns an error status.
    // It always returns SAO_STATUS_OK and reports its verdict via the
    // bool out-param.  This test asserts both halves.
    bool available_first = true;
    sao_status_t st = sao_net_npcap_available(&available_first);
    REQUIRE(st == SAO_STATUS_OK);

    // Idempotency — the once_flag-guarded loader should give the same
    // answer on a second probe.
    bool available_second = !available_first;
    st = sao_net_npcap_available(&available_second);
    REQUIRE(st == SAO_STATUS_OK);
    REQUIRE(available_first == available_second);

    // NULL out-param is a caller bug and must be rejected.
    REQUIRE(sao_net_npcap_available(nullptr) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("npcap_list_devices_returns_nonempty", "[net][automation][npcap]") {
    if (!npcap_installed()) {
        // Sizing probe still returns NOT_FOUND, not garbage.
        size_t count = 42;
        sao_status_t st = sao_net_npcap_list_devices(nullptr, 0, &count);
        REQUIRE(st == SAO_STATUS_ERR_NOT_FOUND);
        REQUIRE(count == 0);
        SKIP("Npcap not installed on this runner");
        return;
    }

    // Size probe.
    size_t count = 0;
    sao_status_t st = sao_net_npcap_list_devices(nullptr, 0, &count);
    REQUIRE(st == SAO_STATUS_OK);
    REQUIRE(count > 0);

    std::vector<SaoNpcapDevice> rows(count);
    st = sao_net_npcap_list_devices(rows.data(), rows.size(), &count);
    REQUIRE(st == SAO_STATUS_OK);
    REQUIRE(count == rows.size());
    // Every returned row must have a non-empty NPF name.
    for (size_t i = 0; i < count; ++i) {
        REQUIRE(std::strlen(rows[i].name_utf8) > 0);
    }
}

TEST_CASE("npcap_open_close_localhost", "[net][automation][npcap]") {
    if (!npcap_installed()) {
        SKIP("Npcap not installed on this runner");
        return;
    }
    size_t count = 0;
    REQUIRE(sao_net_npcap_list_devices(nullptr, 0, &count) == SAO_STATUS_OK);
    if (count == 0) {
        SKIP("No NPF devices available");
        return;
    }
    std::vector<SaoNpcapDevice> rows(count);
    REQUIRE(sao_net_npcap_list_devices(rows.data(), rows.size(), &count) ==
            SAO_STATUS_OK);

    // Open first available device — do NOT enforce any traffic, this
    // test only exercises open + close.
    sao_net_npcap_handle_t handle = nullptr;
    sao_status_t st = sao_net_npcap_open_live(rows[0].name_utf8, 65535,
                                               100, &handle);
    REQUIRE(st == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);

    // Close is idempotent on NULL and must not crash on the real handle.
    sao_net_npcap_close(handle);
    sao_net_npcap_close(nullptr);
}

TEST_CASE("npcap_set_filter_valid_expr", "[net][automation][npcap]") {
    if (!npcap_installed()) {
        SKIP("Npcap not installed on this runner");
        return;
    }
    size_t count = 0;
    REQUIRE(sao_net_npcap_list_devices(nullptr, 0, &count) == SAO_STATUS_OK);
    if (count == 0) {
        SKIP("No NPF devices available");
        return;
    }
    std::vector<SaoNpcapDevice> rows(count);
    REQUIRE(sao_net_npcap_list_devices(rows.data(), rows.size(), &count) ==
            SAO_STATUS_OK);

    sao_net_npcap_handle_t handle = nullptr;
    REQUIRE(sao_net_npcap_open_live(rows[0].name_utf8, 65535, 100, &handle) ==
            SAO_STATUS_OK);

    // Valid BPF — must compile & apply.
    sao_status_t st = sao_net_npcap_set_filter(handle, "tcp port 8888");
    REQUIRE(st == SAO_STATUS_OK);

    // Rewriting the filter is allowed (freecode + recompile).
    st = sao_net_npcap_set_filter(handle, "tcp");
    REQUIRE(st == SAO_STATUS_OK);

    // NULL clears (returns OK, no filter installed).
    st = sao_net_npcap_set_filter(handle, nullptr);
    REQUIRE(st == SAO_STATUS_OK);

    // Malformed BPF should be reported as INVALID_ARGUMENT, not a crash.
    st = sao_net_npcap_set_filter(handle, "definitely not valid bpf ??");
    REQUIRE(st == SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_net_npcap_close(handle);
}

TEST_CASE("npcap_legacy_capture_composes_with_packet_pipeline",
          "[net][npcap][pipeline]") {
    if (!npcap_installed()) {
        SKIP("Npcap provider unavailable");
        return;
    }
    size_t count = 0;
    REQUIRE(sao_net_capture_enum_interfaces(nullptr, 0, &count) ==
            SAO_STATUS_OK);
    if (count == 0) {
        SKIP("No NPF devices available");
        return;
    }
    std::vector<SaoCaptureInterface> interfaces(count);
    REQUIRE(sao_net_capture_enum_interfaces(
                interfaces.data(), interfaces.size(), &count) == SAO_STATUS_OK);

    sao_net_capture_handle_t capture = nullptr;
    REQUIRE(sao_net_capture_open(interfaces[0].name_utf8, "tcp", 65535, 10,
                                 &capture) == SAO_STATUS_OK);
    REQUIRE(capture != nullptr);

    sao_net_pipeline_handle_t pipeline = nullptr;
    REQUIRE(sao_net_pipeline_create(capture, &pipeline) == SAO_STATUS_OK);
    REQUIRE(pipeline != nullptr);
    SaoTcpEndpoint endpoint{{127, 0, 0, 1}, 443, 0};
    REQUIRE(sao_net_pipeline_lock_endpoint(pipeline, &endpoint) ==
            SAO_STATUS_OK);
    SaoPipelineStats stats{};
    REQUIRE(sao_net_pipeline_snapshot_stats(pipeline, &stats) == SAO_STATUS_OK);
    REQUIRE(stats.endpoint_locked);
    REQUIRE(stats.locked_remote.port == 443);

    sao_net_pipeline_destroy(pipeline);
    sao_net_capture_close(capture);
}
