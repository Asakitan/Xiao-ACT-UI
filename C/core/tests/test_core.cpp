#include <catch2/catch_test_macros.hpp>

#include "sao_core/sao_core.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#define sao_core_abi_version sao_legacy_core_abi_version
#define sao_core_process_handle_t sao_legacy_core_process_handle_t
#define SaoCoreProcessInfo SaoLegacyCoreProcessInfo
#define SAO_CORE_PROCESS_IMAGE_PATH_MAX SAO_LEGACY_CORE_PROCESS_IMAGE_PATH_MAX
#define sao_core_process_open sao_legacy_core_process_open
#define sao_core_process_close sao_legacy_core_process_close
#define sao_core_process_get_info sao_legacy_core_process_get_info
#define sao_core_read_bytes sao_legacy_core_read_bytes
#define sao_core_scan_find_pattern sao_legacy_core_scan_find_pattern
#define sao_core_scan_find_aligned_u64 sao_legacy_core_scan_find_aligned_u64
#define sao_core_pixels_premultiply_blend sao_legacy_core_pixels_premultiply_blend
#define sao_core_pixels_find_alpha_spans sao_legacy_core_pixels_find_alpha_spans
#define sao_core_pixels_validate_region sao_legacy_core_pixels_validate_region
#define sao_core_pixels_sample_bgra sao_legacy_core_pixels_sample_bgra
#define SaoCoreBgraPixel SaoLegacyCoreBgraPixel
#define sao_core_class_index_register sao_legacy_core_class_index_register
#define sao_core_class_index_find sao_legacy_core_class_index_find
#define sao_core_class_index_get_name sao_legacy_core_class_index_get_name
#define sao_core_class_index_resolve sao_legacy_core_class_index_resolve
#define sao_core_class_index_resolve_field_offset sao_legacy_core_class_index_resolve_field_offset
#define sao_core_class_index_configure_provider                                                \
    sao_legacy_core_class_index_configure_provider
#define sao_core_class_index_release sao_legacy_core_class_index_release
#define SaoCoreClassMetadataProvider SaoLegacyCoreClassMetadataProvider
#define sao_core_window_create_layered_topmost sao_legacy_core_window_create_layered_topmost
#define sao_core_window_destroy sao_legacy_core_window_destroy
#define sao_core_window_enumerate sao_legacy_core_window_enumerate
#define sao_core_window_get_info sao_legacy_core_window_get_info
#define SaoCoreWindowInfo SaoLegacyCoreWindowInfo

namespace {

struct CoreProcess {
    sao_core_process_handle_t handle = nullptr;

    explicit CoreProcess(uint32_t pid) {
        REQUIRE(sao_core_process_open(pid, &handle) == SAO_OK);
        REQUIRE(handle != nullptr);
    }

    ~CoreProcess() {
        sao_core_process_close(handle);
    }

    CoreProcess(const CoreProcess&) = delete;
    CoreProcess& operator=(const CoreProcess&) = delete;
};

struct SuspendedProcess {
    PROCESS_INFORMATION process{};

    SuspendedProcess() {
        std::array<wchar_t, MAX_PATH> system_directory{};
        const UINT length = GetSystemDirectoryW(system_directory.data(),
                                                static_cast<UINT>(system_directory.size()));
        REQUIRE(length != 0);
        REQUIRE(length < system_directory.size());
        std::wstring executable(system_directory.data(), length);
        executable += L"\\cmd.exe";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        REQUIRE(CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, FALSE,
                               CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                               &process) != FALSE);
    }

    ~SuspendedProcess() {
        if (process.hProcess != nullptr) {
            (void)TerminateProcess(process.hProcess, 0);
            (void)WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
        if (process.hThread != nullptr) {
            CloseHandle(process.hThread);
        }
    }

    SuspendedProcess(const SuspendedProcess&) = delete;
    SuspendedProcess& operator=(const SuspendedProcess&) = delete;
};

struct MetadataFixture {
    struct ClassLease {
        sao_core_process_handle_t process = nullptr;
        std::string class_name;
    };

