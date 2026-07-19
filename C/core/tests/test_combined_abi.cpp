#include <sao_core/sao_core.h>

#include <sao/core/abi.h>
#include <sao/core/logging.h>
#include <sao/core/process.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>

namespace {

using legacy_abi_fn = uint32_t(SAO_LEGACY_CORE_CALL*)();

} // namespace

TEST_CASE("legacy and platform core headers and links coexist", "[abi][combined][link]") {
    CHECK(sao_legacy_core_abi_version() == SAO_LEGACY_CORE_ABI_VERSION);
    CHECK(sao_core_abi_version() == SAO_CORE_ABI_VERSION);

    const HMODULE legacy_module = GetModuleHandleW(L"sao_core.dll");
    REQUIRE(legacy_module != nullptr);
    const auto legacy_abi =
        reinterpret_cast<legacy_abi_fn>(GetProcAddress(legacy_module, "sao_core_abi_version"));
    REQUIRE(legacy_abi != nullptr);
    CHECK(legacy_abi() == SAO_LEGACY_CORE_ABI_VERSION);

    constexpr std::array legacy_aliases{
        "sao_core_process_open",        "sao_core_process_close",
        "sao_core_process_get_info",    "sao_core_read_bytes",
        "sao_core_class_index_resolve", "sao_core_scan_find_pattern",
        "sao_core_pixels_sample_bgra",  "sao_core_window_get_info",
        "sao_core_set_log_callback",    "sao_core_set_structured_log_callback",
    };
    for (const char* symbol : legacy_aliases)
        CHECK(GetProcAddress(legacy_module, symbol) != nullptr);
}
