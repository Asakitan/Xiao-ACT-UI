// SAO Auto — generic crypto primitives.
//
// A stable façade over the algorithms platform code needs internally:
// AES-GCM (settings encryption, license), SHA-256/512, HMAC.  This
// header is intentionally *narrow* — it exposes only what the platform
// itself uses.  Full crypto exports for plugins go through sdk/.
//
// Deep implementations delegate to `../security/crypto/`.  The public surface
// here stays stable regardless of which backend is linked.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sao_hash_algo_e : int32_t {
    SAO_HASH_SHA256 = 0,
    SAO_HASH_SHA512 = 1,
    SAO_HASH_SHA1   = 2,  // legacy compatibility only
};

// Fixed digest sizes: SHA-256=32, SHA-512=64, SHA-1=20.
enum : size_t {
    SAO_HASH_SHA256_BYTES = 32,
    SAO_HASH_SHA512_BYTES = 64,
    SAO_HASH_SHA1_BYTES   = 20,
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_hash(
    int32_t algo,
    const uint8_t* data,
    size_t data_len,
    uint8_t* out_digest,
    size_t digest_capacity);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_hmac(
    int32_t algo,
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t* out_mac,
    size_t mac_capacity);

// AES-256-GCM.  nonce is 12 bytes, tag is 16 bytes.  ciphertext buffer
// must be at least plaintext_len bytes.  aad may be null when aad_len=0.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_aes_gcm_encrypt(
    const uint8_t* key32,
    const uint8_t* nonce12,
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* plaintext,
    size_t plaintext_len,
    uint8_t* out_ciphertext,
    uint8_t out_tag16[16]);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_aes_gcm_decrypt(
    const uint8_t* key32,
    const uint8_t* nonce12,
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* ciphertext,
    size_t ciphertext_len,
    const uint8_t tag16[16],
    uint8_t* out_plaintext);

// Cryptographically secure random bytes (BCryptGenRandom under the hood).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_random_bytes(
    uint8_t* out_buffer, size_t byte_count);

// DPAPI vault helpers (CryptProtectData / CryptUnprotectData with
// CRYPTPROTECT_LOCAL_MACHINE — identical policy to the rt_io SAO3
// envelope so a machine-bound blob unwraps for any local principal,
// including LocalSystem services).  Two-pass sizing: call with
// out=nullptr/capacity=0 to query out_size.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_dpapi_protect(
    const uint8_t* plaintext, size_t plaintext_size,
    uint8_t* out, size_t capacity, size_t* out_size);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_dpapi_unprotect(
    const uint8_t* ciphertext, size_t ciphertext_size,
    uint8_t* out, size_t capacity, size_t* out_size);

// SAO3 wire-format data envelope.  Byte-identical layout to
// platform/rt_io/src/crypto/sao3_envelope.cpp:
//
//   +0..+3    magic       "SAO3"
//   +4..+7    version     little u32 (1)
//   +8..+11   dpapi_len   little u32
//   +12..     dpapi_blob  (fresh 32-byte AES key under DPAPI)
//   ...       nonce       12 bytes
//   ...       tag         16 bytes
//   ...       pt_size     little u32
//   ...       ct          AES-256-GCM ciphertext
//
// Unlike the rt_io driver variant this decoder accepts ANY plaintext;
// the driver path keeps its additional "MZ" prefix assertion because it
// only ever wraps PE images.  Used for on-disk configuration payloads
// (e.g. SaoAuto.provider.json) so they never sit as plaintext.
SAO_CORE_API bool SAO_CORE_CALL sao_core_sao3_is_envelope(
    const uint8_t* data, size_t data_size);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_sao3_envelope_encode(
    const uint8_t* plaintext, size_t plaintext_size,
    uint8_t* out, size_t out_capacity, size_t* out_size);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_sao3_envelope_decode(
    const uint8_t* envelope, size_t envelope_size,
    uint8_t* pt_out, size_t pt_capacity, size_t* bytes_written);

#ifdef __cplusplus
}  // extern "C"
#endif