    std::mutex mutex;
    uint64_t next_token = 1000;
    int retain_calls = 0;
    int release_attempts = 0;
    int release_calls = 0;
    int resolve_calls = 0;
    int field_calls = 0;
    int class_release_attempts = 0;
    int class_release_calls = 0;
    bool throw_next_release = false;
    bool throw_next_class_release = false;
    bool block_field = false;
    bool field_entered = false;
    bool allow_field = false;
    bool reenter_on_class_release = false;
    int32_t class_release_reentry_status = SAO_OK;
    std::condition_variable condition;
    std::unordered_map<uint64_t, ClassLease> classes;
};

void SAO_LEGACY_CORE_CALL metadata_retain(void* user_data) {
    auto& fixture = *static_cast<MetadataFixture*>(user_data);
    std::lock_guard lock(fixture.mutex);
    ++fixture.retain_calls;
}

void SAO_LEGACY_CORE_CALL metadata_release(void* user_data) {
    auto& fixture = *static_cast<MetadataFixture*>(user_data);
    std::lock_guard lock(fixture.mutex);
    ++fixture.release_attempts;
    if (fixture.throw_next_release) {
        fixture.throw_next_release = false;
        throw std::runtime_error("metadata owner release fixture");
    }
    ++fixture.release_calls;
}

int32_t SAO_LEGACY_CORE_CALL metadata_resolve_class(
    void* user_data, sao_core_process_handle_t process, const char* class_name_utf8,
    uint64_t* out_provider_class_token) {
    if (process == nullptr || class_name_utf8 == nullptr || out_provider_class_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_provider_class_token = 0;
    auto& fixture = *static_cast<MetadataFixture*>(user_data);
    std::lock_guard lock(fixture.mutex);
    ++fixture.resolve_calls;
    if (std::strcmp(class_name_utf8, "Fixture.Entity") != 0) {
        return SAO_ERR_NOT_FOUND;
    }
    const uint64_t token = ++fixture.next_token;
    fixture.classes.emplace(token, MetadataFixture::ClassLease{process, class_name_utf8});
    *out_provider_class_token = token;
    return SAO_OK;
}

int32_t SAO_LEGACY_CORE_CALL metadata_resolve_field_offset(
    void* user_data, sao_core_process_handle_t process, uint64_t provider_class_token,
    const char* field_name_utf8, uint32_t* out_offset) {
    if (process == nullptr || provider_class_token == 0 || field_name_utf8 == nullptr ||
        out_offset == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_offset = 0;
    auto& fixture = *static_cast<MetadataFixture*>(user_data);
    std::unique_lock lock(fixture.mutex);
    ++fixture.field_calls;
    const auto found = fixture.classes.find(provider_class_token);
    if (found == fixture.classes.end() || found->second.process != process) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (std::strcmp(field_name_utf8, "value") != 0) {
        return SAO_ERR_NOT_FOUND;
    }
    if (fixture.block_field) {
        fixture.field_entered = true;
        fixture.condition.notify_all();
        fixture.condition.wait(lock, [&fixture] { return fixture.allow_field; });
    }
    *out_offset = 0x28;
    return SAO_OK;
}

void SAO_LEGACY_CORE_CALL metadata_release_class(void* user_data,
                                                 uint64_t provider_class_token) {
    auto& fixture = *static_cast<MetadataFixture*>(user_data);
    std::lock_guard lock(fixture.mutex);
    ++fixture.class_release_attempts;
    if (fixture.throw_next_class_release) {
        fixture.throw_next_class_release = false;
        throw std::runtime_error("metadata class release fixture");
    }
    if (fixture.reenter_on_class_release) {
        fixture.class_release_reentry_status = sao_core_class_index_configure_provider(nullptr);
    }
    fixture.classes.erase(provider_class_token);
    ++fixture.class_release_calls;
}

SaoCoreClassMetadataProvider metadata_provider(MetadataFixture& fixture) {
    SaoCoreClassMetadataProvider provider{};
    provider.struct_size = sizeof(provider);
    provider.abi_version = SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION;
    provider.user_data = &fixture;
    provider.retain = metadata_retain;
    provider.release = metadata_release;
    provider.resolve_class = metadata_resolve_class;
    provider.resolve_field_offset = metadata_resolve_field_offset;
    provider.release_class = metadata_release_class;
    return provider;
}

struct MetadataProviderReset {
    ~MetadataProviderReset() {
        (void)sao_core_class_index_configure_provider(nullptr);
    }
};

} // namespace

TEST_CASE("sao_legacy_core_abi_version reports legacy ABI 1.2", "[abi][behavior]") {
    REQUIRE(sao_core_abi_version() == 0x00010002u);
}

TEST_CASE("process open/close succeeds for the current process", "[process]") {
    sao_core_process_handle_t handle = nullptr;
    REQUIRE(sao_core_process_open(static_cast<uint32_t>(GetCurrentProcessId()), &handle) == SAO_OK);
    REQUIRE(handle != nullptr);

    SaoCoreProcessInfo info{};
    std::array<char, SAO_CORE_PROCESS_IMAGE_PATH_MAX> image_path{};
    REQUIRE(sao_core_process_get_info(handle, &info, image_path.data(), image_path.size()) ==
            SAO_OK);
    REQUIRE(info.pid == GetCurrentProcessId());
    REQUIRE(info.start_time_100ns != 0);
    REQUIRE(image_path[0] != '\0');

    const std::array<uint8_t, 8> source{0x10, 0x20, 0x30, 0x40, 0xA0, 0xB0, 0xC0, 0xD0};
    std::array<uint8_t, 8> copy{};
    size_t bytes_read = 0;
    REQUIRE(sao_core_read_bytes(handle, reinterpret_cast<uint64_t>(source.data()), copy.data(),
                                copy.size(), &bytes_read) == SAO_OK);
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
    REQUIRE(sao_core_scan_find_pattern(haystack.data(), haystack.size(), pattern.data(),
                                       mask.data(), pattern.size(), &offset) == SAO_OK);
    REQUIRE(offset == 1);

    const std::array<uint8_t, 2> missing{0xCC, 0xCC};
    const std::array<uint8_t, 2> exact_mask{0xFF, 0xFF};
    offset = 99;
    REQUIRE(sao_core_scan_find_pattern(haystack.data(), haystack.size(), missing.data(),
                                       exact_mask.data(), missing.size(),
                                       &offset) == SAO_ERR_NOT_FOUND);
    REQUIRE(offset == 0);

    const std::array<uint8_t, 2> invalid_mask{0xFF, 0x7F};
    offset = 99;
    REQUIRE(sao_core_scan_find_pattern(haystack.data(), haystack.size(), missing.data(),
                                       invalid_mask.data(), missing.size(),
                                       &offset) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(offset == 0);
}

TEST_CASE("aligned u64 scan supports size query and caller buffers", "[scan][behavior]") {
    const std::array<uint64_t, 5> values{7, 9, 7, 11, 9};
    const std::array<uint64_t, 2> targets{7, 9};
    size_t match_count = 0;
    REQUIRE(sao_core_scan_find_aligned_u64(reinterpret_cast<const uint8_t*>(values.data()),
                                           sizeof(values), targets.data(), targets.size(), nullptr,
                                           0, &match_count) == SAO_OK);
    REQUIRE(match_count == 4);

    std::array<size_t, 3> too_small{99, 99, 99};
    REQUIRE(sao_core_scan_find_aligned_u64(reinterpret_cast<const uint8_t*>(values.data()),
                                           sizeof(values), targets.data(), targets.size(),
                                           too_small.data(), too_small.size(),
                                           &match_count) == SAO_ERR_BUFFER_TOO_SMALL);
    REQUIRE(too_small == std::array<size_t, 3>{});
    REQUIRE(match_count == 4);

    std::array<size_t, 4> offsets{};
    REQUIRE(sao_core_scan_find_aligned_u64(reinterpret_cast<const uint8_t*>(values.data()),
                                           sizeof(values), targets.data(), targets.size(),
                                           offsets.data(), offsets.size(), &match_count) == SAO_OK);
    REQUIRE(offsets == std::array<size_t, 4>{0, 8, 16, 32});
}

TEST_CASE("BGRA operations sample and validate bounded regions", "[pixels][behavior]") {
    const std::array<uint8_t, 8> source{200, 100, 50, 128, 1, 2, 3, 0};
    std::array<uint8_t, 8> destination{20, 40, 60, 255, 9, 8, 7, 255};
    REQUIRE(sao_core_pixels_premultiply_blend(source.data(), destination.data(), 2, 1, 8) ==
            SAO_OK);
    REQUIRE(destination == std::array<uint8_t, 8>{110, 70, 55, 255, 9, 8, 7, 255});

    const std::array<uint8_t, 24> pixels{
        0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 255, 0, 0, 0, 0, 4, 5, 6, 2, 0, 0, 0, 0,
    };
    uint32_t span_count = 0;
    REQUIRE(sao_core_pixels_find_alpha_spans(pixels.data(), 6, 1, 24, 0, nullptr, 0, &span_count) ==
            SAO_OK);
    REQUIRE(span_count == 2);
    std::array<uint32_t, 4> spans{};
    REQUIRE(sao_core_pixels_find_alpha_spans(pixels.data(), 6, 1, 24, 0, spans.data(), 2,
                                             &span_count) == SAO_OK);
    REQUIRE(spans == std::array<uint32_t, 4>{1, 3, 4, 5});

    std::array<uint32_t, 2> too_few_spans{99, 99};
    REQUIRE(sao_core_pixels_find_alpha_spans(pixels.data(), 6, 1, 24, 0, too_few_spans.data(), 1,
                                             &span_count) == SAO_ERR_BUFFER_TOO_SMALL);
    REQUIRE(too_few_spans == std::array<uint32_t, 2>{});
    REQUIRE(span_count == 2);

    size_t first_offset = 0;
    size_t required_end = 0;
    REQUIRE(sao_core_pixels_validate_region(pixels.size(), 3, 2, 12, 1, 1, 2, 1, &first_offset,
                                            &required_end) == SAO_OK);
    REQUIRE(first_offset == 16);
    REQUIRE(required_end == 24);

    SaoCoreBgraPixel pixel{};
    REQUIRE(sao_core_pixels_sample_bgra(pixels.data(), pixels.size(), 3, 2, 12, 1, 1, &pixel) ==
            SAO_OK);
    REQUIRE(pixel.blue == 4);
    REQUIRE(pixel.alpha == 2);

    pixel = SaoCoreBgraPixel{1, 2, 3, 4};
    REQUIRE(sao_core_pixels_sample_bgra(pixels.data(), 8, 3, 2, 12, 1, 1, &pixel) ==
            SAO_ERR_BUFFER_TOO_SMALL);
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
    REQUIRE(sao_core_class_index_get_name(second_index, name.data(), name.size(), &required_size) ==
            SAO_OK);
    REQUIRE(std::string(name.data()) == second);

    std::array<char, 2> too_small{'x', 'x'};
    REQUIRE(sao_core_class_index_get_name(second_index, too_small.data(), too_small.size(),
                                          &required_size) == SAO_ERR_BUFFER_TOO_SMALL);
    REQUIRE(too_small[0] == '\0');
    REQUIRE(required_size == second.size() + 1);

    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    CoreProcess process(static_cast<uint32_t>(GetCurrentProcessId()));
    uint64_t token = 99;
    REQUIRE(sao_core_class_index_resolve(process.handle, first.c_str(), &token) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(token == 0);

    token = 99;
    REQUIRE(sao_core_class_index_resolve(nullptr, first.c_str(), &token) ==
            SAO_ERR_HANDLE_INVALID);
    REQUIRE(token == 0);
    uint32_t offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(nullptr, uint64_t{1} << 63u, "field",
                                                      &offset) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(offset == 0);
}

TEST_CASE("class metadata provider owns process-bound opaque tokens",
          "[class_index][provider][lifecycle]") {
    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    MetadataFixture first_fixture;
    MetadataFixture second_fixture;
    MetadataProviderReset reset;
    auto first_provider = metadata_provider(first_fixture);
    auto second_provider = metadata_provider(second_fixture);

    SuspendedProcess child;
    CoreProcess current_process(static_cast<uint32_t>(GetCurrentProcessId()));
    CoreProcess child_process(static_cast<uint32_t>(child.process.dwProcessId));

    REQUIRE(sao_core_class_index_configure_provider(&first_provider) == SAO_OK);
    CHECK(first_fixture.retain_calls == 1);

    uint64_t missing_token = 99;
    REQUIRE(sao_core_class_index_resolve(current_process.handle, "Fixture.Missing",
                                         &missing_token) == SAO_ERR_NOT_FOUND);
    CHECK(missing_token == 0);

    uint64_t current_token = 0;
    REQUIRE(sao_core_class_index_resolve(current_process.handle, "Fixture.Entity",
                                         &current_token) == SAO_OK);
    REQUIRE(current_token != 0);
    CHECK((current_token & (uint64_t{1} << 63u)) != 0);

    uint32_t field_offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(current_process.handle, current_token,
                                                      "value", &field_offset) == SAO_OK);
    CHECK(field_offset == 0x28);

    field_offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(current_process.handle, current_token,
                                                      "missing", &field_offset) ==
            SAO_ERR_NOT_FOUND);
    CHECK(field_offset == 0);

    const int field_calls_before_foreign_lookup = first_fixture.field_calls;
    field_offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(child_process.handle, current_token, "value",
                                                      &field_offset) == SAO_ERR_HANDLE_INVALID);
    CHECK(field_offset == 0);
    CHECK(first_fixture.field_calls == field_calls_before_foreign_lookup);

    uint64_t child_token = 0;
    REQUIRE(sao_core_class_index_resolve(child_process.handle, "Fixture.Entity", &child_token) ==
            SAO_OK);
    REQUIRE(child_token != current_token);
    field_offset = 0;
    REQUIRE(sao_core_class_index_resolve_field_offset(child_process.handle, child_token, "value",
                                                      &field_offset) == SAO_OK);
    CHECK(field_offset == 0x28);

    REQUIRE(sao_core_class_index_release(current_process.handle, current_token) == SAO_OK);
    CHECK(first_fixture.class_release_calls == 1);
    field_offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(current_process.handle, current_token,
                                                      "value", &field_offset) ==
            SAO_ERR_HANDLE_INVALID);
    CHECK(field_offset == 0);
    CHECK(sao_core_class_index_release(current_process.handle, current_token) ==
          SAO_ERR_HANDLE_INVALID);

