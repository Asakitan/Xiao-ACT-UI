// SAO — platform/core/diag_log.cpp
//
// Implementation notes
//   Debug build   : plaintext append to pp_diag.log (or SAO_RT_IO_PP_DEBUG).
//   Non-debug     : each line -> "E1 <base64(ephPubXY|nonce|tag|ct)>" where the
//                   ephemeral P-256 public key is agreed against the embedded
//                   static public key via CNG ECDH and the line body is
//                   AES-256-GCM encrypted. No private key material is present
//                   in the shipped binary.
//   Truncation    : sao_diag_begin_session() uses CREATE_ALWAYS, so every new
//                   driver-chain launch starts a fresh diagnostic file.

#include "sao/core/diag_log.h"

#include <cstdio>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cstring>

#if defined(_WIN32)
#    include <windows.h>
#    include <bcrypt.h>
#    pragma comment(lib, "bcrypt.lib")
#endif

namespace {

const char kDiagPath[] = "C:\\ProgramData\\SAO\\rt_io\\pp_diag.log";

#if !defined(SAO_RT_IO_ACTUAL_DEBUG)

// Static diagnostic public key (P-256 uncompressed point, X||Y after 0x04).
// Matching private scalar lives only on the build operator's machine at
// E:\VC\secrets\sao_diag_priv.hex — never shipped.
const uint8_t kDiagPubkey[65] = {
    0x04, 0x26, 0x65, 0x79, 0x42, 0x80, 0x6C, 0x0F, 0x70, 0x5E, 0x72, 0xC8, 0x33,
    0x48, 0x6F, 0x57, 0x49, 0x78, 0xFF, 0x8A, 0x2F, 0x9E, 0x81, 0xDA, 0xC6, 0x9A,
    0xD4, 0x28, 0xC5, 0x1A, 0x37, 0x24, 0x0B, 0x94, 0xF1, 0x6D, 0x37, 0xEA, 0x4D,
    0x8E, 0x20, 0xC3, 0x07, 0xE8, 0x38, 0x06, 0x46, 0xFC, 0xA7, 0x65, 0x25, 0x2A,
    0xE9, 0x46, 0x31, 0x6F, 0xEF, 0x88, 0x88, 0x33, 0x85, 0xA3, 0x94, 0x3B, 0xF6,
};

const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t* data, size_t size, char* out, size_t out_capacity) {
    const size_t produced = ((size + 2u) / 3u) * 4u;
    if (out == nullptr || out_capacity < produced + 1u)
        return 0u;
    size_t o = 0u;
    for (size_t i = 0u; i < size; i += 3u) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1u < size) ? data[i + 1u] : 0u;
        const uint32_t b2 = (i + 2u < size) ? data[i + 2u] : 0u;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = kB64Alphabet[(triple >> 18) & 0x3Fu];
        out[o++] = kB64Alphabet[(triple >> 12) & 0x3Fu];
        out[o++] = (i + 1u < size) ? kB64Alphabet[(triple >> 6) & 0x3Fu] : '=';
        out[o++] = (i + 2u < size) ? kB64Alphabet[triple & 0x3Fu] : '=';
    }
    out[o] = '\0';
    return o;
}

#    if defined(_WIN32)

// Derive an AES-256 key from the ECDH shared secret: fetch the raw secret via
// BCRYPT_KDF_RAW_SECRET, then SHA-256 it locally (matches the decryptor's
// plain-SHA256 KDF). Returns false on any CNG failure — the caller then
// drops the line rather than logging plaintext.
bool ecies_derive_key(BCRYPT_SECRET_HANDLE secret, uint8_t out_key[32]) {
    uint8_t raw[32];
    ULONG produced = 0u;
    const NTSTATUS st = ::BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr,
                                          raw, sizeof(raw), &produced, 0u);
    const bool got_raw = BCRYPT_SUCCESS(st) && produced == sizeof(raw);
    BCRYPT_ALG_HANDLE hash_alg = nullptr;
    bool ok = false;
    if (got_raw &&
        BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&hash_alg, BCRYPT_SHA256_ALGORITHM,
                                                     nullptr, 0))) {
        ok = BCRYPT_SUCCESS(::BCryptHash(hash_alg, nullptr, 0u, raw, sizeof(raw),
                                         out_key, 32u));
        ::BCryptCloseAlgorithmProvider(hash_alg, 0u);
    }
    std::memset(raw, 0, sizeof(raw));
    return ok;
}

