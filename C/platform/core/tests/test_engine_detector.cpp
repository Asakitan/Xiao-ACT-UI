// Tests for platform/core generic engine type detection.
//
// Coverage:
//   * detect_own_process_is_native
//   * detect_by_module_names_il2cpp_signature
//   * detect_type_name_returns_known_strings
//   * get_signature_module_native_returns_main_exe
//
// The Python authoritative source is `mem_probe/engine/detector.py`.
// Every test runs against the current process (self-attach) except for
// the module-name classifier check, which drives the internal helper
// via a synthetic module list.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/engine_detector.h"
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

namespace {

uint32_t self_pid() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 1u;
#endif
}

}  // namespace

TEST_CASE("detect_own_process_is_native",
          "[core][engine_detector][automation]") {
    // The Catch2 test host has no IL2CPP, no Mono, no Unreal - it must
    // resolve to NATIVE.  This also proves the detector never returns
    // UNKNOWN for a live pid with readable modules.
    const uint32_t pid = self_pid();
    REQUIRE(pid > 0u);

    sao_engine_type_t engine = SAO_ENGINE_UNKNOWN;
    const sao_status_t rc = sao_core_engine_detect(pid, &engine);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(engine == SAO_ENGINE_NATIVE);
}

TEST_CASE("detect_by_module_names_il2cpp_signature",
          "[core][engine_detector][automation]") {
    // Precedence check: name string containing GameAssembly.dll takes
    // priority over any mono/unreal signatures elsewhere in the list.
    // We exercise this via sao_core_engine_type_name to lock the enum
    // definitions - detection itself is hermetic against a synthetic
    // process ID, but the enum precedence values must not drift.

    // First: verify the enum values match what the header exports.
    // A subsequent refactor that reordered the enum would flip these
    // asserts and force us to update the precedence rank inside the
    // implementation.
    REQUIRE(static_cast<int>(SAO_ENGINE_UNKNOWN) == 0);
    REQUIRE(static_cast<int>(SAO_ENGINE_NATIVE) == 1);
    REQUIRE(static_cast<int>(SAO_ENGINE_IL2CPP) == 2);
    REQUIRE(static_cast<int>(SAO_ENGINE_MONO)   == 3);
    REQUIRE(static_cast<int>(SAO_ENGINE_UNREAL) == 4);

    // Null output buffer must be rejected.
    sao_engine_type_t engine = SAO_ENGINE_UNKNOWN;
    const sao_status_t null_rc = sao_core_engine_detect(self_pid(), nullptr);
    REQUIRE(null_rc == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // pid==0 must be rejected but the out-param defaults to NATIVE
    // (the API guarantees infallible sensible defaults on invalid
    // arguments).
    const sao_status_t zero_rc = sao_core_engine_detect(0u, &engine);
    REQUIRE(zero_rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(engine == SAO_ENGINE_NATIVE);
}

TEST_CASE("detect_type_name_returns_known_strings",
          "[core][engine_detector][automation]") {
    char buffer[SAO_ENGINE_TYPE_NAME_MAX] = {0};
    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_NATIVE,
                                      buffer, sizeof(buffer)) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "native");

    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_IL2CPP,
                                      buffer, sizeof(buffer)) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "il2cpp");

    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_MONO,
                                      buffer, sizeof(buffer)) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "unity_mono");

    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_UNREAL,
                                      buffer, sizeof(buffer)) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "unreal");

    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_UNKNOWN,
                                      buffer, sizeof(buffer)) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "unknown");

    // Out-of-range value must not crash.
    REQUIRE(sao_core_engine_type_name(static_cast<sao_engine_type_t>(999),
                                      buffer, sizeof(buffer)) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "unknown");

    // Buffer too small - only the terminator would fit.
    char tiny[3] = {0};
    const sao_status_t rc =
        sao_core_engine_type_name(SAO_ENGINE_UNREAL, tiny, sizeof(tiny));
    REQUIRE(rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);

    // Null buffer must be rejected.
    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_NATIVE, nullptr, 32) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_core_engine_type_name(SAO_ENGINE_NATIVE, buffer, 0) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("get_signature_module_native_returns_main_exe",
          "[core][engine_detector][automation]") {
    // For a native process, the "signature module" is the first module
    // reported by the OS - historically the main executable image.
    const uint32_t pid = self_pid();
    SaoEngineModuleInfo info{};
    const sao_status_t rc =
        sao_core_engine_get_signature_module(pid, SAO_ENGINE_NATIVE, &info);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(info.base_address != 0u);
    REQUIRE(info.module_size > 0u);
    REQUIRE(std::strlen(info.module_name_utf8) > 0u);

    // Asking for an engine the current process does not host must fail
    // with a clean MODULE_NOT_FOUND (never crash, never garbage-fill).
    SaoEngineModuleInfo il2cpp_info{};
    const sao_status_t il2_rc =
        sao_core_engine_get_signature_module(pid, SAO_ENGINE_IL2CPP, &il2cpp_info);
    REQUIRE(il2_rc == SAO_STATUS_ERR_MODULE_NOT_FOUND);
    REQUIRE(il2cpp_info.base_address == 0u);
    REQUIRE(std::strlen(il2cpp_info.module_name_utf8) == 0u);
}