    REQUIRE(sao_core_class_index_configure_provider(&second_provider) == SAO_OK);
    CHECK(first_fixture.class_release_calls == 2);
    CHECK(first_fixture.classes.empty());
    CHECK(first_fixture.release_calls == 1);
    CHECK(second_fixture.retain_calls == 1);

    field_offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(child_process.handle, child_token, "value",
                                                      &field_offset) == SAO_ERR_HANDLE_INVALID);
    CHECK(field_offset == 0);

    uint64_t replacement_token = 0;
    REQUIRE(sao_core_class_index_resolve(current_process.handle, "Fixture.Entity",
                                         &replacement_token) == SAO_OK);
    REQUIRE(replacement_token != current_token);
    REQUIRE(replacement_token != child_token);

    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    CHECK(second_fixture.class_release_calls == 1);
    CHECK(second_fixture.classes.empty());
    CHECK(second_fixture.release_calls == 1);

    field_offset = 99;
    REQUIRE(sao_core_class_index_resolve_field_offset(current_process.handle, replacement_token,
                                                      "value", &field_offset) ==
            SAO_ERR_HANDLE_INVALID);
    CHECK(field_offset == 0);
    replacement_token = 99;
    REQUIRE(sao_core_class_index_resolve(current_process.handle, "Fixture.Entity",
                                         &replacement_token) == SAO_ERR_NOT_INITIALIZED);
    CHECK(replacement_token == 0);
}

