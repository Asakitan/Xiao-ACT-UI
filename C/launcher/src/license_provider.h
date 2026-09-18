#pragma once

// license_provider.h — license entitlement service for the launcher.
//
// sao_license_verify() (init_pipeline.h) is the one-shot gate at startup.
// This header provides the persistent entitlement surface: per-feature
// checks, a subscribe/unsubscribe surface so panels and the entity system
// see license transitions (EXPIRED / REVOKED / TIER_CHANGED) instead of a
// stale snapshot taken once at boot, and a composited provider status for
// UI.
//
// Headers this file depends on:
//   launcher/include/sao/launcher/init_pipeline.h — sao_status_t
//   license/sdk/include/sao/license/sdk/license_types.h — SAO_LICENSE_FEATURE_*
//
// Concurrency: entitlement callbacks run on the license client's worker
// thread; they must not block, own locks, or call back into the license
// surface.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Entitlement feature ids. Mirrors SAO_LICENSE_FEATURE_* — stable
// because sao_license_entitled_for_feature maps names, not numbers.
// Named *_id_t (not the SDK bitmask sao_license_feature_t in
// license_types.h) — ids are indexes, SDK features are bit positions.
typedef enum sao_license_feature_id_t {
    SAO_LICENSE_FEATURE_STREAMING_ID = 0,
    SAO_LICENSE_FEATURE_AI_CHAT_ID = 1,
    SAO_LICENSE_FEATURE_WORKSHOP_DOWNLOAD_ID = 2,
    SAO_LICENSE_FEATURE_KERNEL_MAP_ID = 3,
    SAO_LICENSE_FEATURE_COUNT_ID
} sao_license_feature_id_t;

// Entitlement-change callback. `new_mask` is a bitmask of the
// sao_license_feature_id_t ids currently entitled; `user_data` is the
// opaque value passed at subscribe time. Called on the license client's
// worker thread — must not block.
typedef void (*sao_license_entitlement_changed_fn)(
    uint32_t new_mask, void* user_data);

// Subscribe an entitlement-change listener. Returns 0 on success.
int32_t sao_license_subscribe_entitlement(
    sao_license_entitlement_changed_fn fn, void* user_data);

// Remove a previously-subscribed listener. Waits for any in-flight
// dispatch to finish before returning, so the caller may free the
// user_data after this call (mirrors the SDK event surface).
int32_t sao_license_unsubscribe_entitlement(
    sao_license_entitlement_changed_fn fn, void* user_data);

// Returns non-zero when the feature is currently entitled.
// Fail-closed — returns 0 when the provider is uninitialized, the tier
// cannot be read, or the feature id is out of range.
int32_t sao_license_entitled_for_feature(
    sao_license_feature_id_t feature);

// Snapshot of the license state used by UI panels. All numeric fields
// reset to 0 and `hwid_masked` to "" when the provider is not running.
typedef struct sao_license_provider_status {
    uint32_t struct_size;        /* caller sets sizeof(this struct) */
    int32_t initialized;         /* 1 once sao_license_provider_auto_start succeeded */
    int32_t tier;                /* sao_license_client_tier_t value (0..3) */
    int32_t activated;           /* 1 when a server token is installed */
    int64_t expiry_ms;           /* epoch-ms, 0 = no/unknown expiry */
    int32_t heartbeat_running;   /* 1 when the client heartbeat thread is up */
    int32_t revoked;             /* 1 when the server marked the token revoked */
    int32_t retry_failures;      /* consecutive renewal failures */
    int64_t retry_next_ms;       /* epoch-ms of next permitted heartbeat retry */
    char hwid_masked[48];        /* "first8...last8" — display only */
} sao_license_provider_status_t;

// Fill `*out` with the current provider status. Returns 0 on success.
int32_t sao_license_provider_status(sao_license_provider_status_t* out);

// Start the provider independently of the init pipeline gate. Installs
// the WinHTTP transport + config regardless of whether the configuration
// enabled the license system — activation itself always runs through
// sao_license_verify at startup — so the license panel can activate later
// even when the gate stayed inert. Safe to call more than once; the
// second call performs an sdk_refresh (remote verify) if the previous
// install succeeded.
//
// Returns 0 on success. Failure to install the provider surfaces
// sao_status-style negative error; callers that require licensing must
// still go through the init_pipeline fatal path.
int32_t sao_license_provider_auto_start(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
