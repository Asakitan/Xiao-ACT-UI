// SAO Auto — capability and implementation gap-closure tests for platform/core.
//
// Coverage matrix:
//   A. capability gate  — crypto/window Windows-only fallbacks.  On the
//      shipping Windows build the real path answers; we exercise the
//      real path and assert the entry point is no longer NOT_IMPLEMENTED.
//   B. real implementation — event / thread / path / string / time entry
//      points.  These used to sit behind a NOT_IMPLEMENTED stub on
//      non-Windows; the portable fallback lives inside the shared
//      implementation and can be observed via the ABI even on Windows.
//   C. legacy stub — none for core.
//
// Explicit non-goals: no driver, no real Npcap, no zstd stream, no real
// process spawn.  All state is synthesised locally through the ABI.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "sao/core/crypto.h"
#include "sao/core/event.h"
#include "sao/core/path.h"
#include "sao/core/status.h"
#include "sao/core/string.h"
#include "sao/core/thread.h"
#include "sao/core/time.h"
#include "sao/core/window.h"

#include "core_gap_support.h"

using sao::core_gap::GapKind;
using sao::core_gap::make_pattern_bytes;
using sao::core_gap::matches_gap_kind;
using sao::core_gap::utf8_byte_count_for;
using sao::core_gap::wide_from_utf8;

