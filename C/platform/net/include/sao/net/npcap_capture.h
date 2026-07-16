// SAO Auto — Npcap (wpcap.lib) capture wrapper.
//
// 1:1 rewrite of `sao_auto/python/plugins/star_resonance_plugin/net/
// packet_capture.py`.  The wrapper hides Npcap's C API behind a stable
// callback surface so upstream code doesn't include pcap.h.
//
// The capture driver decides its own remote endpoint discovery; the
// wrapper only owns the pcap handle, the BPF filter and the delivery
// thread.  Endpoint-lock logic lives one layer above in
// `packet_pipeline.h` (matching the Python `PacketBridge` design).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// ABI export macros — net/ ships as a SHARED library so Agent 4's
// plugins can dynamically link the packet types without pulling core in.
#if defined(_WIN32)
#  if defined(SAO_NET_BUILDING_DLL)
#    define SAO_NET_API __declspec(dllexport)
#  elif defined(SAO_NET_USING_DLL)
#    define SAO_NET_API __declspec(dllimport)
#  else
#    define SAO_NET_API
#  endif
#else
#  define SAO_NET_API
#endif
#define SAO_NET_CALL __cdecl

#define SAO_NET_ABI_VERSION_MAJOR 1u
#define SAO_NET_ABI_VERSION_MINOR 0u
#define SAO_NET_ABI_VERSION \
    ((SAO_NET_ABI_VERSION_MAJOR << 16) | SAO_NET_ABI_VERSION_MINOR)

SAO_NET_API uint32_t SAO_NET_CALL sao_net_abi_version(void);

typedef struct sao_net_capture_s* sao_net_capture_handle_t;

struct SaoCaptureInterface {
    char        name_utf8[256];        // Npcap NPF device name (\\Device\\NPF_{GUID})
    char        description_utf8[256]; // Human-readable
    uint32_t    ipv4_count;
    uint32_t    _pad;
};

// Enumerate the local capture devices — thin wrapper over pcap_findalldevs.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_capture_enum_interfaces(
    SaoCaptureInterface* out_interfaces,
    size_t max_interfaces,
    size_t* out_interface_count);

// Delivered from the capture worker thread — bpf-filtered packets in
// wire order.  ts_unix_ns is the arrival time snapshot.  Do not free
// bytes; validity ends when the callback returns.
typedef void (SAO_NET_CALL* sao_net_packet_callback_t)(
    const uint8_t* bytes,
    size_t length,
    uint64_t ts_unix_ns,
    void* user_data);

// bpf_utf8 is the Berkeley Packet Filter expression, e.g.
// "tcp and (port 12345 or port 54321)".  0 selects a sane default.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_capture_open(
    const char* interface_name_utf8,
    const char* bpf_utf8,
    uint32_t snap_len,
    uint32_t read_timeout_ms,
    sao_net_capture_handle_t* out_handle);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_capture_start(
    sao_net_capture_handle_t handle,
    sao_net_packet_callback_t callback,
    void* user_data);

SAO_NET_API sao_status_t SAO_NET_CALL sao_net_capture_stop(
    sao_net_capture_handle_t handle);

SAO_NET_API void SAO_NET_CALL sao_net_capture_close(
    sao_net_capture_handle_t handle);

// ───────────────────────────────────────────────────────────────────────
// Wave 6 — low-level Npcap wrapper (game-agnostic).
//
// The signatures above expose a callback-driven "delivery thread" surface
// on top of Npcap; the Wave 6 API sits underneath: it exposes the pcap
// primitives themselves (probe / list / open / filter / next / dispatch /
// close) so higher layers can drive their own pump.  All Wave 6 calls
// route through `GetProcAddress("wpcap.dll", …)` — there is no
// wpcap.lib link dependency, so builds without the Npcap SDK still
// compile and the runtime gracefully reports `SAO_STATUS_ERR_NOT_FOUND`
// when Npcap is not installed.
// ───────────────────────────────────────────────────────────────────────

