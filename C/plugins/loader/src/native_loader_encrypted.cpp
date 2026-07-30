// native_loader_encrypted.cpp — decrypted .pyd / .dll load path (Phase 14 live).
//
// Port of act_platform/protect/native_loader.py. Reads encrypted blob:
//   [12-byte nonce][16-byte tag][ciphertext...]
// Decrypts via security::crypto AES-256-GCM with a 32-byte key derived from
// hex string; the decrypted image is loaded entirely in memory via the
// reflective loader (no temp file on disk, no PEB loader-list registration);
// caller receives the loaded image base as the module handle.

#include "sao_security/crypto/aes.h"
#include "sao_security/loader/reflective_dll.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sao::plugins::loader::native_loader {

namespace {

bool parse_hex_key(const char* hex, uint8_t key32[32]) {
    if (hex == nullptr) return false;
    std::string s = hex;
    if (s.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        auto val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = val(s[i * 2]);
        int lo = val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        key32[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace

extern "C" int sao_plugins_native_loader_load_encrypted(
    const char* encrypted_path, const char* key_hex,
    void** out_module_handle) {
    if (encrypted_path == nullptr || key_hex == nullptr ||
        out_module_handle == nullptr)
        return -1;
    *out_module_handle = nullptr;
    uint8_t key32[32];
    if (!parse_hex_key(key_hex, key32)) return -2;

    // Read blob.
    std::ifstream in(encrypted_path, std::ios::binary);
    if (!in) return -3;
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    if (blob.size() < 12 + 16 + 1) return -4;
    const uint8_t* nonce = blob.data();
    uint8_t* tag = blob.data() + 12;
    const uint8_t* ct = blob.data() + 12 + 16;
    size_t ct_len = blob.size() - 12 - 16;
    if (ct_len > static_cast<size_t>(UINT32_MAX)) return -4;
    const uint32_t ct_len_u32 = static_cast<uint32_t>(ct_len);
    std::vector<uint8_t> plaintext(ct_len);

    SaoAesGcmParams params{};
    params.key = key32;
    params.key_len = 32;
    params.iv = const_cast<uint8_t*>(nonce);
    params.iv_len = 12;
    params.aad = nullptr;
    params.aad_len = 0;
    params.tag = tag;
    params.tag_len = 16;
    if (sao_security_crypto_aes256_gcm_decrypt(
            &params, ct, ct_len_u32, plaintext.data(), ct_len_u32) != 0)
        return -5;

    // Load the decrypted image entirely in memory (no disk, no PEB Ldr
    // registration) via the reflective loader; the mapped base is the handle.
    void* image_base = nullptr;
    const int32_t load_rc = sao_security_loader_reflective_load(
        plaintext.data(), static_cast<uint32_t>(plaintext.size()), &image_base);

    // Wipe plaintext buffer from memory before yielding control.
    volatile uint8_t* wp = plaintext.data();
    for (size_t i = 0; i < plaintext.size(); ++i) wp[i] = 0;

    if (load_rc != 0 || image_base == nullptr) return -7;

#if defined(_WIN32)
    // Invoke DllMain(DLL_PROCESS_ATTACH) on the mapped image entry point.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(image_base) + dos->e_lfanew);
    const uint32_t entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
    if (entry_rva != 0) {
        using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto dll_main = reinterpret_cast<DllMainFn>(
            reinterpret_cast<uint8_t*>(image_base) + entry_rva);
        if (!dll_main(reinterpret_cast<HINSTANCE>(image_base), DLL_PROCESS_ATTACH, nullptr))
            return -7;
    }
#endif
    *out_module_handle = image_base;
    return 0;
}

} // namespace sao::plugins::loader::native_loader
