// SAO Auto — TLS façade.
//
// The current implementation delegates to WinHTTP for HTTPS/WSS.  This
// header exists so a future replacement (mbedTLS, OpenSSL) can slot in
// without ABI churn.  Only the entry points the launcher needs are
// exposed.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/net/npcap_capture.h"  // SAO_NET_API macro

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_net_tls_pin_set_s* sao_net_tls_pin_set_handle_t;

#define SAO_NET_TLS_SCOPE_LICENSE "license"
#define SAO_NET_TLS_SCOPE_UPDATE "update"
#define SAO_NET_TLS_SCOPE_WORKSHOP "workshop"
#define SAO_NET_TLS_SCOPE_CLOUD "cloud"
#define SAO_NET_TLS_SCOPE_DEFAULT "default"

// Pin the accepted server SPKI hashes for the License channel so a MITM
// (corp proxy, malicious wifi) can't hand us a fresh RootCA.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_tls_pin_set_create(
    sao_net_tls_pin_set_handle_t* out_handle);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_tls_pin_set_add_sha256(
    sao_net_tls_pin_set_handle_t handle,
    const uint8_t digest[32]);

SAO_NET_API void SAO_NET_CALL sao_net_tls_pin_set_destroy(
    sao_net_tls_pin_set_handle_t handle);

// Install a process-global pin set for all subsequent HTTPS/WSS calls
// on the "license" scope.  Other scopes (e.g. plugin manifests) are
// pinned separately.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_tls_install_pin_set(
    const char* scope_utf8,
    sao_net_tls_pin_set_handle_t handle);

// Extract SHA-256(SubjectPublicKeyInfo) from a DER encoded X.509 certificate.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_tls_certificate_spki_sha256(
    const uint8_t* certificate_der,
    size_t certificate_der_length,
    uint8_t digest_out[32]);

// Validate a DER encoded certificate against a caller-owned pin set.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_tls_validate_certificate(
    sao_net_tls_pin_set_handle_t handle,
    const uint8_t* certificate_der,
    size_t certificate_der_length);

#ifdef __cplusplus
}  // extern "C"
#endif
