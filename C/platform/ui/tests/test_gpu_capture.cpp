// Tests for the gpu_capture stateless helpers (BGRA readback,
// premultiplication, stable hash).
//
// Coverage (4 test cases):
//   * gpu_capture_bgra_from_texture_extract_or_skip — requires an
//     interactive D3D11 device; SKIP on session 0 / no-GPU.
//   * gpu_capture_premultiply_naive_gamma
//   * gpu_capture_premultiply_alpha_correct_srgb
//   * gpu_capture_compute_hash_is_stable
//
// The hash + premultiply tests are pure math (no D3D); they run
// everywhere the shared lib links.  BGRA readback requires a real
// D3D11 device and SKIPs if D3D11CreateDevice fails.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/gpu_capture.h"
#include "sao/core/status.h"

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d11.h>
#endif

#if defined(_WIN32)

TEST_CASE("gpu_capture_session_lifecycle_or_explicit_capability_gate",
          "[ui][gpu_capture][lifecycle]") {
    HWND capture_window = ::CreateWindowExW(
        0, L"STATIC", L"SAO WGC lifecycle", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        32, 32, 320, 180, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    REQUIRE(capture_window != nullptr);
    SaoGpuCaptureConfig config{};
    config.hwnd = capture_window;
    config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.disable_cursor = true;
    config.max_frame_age_sec = 1.0;
    sao_ui_gpu_capture_handle_t capture = nullptr;
    const sao_status_t create_status = sao_ui_gpu_capture_create(&config, &capture);
    if (!sao_ui_gpu_capture_supported()) {
        CHECK(create_status == SAO_STATUS_ERR_NOT_IMPLEMENTED);
        CHECK(capture == nullptr);
        ::DestroyWindow(capture_window);
        return;
    }
    REQUIRE(create_status == SAO_STATUS_OK);
    REQUIRE(capture != nullptr);
    REQUIRE(sao_ui_gpu_capture_ensure_session(capture) == SAO_STATUS_OK);
    int32_t state = -1;
    REQUIRE(sao_ui_gpu_capture_get_state(capture, &state) == SAO_STATUS_OK);
    CHECK(state == SAO_UI_GPU_CAPTURE_RUNNING);
    SaoGpuCaptureFrame frame{};
    const sao_status_t frame_status = sao_ui_gpu_capture_get_latest(capture, &frame);
    CHECK((frame_status == SAO_STATUS_OK || frame_status == SAO_STATUS_ERR_NOT_FOUND));
    if (frame_status == SAO_STATUS_OK) {
        CHECK(frame.pixels != nullptr);
        CHECK(frame.width > 0);
        CHECK(frame.height > 0);
        CHECK(frame.row_pitch == frame.width * 4u);
        CHECK(frame.channels == 4);
    }
    REQUIRE(sao_ui_gpu_capture_stop(capture) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_capture_get_state(capture, &state) == SAO_STATUS_OK);
    CHECK(state == SAO_UI_GPU_CAPTURE_STOPPED);
    sao_ui_gpu_capture_destroy(capture);
    REQUIRE(::DestroyWindow(capture_window) != FALSE);
}

TEST_CASE("gpu_capture_bgra_from_texture_extract_or_skip",
          "[ui][gpu_capture][real_plugins]") {
    // Create a stand-alone D3D11 device with WARP fallback and stamp
    // a synthetic 4x4 BGRA texture to prove the readback wiring.  If
    // no D3D11 device comes up, SKIP — this is a pure-plumbing test,
    // not a hardware guarantee.
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* ctx    = nullptr;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &device, &got, &ctx);
    if (FAILED(hr)) {
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &device, &got, &ctx);
    }
    if (FAILED(hr) || device == nullptr) {
        SKIP("D3D11CreateDevice failed on this host");
    }

    // 4x4 BGRA-UNORM texture, initial data is opaque red.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 4;
    td.Height = 4;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;

    uint8_t initial[4 * 4 * 4];
    for (int i = 0; i < 4 * 4; ++i) {
        initial[i * 4 + 0] = 0x00;  // B
        initial[i * 4 + 1] = 0x00;  // G
        initial[i * 4 + 2] = 0xFF;  // R
        initial[i * 4 + 3] = 0xFF;  // A
    }
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = initial;
    init.SysMemPitch = 4 * 4;

    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr) || tex == nullptr) {
        if (ctx) ctx->Release();
        device->Release();
        SKIP("CreateTexture2D failed");
    }

    // Undersized buffer must error.
    uint8_t tiny[8];
    uint32_t stride = 0;
    sao_status_t rc = sao_ui_gpu_capture_bgra_from_texture(
        tex, 4, 4, tiny, sizeof(tiny), &stride);
    CHECK(rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    CHECK(stride >= 4u * 4u);

    // Sized read.
    const uint32_t needed = stride * 4u;
    std::vector<uint8_t> buf(needed, 0u);
    rc = sao_ui_gpu_capture_bgra_from_texture(
        tex, 4, 4, buf.data(), static_cast<uint32_t>(buf.size()), &stride);
    CHECK(rc == SAO_STATUS_OK);
    // Row 0, pixel 0: opaque red = 0x00 0x00 0xFF 0xFF.
    CHECK(buf[0] == 0x00);
    CHECK(buf[1] == 0x00);
    CHECK(buf[2] == 0xFF);
    CHECK(buf[3] == 0xFF);

    // Format mismatch is rejected.
    D3D11_TEXTURE2D_DESC td2 = td;
    td2.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // NOT BGRA
    uint8_t alt_pixels[4 * 4 * 4] = {0};
    init.pSysMem = alt_pixels;
    ID3D11Texture2D* tex2 = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&td2, &init, &tex2)) &&
        tex2 != nullptr) {
        rc = sao_ui_gpu_capture_bgra_from_texture(
            tex2, 4, 4, buf.data(),
            static_cast<uint32_t>(buf.size()), &stride);
        CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
        tex2->Release();
    }
    // NULL texture argument rejected.
    rc = sao_ui_gpu_capture_bgra_from_texture(
        nullptr, 4, 4, buf.data(),
        static_cast<uint32_t>(buf.size()), &stride);
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);

    tex->Release();
    ctx->Release();
    device->Release();
}

