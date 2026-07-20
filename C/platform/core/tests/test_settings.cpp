// SAO Auto — platform/core/tests/test_settings.cpp
//
// Wave 5 / Phase 1 — settings envelope coverage.
//
// The envelope is a dict-of-variant that serialises to disk with the
// same shape Python's ``json.dumps(indent=2, sort_keys=True)`` emits.
// These tests lock the on-disk format so a subsequent refactor of the
// C store or a hop back to a Python writer can't silently drift.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <process.h>   // _getpid on Windows

#include "sao/core/config.h"

namespace {

std::string tempSettingsPath(const char* stem) {
    std::filesystem::path p = std::filesystem::temp_directory_path();
    p /= std::string("sao_settings_") + stem + "_" +
         std::to_string(::_getpid()) + ".json";
    // Wipe any leftover from a previous crashed run.
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.string();
}

std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

TEST_CASE("settings_load_missing_file_returns_default",
          "[core][settings]") {
    // Nonexistent path is not an error — the API hands back an empty
    // envelope so the caller can seed defaults and save().
    const std::string path = tempSettingsPath("missing");
    sao_core_settings_t* settings = nullptr;
    sao_status_t rc = sao_core_settings_load(path.c_str(), &settings);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(settings != nullptr);

    size_t count = 42;
    REQUIRE(sao_core_settings_key_count(settings, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);

    sao_core_settings_free(settings);
}

TEST_CASE("settings_save_load_roundtrip", "[core][settings]") {
    const std::string path = tempSettingsPath("roundtrip");

    sao_core_settings_t* out = nullptr;
    REQUIRE(sao_core_settings_create(&out) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_int(out, "answer", 42) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_string(out, "name", "sao") == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_bool(out, "enabled", true) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_float(out, "ratio", 1.5) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_save(path.c_str(), out) == SAO_STATUS_OK);
    sao_core_settings_free(out);

    sao_core_settings_t* in = nullptr;
    REQUIRE(sao_core_settings_load(path.c_str(), &in) == SAO_STATUS_OK);

    int64_t answer = 0;
    REQUIRE(sao_core_settings_get_int(in, "answer", -1, &answer) == SAO_STATUS_OK);
    REQUIRE(answer == 42);

    bool enabled = false;
    REQUIRE(sao_core_settings_get_bool(in, "enabled", false, &enabled) == SAO_STATUS_OK);
    REQUIRE(enabled == true);

    double ratio = 0.0;
    REQUIRE(sao_core_settings_get_float(in, "ratio", 0.0, &ratio) == SAO_STATUS_OK);
    REQUIRE(ratio == 1.5);

    char buffer[32] = {0};
    size_t needed = 0;
    REQUIRE(sao_core_settings_get_string(in, "name", "",
                                         buffer, sizeof(buffer), &needed) == SAO_STATUS_OK);
    REQUIRE(std::string(buffer) == "sao");

    sao_core_settings_free(in);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("settings_get_int_missing_returns_default", "[core][settings]") {
    sao_core_settings_t* s = nullptr;
    REQUIRE(sao_core_settings_create(&s) == SAO_STATUS_OK);

    int64_t got = 0;
    REQUIRE(sao_core_settings_get_int(s, "nope", 777, &got) == SAO_STATUS_OK);
    REQUIRE(got == 777);

    bool bgot = true;
    REQUIRE(sao_core_settings_get_bool(s, "nope", false, &bgot) == SAO_STATUS_OK);
    REQUIRE(bgot == false);

    double dgot = 0;
    REQUIRE(sao_core_settings_get_float(s, "nope", 3.25, &dgot) == SAO_STATUS_OK);
    REQUIRE(dgot == 3.25);

    char buf[16] = {0};
    size_t needed = 0;
    REQUIRE(sao_core_settings_get_string(s, "nope", "fallback",
                                         buf, sizeof(buf), &needed) == SAO_STATUS_OK);
    REQUIRE(std::string(buf) == "fallback");

    sao_core_settings_free(s);
}

TEST_CASE("settings_set_get_multiple_types", "[core][settings]") {
    sao_core_settings_t* s = nullptr;
    REQUIRE(sao_core_settings_create(&s) == SAO_STATUS_OK);

    REQUIRE(sao_core_settings_set_int(s, "i", 0x7FFFFFFFFF) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_bool(s, "b", true) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_float(s, "f", -2.5) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_string(s, "s", "hello world") == SAO_STATUS_OK);

    int64_t i = 0;
    REQUIRE(sao_core_settings_get_int(s, "i", 0, &i) == SAO_STATUS_OK);
    REQUIRE(i == 0x7FFFFFFFFF);

    bool b = false;
    REQUIRE(sao_core_settings_get_bool(s, "b", false, &b) == SAO_STATUS_OK);
    REQUIRE(b);

    double f = 0.0;
    REQUIRE(sao_core_settings_get_float(s, "f", 0.0, &f) == SAO_STATUS_OK);
    REQUIRE(f == -2.5);

    char buf[64] = {0};
    size_t needed = 0;
    REQUIRE(sao_core_settings_get_string(s, "s", "",
                                         buf, sizeof(buf), &needed) == SAO_STATUS_OK);
    REQUIRE(std::string(buf) == "hello world");

    // Type mismatch: reading "i" as bool must NOT silently coerce.
    bool wrong = false;
    REQUIRE(sao_core_settings_get_bool(s, "i", false, &wrong) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_core_settings_free(s);
}

TEST_CASE("settings_save_json_format_matches_python_style",
          "[core][settings]") {
    // Python emits:
    //   {
    //     "alpha": 1,
    //     "beta": "two",
    //     "gamma": true
    //   }
    // Note the sorted keys, 2-space indent, single-space after ':',
    // no trailing comma.  Our writer must produce byte-identical
    // output for these types.
    const std::string path = tempSettingsPath("format");
    sao_core_settings_t* s = nullptr;
    REQUIRE(sao_core_settings_create(&s) == SAO_STATUS_OK);
    // Insert out-of-order so the sort really has to do work.
    REQUIRE(sao_core_settings_set_bool(s, "gamma", true) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_int(s, "alpha", 1) == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_set_string(s, "beta", "two") == SAO_STATUS_OK);
    REQUIRE(sao_core_settings_save(path.c_str(), s) == SAO_STATUS_OK);
    sao_core_settings_free(s);

    const std::string blob = readWholeFile(path);
    const std::string expected =
        "{\n"
        "  \"alpha\": 1,\n"
        "  \"beta\": \"two\",\n"
        "  \"gamma\": true\n"
        "}";
    REQUIRE(blob == expected);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("settings_load_malformed_json_returns_error",
          "[core][settings]") {
    const std::string path = tempSettingsPath("malformed");
    {
        std::ofstream f(path, std::ios::binary);
        f << "{ this is not JSON at all }";
    }

    sao_core_settings_t* out = nullptr;
    sao_status_t rc = sao_core_settings_load(path.c_str(), &out);
    REQUIRE(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(out == nullptr);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("legacy config facade validates handles and open arguments",
          "[core][config][legacy]") {
    sao_core_config_handle_t handle = nullptr;
    REQUIRE(sao_core_config_open(nullptr, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
    REQUIRE(sao_core_config_open("unused.json", nullptr) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(sao_core_config_flush(nullptr) == SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(sao_core_config_get_bool(nullptr, "enabled", nullptr) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(sao_core_config_set_int(nullptr, "answer", 42) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(sao_core_config_erase(nullptr, "answer") ==
            SAO_STATUS_ERR_HANDLE_INVALID);

    sao_core_config_close(nullptr);
}

TEST_CASE("legacy config facade preserves typed settings semantics",
          "[core][config][legacy]") {
    const std::string path = tempSettingsPath("legacy_types");
    sao_core_config_handle_t handle = nullptr;
    REQUIRE(sao_core_config_open(path.c_str(), &handle) == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);

    int64_t missing = 0;
    REQUIRE(sao_core_config_get_int(handle, "missing", &missing) ==
            SAO_STATUS_ERR_NOT_FOUND);

    REQUIRE(sao_core_config_set_bool(handle, "enabled", true) == SAO_STATUS_OK);
    REQUIRE(sao_core_config_set_int(handle, "answer", 42) == SAO_STATUS_OK);
    REQUIRE(sao_core_config_set_double(handle, "ratio", 1.5) == SAO_STATUS_OK);
    REQUIRE(sao_core_config_set_string(handle, "name", "SAO-蓝") ==
            SAO_STATUS_OK);

    bool enabled = false;
    int64_t answer = 0;
    double ratio = 0.0;
    REQUIRE(sao_core_config_get_bool(handle, "enabled", &enabled) == SAO_STATUS_OK);
    REQUIRE(enabled);
    REQUIRE(sao_core_config_get_int(handle, "answer", &answer) == SAO_STATUS_OK);
    REQUIRE(answer == 42);
    REQUIRE(sao_core_config_get_double(handle, "ratio", &ratio) == SAO_STATUS_OK);
    REQUIRE(ratio == 1.5);

    REQUIRE(sao_core_config_get_bool(handle, "answer", &enabled) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_core_config_get_int(handle, "ratio", &answer) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    size_t needed = 0;
    REQUIRE(sao_core_config_get_string(handle, "name", nullptr, 0, &needed) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(needed == std::strlen("SAO-蓝") + 1);

    char too_small[4] = {};
    REQUIRE(sao_core_config_get_string(
            handle, "name", too_small, sizeof(too_small), &needed) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    char value[32] = {};
    REQUIRE(sao_core_config_get_string(
            handle, "name", value, sizeof(value), &needed) == SAO_STATUS_OK);
    REQUIRE(std::string(value) == "SAO-蓝");

    sao_core_config_close(handle);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("legacy config facade flushes and erases through settings storage",
          "[core][config][legacy]") {
    const std::string path = tempSettingsPath("legacy_flush");
    {
        std::ofstream old(path, std::ios::binary | std::ios::trunc);
        old << "{\"stale\": true}";
    }

    sao_core_config_handle_t handle = nullptr;
    REQUIRE(sao_core_config_open(path.c_str(), &handle) == SAO_STATUS_OK);
    REQUIRE(sao_core_config_erase(handle, "stale") == SAO_STATUS_OK);
    REQUIRE(sao_core_config_erase(handle, "stale") == SAO_STATUS_OK);
    REQUIRE(sao_core_config_set_string(handle, "profile.name", "Aldina") ==
            SAO_STATUS_OK);
    REQUIRE(sao_core_config_set_int(handle, "overlay.top_n", 8) == SAO_STATUS_OK);
    REQUIRE(sao_core_config_flush(handle) == SAO_STATUS_OK);
    REQUIRE_FALSE(std::filesystem::exists(path + ".tmp"));
    sao_core_config_close(handle);

    handle = nullptr;
    REQUIRE(sao_core_config_open(path.c_str(), &handle) == SAO_STATUS_OK);
    int64_t top_n = 0;
    REQUIRE(sao_core_config_get_int(handle, "overlay.top_n", &top_n) ==
            SAO_STATUS_OK);
    REQUIRE(top_n == 8);
    bool stale = false;
    REQUIRE(sao_core_config_get_bool(handle, "stale", &stale) ==
            SAO_STATUS_ERR_NOT_FOUND);
    sao_core_config_close(handle);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
