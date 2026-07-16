// Smoke test — confirms the module links, ABI version is queryable,
// and status codes stringify.  Real per-function tests land alongside
// each phase-2 implementation.

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "sao/core/abi.h"
#include "sao/core/crypto.h"
#include "sao/core/event.h"
#include "sao/core/path.h"
#include "sao/core/status.h"
#include "sao/core/string.h"
#include "sao/core/thread.h"
#include "sao/core/time.h"
#include "sao/core/window.h"

namespace {

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void SAO_CORE_CALL increment_counter(void* user_data) {
    static_cast<std::atomic<uint32_t>*>(user_data)->fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

TEST_CASE("core ABI version is non-zero", "[core][abi]") {
    REQUIRE(sao_core_abi_version() == SAO_CORE_ABI_VERSION);
    REQUIRE(sao_core_abi_version() != 0u);
}

TEST_CASE("status codes stringify", "[core][status]") {
    REQUIRE(sao_status_str(SAO_STATUS_OK) != nullptr);
    REQUIRE(sao_status_str(SAO_STATUS_ERR_NOT_IMPLEMENTED) != nullptr);
    // Out-of-range still returns a non-null pointer.
    REQUIRE(sao_status_str(static_cast<sao_status_t>(-9999)) != nullptr);
}

TEST_CASE("fnv1a64 is stable", "[core][string]") {
    const char* data = "sao";
    const uint64_t h1 = sao_core_string_fnv1a64(data, 3);
    const uint64_t h2 = sao_core_string_fnv1a64(data, 3);
    REQUIRE(h1 == h2);
    REQUIRE(h1 != 0u);
}

TEST_CASE("monotonic and wall clocks report usable time", "[core][time]") {
    const uint64_t wall_ns = sao_core_time_wall_ns();
    REQUIRE(wall_ns > 1'700'000'000'000'000'000ULL);

    const sao_core_time_scope_t start = sao_core_time_scope_begin();
    REQUIRE(sao_core_time_sleep_ms(2) == SAO_STATUS_OK);
    const uint64_t elapsed = sao_core_time_scope_end(start);
    REQUIRE(elapsed >= 1'000'000ULL);
    REQUIRE(sao_core_time_now_ms() <= sao_core_time_now_ns() / 1'000'000ULL + 1);
}

TEST_CASE("wait events honor auto and manual reset", "[core][event]") {
    sao_core_wait_event_handle_t event = nullptr;
    REQUIRE(sao_core_wait_event_create(SAO_WAIT_RESET_AUTO, false, &event) ==
            SAO_STATUS_OK);
    REQUIRE(event != nullptr);
    REQUIRE(sao_core_wait_event_wait(event, 0) == SAO_STATUS_ERR_TIMEOUT);
    REQUIRE(sao_core_wait_event_signal(event) == SAO_STATUS_OK);
    REQUIRE(sao_core_wait_event_wait(event, 0) == SAO_STATUS_OK);
    REQUIRE(sao_core_wait_event_wait(event, 0) == SAO_STATUS_ERR_TIMEOUT);
    sao_core_wait_event_destroy(event);

    event = nullptr;
    REQUIRE(sao_core_wait_event_create(SAO_WAIT_RESET_MANUAL, true, &event) ==
            SAO_STATUS_OK);
    REQUIRE(sao_core_wait_event_wait(event, 0) == SAO_STATUS_OK);
    REQUIRE(sao_core_wait_event_wait(event, 0) == SAO_STATUS_OK);
    REQUIRE(sao_core_wait_event_reset(event) == SAO_STATUS_OK);
    REQUIRE(sao_core_wait_event_wait(event, 0) == SAO_STATUS_ERR_TIMEOUT);
    sao_core_wait_event_destroy(event);
}

TEST_CASE("UTF conversions use caller allocated buffers", "[core][string]") {
    constexpr const char* utf8 = "Aldina-蓝";
    size_t wide_count = 0;
    REQUIRE(sao_core_string_utf8_to_utf16(utf8, nullptr, 0, &wide_count) ==
            SAO_STATUS_OK);
    REQUIRE(wide_count > 1);

    std::vector<wchar_t> too_small(wide_count - 1);
    REQUIRE(sao_core_string_utf8_to_utf16(
                utf8, too_small.data(), too_small.size(), &wide_count) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);

    std::vector<wchar_t> wide(wide_count);
    REQUIRE(sao_core_string_utf8_to_utf16(
                utf8, wide.data(), wide.size(), &wide_count) == SAO_STATUS_OK);

    size_t utf8_count = 0;
    REQUIRE(sao_core_string_utf16_to_utf8(
                wide.data(), nullptr, 0, &utf8_count) == SAO_STATUS_OK);
    std::vector<char> roundtrip(utf8_count);
    REQUIRE(sao_core_string_utf16_to_utf8(
                wide.data(), roundtrip.data(), roundtrip.size(), &utf8_count) ==
            SAO_STATUS_OK);
    REQUIRE(std::string(roundtrip.data()) == utf8);

    const char invalid_utf8[] = {static_cast<char>(0xC3), static_cast<char>(0x28), '\0'};
    REQUIRE(sao_core_string_utf8_to_utf16(
                invalid_utf8, nullptr, 0, &wide_count) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("ASCII comparison and trim are deterministic", "[core][string]") {
    REQUIRE(sao_core_string_ascii_icmp("GameAssembly.DLL", "gameassembly.dll") == 0);
    REQUIRE(sao_core_string_ascii_icmp("alpha", "beta") < 0);
    REQUIRE(sao_core_string_ascii_icmp(nullptr, "beta") < 0);

    char text[] = " \t  SAO Auto \r\n";
    REQUIRE(sao_core_string_ascii_trim(text) == 3);
    REQUIRE(std::strcmp(text, "SAO Auto") == 0);
}

TEST_CASE("path helpers preserve capacity and filesystem semantics", "[core][path]") {
    size_t base_needed = 0;
    REQUIRE(sao_core_path_base_dir(nullptr, 0, &base_needed) == SAO_STATUS_OK);
    REQUIRE(base_needed > 1);
    std::vector<char> base(base_needed);
    REQUIRE(sao_core_path_base_dir(base.data(), base.size(), &base_needed) ==
            SAO_STATUS_OK);
    REQUIRE(sao_core_path_is_directory(base.data()));

    const auto unique = std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(sao_core_time_now_ns());
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (L"sao-core-" + unique);
    const std::string root_utf8 = path_utf8(root);
    const char* segments[] = {root_utf8.c_str(), "nested", "..", "leaf"};
    size_t joined_needed = 0;
    REQUIRE(sao_core_path_join(segments, 4, nullptr, 0, &joined_needed) ==
            SAO_STATUS_OK);
    std::vector<char> joined(joined_needed);
    REQUIRE(sao_core_path_join(
                segments, 4, joined.data(), joined.size(), &joined_needed) ==
            SAO_STATUS_OK);
    REQUIRE(sao_core_path_make_dirs(joined.data()) == SAO_STATUS_OK);
    REQUIRE(sao_core_path_exists(joined.data()));
    REQUIRE(sao_core_path_is_directory(joined.data()));

    const std::filesystem::path file_path = root / L"leaf" / L"sample.txt";
    const HANDLE file = CreateFileW(file_path.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    REQUIRE(CloseHandle(file) != FALSE);
    const std::string file_utf8 = path_utf8(file_path);
    REQUIRE(sao_core_path_is_file(file_utf8.c_str()));

    size_t canonical_needed = 0;
    REQUIRE(sao_core_path_canonicalize(
                file_utf8.c_str(), nullptr, 0, &canonical_needed) == SAO_STATUS_OK);
    std::vector<char> canonical(canonical_needed - 1);
    REQUIRE(sao_core_path_canonicalize(file_utf8.c_str(),
                                       canonical.data(),
                                       canonical.size(),
                                       &canonical_needed) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    REQUIRE_FALSE(cleanup_error);
}

TEST_CASE("window discovery and geometry use real HWNDs", "[core][window]") {
    const wchar_t* class_name = L"SaoPlatformCoreBehaviorWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    const ATOM atom = RegisterClassW(&window_class);
    REQUIRE((atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS));

    const HWND hwnd = CreateWindowExW(0,
                                      class_name,
                                      L"SAO core discovery target",
                                      WS_OVERLAPPEDWINDOW,
                                      20,
                                      30,
                                      160,
                                      120,
                                      nullptr,
                                      nullptr,
                                      window_class.hInstance,
                                      nullptr);
    REQUIRE(hwnd != nullptr);

    void* found = nullptr;
    REQUIRE(sao_core_window_find_top_level_by_pid(GetCurrentProcessId(),
                                                   class_name,
                                                   L"discovery target",
                                                   &found) == SAO_STATUS_OK);
    REQUIRE(found == hwnd);

    SaoRect rect{};
    REQUIRE(sao_core_window_get_client_rect_screen(found, &rect) == SAO_STATUS_OK);
    REQUIRE(rect.right > rect.left);
    REQUIRE(rect.bottom > rect.top);
    bool visible = true;
    REQUIRE(sao_core_window_is_visible(found, &visible) == SAO_STATUS_OK);
    REQUIRE_FALSE(visible);

    SaoScreenInfo screen{};
    REQUIRE(sao_core_window_get_screen_info(&screen) == SAO_STATUS_OK);
    REQUIRE(screen.primary_width > 0);
    REQUIRE(screen.primary_height > 0);
    REQUIRE(screen.monitor_count > 0);

    REQUIRE(DestroyWindow(hwnd) != FALSE);
    UnregisterClassW(class_name, window_class.hInstance);
}

TEST_CASE("thread pool drains tasks and timer stops", "[core][thread]") {
    REQUIRE(sao_core_thread_pool_configure(2) == SAO_STATUS_OK);
    std::atomic<uint32_t> count{0};
    for (uint32_t index = 0; index < 16; ++index) {
        REQUIRE(sao_core_thread_pool_submit(increment_counter, &count) == SAO_STATUS_OK);
    }
    REQUIRE(sao_core_thread_pool_drain() == SAO_STATUS_OK);
    REQUIRE(count.load(std::memory_order_relaxed) == 16);
    REQUIRE(sao_core_thread_current_id() == GetCurrentThreadId());
    sao_core_thread_yield();

    sao_core_timer_handle_t timer = nullptr;
    REQUIRE(sao_core_timer_create(5, increment_counter, &count, &timer) == SAO_STATUS_OK);
    REQUIRE(timer != nullptr);
    const uint32_t before = count.load(std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (count.load(std::memory_order_relaxed) == before &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    sao_core_timer_destroy(timer);
    REQUIRE(count.load(std::memory_order_relaxed) > before);
}

TEST_CASE("BCrypt facade matches known vectors", "[core][crypto]") {
    constexpr std::array<uint8_t, 3> abc{'a', 'b', 'c'};
    constexpr std::array<uint8_t, 32> expected_sha256{
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
    };
    std::array<uint8_t, 32> digest{};
    REQUIRE(sao_core_hash(
                SAO_HASH_SHA256, abc.data(), abc.size(), digest.data(), digest.size()) ==
            SAO_STATUS_OK);
    REQUIRE(digest == expected_sha256);
    REQUIRE(sao_core_hash(
                SAO_HASH_SHA256, abc.data(), abc.size(), digest.data(), 31) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);

    constexpr std::array<uint8_t, 3> hmac_key{'k', 'e', 'y'};
    constexpr std::array<uint8_t, 43> hmac_message{
        'T', 'h', 'e', ' ', 'q', 'u', 'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n',
        ' ', 'f', 'o', 'x', ' ', 'j', 'u', 'm', 'p', 's', ' ', 'o', 'v', 'e', 'r',
        ' ', 't', 'h', 'e', ' ', 'l', 'a', 'z', 'y', ' ', 'd', 'o', 'g',
    };
    constexpr std::array<uint8_t, 32> expected_hmac{
        0xF7, 0xBC, 0x83, 0xF4, 0x30, 0x53, 0x84, 0x24,
        0xB1, 0x32, 0x98, 0xE6, 0xAA, 0x6F, 0xB1, 0x43,
        0xEF, 0x4D, 0x59, 0xA1, 0x49, 0x46, 0x17, 0x59,
        0x97, 0x47, 0x9D, 0xBC, 0x2D, 0x1A, 0x3C, 0xD8,
    };
    std::array<uint8_t, 32> mac{};
    REQUIRE(sao_core_hmac(SAO_HASH_SHA256,
                           hmac_key.data(),
                           hmac_key.size(),
                           hmac_message.data(),
                           hmac_message.size(),
                           mac.data(),
                           mac.size()) == SAO_STATUS_OK);
    REQUIRE(mac == expected_hmac);

    constexpr std::array<uint8_t, 32> key{};
    constexpr std::array<uint8_t, 12> nonce{};
    constexpr std::array<uint8_t, 16> plaintext{};
    constexpr std::array<uint8_t, 16> expected_ciphertext{
        0xCE, 0xA7, 0x40, 0x3D, 0x4D, 0x60, 0x6B, 0x6E,
        0x07, 0x4E, 0xC5, 0xD3, 0xBA, 0xF3, 0x9D, 0x18,
    };
    constexpr std::array<uint8_t, 16> expected_tag{
        0xD0, 0xD1, 0xC8, 0xA7, 0x99, 0x99, 0x6B, 0xF0,
        0x26, 0x5B, 0x98, 0xB5, 0xD4, 0x8A, 0xB9, 0x19,
    };
    std::array<uint8_t, 16> ciphertext{};
    std::array<uint8_t, 16> tag{};
    REQUIRE(sao_core_aes_gcm_encrypt(key.data(),
                                      nonce.data(),
                                      nullptr,
                                      0,
                                      plaintext.data(),
                                      plaintext.size(),
                                      ciphertext.data(),
                                      tag.data()) == SAO_STATUS_OK);
    REQUIRE(ciphertext == expected_ciphertext);
    REQUIRE(tag == expected_tag);

    std::array<uint8_t, 16> decrypted{};
    REQUIRE(sao_core_aes_gcm_decrypt(key.data(),
                                      nonce.data(),
                                      nullptr,
                                      0,
                                      ciphertext.data(),
                                      ciphertext.size(),
                                      tag.data(),
                                      decrypted.data()) == SAO_STATUS_OK);
    REQUIRE(decrypted == plaintext);

    std::array<uint8_t, 32> random{};
    REQUIRE(sao_core_random_bytes(random.data(), random.size()) == SAO_STATUS_OK);
    REQUIRE(random != std::array<uint8_t, 32>{});
}
