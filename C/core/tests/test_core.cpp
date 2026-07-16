#include <catch2/catch_test_macros.hpp>

#include "sao_core/sao_core.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("sao_core_abi_version reports 1.1", "[abi][behavior]") {
        REQUIRE(sao_core_abi_version() == 0x00010001u);
}

TEST_CASE("process open/close succeeds for the current process", "[process]") {
    sao_core_process_handle_t handle = nullptr;
    REQUIRE(sao_core_process_open(static_cast<uint32_t>(GetCurrentProcessId()), &handle) ==
            SAO_OK);
    REQUIRE(handle != nullptr);

    SaoCoreProcessInfo info{};
    std::array<char, SAO_CORE_PROCESS_IMAGE_PATH_MAX> image_path{};
    REQUIRE(sao_core_process_get_info(
                handle, &info, image_path.data(), image_path.size()) == SAO_OK);
    REQUIRE(info.pid == GetCurrentProcessId());
    REQUIRE(info.start_time_100ns != 0);
    REQUIRE(image_path[0] != '\0');

    const std::array<uint8_t, 8> source{0x10, 0x20, 0x30, 0x40, 0xA0, 0xB0, 0xC0, 0xD0};
    std::array<uint8_t, 8> copy{};
    size_t bytes_read = 0;
    REQUIRE(sao_core_read_bytes(
                handle,
                reinterpret_cast<uint64_t>(source.data()),
                copy.data(),
                copy.size(),
                &bytes_read) == SAO_OK);
    REQUIRE(bytes_read == source.size());
    REQUIRE(copy == source);

    copy.fill(0xFF);
    bytes_read = 99;
    REQUIRE(sao_core_read_bytes(handle, 0, copy.data(), copy.size(), &bytes_read) ==
            SAO_ERR_READ_FAULT);
    REQUIRE(bytes_read == 0);
    REQUIRE(copy == std::array<uint8_t, 8>{});

    sao_core_process_close(handle);
}

TEST_CASE("process open rejects invalid arguments", "[process]") {
        sao_core_process_handle_t handle = reinterpret_cast<sao_core_process_handle_t>(1);
    REQUIRE(sao_core_process_open(0, &handle) == SAO_ERR_INVALID_ARGUMENT);
        REQUIRE(handle == nullptr);
    REQUIRE(sao_core_process_open(static_cast<uint32_t>(GetCurrentProcessId()), nullptr) ==
            SAO_ERR_INVALID_ARGUMENT);

        SaoCoreProcessInfo info{1, 2, 3, 4, 5};
        char path[4] = {'x', 'x', 'x', '\0'};
        REQUIRE(sao_core_process_get_info(nullptr, &info, path, sizeof(path)) ==
                        SAO_ERR_HANDLE_INVALID);
        REQUIRE(info.pid == 0);
        REQUIRE(path[0] == '\0');

    sao_core_process_close(nullptr);
}

TEST_CASE("pattern scan supports bounded wildcard matches", "[scan][behavior]") {
        const std::array<uint8_t, 10> haystack{0x90, 0x48, 0x8B, 0x11, 0x22,
                                                                                        0x90, 0x48, 0x8B, 0xAA, 0x22};
        const std::array<uint8_t, 4> pattern{0x48, 0x8B, 0x00, 0x22};
        const std::array<uint8_t, 4> mask{0xFF, 0xFF, 0x00, 0xFF};
        size_t offset = 99;
        REQUIRE(sao_core_scan_find_pattern(
                                haystack.data(), haystack.size(), pattern.data(), mask.data(), pattern.size(),
                                &offset) == SAO_OK);
        REQUIRE(offset == 1);

        const std::array<uint8_t, 2> missing{0xCC, 0xCC};
        const std::array<uint8_t, 2> exact_mask{0xFF, 0xFF};
        offset = 99;
        REQUIRE(sao_core_scan_find_pattern(
                                haystack.data(), haystack.size(), missing.data(), exact_mask.data(),
                                missing.size(), &offset) == SAO_ERR_NOT_FOUND);
        REQUIRE(offset == 0);

        const std::array<uint8_t, 2> invalid_mask{0xFF, 0x7F};
        offset = 99;
        REQUIRE(sao_core_scan_find_pattern(
                                haystack.data(), haystack.size(), missing.data(), invalid_mask.data(),
                                missing.size(), &offset) == SAO_ERR_INVALID_ARGUMENT);
        REQUIRE(offset == 0);
}

