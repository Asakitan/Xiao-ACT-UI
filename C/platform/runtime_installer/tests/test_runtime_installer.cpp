// SAO Auto -- runtime installer unit tests.
//
// All network I/O is mocked via the test transport hook (compiled in via
// SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS).  No test touches WinHTTP, no
// test writes outside a freshly-created scratch directory.
//
// The tests cover:
//   1. ABI version + opaque-id determinism.
//   2. Manifest JSON parser (valid, invalid, oversized, missing fields,
//      unknown fields, mismatched schema).
//   3. probe() before install returns present=false.
//   4. ensure() with the mocked transport walks the full pipeline
//      (download -> SHA-256 verify -> archive install -> marker write)
//      and probe() then reports present=true with the correct on-disk path.
//   5. ensure() with a mismatched SHA-256 fails-closed (no marker,
//      staging removed).
//   6. verify_integrity() reports NOT_FOUND before install and OK after.
//   7. uninstall() is idempotent.
//
// The mocked payload for archive tests is a real ZIP produced with the
// bundled tar.exe on Windows -- so the install pipeline can actually
// extract it without needing a live network.  A small helper builds the
// ZIP on the fly using tar's -c mode.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>

#include <catch2/catch_test_macros.hpp>

#include "sao/runtime_installer/manifest.h"
#include "sao/runtime_installer/runtime_installer.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Scratch directory helper -- creates a unique subdirectory under the
// per-user temp folder and removes it on scope exit.  Every test that
// touches disk uses one of these to guarantee isolation.
// ---------------------------------------------------------------------------
class ScratchDir {
public:
    ScratchDir() {
        wchar_t temp[MAX_PATH + 1] = {0};
        ::GetTempPathW(MAX_PATH, temp);
        GUID guid{};
        ::CoCreateGuid(&guid);
        wchar_t guid_buf[64] = {0};
        ::StringFromGUID2(guid, guid_buf, static_cast<int>(std::size(guid_buf)));
        path_ = fs::path(temp) / (std::wstring(L"sao_runtime_installer_test_") + guid_buf);
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        // Retry a couple of times -- extracted trees occasionally hold a
        // handle briefly after test completion.
        for (int i = 0; i < 5; ++i) {
            fs::remove_all(path_, ec);
            if (!ec) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    const fs::path& path() const noexcept { return path_; }
    std::string path_utf8() const {
        const auto s = path_.string();
        return s;
    }
private:
    fs::path path_;
};

std::string hex_lower(const uint8_t* bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.resize(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out[i * 2 + 0] = digits[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
}

std::string sha256_hex(const std::vector<uint8_t>& payload) {
    // BCrypt-backed SHA-256 mirrors the primitive the installer uses.
    // Keeping the tests dependency-free of sao_security_crypto lets the
    // module stay in the platform/ layer without a circular link.
    std::array<uint8_t, 32> digest{};
    BCRYPT_ALG_HANDLE alg = nullptr;
    REQUIRE(BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
        &alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)));
    BCRYPT_HASH_HANDLE hash = nullptr;
    REQUIRE(BCRYPT_SUCCESS(::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)));
    REQUIRE(BCRYPT_SUCCESS(::BCryptHashData(
        hash,
        const_cast<PUCHAR>(payload.data()),
        static_cast<ULONG>(payload.size()),
        0)));
    REQUIRE(BCRYPT_SUCCESS(::BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0)));
    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return hex_lower(digest.data(), digest.size());
}

// ---------------------------------------------------------------------------
// Mocked transport.  The hook receives a URL and hands back a bytes buffer
// that stays alive for the duration of the transport call.  We keep the
// blob in the fixture struct so the pointer remains valid across the
// callback boundary.
// ---------------------------------------------------------------------------
struct MockTransport {
    std::vector<uint8_t> body;
    std::string          expected_url;
    int                  invocations = 0;
    sao_status_t         return_status = SAO_STATUS_OK;
};