TEST_CASE("class token release exception preserves quarantined ownership for retry",
          "[class_index][provider][release][exception][retry]") {
    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    MetadataFixture fixture;
    MetadataProviderReset reset;
    auto provider = metadata_provider(fixture);
    CoreProcess process(static_cast<uint32_t>(GetCurrentProcessId()));

    REQUIRE(sao_core_class_index_configure_provider(&provider) == SAO_OK);
    uint64_t token = 0;
    REQUIRE(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &token) == SAO_OK);
    REQUIRE(token != 0);

    fixture.throw_next_class_release = true;
    int32_t release_status = SAO_OK;
    CHECK_NOTHROW(release_status = sao_core_class_index_release(process.handle, token));
    CHECK(release_status == SAO_ERR_UNKNOWN);
    CHECK(fixture.class_release_attempts == 1);
    CHECK(fixture.class_release_calls == 0);
    CHECK(fixture.classes.size() == 1);
    CHECK(fixture.release_calls == 0);

    uint32_t offset = 99;
    CHECK(sao_core_class_index_resolve_field_offset(process.handle, token, "value", &offset) ==
          SAO_ERR_HANDLE_INVALID);
    CHECK(offset == 0);

    fixture.reenter_on_class_release = true;
    REQUIRE(sao_core_class_index_release(process.handle, token) == SAO_OK);
    CHECK(fixture.class_release_attempts == 2);
    CHECK(fixture.class_release_calls == 1);
    CHECK(fixture.class_release_reentry_status == SAO_ERR_OS_CALL_FAILED);
    CHECK(fixture.classes.empty());
    CHECK(fixture.release_calls == 0);

    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    CHECK(fixture.release_attempts == 1);
    CHECK(fixture.release_calls == 1);
}