bool aes_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* pt,
                     size_t pt_size, uint8_t* ct, uint8_t tag[16]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM,
                                                      nullptr, 0)))
        return false;
    BCRYPT_KEY_HANDLE hkey = nullptr;
    bool ok = false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ainfo{};
    do {
        if (!BCRYPT_SUCCESS(::BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                                reinterpret_cast<PUCHAR>(
                                                    const_cast<wchar_t*>(
                                                        BCRYPT_CHAIN_MODE_GCM)),
                                                static_cast<ULONG>(
                                                    sizeof(BCRYPT_CHAIN_MODE_GCM)),
                                                0)))
            break;
        if (!BCRYPT_SUCCESS(::BCryptGenerateSymmetricKey(
                alg, &hkey, nullptr, 0u, const_cast<uint8_t*>(key), 32u, 0)))
            break;
        uint8_t nonce_copy[12];
        std::memcpy(nonce_copy, nonce, sizeof(nonce_copy));
        ainfo.cbSize = sizeof(ainfo);
        ainfo.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
        ainfo.pbNonce = nonce_copy;
        ainfo.cbNonce = sizeof(nonce_copy);
        ainfo.pbTag = tag;
        ainfo.cbTag = 16u;
        ULONG produced = 0u;
        ok = BCRYPT_SUCCESS(::BCryptEncrypt(hkey, const_cast<uint8_t*>(pt),
                                            static_cast<ULONG>(pt_size), &ainfo, nullptr,
                                            0u, ct, static_cast<ULONG>(pt_size),
                                            &produced, 0u));
    } while (false);
    if (hkey != nullptr)
        ::BCryptDestroyKey(hkey);
    ::BCryptCloseAlgorithmProvider(alg, 0u);
    return ok;
}

// Encrypt one line; writes "E1 <b64>\n" into out. Returns false on failure.
bool ecies_wrap(const char* line, size_t length, char* out, size_t out_capacity,
                size_t* out_written) {
    *out_written = 0u;
    BCRYPT_ALG_HANDLE ecdh = nullptr;
    BCRYPT_KEY_HANDLE eph = nullptr;
    BCRYPT_KEY_HANDLE remote = nullptr;
    BCRYPT_SECRET_HANDLE secret = nullptr;
    bool ok = false;
    uint8_t eph_xy[64];
    uint8_t key[32]{};
    do {
        if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM,
                                                        nullptr, 0)))
            break;
        if (!BCRYPT_SUCCESS(::BCryptGenerateKeyPair(ecdh, &eph, 256u, 0u)))
            break;
        if (!BCRYPT_SUCCESS(::BCryptFinalizeKeyPair(eph, 0u)))
            break;
        UCHAR blob[sizeof(BCRYPT_ECCKEY_BLOB) + sizeof(eph_xy)];
        ULONG blob_size = 0u;
        if (!BCRYPT_SUCCESS(::BCryptExportKey(eph, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob,
                                              sizeof(blob), &blob_size, 0u)) ||
            blob_size != sizeof(blob))
            break;
        std::memcpy(eph_xy, blob + sizeof(BCRYPT_ECCKEY_BLOB), sizeof(eph_xy));

        BCRYPT_ECCKEY_BLOB import_blob{};
        import_blob.dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
        import_blob.cbKey = 32u;
        uint8_t import_buffer[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
        std::memcpy(import_buffer, &import_blob, sizeof(import_blob));
        std::memcpy(import_buffer + sizeof(import_blob), kDiagPubkey + 1, 64u);
        if (!BCRYPT_SUCCESS(::BCryptImportKeyPair(ecdh, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                                  &remote, import_buffer,
                                                  sizeof(import_buffer), 0u)))
            break;
        if (!BCRYPT_SUCCESS(::BCryptSecretAgreement(eph, remote, &secret, 0u)))
            break;
        if (!ecies_derive_key(secret, key))
            break;
        uint8_t nonce[12];
        if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, nonce, sizeof(nonce),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            break;
        uint8_t ct[768];
        uint8_t tag[16];
        if (length == 0u || length > sizeof(ct))
            break;
        if (!aes_gcm_encrypt(key, nonce, reinterpret_cast<const uint8_t*>(line), length,
                             ct, tag))
            break;
        uint8_t frame[64 + 12 + 16 + 768];
        std::memcpy(frame, eph_xy, 64u);
        std::memcpy(frame + 64, nonce, 12u);
        std::memcpy(frame + 76, tag, 16u);
        std::memcpy(frame + 92, ct, length);
        const size_t frame_size = 92u + length;
        if (out_capacity < 4u + ((frame_size + 2u) / 3u) * 4u + 2u)
            break;
        std::memcpy(out, "E1 ", 3u);
        const size_t b64_len =
            b64_encode(frame, frame_size, out + 3u, out_capacity - 3u - 2u);
        if (b64_len == 0u)
            break;
        out[3u + b64_len] = '\n';
        *out_written = 3u + b64_len + 1u;
        ok = true;
    } while (false);
    std::memset(key, 0, sizeof(key));
    if (secret != nullptr)
        ::BCryptDestroySecret(secret);
    if (remote != nullptr)
        ::BCryptDestroyKey(remote);
    if (eph != nullptr)
        ::BCryptDestroyKey(eph);
    if (ecdh != nullptr)
        ::BCryptCloseAlgorithmProvider(ecdh, 0u);
    return ok;
}