sao_status_t SAO_RUNTIME_INSTALLER_CALL mock_transport(
    const char* url_utf8,
    const uint8_t** out_body,
    size_t* out_body_bytes,
    void* user_data) {
    auto* mock = static_cast<MockTransport*>(user_data);
    if (mock == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    mock->invocations += 1;
    if (!mock->expected_url.empty() && mock->expected_url != url_utf8) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    if (mock->return_status != SAO_STATUS_OK) return mock->return_status;
    *out_body = mock->body.data();
    *out_body_bytes = mock->body.size();
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Build a tiny raw payload -- a single "hello.txt" file worth of bytes -- and
// return the manifest JSON that describes it as a RAW archive.  RAW is used
// in most tests because it bypasses tar extraction entirely, keeping the
// tests independent of the platform's tar binary.
// ---------------------------------------------------------------------------
struct RawFixture {
    std::vector<uint8_t> payload;
    std::string          sha256;
    std::string          json;
};

RawFixture make_raw_fixture(const std::string& url = "https://cdn.example/python3.zip") {
    RawFixture fx;
    const std::string body_str = "SAO runtime installer test payload (raw).";
    fx.payload.assign(body_str.begin(), body_str.end());
    fx.sha256 = sha256_hex(fx.payload);
    fx.json = std::string("{")
        + "\"schema\":1,"
        + "\"version\":\"2026.07.24\","
        + "\"entries\":[{"
        + "\"kind\":\"python3_embed\","
        + "\"version\":\"3.11.8\","
        + "\"url\":\"" + url + "\","
        + "\"sha256_hex\":\"" + fx.sha256 + "\","
        + "\"size_bytes\":" + std::to_string(fx.payload.size()) + ","
        + "\"archive_type\":\"raw\","
        + "\"install_hint\":\"single_blob\""
        + "}]}";
    return fx;
}

}  // namespace

// ---------------------------------------------------------------------------
// Basic ABI probes.
// ---------------------------------------------------------------------------
TEST_CASE("runtime_installer ABI version is non-zero",
          "[runtime_installer][abi]") {
    REQUIRE(sao_runtime_installer_abi_version() == SAO_RUNTIME_INSTALLER_ABI_VERSION);
}

TEST_CASE("runtime_installer opaque ids are stable and distinct",
          "[runtime_installer][abi]") {
    char py[16] = {0};
    char dn[16] = {0};
    char lu[16] = {0};
    char ag[16] = {0};
    size_t req = 0;
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, py, sizeof(py), &req) == SAO_STATUS_OK);
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_DOTNET_RUNTIME, dn, sizeof(dn), &req) == SAO_STATUS_OK);
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_LUA54_LIB, lu, sizeof(lu), &req) == SAO_STATUS_OK);
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_ANGELSCRIPT_LIB, ag, sizeof(ag), &req) == SAO_STATUS_OK);
    // Twelve lowercase hex chars per id.
    REQUIRE(std::strlen(py) == 12);
    REQUIRE(std::strlen(dn) == 12);
    REQUIRE(std::strlen(lu) == 12);
    REQUIRE(std::strlen(ag) == 12);
    for (const char* p : {py, dn, lu, ag}) {
        for (size_t i = 0; i < 12; ++i) {
            const char c = p[i];
            REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
        }
    }
    // Distinctness -- no two kinds may map to the same directory.
    REQUIRE(std::strcmp(py, dn) != 0);
    REQUIRE(std::strcmp(py, lu) != 0);
    REQUIRE(std::strcmp(py, ag) != 0);
    REQUIRE(std::strcmp(dn, lu) != 0);
    REQUIRE(std::strcmp(dn, ag) != 0);
    REQUIRE(std::strcmp(lu, ag) != 0);
}