TEST_CASE("core gap status taxonomy exposes capability_missing",
          "[core][gap][status]") {
    const char* label = sao_status_str(SAO_STATUS_ERR_CAPABILITY_MISSING);
    REQUIRE(label != nullptr);
    CHECK(std::string(label) == "capability_missing");

    // NOT_IMPLEMENTED remains reachable — legacy stubs still return it.
    const char* not_impl = sao_status_str(SAO_STATUS_ERR_NOT_IMPLEMENTED);
    REQUIRE(not_impl != nullptr);
    CHECK(std::string(not_impl) == "not_implemented");

    // The two codes are distinct.
    CHECK(SAO_STATUS_ERR_CAPABILITY_MISSING != SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

TEST_CASE("core gap: crypto sha256 no longer reports NOT_IMPLEMENTED",
          "[core][gap][crypto]") {
    const auto bytes = make_pattern_bytes(64);
    std::array<std::uint8_t, SAO_HASH_SHA256_BYTES> digest{};
    const sao_status_t rc = sao_core_hash(
        SAO_HASH_SHA256, bytes.data(), bytes.size(), digest.data(),
        digest.size());
    CAPTURE(rc);
    // On Windows this is the real path; on non-Windows the fallback answers
    // CAPABILITY_MISSING.  Either way the legacy NOT_IMPLEMENTED must be
    // gone.
    CHECK(rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);
    if (rc == SAO_STATUS_OK) {
        // Sanity — a run through the CNG path leaves *something* in the
        // digest.  A zero buffer would signal a silent stub.
        bool any_non_zero = false;
        for (auto byte : digest) {
            if (byte != 0) {
                any_non_zero = true;
                break;
            }
        }
        CHECK(any_non_zero);
    } else {
        CHECK(matches_gap_kind(rc, GapKind::CapabilityGate));
    }
}

TEST_CASE("core gap: crypto hmac + aes-gcm reach capability parity",
          "[core][gap][crypto]") {
    // HMAC probe with a nominal key so we hit the real path on Windows and
    // the capability gate elsewhere.
    const auto data = make_pattern_bytes(32);
    const auto key = make_pattern_bytes(32);
    std::array<std::uint8_t, SAO_HASH_SHA256_BYTES> mac{};
    const sao_status_t hmac_rc = sao_core_hmac(
        SAO_HASH_SHA256, key.data(), key.size(), data.data(), data.size(),
        mac.data(), mac.size());
    CAPTURE(hmac_rc);
    CHECK(hmac_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);

    // AES-GCM encrypt then decrypt round trip.
    const std::array<std::uint8_t, 32> aes_key{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    const std::array<std::uint8_t, 12> nonce{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto plaintext = make_pattern_bytes(64);
    std::vector<std::uint8_t> ciphertext(plaintext.size());
    std::array<std::uint8_t, 16> tag{};
    const sao_status_t enc_rc = sao_core_aes_gcm_encrypt(
        aes_key.data(), nonce.data(), nullptr, 0, plaintext.data(),
        plaintext.size(), ciphertext.data(), tag.data());
    CAPTURE(enc_rc);
    CHECK(enc_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);
    if (enc_rc == SAO_STATUS_OK) {
        std::vector<std::uint8_t> roundtrip(plaintext.size());
        const sao_status_t dec_rc = sao_core_aes_gcm_decrypt(
            aes_key.data(), nonce.data(), nullptr, 0, ciphertext.data(),
            ciphertext.size(), tag.data(), roundtrip.data());
        REQUIRE(dec_rc == SAO_STATUS_OK);
        CHECK(std::memcmp(roundtrip.data(), plaintext.data(),
                          plaintext.size()) == 0);
    } else {
        CHECK(matches_gap_kind(enc_rc, GapKind::CapabilityGate));
    }

    // Random bytes — should never regress to NOT_IMPLEMENTED.
    std::array<std::uint8_t, 32> random{};
    const sao_status_t rand_rc =
        sao_core_random_bytes(random.data(), random.size());
    CAPTURE(rand_rc);
    CHECK(rand_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

TEST_CASE("core gap: window screen info + find-by-pid alive on Windows",
          "[core][gap][window]") {
    SaoScreenInfo info{};
    const sao_status_t screen_rc = sao_core_window_get_screen_info(&info);
    CAPTURE(screen_rc);
    CHECK(screen_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);

    // Find-by-pid against a non-existent PID probes the enumerator without
    // opening any external window — hermetic.
    void* hwnd = nullptr;
    const sao_status_t find_rc = sao_core_window_find_top_level_by_pid(
        0xFFFFFFFEu, nullptr, nullptr, &hwnd);
    CAPTURE(find_rc);
    CHECK(find_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);
    // Either the Windows path returned NOT_FOUND (nothing owned by that
    // impossible PID) or the fallback CAPABILITY_MISSING on non-Windows.
    CHECK((find_rc == SAO_STATUS_ERR_NOT_FOUND ||
           find_rc == SAO_STATUS_ERR_CAPABILITY_MISSING));

    // Is-visible on a null handle is HANDLE_INVALID on Windows and the
    // fallback CAPABILITY_MISSING elsewhere.  We *do* accept
    // INVALID_ARGUMENT because the pointer probe fires before the OS gate.
    bool visible = true;
    const sao_status_t vis_rc =
        sao_core_window_is_visible(nullptr, &visible);
    CAPTURE(vis_rc);
    CHECK(vis_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(visible == false);
}

TEST_CASE("core gap: event auto/manual reset lifecycle",
          "[core][gap][event]") {
    // Auto-reset — one signal wakes one waiter, second waiter times out.
    sao_core_wait_event_handle_t auto_evt = nullptr;
    REQUIRE(sao_core_wait_event_create(SAO_WAIT_RESET_AUTO, false,
                                       &auto_evt) == SAO_STATUS_OK);
    REQUIRE(auto_evt != nullptr);
    CHECK(sao_core_wait_event_wait(auto_evt, 5) == SAO_STATUS_ERR_TIMEOUT);
    CHECK(sao_core_wait_event_signal(auto_evt) == SAO_STATUS_OK);
    CHECK(sao_core_wait_event_wait(auto_evt, 100) == SAO_STATUS_OK);
    CHECK(sao_core_wait_event_wait(auto_evt, 5) == SAO_STATUS_ERR_TIMEOUT);
    sao_core_wait_event_destroy(auto_evt);

    // Manual reset — signal stays up until explicit reset.
    sao_core_wait_event_handle_t manual_evt = nullptr;
    REQUIRE(sao_core_wait_event_create(SAO_WAIT_RESET_MANUAL, true,
                                       &manual_evt) == SAO_STATUS_OK);
    CHECK(sao_core_wait_event_wait(manual_evt, 100) == SAO_STATUS_OK);
    CHECK(sao_core_wait_event_wait(manual_evt, 100) == SAO_STATUS_OK);
    CHECK(sao_core_wait_event_reset(manual_evt) == SAO_STATUS_OK);
    CHECK(sao_core_wait_event_wait(manual_evt, 5) == SAO_STATUS_ERR_TIMEOUT);
    sao_core_wait_event_destroy(manual_evt);
}

TEST_CASE("core gap: event cross-thread signal wakes waiter",
          "[core][gap][event]") {
    sao_core_wait_event_handle_t evt = nullptr;
    REQUIRE(sao_core_wait_event_create(SAO_WAIT_RESET_AUTO, false, &evt) ==
            SAO_STATUS_OK);
    std::atomic<sao_status_t> observed{SAO_STATUS_ERR_UNKNOWN};
    std::thread waiter([evt, &observed] {
        observed.store(sao_core_wait_event_wait(evt, 2000));
    });
    // Give the waiter time to enter wait_for before we fire.  The event
    // implementation guarantees "signal before wait" also works, so we
    // don't strictly need this, but it exercises the "wait -> signal"
    // sequence which is the interesting one.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(sao_core_wait_event_signal(evt) == SAO_STATUS_OK);
    waiter.join();
    CHECK(observed.load() == SAO_STATUS_OK);
    sao_core_wait_event_destroy(evt);
}

TEST_CASE("core gap: thread pool submit + drain",
          "[core][gap][thread]") {
    CHECK(sao_core_thread_pool_configure(2) == SAO_STATUS_OK);
    std::atomic<std::uint32_t> counter{0};
    auto increment = [](void* arg) {
        static_cast<std::atomic<std::uint32_t>*>(arg)->fetch_add(1);
    };
    for (int index = 0; index < 8; ++index) {
        CHECK(sao_core_thread_pool_submit(increment, &counter) ==
              SAO_STATUS_OK);
    }
    CHECK(sao_core_thread_pool_drain() == SAO_STATUS_OK);
    CHECK(counter.load() == 8);
    CHECK(sao_core_thread_current_id() != 0);
    // Yield is void — just prove we can call it without linker errors.
    sao_core_thread_yield();
}

TEST_CASE("core gap: timer fires periodically then stops cleanly",
          "[core][gap][thread][timer]") {
    std::atomic<std::uint32_t> ticks{0};
    auto tick = [](void* arg) {
        static_cast<std::atomic<std::uint32_t>*>(arg)->fetch_add(1);
    };
    sao_core_timer_handle_t timer = nullptr;
    REQUIRE(sao_core_timer_create(15, tick, &ticks, &timer) == SAO_STATUS_OK);
    REQUIRE(timer != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    sao_core_timer_destroy(timer);
    // We expect at least a couple of ticks; the exact number is
    // scheduler-dependent, but zero would mean the timer never fired.
    CHECK(ticks.load() >= 2);
}

TEST_CASE("core gap: path base dir + join + is_directory",
          "[core][gap][path]") {
    std::size_t needed = 0;
    REQUIRE(sao_core_path_base_dir(nullptr, 0, &needed) == SAO_STATUS_OK);
    REQUIRE(needed > 1);
    std::string buffer(needed, '\0');
    REQUIRE(sao_core_path_base_dir(buffer.data(), buffer.size(), &needed) ==
            SAO_STATUS_OK);
    // Strip trailing NUL (needed includes it).
    while (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    CHECK_FALSE(buffer.empty());

    const char* segments[] = {"parent", "child", "leaf.txt"};
    std::size_t join_needed = 0;
    REQUIRE(sao_core_path_join(segments, 3, nullptr, 0, &join_needed) ==
            SAO_STATUS_OK);
    std::string joined(join_needed, '\0');
    REQUIRE(sao_core_path_join(segments, 3, joined.data(), joined.size(),
                               &join_needed) == SAO_STATUS_OK);
    while (!joined.empty() && joined.back() == '\0') joined.pop_back();
    CHECK(joined.find("leaf.txt") != std::string::npos);
    CHECK_FALSE(sao_core_path_is_file(""));
    CHECK_FALSE(sao_core_path_is_directory(""));
    CHECK_FALSE(sao_core_path_exists(""));
}

TEST_CASE("core gap: string utf8/utf16 codec round trip",
          "[core][gap][string]") {
    // hello + Chinese "zhwn" (U+4E2D U+6587) + grinning-face emoji (U+1F600)
    // — hand-encoded UTF-8 so we don't rely on u8"" giving us char*
    // (C++20 changed the type to char8_t which no longer implicitly
    // converts to std::string).
    const std::string sample =
        "hello\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x98\x80";
    const std::wstring wide = wide_from_utf8(sample);
    REQUIRE_FALSE(wide.empty());
    CHECK(utf8_byte_count_for(wide) == sample.size() + 1);

    // Reject malformed byte should be flagged.
    const char malformed[] = {static_cast<char>(0xC3), '\0'};
    std::size_t probe = 0;
    const sao_status_t rc =
        sao_core_string_utf8_to_utf16(malformed, nullptr, 0, &probe);
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // ascii_icmp + trim helpers are pure C++ — quick sanity.
    CHECK(sao_core_string_ascii_icmp("Alpha", "alpha") == 0);
    CHECK(sao_core_string_ascii_icmp("a", "b") < 0);
    char buffer[] = "  padded  \0";
    const std::size_t trailing = sao_core_string_ascii_trim(buffer);
    CHECK(trailing == 2);
    CHECK(std::string(buffer) == "padded");

    // FNV-1a — deterministic tag over synthesised bytes.
    const auto bytes = make_pattern_bytes(16);
    const std::uint64_t hash =
        sao_core_string_fnv1a64(bytes.data(), bytes.size());
    CHECK(hash != 0);
}

TEST_CASE("core gap: time now / wall / sleep / scope",
          "[core][gap][time]") {
    const auto scope = sao_core_time_scope_begin();
    const std::uint64_t first_now = sao_core_time_now_ns();
    const std::uint64_t first_ms = sao_core_time_now_ms();
    CHECK(sao_core_time_sleep_ms(5) == SAO_STATUS_OK);
    const std::uint64_t second_now = sao_core_time_now_ns();
    const std::uint64_t elapsed = sao_core_time_scope_end(scope);
    CHECK(second_now > first_now);
    CHECK(first_ms == first_now / 1'000'000ULL);
    CHECK(elapsed > 0);
    CHECK(sao_core_time_wall_ns() > 0);
    // sleep_ms(0) must not report NOT_IMPLEMENTED under the time API contract.
    CHECK(sao_core_time_sleep_ms(0) == SAO_STATUS_OK);
}