TEST_CASE("aligned u64 scan supports size query and caller buffers", "[scan][behavior]") {
        const std::array<uint64_t, 5> values{7, 9, 7, 11, 9};
        const std::array<uint64_t, 2> targets{7, 9};
        size_t match_count = 0;
        REQUIRE(sao_core_scan_find_aligned_u64(
                                reinterpret_cast<const uint8_t*>(values.data()), sizeof(values), targets.data(),
                                targets.size(), nullptr, 0, &match_count) == SAO_OK);
        REQUIRE(match_count == 4);

        std::array<size_t, 3> too_small{99, 99, 99};
        REQUIRE(sao_core_scan_find_aligned_u64(
                                reinterpret_cast<const uint8_t*>(values.data()), sizeof(values), targets.data(),
                                targets.size(), too_small.data(), too_small.size(), &match_count) ==
                        SAO_ERR_BUFFER_TOO_SMALL);
        REQUIRE(too_small == std::array<size_t, 3>{});
        REQUIRE(match_count == 4);

        std::array<size_t, 4> offsets{};
        REQUIRE(sao_core_scan_find_aligned_u64(
                                reinterpret_cast<const uint8_t*>(values.data()), sizeof(values), targets.data(),
                                targets.size(), offsets.data(), offsets.size(), &match_count) == SAO_OK);
        REQUIRE(offsets == std::array<size_t, 4>{0, 8, 16, 32});
}

TEST_CASE("BGRA operations sample and validate bounded regions", "[pixels][behavior]") {
        const std::array<uint8_t, 8> source{200, 100, 50, 128, 1, 2, 3, 0};
        std::array<uint8_t, 8> destination{20, 40, 60, 255, 9, 8, 7, 255};
        REQUIRE(sao_core_pixels_premultiply_blend(
                                source.data(), destination.data(), 2, 1, 8) == SAO_OK);
        REQUIRE(destination == std::array<uint8_t, 8>{110, 70, 55, 255, 9, 8, 7, 255});

        const std::array<uint8_t, 24> pixels{
                0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 255,
                0, 0, 0, 0, 4, 5, 6, 2, 0, 0, 0, 0,
        };
        uint32_t span_count = 0;
        REQUIRE(sao_core_pixels_find_alpha_spans(
                                pixels.data(), 6, 1, 24, 0, nullptr, 0, &span_count) == SAO_OK);
        REQUIRE(span_count == 2);
        std::array<uint32_t, 4> spans{};
        REQUIRE(sao_core_pixels_find_alpha_spans(
                                pixels.data(), 6, 1, 24, 0, spans.data(), 2, &span_count) == SAO_OK);
        REQUIRE(spans == std::array<uint32_t, 4>{1, 3, 4, 5});

        std::array<uint32_t, 2> too_few_spans{99, 99};
        REQUIRE(sao_core_pixels_find_alpha_spans(
                                pixels.data(), 6, 1, 24, 0, too_few_spans.data(), 1, &span_count) ==
                        SAO_ERR_BUFFER_TOO_SMALL);
        REQUIRE(too_few_spans == std::array<uint32_t, 2>{});
        REQUIRE(span_count == 2);

        size_t first_offset = 0;
        size_t required_end = 0;
        REQUIRE(sao_core_pixels_validate_region(
                                pixels.size(), 3, 2, 12, 1, 1, 2, 1, &first_offset, &required_end) == SAO_OK);
        REQUIRE(first_offset == 16);
        REQUIRE(required_end == 24);

        SaoCoreBgraPixel pixel{};
        REQUIRE(sao_core_pixels_sample_bgra(
                                pixels.data(), pixels.size(), 3, 2, 12, 1, 1, &pixel) == SAO_OK);
        REQUIRE(pixel.blue == 4);
        REQUIRE(pixel.alpha == 2);

        pixel = SaoCoreBgraPixel{1, 2, 3, 4};
        REQUIRE(sao_core_pixels_sample_bgra(
                                pixels.data(), 8, 3, 2, 12, 1, 1, &pixel) == SAO_ERR_BUFFER_TOO_SMALL);
        REQUIRE(pixel.alpha == 0);
}

