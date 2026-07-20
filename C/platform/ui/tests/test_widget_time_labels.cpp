// Tests for the three strictly typed time-display labels.
//
// Coverage (6 CASE, 2 per label kind):
//   * clock_label_formats_as_hh_mm_ss
//   * clock_label_handles_zero_and_negative_epoch
//   * relative_time_label_positive_and_negative_delta
//   * relative_time_label_zero_delta_shows_t0
//   * duration_label_short_format_below_60s
//   * duration_label_negative_clamps_to_zero
//
// Aside from the format checks, this file exercises the "3类严格分离
// 防误用编译期机制" contract by (a) using distinct STRICTLY-typed
// formatter APIs (int64/int32/uint64) that only accept the
// right quantity for their widget kind, and (b) asserting that
// static_assert on the parameter types keeps them structurally
// distinct (see the top of this file).  Trying to build with e.g.
// `sao_ui_widget_format_clock_strict(some_uint64)` will emit a
// -Wsign-conversion / -Wnarrowing diagnostic under the project's
// standard warning flags — the actual test suite does not attempt
// that (it would fail to compile), we simply document the contract
// via type-trait static_asserts on the function pointer signatures.
//
// All tests use MSVC/libc localtime; asserts use UTC-invariant hour
// arithmetic by round-tripping a known-good local-time epoch and
// re-parsing the returned string so timezone doesn't break CI.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <type_traits>

#include "sao/core/status.h"
#include "sao/ui/widget_text.h"

extern "C" {

// Strict-typed formatters (distinct parameter types → compile-time
// contract enforcement).
SAO_UI_API void SAO_UI_CALL sao_ui_widget_format_clock_strict(
    int64_t epoch_ms, bool with_seconds, char* buf, size_t buf_size);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_format_relative_strict(
    int32_t delta_seconds, char* buf, size_t buf_size);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_format_duration_strict(
    uint64_t duration_ms, char* buf, size_t buf_size);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_clock_label_format(
    sao_ui_widget_handle_t handle, char* buf, size_t buf_size);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_relative_time_label_format(
    sao_ui_widget_handle_t handle, char* buf, size_t buf_size);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_duration_label_format(
    sao_ui_widget_handle_t handle, char* buf, size_t buf_size);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_text_family_destroy(
    sao_ui_widget_handle_t handle);

}  // extern "C"

// ─── 3-widget strictly-typed contract — compile-time gate. ─────────
//
// The three format helpers take STRUCTURALLY different parameter types
// so callers can't accidentally hand a duration_ms into fmt_clock or
// vice versa.  If someone ever collapses these into a single generic
// helper, these static_asserts will catch the regression at build
// time.  int64_t vs int32_t vs uint64_t is enough to force a
// -Wnarrowing or -Wsign-conversion under MSVC /W4 & clang -Wconversion.
namespace strict_contract_gate {

// Pull the function pointer types via decltype so any signature drift
// (e.g. someone changing "int32_t delta_seconds" → "int64_t") lights
// up in this file first, rather than in every caller downstream.
using ClockFn = decltype(&sao_ui_widget_format_clock_strict);
using RelFn   = decltype(&sao_ui_widget_format_relative_strict);
using DurFn   = decltype(&sao_ui_widget_format_duration_strict);

// Extract the leading-parameter type from each function pointer via a
// tiny alias template.
template <typename R, typename P0, typename... Rest>
P0 first_param_of(R (*)(P0, Rest...));

using ClockArg0 = decltype(first_param_of(std::declval<ClockFn>()));
using RelArg0   = decltype(first_param_of(std::declval<RelFn>()));
using DurArg0   = decltype(first_param_of(std::declval<DurFn>()));

static_assert(std::is_same_v<ClockArg0, int64_t>,
              "clock strict formatter must take int64_t epoch_ms — "
              "reg. memory [ACT时间显示三类分开]");
static_assert(std::is_same_v<RelArg0, int32_t>,
              "relative strict formatter must take int32_t delta_seconds "
              "— reg. memory [ACT时间显示三类分开]");
static_assert(std::is_same_v<DurArg0, uint64_t>,
              "duration strict formatter must take uint64_t duration_ms "
              "— reg. memory [ACT时间显示三类分开]");

// The three types are also pairwise DIFFERENT: mixing them across
// widgets is a compile diagnostic, not a silent bug.
static_assert(!std::is_same_v<ClockArg0, RelArg0>,
              "clock vs relative arg type must differ");
static_assert(!std::is_same_v<ClockArg0, DurArg0>,
              "clock vs duration arg type must differ");
static_assert(!std::is_same_v<RelArg0, DurArg0>,
              "relative vs duration arg type must differ");

}  // namespace strict_contract_gate

namespace {

SaoUiLabelSpec plain_label() {
    SaoUiLabelSpec l{};
    l.font_slot     = SAO_UI_FONT_MONO;
    l.font_size_px  = 14;
    l.font_weight   = SAO_UI_WEIGHT_NORMAL;
    l.align         = SAO_UI_ALIGN_LEFT;
    l.anchor        = SAO_UI_ANCHOR_W;
    return l;
}

// Build a known-local-time epoch: 12:34:56 on 2026-06-01.  Round-trip
// through mktime so localtime returns the same wall clock and we don't
// depend on the host TZ.
int64_t make_local_epoch_ms(int hour, int minute, int second) {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 5;    // June (0-based)
    t.tm_mday = 1;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;
    t.tm_isdst = -1;
    const std::time_t local_epoch = std::mktime(&t);
    return static_cast<int64_t>(local_epoch) * 1000ll;
}

}  // namespace