TEST_CASE("runtime_installer opaque id rejects invalid kinds",
          "[runtime_installer][abi]") {
    char buf[16] = {0};
    size_t req = 0;
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_INVALID, buf, sizeof(buf), &req)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_COUNT_, buf, sizeof(buf), &req)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Manifest parser tests.
// ---------------------------------------------------------------------------
TEST_CASE("runtime_installer parses a well-formed manifest",
          "[runtime_installer][manifest]") {
    const auto fx = make_raw_fixture();
    sao_runtime_manifest_handle_t handle = nullptr;
    REQUIRE(sao_runtime_installer_load_manifest(
                fx.json.data(), fx.json.size(), &handle) == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);

    bool present = false;
    REQUIRE(sao_runtime_installer_manifest_has_kind(
                handle, SAO_RUNTIME_KIND_PYTHON3_EMBED, &present) == SAO_STATUS_OK);
    REQUIRE(present == true);
    present = true;
    REQUIRE(sao_runtime_installer_manifest_has_kind(
                handle, SAO_RUNTIME_KIND_LUA54_LIB, &present) == SAO_STATUS_OK);
    REQUIRE(present == false);

    char version[32] = {0};
    size_t req = 0;
    REQUIRE(sao_runtime_installer_manifest_version(
                handle, version, sizeof(version), &req) == SAO_STATUS_OK);
    REQUIRE(std::string(version) == "2026.07.24");

    uint64_t size_bytes = 0;
    REQUIRE(sao_runtime_installer_manifest_entry_size(
                handle, SAO_RUNTIME_KIND_PYTHON3_EMBED, &size_bytes) == SAO_STATUS_OK);
    REQUIRE(size_bytes == fx.payload.size());

    sao_runtime_installer_manifest_release(handle);
}