#else

TEST_CASE("gpu_capture_bgra_from_texture_windows_only",
          "[ui][gpu_capture][real_plugins]") {
    SUCCEED("D3D11 readback is Windows-only");
}

#endif  // _WIN32

TEST_CASE("gpu_capture_premultiply_naive_gamma",
          "[ui][gpu_capture][real_plugins]") {
    // 3 BGRA pixels: full opaque, half alpha, fully transparent.
    uint8_t px[3 * 4] = {
        200, 100,  50, 255,   // opaque — must not change
        200, 100,  50, 128,   // half alpha — naive: floor((C*α+127)/255)
        200, 100,  50,   0,   // transparent — must become black
    };
    REQUIRE(sao_ui_gpu_capture_premultiply_bgra(
        px, sizeof(px), false) == SAO_STATUS_OK);
    // Opaque unchanged.
    CHECK(px[0] == 200);
    CHECK(px[1] == 100);
    CHECK(px[2] == 50);
    CHECK(px[3] == 255);
    // Half alpha: (200 * 128 + 127) / 255 = (25600 + 127) / 255 = 100
    CHECK(px[4]  == static_cast<uint8_t>((200u * 128u + 127u) / 255u));
    CHECK(px[5]  == static_cast<uint8_t>((100u * 128u + 127u) / 255u));
    CHECK(px[6]  == static_cast<uint8_t>(( 50u * 128u + 127u) / 255u));
    CHECK(px[7]  == 128);
    // Transparent flattened to black.
    CHECK(px[8]  == 0);
    CHECK(px[9]  == 0);
    CHECK(px[10] == 0);
    CHECK(px[11] == 0);

    // Odd-sized (not %4) rejected.
    uint8_t odd[7] = {0};
    CHECK(sao_ui_gpu_capture_premultiply_bgra(odd, 7, false) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    // NULL rejected.
    CHECK(sao_ui_gpu_capture_premultiply_bgra(nullptr, 4, false) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("gpu_capture_premultiply_alpha_correct_srgb",
          "[ui][gpu_capture][real_plugins]") {
    // Alpha-correct branch: linearise → scale → re-encode.  For a
    // mid-gray with half alpha the sRGB path produces a distinctly
    // darker result than the naive gamma-space multiply (because
    // the darker pixel value survives the linear round-trip better).
    uint8_t naive[4] = {128, 128, 128, 128};
    uint8_t alpha_corr[4] = {128, 128, 128, 128};
    REQUIRE(sao_ui_gpu_capture_premultiply_bgra(naive, 4, false) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_capture_premultiply_bgra(alpha_corr, 4, true) ==
            SAO_STATUS_OK);
    // Both must clamp non-alpha channels to <= 128 (α scaling shrinks).
    CHECK(naive[0]      <= 128);
    CHECK(alpha_corr[0] <= 128);
    // Both must preserve alpha channel unchanged.
    CHECK(naive[3]      == 128);
    CHECK(alpha_corr[3] == 128);
    // The two branches produce different mid-gray values — the sRGB
    // path attenuates less than naive gamma multiplication.  This is
    // the whole point of exposing both branches to the caller.
    CHECK(alpha_corr[0] != naive[0]);

    // Opaque pixel unchanged in either branch.
    uint8_t opaque_naive[4] = {50, 100, 200, 255};
    uint8_t opaque_srgb[4]  = {50, 100, 200, 255};
    REQUIRE(sao_ui_gpu_capture_premultiply_bgra(
        opaque_naive, 4, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_capture_premultiply_bgra(
        opaque_srgb, 4, true) == SAO_STATUS_OK);
    CHECK(opaque_naive[0] == 50);
    CHECK(opaque_srgb[0]  == 50);
    CHECK(opaque_naive[3] == 255);
    CHECK(opaque_srgb[3]  == 255);
}

TEST_CASE("gpu_capture_compute_hash_is_stable",
          "[ui][gpu_capture][real_plugins]") {
    // Deterministic across calls, byte-order independent, sensitive
    // to single-bit changes.
    const uint8_t a[16] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
    };
    uint64_t a_hi = 0, a_lo = 0;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        a, sizeof(a), &a_hi, &a_lo) == SAO_STATUS_OK);
    CHECK(a_hi != 0);
    CHECK(a_lo != 0);

    // Second call: bit-identical output.
    uint64_t a_hi2 = 0, a_lo2 = 0;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        a, sizeof(a), &a_hi2, &a_lo2) == SAO_STATUS_OK);
    CHECK(a_hi == a_hi2);
    CHECK(a_lo == a_lo2);

    // Change a single byte — hash must differ (avalanche).
    uint8_t b[16];
    std::memcpy(b, a, sizeof(b));
    b[8] ^= 0x40;                             // flip a middle bit
    uint64_t b_hi = 0, b_lo = 0;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        b, sizeof(b), &b_hi, &b_lo) == SAO_STATUS_OK);
    CHECK((b_hi != a_hi || b_lo != a_lo));

    // Longer payload (spans multiple 8-byte blocks + tail).
    const uint32_t big_size = 1024u + 5u;   // 1029 bytes = 128 blocks + 5 tail
    std::vector<uint8_t> big(big_size, 0u);
    for (uint32_t i = 0; i < big_size; ++i) {
        big[i] = static_cast<uint8_t>((i * 31u) & 0xFFu);
    }
    uint64_t big_hi = 0, big_lo = 0;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        big.data(), big_size, &big_hi, &big_lo) == SAO_STATUS_OK);
    CHECK(big_hi != 0);
    CHECK(big_lo != 0);

    // Reversed-endianness sanity: reverse the byte order of `a` and
    // confirm the hash is still deterministic (not necessarily equal
    // to the forward hash — endianness independence at the wire
    // level, not at the payload level).
    uint8_t rev[16];
    for (int i = 0; i < 16; ++i) rev[i] = a[15 - i];
    uint64_t rev_hi = 0, rev_lo = 0;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        rev, sizeof(rev), &rev_hi, &rev_lo) == SAO_STATUS_OK);
    CHECK((rev_hi != a_hi || rev_lo != a_lo));

    // Empty payload — deterministic (may be non-zero because we seed
    // with the length).
    uint64_t z_hi = 0xDEADBEEF, z_lo = 0xCAFEF00D;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        nullptr, 0, &z_hi, &z_lo) == SAO_STATUS_OK);
    // Second empty call: bit-identical.
    uint64_t z_hi2 = 0, z_lo2 = 0;
    REQUIRE(sao_ui_gpu_capture_compute_hash(
        nullptr, 0, &z_hi2, &z_lo2) == SAO_STATUS_OK);
    CHECK(z_hi == z_hi2);
    CHECK(z_lo == z_lo2);

    // NULL out-pointer rejected.
    CHECK(sao_ui_gpu_capture_compute_hash(
        a, sizeof(a), nullptr, &a_lo) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_gpu_capture_compute_hash(
        a, sizeof(a), &a_hi, nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);
}
