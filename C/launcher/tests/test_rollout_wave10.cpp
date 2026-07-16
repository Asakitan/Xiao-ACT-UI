// SAO Auto - launcher/tests/test_rollout_wave10.cpp
//
// Wave 10 / Agent a - Phase 12 rollout system tests.
//
// Covers the seven public APIs declared in rollout.h:
//   config_load / config_save          - schema round-trip, defaults
//   compute_bucket                     - determinism + uniform distribution
//   should_use_cpp                     - percent + override interaction
//   record_success / record_failure    - stats persistence + telemetry
//   check_auto_retreat                 - 3-of-5 rule
//   dispatch                           - priority ladder fallbacks
//
// Each case redirects %APPDATA%\SaoAuto\ to a scratch dir under %TEMP% via
// sao_rollout_test_set_appdata_dir(), and installs a telemetry hook that
// captures events into a thread-local buffer.

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/rollout.h"
#include "sao/launcher/dual_run.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Scratch dir helper.  Each case creates a fresh dir under %TEMP% and points
// the rollout module at it; guard tears the dir down.
// ---------------------------------------------------------------------------
fs::path unique_scratch_dir(const char* stem) {
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    wchar_t buf[MAX_PATH]{};
    _snwprintf_s(buf, MAX_PATH, _TRUNCATE,
                 L"%ssao_rollout_%hs_%llx",
                 tmp, stem, static_cast<unsigned long long>(li.QuadPart));
    fs::path p(buf);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// ---------------------------------------------------------------------------
// Telemetry capture - the rollout module can be pointed at a hook that
// receives every published event verbatim.  We stash them per-test in a
// static vector guarded by a mutex.
// ---------------------------------------------------------------------------
struct CapturedEvent {
    std::string name;
    std::string props;
};
std::mutex g_captured_mtx;
std::vector<CapturedEvent> g_captured;

void telemetry_capture_hook(const char* name, const char* props) {
    std::lock_guard<std::mutex> g(g_captured_mtx);
    g_captured.push_back({name ? name : "", props ? props : ""});
}

void reset_captured() {
    std::lock_guard<std::mutex> g(g_captured_mtx);
    g_captured.clear();
}

size_t captured_count() {
    std::lock_guard<std::mutex> g(g_captured_mtx);
    return g_captured.size();
}

std::vector<CapturedEvent> captured_snapshot() {
    std::lock_guard<std::mutex> g(g_captured_mtx);
    return g_captured;
}

// ---------------------------------------------------------------------------
// Per-case guard: fresh scratch dir + reset module state + capture hook +
// tear down on destruct.
// ---------------------------------------------------------------------------
struct RolloutGuard {
    fs::path dir;
    RolloutGuard(const char* stem) : dir(unique_scratch_dir(stem)) {
        sao_rollout_reset_for_test();
        sao_rollout_test_set_appdata_dir(dir.wstring().c_str());
        sao_rollout_test_set_telemetry_hook(&telemetry_capture_hook);
        reset_captured();
    }
    ~RolloutGuard() {
        sao_rollout_test_set_telemetry_hook(nullptr);
        sao_rollout_test_set_appdata_dir(nullptr);
        sao_rollout_reset_for_test();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// ---------------------------------------------------------------------------
// String helpers so per-case setup stays terse.  We use BCryptGenRandom for
// entropy so bucket-uniformity tests aren't fighting the LCG's own bias.
// ---------------------------------------------------------------------------
struct RngHolder {
    BCRYPT_ALG_HANDLE alg = nullptr;
    RngHolder() {
        ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_RNG_ALGORITHM, nullptr, 0);
    }
    ~RngHolder() { if (alg) ::BCryptCloseAlgorithmProvider(alg, 0); }
};

std::string synth_anon_id(BCRYPT_ALG_HANDLE rng) {
    uint8_t bytes[16]{};
    ::BCryptGenRandom(rng, bytes, sizeof(bytes), 0);
    char buf[33]{};
    for (int i = 0; i < 16; ++i) {
        _snprintf_s(buf + i * 2, 3, _TRUNCATE, "%02x", bytes[i]);
    }
    return std::string(buf, 32);
}

} // namespace

// ===========================================================================
// 1) config_load_default_when_missing
// ===========================================================================
TEST_CASE("rollout_config_load_default_when_missing",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("cfg_missing");
    sao_rollout_config cfg{};
    // Deliberately corrupt with sentinels so we can prove _load overwrote
    // with defaults.
    std::memset(&cfg, 0xAB, sizeof(cfg));

    REQUIRE(sao_rollout_config_load(&cfg) == SAO_STATUS_OK);
    REQUIRE(cfg.schema == 1);
    REQUIRE(cfg.cpp_percent == 100);
    REQUIRE(cfg.user_override == SAO_ROLLOUT_USER_OVERRIDE_UNSET);
    REQUIRE(cfg.retreat_history_count == 0);
}

// ===========================================================================
// 2) config_roundtrip_persists_all_fields
// ===========================================================================
TEST_CASE("rollout_config_roundtrip_persists_all_fields",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("cfg_roundtrip");

    sao_rollout_config in{};
    sao_rollout_config_default(&in);
    in.cpp_percent = 37;
    in.user_override = SAO_ROLLOUT_USER_OVERRIDE_CPP;
    in.retreat_history_count = 2;
    in.retreat_history[0] = {1'700'000'000'000LL, 100, 50, "3_of_5_failed"};
    in.retreat_history[1] = {1'700'000'060'000LL,  50, 25, "3_of_5_failed"};

    REQUIRE(sao_rollout_config_save(&in) == SAO_STATUS_OK);

    sao_rollout_config out{};
    REQUIRE(sao_rollout_config_load(&out) == SAO_STATUS_OK);

    REQUIRE(out.schema        == in.schema);
    REQUIRE(out.cpp_percent   == in.cpp_percent);
    REQUIRE(out.user_override == in.user_override);
    REQUIRE(out.retreat_history_count == in.retreat_history_count);
    REQUIRE(out.retreat_history[0].ts_ms       == in.retreat_history[0].ts_ms);
    REQUIRE(out.retreat_history[0].old_percent == in.retreat_history[0].old_percent);
    REQUIRE(out.retreat_history[0].new_percent == in.retreat_history[0].new_percent);
    REQUIRE(std::strcmp(out.retreat_history[0].reason,
                        in.retreat_history[0].reason) == 0);
    REQUIRE(out.retreat_history[1].ts_ms       == in.retreat_history[1].ts_ms);
    REQUIRE(out.retreat_history[1].old_percent == in.retreat_history[1].old_percent);
    REQUIRE(out.retreat_history[1].new_percent == in.retreat_history[1].new_percent);
}

// ===========================================================================
// 3) bucket_deterministic_same_anon_same_salt_same_result
// ===========================================================================
TEST_CASE("rollout_bucket_deterministic_same_anon_same_salt_same_result",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("bucket_det");
    // Same input -> same bucket, repeatable, in-range.
    const char* anon = "00112233-4455-6677-8899-aabbccddeeff";
    int32_t b1 = sao_rollout_compute_bucket(anon, "");
    int32_t b2 = sao_rollout_compute_bucket(anon, "");
    int32_t b3 = sao_rollout_compute_bucket(anon, "salt_change");

    REQUIRE(b1 >= 0);
    REQUIRE(b1 < 100);
    REQUIRE(b1 == b2);
    // Different salt -> different bucket (with extremely high probability).
    // If they happen to collide the test still passes when b1 == b3 but
    // that's a 1/100 chance we accept as noise; run the assertion the other
    // way and require that "same salt" is what pins b1==b2.
    REQUIRE(b3 >= 0);
    REQUIRE(b3 < 100);

    // Missing anon -> -1 sentinel.
    REQUIRE(sao_rollout_compute_bucket(nullptr, "") == -1);
    REQUIRE(sao_rollout_compute_bucket("", "") == -1);
}

// ===========================================================================
// 4) bucket_uniform_distribution_10000_samples (stddev < 20)
// ===========================================================================
TEST_CASE("rollout_bucket_uniform_distribution_10000_samples",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("bucket_uniform");
    RngHolder rng;
    REQUIRE(rng.alg != nullptr);
    constexpr int kN = 10'000;
    std::vector<int32_t> hist(100, 0);
    for (int i = 0; i < kN; ++i) {
        auto id = synth_anon_id(rng.alg);
        int32_t b = sao_rollout_compute_bucket(id.c_str(), "");
        REQUIRE(b >= 0);
        REQUIRE(b < 100);
        ++hist[b];
    }
    double mean = static_cast<double>(kN) / 100.0;   // = 100
    double var = 0.0;
    int32_t hmin = INT32_MAX;
    int32_t hmax = 0;
    for (auto v : hist) {
        double d = static_cast<double>(v) - mean;
        var += d * d;
        if (v < hmin) hmin = v;
        if (v > hmax) hmax = v;
    }
    var /= 100.0;
    double stddev = 0.0;
    if (var > 0.0) {
        stddev = var;
        for (int i = 0; i < 32; ++i) stddev = 0.5 * (stddev + var / stddev);
    }
    REQUIRE(stddev < 20.0);
    REQUIRE(hmin > 0);       // no empty buckets
    REQUIRE(hmax < 3 * mean); // no runaway hotspot
}

// ===========================================================================
// 5) should_use_cpp_percent_0_returns_false_all
// ===========================================================================
TEST_CASE("rollout_should_use_cpp_percent_0_returns_false_all",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("sucpp_0");
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 0;
    // No user override.
    for (int b = 0; b < 100; ++b) {
        REQUIRE(sao_rollout_should_use_cpp(b, &cfg) == 0);
    }
    REQUIRE(sao_rollout_should_use_cpp(-1, &cfg) == 0);
    REQUIRE(sao_rollout_should_use_cpp(50, &cfg) == 0);
}

// ===========================================================================
// 6) should_use_cpp_percent_100_returns_true_all
// ===========================================================================
TEST_CASE("rollout_should_use_cpp_percent_100_returns_true_all",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("sucpp_100");
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 100;
    for (int b = 0; b < 100; ++b) {
        REQUIRE(sao_rollout_should_use_cpp(b, &cfg) == 1);
    }
    REQUIRE(sao_rollout_should_use_cpp(-1, &cfg) == 1);
    REQUIRE(sao_rollout_should_use_cpp(200, &cfg) == 1);
}

// ===========================================================================
// 7) should_use_cpp_percent_50_splits_evenly (+/- 5%)
// ===========================================================================
TEST_CASE("rollout_should_use_cpp_percent_50_splits_evenly",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("sucpp_50");
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 50;
    // With a uniform bucket, exactly buckets 0..49 -> CPP, 50..99 -> Python.
    int cpp_count = 0;
    for (int b = 0; b < 100; ++b) {
        if (sao_rollout_should_use_cpp(b, &cfg)) ++cpp_count;
    }
    REQUIRE(cpp_count == 50);

    // And when driven by synthetic anon UUIDs the split stays within 5%.
    RngHolder rng;
    REQUIRE(rng.alg != nullptr);
    constexpr int kN = 5000;
    int cpp_bucket_count = 0;
    for (int i = 0; i < kN; ++i) {
        auto id = synth_anon_id(rng.alg);
        int32_t b = sao_rollout_compute_bucket(id.c_str(), "");
        if (sao_rollout_should_use_cpp(b, &cfg)) ++cpp_bucket_count;
    }
    double ratio = static_cast<double>(cpp_bucket_count) / static_cast<double>(kN);
    REQUIRE(ratio > 0.45);
    REQUIRE(ratio < 0.55);
}

// ===========================================================================
// 8) user_override_wins_over_percent
// ===========================================================================
TEST_CASE("rollout_user_override_wins_over_percent",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("override");
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 0;                    // would say python-only
    cfg.user_override = SAO_ROLLOUT_USER_OVERRIDE_CPP;
    REQUIRE(sao_rollout_should_use_cpp(50, &cfg) == 1);
    REQUIRE(sao_rollout_should_use_cpp(-1, &cfg) == 1);

    cfg.cpp_percent = 100;                  // would say cpp-only
    cfg.user_override = SAO_ROLLOUT_USER_OVERRIDE_PYTHON;
    REQUIRE(sao_rollout_should_use_cpp(50, &cfg) == 0);
    REQUIRE(sao_rollout_should_use_cpp(99, &cfg) == 0);

    // AUTO must NOT win - falls back to percent logic.
    cfg.user_override = SAO_ROLLOUT_USER_OVERRIDE_AUTO;
    cfg.cpp_percent = 100;
    REQUIRE(sao_rollout_should_use_cpp(50, &cfg) == 1);
    cfg.cpp_percent = 0;
    REQUIRE(sao_rollout_should_use_cpp(50, &cfg) == 0);
}

// ===========================================================================
// 9) record_success_publishes_telemetry_event
// ===========================================================================
TEST_CASE("rollout_record_success_publishes_telemetry_event",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("rec_success");
    REQUIRE(captured_count() == 0);

    REQUIRE(sao_rollout_record_success(SAO_DUAL_RUN_MODE_CPP_ONLY, 1234) == SAO_STATUS_OK);
    REQUIRE(captured_count() == 1);
    auto snap = captured_snapshot();
    REQUIRE(snap[0].name == "sao.rollout.launch_success");
    // Duration and mode should be in the props JSON verbatim.
    REQUIRE(snap[0].props.find("\"mode\": \"cpp_only\"") != std::string::npos);
    REQUIRE(snap[0].props.find("\"duration_ms\": 1234") != std::string::npos);

    // Stats file must reflect the success too.
    sao_rollout_stats st{};
    REQUIRE(sao_rollout_stats_load(&st) == SAO_STATUS_OK);
    REQUIRE(st.total_successes == 1);
    REQUIRE(st.total_failures == 0);
    REQUIRE(st.recent_count == 1);
    REQUIRE(st.recent[0].success == 1);
    REQUIRE(st.recent[0].mode == SAO_DUAL_RUN_MODE_CPP_ONLY);
}

// ===========================================================================
// 10) record_failure_increments_counter
// ===========================================================================
TEST_CASE("rollout_record_failure_increments_counter",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("rec_failure");
    REQUIRE(sao_rollout_record_failure(SAO_DUAL_RUN_MODE_CPP_ONLY,
                                        SAO_STATUS_PLATFORM_INIT_FAIL,
                                        "platform_bringup") == SAO_STATUS_OK);
    REQUIRE(sao_rollout_record_failure(SAO_DUAL_RUN_MODE_CPP_ONLY,
                                        SAO_STATUS_PLATFORM_INIT_FAIL,
                                        "platform_bringup") == SAO_STATUS_OK);

    sao_rollout_stats st{};
    REQUIRE(sao_rollout_stats_load(&st) == SAO_STATUS_OK);
    REQUIRE(st.total_failures == 2);
    REQUIRE(st.recent_count == 2);
    REQUIRE(st.recent[1].reason_code == SAO_STATUS_PLATFORM_INIT_FAIL);

    // Two telemetry events fired.
    REQUIRE(captured_count() == 2);
    auto snap = captured_snapshot();
    REQUIRE(snap[0].name == "sao.rollout.launch_failure");
    REQUIRE(snap[0].props.find("\"stack_hint\": \"platform_bringup\"")
            != std::string::npos);
}

// ===========================================================================
// 11) auto_retreat_triggers_at_3_of_5_failures
// ===========================================================================
TEST_CASE("rollout_auto_retreat_triggers_at_3_of_5_failures",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("retreat");

    // Start at 100% cpp.
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 100;
    REQUIRE(sao_rollout_config_save(&cfg) == SAO_STATUS_OK);

    // 2 successes -> not enough failures.
    REQUIRE(sao_rollout_record_success(SAO_DUAL_RUN_MODE_CPP_ONLY, 100) == SAO_STATUS_OK);
    REQUIRE(sao_rollout_record_success(SAO_DUAL_RUN_MODE_CPP_ONLY, 100) == SAO_STATUS_OK);
    int32_t new_pct = -1;
    REQUIRE(sao_rollout_check_auto_retreat(&new_pct) == 0);
    REQUIRE(new_pct == -1);

    // 3 failures on top -> 3-of-5 kicks in.  Recent window will be [succ,
    // succ, fail, fail, fail].
    REQUIRE(sao_rollout_record_failure(SAO_DUAL_RUN_MODE_CPP_ONLY, -1, "ac") == SAO_STATUS_OK);
    REQUIRE(sao_rollout_record_failure(SAO_DUAL_RUN_MODE_CPP_ONLY, -1, "ac") == SAO_STATUS_OK);
    REQUIRE(sao_rollout_record_failure(SAO_DUAL_RUN_MODE_CPP_ONLY, -1, "ac") == SAO_STATUS_OK);

    new_pct = -1;
    REQUIRE(sao_rollout_check_auto_retreat(&new_pct) == 1);
    REQUIRE(new_pct == 50);

    // Config was persisted with the new percent and a retreat_history entry.
    sao_rollout_config after{};
    REQUIRE(sao_rollout_config_load(&after) == SAO_STATUS_OK);
    REQUIRE(after.cpp_percent == 50);
    REQUIRE(after.retreat_history_count == 1);
    REQUIRE(after.retreat_history[0].old_percent == 100);
    REQUIRE(after.retreat_history[0].new_percent == 50);
    REQUIRE(std::strcmp(after.retreat_history[0].reason, "3_of_5_failed") == 0);

    // Telemetry: 5 records (2 success + 3 failure) + 1 retreat = 6 events.
    REQUIRE(captured_count() == 6);
    auto snap = captured_snapshot();
    REQUIRE(snap.back().name == "sao.rollout.auto_retreat");
    REQUIRE(snap.back().props.find("\"new_percent\": 50") != std::string::npos);
    REQUIRE(snap.back().props.find("\"old_percent\": 100") != std::string::npos);
}

// ===========================================================================
// 12) dispatcher_falls_back_to_dual_run_when_no_rollout
// ===========================================================================
TEST_CASE("rollout_dispatcher_falls_back_to_dual_run_when_no_rollout",
          "[launcher][rollout][wave10]") {
    RolloutGuard g("dispatch_fallback");

    sao_dual_run_config dual{};
    sao_launcher_dual_run_config_default(&dual);
    dual.mode = SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK;

    // Case A: no anon_id + default (100%) cpp_percent -> CPP_ONLY still wins
    // because cpp_percent==100 short-circuits before bucket derivation.
    sao_launcher_dual_run_mode_t mode = SAO_DUAL_RUN_MODE_DUAL_SIDE_BY_SIDE;
    REQUIRE(sao_rollout_dispatch(nullptr, &dual, &mode) == SAO_STATUS_OK);
    REQUIRE(mode == SAO_DUAL_RUN_MODE_CPP_ONLY);

    // Case B: set cpp_percent to 50 (indeterminate without anon_id) -> fall
    // back to dual.mode.
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 50;
    REQUIRE(sao_rollout_config_save(&cfg) == SAO_STATUS_OK);

    mode = SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
    REQUIRE(sao_rollout_dispatch(nullptr, &dual, &mode) == SAO_STATUS_OK);
    REQUIRE(mode == dual.mode);

    // Case C: user_override wins even with no anon_id.
    cfg.user_override = SAO_ROLLOUT_USER_OVERRIDE_PYTHON;
    REQUIRE(sao_rollout_config_save(&cfg) == SAO_STATUS_OK);
    mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
    REQUIRE(sao_rollout_dispatch(nullptr, &dual, &mode) == SAO_STATUS_OK);
    REQUIRE(mode == SAO_DUAL_RUN_MODE_PYTHON_ONLY);

    // Case D: with an anon_id + user_override=UNSET + 50%, decision is
    // deterministic per anon.  Two calls same anon -> same mode.
    cfg.user_override = SAO_ROLLOUT_USER_OVERRIDE_UNSET;
    REQUIRE(sao_rollout_config_save(&cfg) == SAO_STATUS_OK);
    sao_launcher_dual_run_mode_t m1 = 0, m2 = 0;
    const char* anon = "test-anon-uuid";
    REQUIRE(sao_rollout_dispatch(anon, &dual, &m1) == SAO_STATUS_OK);
    REQUIRE(sao_rollout_dispatch(anon, &dual, &m2) == SAO_STATUS_OK);
    REQUIRE(m1 == m2);
    REQUIRE((m1 == SAO_DUAL_RUN_MODE_CPP_ONLY
             || m1 == SAO_DUAL_RUN_MODE_PYTHON_ONLY));
}