TEST_CASE("provider replacement drains an in-flight metadata callback",
          "[class_index][provider][replace][concurrency][drain]") {
    using namespace std::chrono_literals;

    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    MetadataFixture fixture;
    MetadataProviderReset reset;
    auto provider = metadata_provider(fixture);
    CoreProcess process(static_cast<uint32_t>(GetCurrentProcessId()));
    REQUIRE(sao_core_class_index_configure_provider(&provider) == SAO_OK);

    uint64_t token = 0;
    REQUIRE(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &token) == SAO_OK);
    {
        std::lock_guard lock(fixture.mutex);
        fixture.block_field = true;
    }
    auto field = std::async(std::launch::async, [&] {
        uint32_t offset = 0;
        return std::pair{sao_core_class_index_resolve_field_offset(
                             process.handle, token, "value", &offset),
                         offset};
    });
    {
        std::unique_lock lock(fixture.mutex);
        REQUIRE(fixture.condition.wait_for(lock, 2s,
                                           [&fixture] { return fixture.field_entered; }));
    }

    auto clear = std::async(std::launch::async,
                            [] { return sao_core_class_index_configure_provider(nullptr); });
    CHECK(clear.wait_for(25ms) == std::future_status::timeout);
    CHECK(fixture.class_release_calls == 0);
    {
        std::lock_guard lock(fixture.mutex);
        fixture.allow_field = true;
    }
    fixture.condition.notify_all();

    const auto [field_status, offset] = field.get();
    CHECK(field_status == SAO_OK);
    CHECK(offset == 0x28);
    REQUIRE(clear.get() == SAO_OK);
    CHECK(fixture.class_release_calls == 1);
    CHECK(fixture.release_calls == 1);
}