#    endif // _WIN32
#endif     // !SAO_RT_IO_ACTUAL_DEBUG

#if defined(_WIN32)
const char* diag_log_path() {
    static char path[MAX_PATH] = {};
#    if defined(SAO_RT_IO_ACTUAL_DEBUG)
    // Debug-only escape hatch for tests/ops that redirect the log.
    if (::GetEnvironmentVariableA("SAO_RT_IO_PP_DEBUG", path, MAX_PATH) != 0u &&
        path[0] != '\0')
        return path;
#    endif
    return kDiagPath;
}

HANDLE diag_open(const char* path, DWORD creation) {
    const DWORD attributes = ::GetFileAttributesA(path);
    // Never follow a reparse point / junction swapped in under ProgramData —
    // CREATE_ALWAYS on a link would truncate an attacker-chosen target.
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0u)
        return INVALID_HANDLE_VALUE;
    // CreateFile does not build parent directories; on a fresh machine
    // C:\ProgramData\SAO\rt_io may not exist at all, which would silently
    // drop every diagnostic line. Create the directory chain first —
    // CreateDirectory is a no-op for components that already exist.
    char dir[MAX_PATH]{};
    const size_t len = std::strlen(path);
    if (len < sizeof(dir)) {
        std::memcpy(dir, path, len + 1u);
        for (size_t i = 0u; i < len; ++i) {
            if ((dir[i] == '\\' || dir[i] == '/') && i > 3u) {
                const char saved = dir[i];
                dir[i] = '\0';
                if (::GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES)
                    (void)::CreateDirectoryA(dir, nullptr);
                dir[i] = saved;
            }
        }
    }
    return ::CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif // _WIN32

} // namespace

void sao_diag_begin_session(void) {
#if defined(_WIN32)
    const char* path = diag_log_path();
    HANDLE file = diag_open(path, CREATE_ALWAYS);
    if (file == INVALID_HANDLE_VALUE)
        return;
    ::CloseHandle(file);
    char stamp[64]{};
    const int stamp_len =
        std::snprintf(stamp, sizeof(stamp), "SAO-DIAG-BEGIN tick=%llu",
                      static_cast<unsigned long long>(::GetTickCount64()));
    if (stamp_len > 0)
        sao_diag_write(stamp, static_cast<size_t>(stamp_len));
#endif
}

void sao_diag_write(const char* line, size_t length) {
#if defined(_WIN32)
    if (line == nullptr || length == 0u)
        return;
    const bool has_lf = line[length - 1u] == '\n';
    const size_t body_len = has_lf ? length - 1u : length;

#    if defined(SAO_RT_IO_ACTUAL_DEBUG)
    HANDLE file = diag_open(diag_log_path(), OPEN_ALWAYS);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD out = 0u;
    (void)::WriteFile(file, line, static_cast<DWORD>(body_len), &out, nullptr);
    static const char kLf[] = "\n";
    (void)::WriteFile(file, kLf, 1u, &out, nullptr);
    ::CloseHandle(file);
#    else
    char wrapped[1400]{};
    size_t wrapped_len = 0u;
    if (!ecies_wrap(line, body_len, wrapped, sizeof(wrapped), &wrapped_len))
        return;
    HANDLE file = diag_open(diag_log_path(), OPEN_ALWAYS);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD out = 0u;
    (void)::WriteFile(file, wrapped, static_cast<DWORD>(wrapped_len), &out, nullptr);
    ::CloseHandle(file);
#    endif
#else
    (void)line;
    (void)length;
#endif
}