// Opaque handle for a live pcap capture.  Owns the pcap_t*.
typedef struct sao_net_npcap_s* sao_net_npcap_handle_t;

// Compact device row for `sao_net_npcap_list_devices`.
struct SaoNpcapDevice {
    char name_utf8[256];         // \Device\NPF_{GUID}
    char description_utf8[256];  // human-readable, may be empty
};

// Returns whether Npcap (or a compatible WinPcap) is loadable.  Never
// fails — sets `*available_out` and returns SAO_STATUS_OK; the caller
// should probe the boolean, not the status.  Version is queried via
// `pcap_lib_version`; a returned string that clearly identifies Npcap
// >= 1.0 or libpcap >= 1.0 sets `*available_out = true`.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_npcap_available(
    bool* available_out);

// Enumerate all NPF devices via `pcap_findalldevs`.  `devices_out` may be
// NULL, in which case only `*count_out` is set (probe pattern).
// Returns SAO_STATUS_ERR_NOT_FOUND if Npcap is not available.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_npcap_list_devices(
    SaoNpcapDevice* devices_out,
    size_t capacity,
    size_t* count_out);

// Open a live capture on `device_name` (must be an NPF name).  The
// wrapper always requests PCAP_OPENFLAG_PROMISCUOUS.  `snaplen` of 0
// means "use 65535" (a sane default that captures full frames).
// `timeout_ms` of 0 falls back to 1 ms so `pcap_next_ex` behaves as a
// short-poll instead of blocking indefinitely.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_npcap_open_live(
    const char* device_name,
    uint32_t snaplen,
    uint32_t timeout_ms,
    sao_net_npcap_handle_t* handle_out);

// Install a BPF filter (e.g. `"tcp port 8888"`).  Passing NULL clears
// the filter.  Compile errors are reported via SAO_STATUS_ERR_INVALID_ARGUMENT.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_npcap_set_filter(
    sao_net_npcap_handle_t handle,
    const char* filter_expr);

// Non-blocking read of the next captured packet.  Returns:
//   SAO_STATUS_OK               — packet delivered, sizes set, ts_ms
//                                 is the pcap epoch (ms since UNIX).
//   SAO_STATUS_ERR_TIMEOUT      — timer elapsed with no packet
//                                 (caller re-polls).
//   SAO_STATUS_ERR_BUFFER_TOO_SMALL — `buf_capacity` short; `*size_out`
//                                     holds the required size and the
//                                     packet is discarded.
//   SAO_STATUS_ERR_OS_CALL_FAILED   — pcap error; handle should be
//                                     closed.
// A NULL `buf_out` with `buf_capacity == 0` is a valid "peek size"
// request when combined with the next call — pcap does not support
// peek, so this is not implemented; callers should size their buffer
// to snaplen up-front.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_npcap_next_packet(
    sao_net_npcap_handle_t handle,
    uint8_t* buf_out,
    size_t buf_capacity,
    uint64_t* ts_ms_out,
    size_t* size_out);

// pcap-level dispatch callback (thin wrapper on `pcap_handler`).
typedef void (SAO_NET_CALL* sao_net_npcap_callback_t)(
    void* user_data,
    const uint8_t* bytes,
    size_t length,
    uint64_t ts_ms);

// Pump up to `max_count` packets through `callback` (0 = pcap default).
// Returns the count of delivered packets in `*delivered_out`.  Wraps
// `pcap_dispatch`.
SAO_NET_API sao_status_t SAO_NET_CALL sao_net_npcap_dispatch(
    sao_net_npcap_handle_t handle,
    int32_t max_count,
    sao_net_npcap_callback_t callback,
    void* user_data,
    int32_t* delivered_out);

// Close the pcap handle.  Safe to pass NULL.
SAO_NET_API void SAO_NET_CALL sao_net_npcap_close(
    sao_net_npcap_handle_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
