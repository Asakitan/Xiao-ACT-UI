// SAO Auto — Wave 6 alerts tests.
//
// Coverage:
//   * speak with empty text is a no-op success (SAPI does not get
//     initialised on this path — callers can call speak("") to poke
//     the module without paying the COM cost)
//   * banner_show gives out a positive id, drain retrieves it,
//     banner_hide on the same id returns OK
//   * sound_play with a missing file yields SAO_STATUS_ERR_NOT_FOUND
//   * voice enumeration query mode returns a non-negative count and
//     tolerates zero-voice hosts (embedded / CI images)

#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/alerts.h"

TEST_CASE("alerts_speak_empty_returns_ok",
          "[ui][alerts][wave6]") {
    // Empty and null strings are documented no-ops that never touch
    // COM / SAPI — the caller can invoke speak("") in a hot loop
    // without paying startup cost.
    REQUIRE(sao_ui_alerts_speak(nullptr, nullptr, 0, 50)
            == SAO_STATUS_OK);
    const uint16_t empty[] = {0};
    REQUIRE(sao_ui_alerts_speak(empty, nullptr, 0, 50)
            == SAO_STATUS_OK);
}

TEST_CASE("alerts_banner_show_hide_queue",
          "[ui][alerts][wave6]") {
    const uint16_t text[] = { 'B', 'o', 's', 's', ' ',
                              'w', 'i', 'n', 'd', 'o', 'w', 0 };
    uint64_t id_a = 0;
    REQUIRE(sao_ui_alerts_banner_show(text, 3000, 7, &id_a)
            == SAO_STATUS_OK);
    REQUIRE(id_a > 0);

    // Query mode reports at least the one we just added.
    size_t count = 0;
    REQUIRE(sao_ui_alerts_banner_drain(nullptr, 0, &count)
            == SAO_STATUS_OK);
    REQUIRE(count >= 1);

    // Actual drain — the record contains our id, duration, color.
    std::vector<SaoUiAlertBanner> out(4);
    size_t drained = 0;
    REQUIRE(sao_ui_alerts_banner_drain(out.data(), out.size(),
                                        &drained)
            == SAO_STATUS_OK);
    bool found = false;
    for (size_t i = 0; i < drained; ++i) {
        if (out[i].banner_id == id_a) {
            REQUIRE(out[i].duration_ms == 3000);
            REQUIRE(out[i].color_id == 7);
            REQUIRE(out[i].text_utf16 != nullptr);
            REQUIRE(out[i].text_len == 11);
            found = true;
        }
    }
    REQUIRE(found);

    // hide on the same id (which is now drained) still returns OK —
    // the API is documented idempotent.
    REQUIRE(sao_ui_alerts_banner_hide(id_a) == SAO_STATUS_OK);

    // hide on an unknown id is also OK.
    REQUIRE(sao_ui_alerts_banner_hide(9999999ull) == SAO_STATUS_OK);
}

TEST_CASE("alerts_sound_play_missing_file_returns_error",
          "[ui][alerts][wave6]") {
    const wchar_t missing[] =
        L"C:/definitely-not-a-real-directory/no-such.wav";
    uint64_t sound_id = 42;
    sao_status_t rc = sao_ui_alerts_sound_play(
        reinterpret_cast<const uint16_t*>(missing), 50, &sound_id);
    // On Windows we return NOT_FOUND for the missing file.  On non-
    // Windows the module short-circuits with CAPABILITY_MISSING (post
    // Wave 17a) — NOT_IMPLEMENTED is still accepted for older builds.
    const bool rc_ok = (rc == SAO_STATUS_ERR_NOT_FOUND) ||
                       (rc == SAO_STATUS_ERR_NOT_IMPLEMENTED) ||
                       (rc == SAO_STATUS_ERR_CAPABILITY_MISSING);
    REQUIRE(rc_ok);
    REQUIRE(sound_id == 0);

    // stop_all is always a legal invocation.
    sao_status_t stop_rc = sao_ui_alerts_sound_stop(0);
    const bool stop_ok = (stop_rc == SAO_STATUS_OK) ||
                        (stop_rc == SAO_STATUS_ERR_NOT_IMPLEMENTED) ||
                        (stop_rc == SAO_STATUS_ERR_CAPABILITY_MISSING);
    REQUIRE(stop_ok);
}

TEST_CASE("alerts_speak_voice_enumeration",
          "[ui][alerts][wave6]") {
    // Query mode — capacity=0 populates count only.
    size_t count = 0xdeadbeef;
    sao_status_t rc = sao_ui_alerts_enum_voices(nullptr, 0, 0, &count);
    // Any of: SAPI absent (OS_CALL_FAILED / NOT_IMPLEMENTED /
    // CAPABILITY_MISSING per Wave 17a) — accept any.  Windows CI images
    // almost always have at least one voice, but a slim container may
    // have none.
    const bool rc_ok = (rc == SAO_STATUS_OK) ||
                       (rc == SAO_STATUS_ERR_OS_CALL_FAILED) ||
                       (rc == SAO_STATUS_ERR_NOT_IMPLEMENTED) ||
                       (rc == SAO_STATUS_ERR_CAPABILITY_MISSING);
    REQUIRE(rc_ok);
    if (rc == SAO_STATUS_OK) {
        // Count is a plausible number.  Zero is acceptable on
        // headless / voice-less images.
        REQUIRE(count < 1024);
        if (count > 0) {
            // Real fetch.
            const size_t stride = 128;
            std::vector<char> buffer(stride * count, 0);
            size_t written = 0;
            REQUIRE(sao_ui_alerts_enum_voices(buffer.data(), count,
                                              stride, &written)
                    == SAO_STATUS_OK);
            REQUIRE(written == count);
            // Every slot must be null-terminated within the stride.
            for (size_t i = 0; i < written; ++i) {
                const char* slot = buffer.data() + i * stride;
                bool nul_found = false;
                for (size_t j = 0; j < stride; ++j) {
                    if (slot[j] == 0) { nul_found = true; break; }
                }
                REQUIRE(nul_found);
            }
        }
    }
}