TEST_CASE("class registry keeps stable bidirectional indexes", "[class_index][behavior]") {
        const std::string first = "Behavior.Class.First";
        const std::string second = "行为.Class.Second";
        uint32_t first_index = 0;
        uint32_t second_index = 0;
        REQUIRE(sao_core_class_index_register(first.c_str(), &first_index) == SAO_OK);
        REQUIRE(sao_core_class_index_register(second.c_str(), &second_index) == SAO_OK);
        REQUIRE(second_index != first_index);

        uint32_t duplicate_index = UINT32_MAX;
        REQUIRE(sao_core_class_index_register(first.c_str(), &duplicate_index) == SAO_OK);
        REQUIRE(duplicate_index == first_index);

        uint32_t found_index = UINT32_MAX;
        REQUIRE(sao_core_class_index_find(second.c_str(), &found_index) == SAO_OK);
        REQUIRE(found_index == second_index);

        size_t required_size = 0;
        REQUIRE(sao_core_class_index_get_name(second_index, nullptr, 0, &required_size) == SAO_OK);
        std::vector<char> name(required_size);
        REQUIRE(sao_core_class_index_get_name(
                                second_index, name.data(), name.size(), &required_size) == SAO_OK);
        REQUIRE(std::string(name.data()) == second);

        std::array<char, 2> too_small{'x', 'x'};
        REQUIRE(sao_core_class_index_get_name(
                                second_index, too_small.data(), too_small.size(), &required_size) ==
                        SAO_ERR_BUFFER_TOO_SMALL);
        REQUIRE(too_small[0] == '\0');
        REQUIRE(required_size == second.size() + 1);

        uint64_t token = 0;
        REQUIRE(sao_core_class_index_resolve(nullptr, first.c_str(), &token) == SAO_OK);
        REQUIRE(token == static_cast<uint64_t>(first_index) + 1);

        uint32_t field_offset = 99;
        REQUIRE(sao_core_class_index_resolve_field_offset(
                                nullptr, token, "missing_field", &field_offset) == SAO_ERR_NOT_FOUND);
        REQUIRE(field_offset == 0);
}

TEST_CASE("window creation enumeration and info use real HWNDs", "[window][behavior]") {
        void* hwnd = nullptr;
        REQUIRE(sao_core_window_create_layered_topmost(
                                L"SAO legacy core behavior", 20, 30, 160, 90, &hwnd) == SAO_OK);
        REQUIRE(hwnd != nullptr);

        SaoCoreWindowInfo info{};
        REQUIRE(sao_core_window_get_info(hwnd, &info) == SAO_OK);
        REQUIRE(info.hwnd == hwnd);
        REQUIRE(info.pid == GetCurrentProcessId());
        REQUIRE(std::wstring(info.title) == L"SAO legacy core behavior");
        REQUIRE((info.extended_style & WS_EX_LAYERED) != 0);
        REQUIRE((info.extended_style & WS_EX_TOPMOST) != 0);

        size_t window_count = 0;
        REQUIRE(sao_core_window_enumerate(nullptr, 0, &window_count) == SAO_OK);
        std::vector<void*> windows(window_count + 64);
        REQUIRE(sao_core_window_enumerate(windows.data(), windows.size(), &window_count) == SAO_OK);
        bool found = false;
        for (size_t index = 0; index < window_count; ++index) {
                if (windows[index] == hwnd) {
                        found = true;
                        break;
                }
        }
        REQUIRE(found);

        REQUIRE(sao_core_window_destroy(hwnd) == SAO_OK);
        info.pid = 123;
        REQUIRE(sao_core_window_get_info(hwnd, &info) == SAO_ERR_HANDLE_INVALID);
        REQUIRE(info.pid == 0);
}