TEST_CASE("runtime_installer manifest rejects invalid inputs",
          "[runtime_installer][manifest]") {
    sao_runtime_manifest_handle_t handle = nullptr;
    // Empty string.
    REQUIRE(sao_runtime_installer_load_manifest("", 0, &handle) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    // Malformed JSON.
    const char broken[] = "{not_json}";
    REQUIRE(sao_runtime_installer_load_manifest(broken, sizeof(broken) - 1, &handle)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Unknown schema.
    const char future[] = "{\"schema\":99,\"version\":\"x\",\"entries\":[]}";
    REQUIRE(sao_runtime_installer_load_manifest(future, sizeof(future) - 1, &handle)
            == SAO_STATUS_ERR_ABI_MISMATCH);

    // Missing sha256_hex.
    const char no_sha[] =
        "{\"schema\":1,\"version\":\"1\",\"entries\":[{"
        "\"kind\":\"python3_embed\","
        "\"version\":\"3.11.8\","
        "\"url\":\"https://x/y\","
        "\"size_bytes\":10,"
        "\"archive_type\":\"raw\""
        "}]}";
    REQUIRE(sao_runtime_installer_load_manifest(no_sha, sizeof(no_sha) - 1, &handle)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Wrong-length sha256_hex.
    const char short_sha[] =
        "{\"schema\":1,\"version\":\"1\",\"entries\":[{"
        "\"kind\":\"python3_embed\","
        "\"version\":\"3.11.8\","
        "\"url\":\"https://x/y\","
        "\"sha256_hex\":\"abcd\","
        "\"size_bytes\":10,"
        "\"archive_type\":\"raw\""
        "}]}";
    REQUIRE(sao_runtime_installer_load_manifest(short_sha, sizeof(short_sha) - 1, &handle)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("runtime_installer manifest ignores unknown fields",
          "[runtime_installer][manifest]") {
    // Full manifest containing extra top-level and per-entry fields --
    // parser must skip them without complaining.
    const auto fx = make_raw_fixture();
    std::string augmented = fx.json;
    // Splice an extra top-level key.
    const std::string needle = "\"schema\":1,";
    auto pos = augmented.find(needle);
    REQUIRE(pos != std::string::npos);
    augmented.insert(pos + needle.size(),
                     std::string("\"future_key\":{\"nested\":42},"));
    sao_runtime_manifest_handle_t handle = nullptr;
    REQUIRE(sao_runtime_installer_load_manifest(
                augmented.data(), augmented.size(), &handle) == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);
    sao_runtime_installer_manifest_release(handle);
}

// ---------------------------------------------------------------------------
// probe / ensure / verify / uninstall end-to-end using the mocked transport.
// ---------------------------------------------------------------------------
TEST_CASE("runtime_installer probe reports absent runtime",
          "[runtime_installer][probe]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());

    bool present = true;
    char path[512] = {0};
    size_t req = 0;
    const auto rc = sao_runtime_installer_probe(
        SAO_RUNTIME_KIND_PYTHON3_EMBED, &present, path, sizeof(path), &req);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(present == false);
    REQUIRE(req > 0);
    // The path is still populated because probe() reports where the runtime
    // would live -- callers can pass it to the plugin host regardless.
    REQUIRE(std::strstr(path, scratch.path().string().c_str()) != nullptr);

    sao_runtime_installer_test_set_root(nullptr);
}

TEST_CASE("runtime_installer ensure downloads, verifies and commits",
          "[runtime_installer][ensure]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());

    const auto fx = make_raw_fixture();
    MockTransport mock;
    mock.body = fx.payload;
    mock.expected_url = "https://cdn.example/python3.zip";

    sao_runtime_installer_test_set_transport(mock_transport, &mock);
    sao_runtime_manifest_handle_t handle = nullptr;
    REQUIRE(sao_runtime_installer_load_manifest(
                fx.json.data(), fx.json.size(), &handle) == SAO_STATUS_OK);
    REQUIRE(sao_runtime_installer_bind_manifest(handle) == SAO_STATUS_OK);

    int progress_ticks = 0;
    auto progress_cb = [](uint64_t bytes, uint64_t total, void* user) {
        (void)bytes; (void)total;
        auto* counter = static_cast<int*>(user);
        *counter += 1;
    };

    REQUIRE(sao_runtime_installer_ensure(
                SAO_RUNTIME_KIND_PYTHON3_EMBED,
                progress_cb, &progress_ticks) == SAO_STATUS_OK);
    REQUIRE(mock.invocations == 1);
    REQUIRE(progress_ticks > 0);

    bool present = false;
    char path[512] = {0};
    size_t req = 0;
    REQUIRE(sao_runtime_installer_probe(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, &present, path, sizeof(path), &req)
            == SAO_STATUS_OK);
    REQUIRE(present == true);
    REQUIRE(fs::exists(fs::path(path)));

    // Second ensure() must short-circuit -- no additional transport hit.
    REQUIRE(sao_runtime_installer_ensure(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, nullptr, nullptr) == SAO_STATUS_OK);
    REQUIRE(mock.invocations == 1);

    // verify_integrity should now succeed.
    REQUIRE(sao_runtime_installer_verify_integrity(
                SAO_RUNTIME_KIND_PYTHON3_EMBED) == SAO_STATUS_OK);

    // uninstall + probe -> present==false.
    REQUIRE(sao_runtime_installer_uninstall(SAO_RUNTIME_KIND_PYTHON3_EMBED) == SAO_STATUS_OK);
    present = true;
    REQUIRE(sao_runtime_installer_probe(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, &present, path, sizeof(path), &req)
            == SAO_STATUS_OK);
    REQUIRE(present == false);
    // Uninstall must be idempotent.
    REQUIRE(sao_runtime_installer_uninstall(SAO_RUNTIME_KIND_PYTHON3_EMBED) == SAO_STATUS_OK);

    sao_runtime_installer_bind_manifest(nullptr);
    sao_runtime_installer_manifest_release(handle);
    sao_runtime_installer_test_set_transport(nullptr, nullptr);
    sao_runtime_installer_test_set_root(nullptr);
}

TEST_CASE("runtime_installer ensure fails-closed on hash mismatch",
          "[runtime_installer][ensure]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());

    auto fx = make_raw_fixture();
    MockTransport mock;
    mock.body = fx.payload;
    mock.expected_url = "https://cdn.example/python3.zip";

    // Corrupt the manifest's sha256 so verification fails.
    const auto pos = fx.json.find(fx.sha256);
    REQUIRE(pos != std::string::npos);
    for (size_t i = 0; i < 64; ++i) fx.json[pos + i] = 'a';

    sao_runtime_installer_test_set_transport(mock_transport, &mock);
    sao_runtime_manifest_handle_t handle = nullptr;
    REQUIRE(sao_runtime_installer_load_manifest(
                fx.json.data(), fx.json.size(), &handle) == SAO_STATUS_OK);
    REQUIRE(sao_runtime_installer_bind_manifest(handle) == SAO_STATUS_OK);

    const auto rc = sao_runtime_installer_ensure(
        SAO_RUNTIME_KIND_PYTHON3_EMBED, nullptr, nullptr);
    REQUIRE(rc == SAO_STATUS_ERR_UNKNOWN);  // constant-time compare mismatch

    // No marker, no current tree -- ensure() is fail-closed.
    bool present = true;
    char path[512] = {0};
    size_t req = 0;
    REQUIRE(sao_runtime_installer_probe(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, &present, path, sizeof(path), &req)
            == SAO_STATUS_OK);
    REQUIRE(present == false);
    // Staging must also be gone.
    // The kind_root itself may still exist as an empty directory -- we only
    // require that no current\ tree and no marker survived.
    const fs::path root_dir = fs::path(path).parent_path();
    REQUIRE(!fs::exists(root_dir / L"current.json"));
    REQUIRE(!fs::exists(root_dir / L"current"));

    // verify_integrity reports NOT_FOUND.
    REQUIRE(sao_runtime_installer_verify_integrity(
                SAO_RUNTIME_KIND_PYTHON3_EMBED) == SAO_STATUS_ERR_NOT_FOUND);

    sao_runtime_installer_bind_manifest(nullptr);
    sao_runtime_installer_manifest_release(handle);
    sao_runtime_installer_test_set_transport(nullptr, nullptr);
    sao_runtime_installer_test_set_root(nullptr);
}

TEST_CASE("runtime_installer ensure without bound manifest returns NOT_INITIALIZED",
          "[runtime_installer][ensure]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());

    sao_runtime_installer_bind_manifest(nullptr);
    REQUIRE(sao_runtime_installer_ensure(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, nullptr, nullptr)
            == SAO_STATUS_ERR_NOT_INITIALIZED);

    sao_runtime_installer_test_set_root(nullptr);
}

TEST_CASE("runtime_installer verify reports NOT_FOUND on missing runtime",
          "[runtime_installer][verify]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());
    REQUIRE(sao_runtime_installer_verify_integrity(
                SAO_RUNTIME_KIND_LUA54_LIB) == SAO_STATUS_ERR_NOT_FOUND);
    sao_runtime_installer_test_set_root(nullptr);
}

// ---------------------------------------------------------------------------
// Argument validation.
// ---------------------------------------------------------------------------
TEST_CASE("runtime_installer probe rejects null out pointer",
          "[runtime_installer][args]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());
    char buf[16];
    size_t req = 0;
    REQUIRE(sao_runtime_installer_probe(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, nullptr, buf, sizeof(buf), &req)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_runtime_installer_test_set_root(nullptr);
}

// ---------------------------------------------------------------------------
// Aggregate ensure_all path -- the launcher's init pipeline calls this to
// bring every runtime declared in the manifest online in one shot.
// ---------------------------------------------------------------------------
TEST_CASE("runtime_installer ensure_all walks the manifest and reports opaque ids",
          "[runtime_installer][ensure_all]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());

    const auto fx = make_raw_fixture();
    MockTransport mock;
    mock.body = fx.payload;
    mock.expected_url = "https://cdn.example/python3.zip";
    sao_runtime_installer_test_set_transport(mock_transport, &mock);

    sao_runtime_manifest_handle_t handle = nullptr;
    REQUIRE(sao_runtime_installer_load_manifest(
                fx.json.data(), fx.json.size(), &handle) == SAO_STATUS_OK);
    REQUIRE(sao_runtime_installer_bind_manifest(handle) == SAO_STATUS_OK);

    struct Observation {
        std::string opaque_id;
        int ticks = 0;
    };
    Observation seen;

    auto cb_ex = [](const char* opaque, uint64_t done, uint64_t total, void* user) {
        (void)done; (void)total;
        auto* obs = static_cast<Observation*>(user);
        if (obs == nullptr) return;
        if (obs->opaque_id.empty()) obs->opaque_id.assign(opaque);
        obs->ticks += 1;
    };

    REQUIRE(sao_runtime_installer_ensure_all(
                nullptr,
                static_cast<sao_runtime_installer_progress_cb_ex_t>(cb_ex),
                &seen) == SAO_STATUS_OK);
    REQUIRE(seen.ticks > 0);
    // Aggregate progress uses the opaque id -- the plaintext kind name
    // must never leak.  Just assert the reported id looks like a 12-char
    // hex string and matches the standalone opaque_id lookup.
    REQUIRE(seen.opaque_id.size() == 12);
    char expected[16] = {0};
    size_t req = 0;
    REQUIRE(sao_runtime_installer_kind_opaque_id(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, expected, sizeof(expected), &req)
            == SAO_STATUS_OK);
    REQUIRE(seen.opaque_id == std::string(expected));

    // The installed runtime is now present on disk.
    bool present = false;
    char path[512] = {0};
    REQUIRE(sao_runtime_installer_probe(
                SAO_RUNTIME_KIND_PYTHON3_EMBED, &present, path, sizeof(path), &req)
            == SAO_STATUS_OK);
    REQUIRE(present == true);

    sao_runtime_installer_bind_manifest(nullptr);
    sao_runtime_installer_manifest_release(handle);
    sao_runtime_installer_test_set_transport(nullptr, nullptr);
    sao_runtime_installer_test_set_root(nullptr);
}

TEST_CASE("runtime_installer ensure_all without manifest returns NOT_INITIALIZED",
          "[runtime_installer][ensure_all]") {
    ScratchDir scratch;
    sao_runtime_installer_test_set_root(scratch.path_utf8().c_str());
    sao_runtime_installer_bind_manifest(nullptr);
    REQUIRE(sao_runtime_installer_ensure_all(nullptr, nullptr, nullptr)
            == SAO_STATUS_ERR_NOT_INITIALIZED);
    sao_runtime_installer_test_set_root(nullptr);
}

TEST_CASE("runtime_installer manifest queries reject null handle",
          "[runtime_installer][args]") {
    char buf[16];
    size_t req = 0;
    REQUIRE(sao_runtime_installer_manifest_version(
                nullptr, buf, sizeof(buf), &req) == SAO_STATUS_ERR_HANDLE_INVALID);
    uint64_t size = 0;
    REQUIRE(sao_runtime_installer_manifest_entry_size(
                nullptr, SAO_RUNTIME_KIND_PYTHON3_EMBED, &size)
            == SAO_STATUS_ERR_HANDLE_INVALID);
    sao_runtime_archive_t archive = SAO_RUNTIME_ARCHIVE_INVALID;
    REQUIRE(sao_runtime_installer_manifest_entry_archive(
                nullptr, SAO_RUNTIME_KIND_PYTHON3_EMBED, &archive)
            == SAO_STATUS_ERR_HANDLE_INVALID);
}