TEST_CASE("provider replacement retries class cleanup before installing the candidate",
          "[class_index][provider][replace][release_class][exception][retry]") {
    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    MetadataFixture original;
    MetadataFixture replacement;
    MetadataProviderReset reset;
    auto original_provider = metadata_provider(original);
    auto replacement_provider = metadata_provider(replacement);
    CoreProcess process(static_cast<uint32_t>(GetCurrentProcessId()));

    REQUIRE(sao_core_class_index_configure_provider(&original_provider) == SAO_OK);
    uint64_t original_token = 0;
    REQUIRE(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &original_token) ==
            SAO_OK);
    original.throw_next_class_release = true;

    int32_t replace_status = SAO_OK;
    CHECK_NOTHROW(replace_status =
                      sao_core_class_index_configure_provider(&replacement_provider));
    CHECK(replace_status == SAO_ERR_UNKNOWN);
    CHECK(original.class_release_attempts == 1);
    CHECK(original.class_release_calls == 0);
    CHECK(original.classes.size() == 1);
    CHECK(original.release_calls == 0);
    CHECK(replacement.retain_calls == 1);
    CHECK(replacement.release_calls == 1);

    uint64_t blocked_token = 99;
    CHECK(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &blocked_token) ==
          SAO_ERR_NOT_INITIALIZED);
    CHECK(blocked_token == 0);

    REQUIRE(sao_core_class_index_configure_provider(&replacement_provider) == SAO_OK);
    CHECK(original.class_release_attempts == 2);
    CHECK(original.class_release_calls == 1);
    CHECK(original.classes.empty());
    CHECK(original.release_attempts == 1);
    CHECK(original.release_calls == 1);
    CHECK(replacement.retain_calls == 2);
    CHECK(replacement.release_calls == 1);

    uint64_t replacement_token = 0;
    REQUIRE(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &replacement_token) ==
            SAO_OK);
    REQUIRE(replacement_token != original_token);
    REQUIRE(sao_core_class_index_release(process.handle, replacement_token) == SAO_OK);
    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    CHECK(replacement.release_attempts == 2);
    CHECK(replacement.release_calls == 2);
}