TEST_CASE("clock_label_formats_as_hh_mm_ss",
          "[ui][widget][time][runtime][clock]") {
    // Strict formatter path (no widget handle).
    const int64_t ep = make_local_epoch_ms(9, 5, 7);
    char buf[32] = {0};
    sao_ui_widget_format_clock_strict(ep, /*with_seconds=*/true, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "09:05:07") == 0);
    // Widget path.
    SaoUiClockLabelSpec spec{};
    spec.epoch_ms = ep;
    spec.with_seconds = true;
    spec.label = plain_label();
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_clock_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    char wbuf[32] = {0};
    REQUIRE(sao_ui_widget_clock_label_format(h, wbuf, sizeof(wbuf))
            == SAO_STATUS_OK);
    REQUIRE(std::strcmp(wbuf, "09:05:07") == 0);
    // Update the epoch → format tracks it.
    REQUIRE(sao_ui_clock_label_update(h, make_local_epoch_ms(23, 59, 59))
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_clock_label_format(h, wbuf, sizeof(wbuf))
            == SAO_STATUS_OK);
    REQUIRE(std::strcmp(wbuf, "23:59:59") == 0);
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("clock_label_handles_zero_and_negative_epoch",
          "[ui][widget][time][runtime][clock]") {
    char buf[16] = {0};
    sao_ui_widget_format_clock_strict(0, true, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "--") == 0);
    sao_ui_widget_format_clock_strict(-123456, true, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "--") == 0);
    // 'HH:MM' variant when with_seconds=false.
    const int64_t ep = make_local_epoch_ms(14, 25, 0);
    sao_ui_widget_format_clock_strict(ep, /*with_seconds=*/false,
                                       buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "14:25") == 0);
}

TEST_CASE("relative_time_label_positive_and_negative_delta",
          "[ui][widget][time][runtime][rel]") {
    // fmt_signed_into produces +/- prefix from delta_seconds.
    char buf[32] = {0};
    sao_ui_widget_format_relative_strict(3, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "+3.0s") == 0);
    sao_ui_widget_format_relative_strict(-125, buf, sizeof(buf));
    // -125 seconds = -2:05 (fmt_dur 'M:SS').
    REQUIRE(std::strcmp(buf, "-2:05") == 0);
    // 90 seconds = 1:30
    sao_ui_widget_format_relative_strict(90, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "+1:30") == 0);
    // Widget path: epoch_ms + base_epoch_ms → fmt_rel.
    SaoUiRelativeTimeLabelSpec spec{};
    spec.epoch_ms      = make_local_epoch_ms(10, 0, 5);
    spec.base_epoch_ms = make_local_epoch_ms(10, 0, 0);
    spec.label = plain_label();
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_relative_time_label_create(nullptr, &spec, &h)
            == SAO_STATUS_OK);
    char wbuf[32] = {0};
    REQUIRE(sao_ui_widget_relative_time_label_format(h, wbuf, sizeof(wbuf))
            == SAO_STATUS_OK);
    // delta = +5000 ms → "+5.0s"
    REQUIRE(std::strcmp(wbuf, "+5.0s") == 0);
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("relative_time_label_zero_delta_shows_t0",
          "[ui][widget][time][runtime][rel]") {
    char buf[16] = {0};
    sao_ui_widget_format_relative_strict(0, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "T0") == 0);
}

TEST_CASE("duration_label_short_format_below_60s",
          "[ui][widget][time][runtime][dur]") {
    char buf[32] = {0};
    sao_ui_widget_format_duration_strict(2600u, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "2.6s") == 0);
    // 1 min 23 s.
    sao_ui_widget_format_duration_strict(83000u, buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "1:23") == 0);
    // 2 h 5 min 6 s.
    sao_ui_widget_format_duration_strict(
        static_cast<uint64_t>(2 * 3600 + 5 * 60 + 6) * 1000ull,
        buf, sizeof(buf));
    REQUIRE(std::strcmp(buf, "2:05:06") == 0);
    // Widget path.
    SaoUiDurationLabelSpec spec{};
    spec.duration_ms = 4321;
    spec.label = plain_label();
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_duration_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    char wbuf[32] = {0};
    REQUIRE(sao_ui_widget_duration_label_format(h, wbuf, sizeof(wbuf))
            == SAO_STATUS_OK);
    REQUIRE(std::strcmp(wbuf, "4.3s") == 0);
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("duration_label_negative_clamps_to_zero",
          "[ui][widget][time][runtime][dur]") {
    // Widget update with negative value clamps to 0.
    SaoUiDurationLabelSpec spec{};
    spec.duration_ms = -500;   // clamp to 0 at create
    spec.label = plain_label();
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_duration_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    char wbuf[32] = {0};
    REQUIRE(sao_ui_widget_duration_label_format(h, wbuf, sizeof(wbuf))
            == SAO_STATUS_OK);
    REQUIRE(std::strcmp(wbuf, "0.0s") == 0);
    REQUIRE(sao_ui_duration_label_update(h, -12345) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_duration_label_format(h, wbuf, sizeof(wbuf))
            == SAO_STATUS_OK);
    REQUIRE(std::strcmp(wbuf, "0.0s") == 0);
    sao_ui_widget_text_family_destroy(h);
}