TEST_CASE("provider owner release exception retains the old owner until replacement retry",
          "[class_index][provider][replace][release][exception][retry]") {
    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    MetadataFixture original;
    MetadataFixture replacement;
    MetadataProviderReset reset;
    auto original_provider = metadata_provider(original);
    auto replacement_provider = metadata_provider(replacement);
    CoreProcess process(static_cast<uint32_t>(GetCurrentProcessId()));

    REQUIRE(sao_core_class_index_configure_provider(&original_provider) == SAO_OK);
    original.throw_next_release = true;

    int32_t replace_status = SAO_OK;
    CHECK_NOTHROW(replace_status =
                      sao_core_class_index_configure_provider(&replacement_provider));
    CHECK(replace_status == SAO_ERR_UNKNOWN);
    CHECK(original.release_attempts == 1);
    CHECK(original.release_calls == 0);
    CHECK(replacement.retain_calls == 1);
    CHECK(replacement.release_attempts == 1);
    CHECK(replacement.release_calls == 1);

    uint64_t blocked_token = 99;
    CHECK(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &blocked_token) ==
          SAO_ERR_NOT_INITIALIZED);
    CHECK(blocked_token == 0);

    REQUIRE(sao_core_class_index_configure_provider(&replacement_provider) == SAO_OK);
    CHECK(original.release_attempts == 2);
    CHECK(original.release_calls == 1);
    CHECK(replacement.retain_calls == 2);
    CHECK(replacement.release_calls == 1);

    uint64_t replacement_token = 0;
    REQUIRE(sao_core_class_index_resolve(process.handle, "Fixture.Entity", &replacement_token) ==
            SAO_OK);
    REQUIRE(sao_core_class_index_release(process.handle, replacement_token) == SAO_OK);
    REQUIRE(sao_core_class_index_configure_provider(nullptr) == SAO_OK);
    CHECK(original.release_calls == 1);
    CHECK(replacement.release_attempts == 2);
    CHECK(replacement.release_calls == 2);
}

TEST_CASE("window creation enumeration and info use real HWNDs", "[window][behavior]") {
    void* hwnd = nullptr;
    REQUIRE(sao_core_window_create_layered_topmost(L"SAO legacy core behavior", 20, 30, 160, 90,
                                                   &hwnd) == SAO_OK);
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
