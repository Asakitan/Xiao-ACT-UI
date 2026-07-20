// SAO Auto — Npcap (wpcap.dll) capture wrapper.
//
// Implements the low-level primitive surface declared in
// `npcap_capture.h`.  wpcap.dll is loaded lazily via GetProcAddress; the
// build does NOT depend on wpcap.lib, so machines without the Npcap SDK
// can still link and machines without Npcap installed can still start
// (they gracefully receive SAO_STATUS_ERR_NOT_FOUND).

#include "sao/net/npcap_capture.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// ───────────────────────────────────────────────────────────────────────
// Minimal pcap type surface — copied only to the extent we call.
// (We deliberately avoid #include <pcap.h> so builds without the Npcap
// SDK still compile.)
// ───────────────────────────────────────────────────────────────────────
namespace sao_npcap_ffi {

// Opaque pcap_t / bpf_program forwarders.
struct pcap_t;

// pcap_pkthdr layout is stable across libpcap versions (documented API).
struct pcap_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
};
struct pcap_pkthdr_t {
    pcap_timeval32 ts;
    uint32_t caplen;
    uint32_t len;
};

// pcap_if_t — we only touch the first four members (all versions).
struct pcap_if_t {
    pcap_if_t* next;
    const char* name;
    const char* description;
    void* addresses;
    uint32_t flags;
};

// bpf_program blob — libpcap keeps two fields we only pass by pointer.
struct bpf_program_t {
    uint32_t bf_len;
    void* bf_insns;
};

using pcap_lib_version_fn = const char* (*)(void);
using pcap_findalldevs_fn = int (*)(pcap_if_t**, char*);
using pcap_freealldevs_fn = void (*)(pcap_if_t*);
using pcap_open_live_fn = pcap_t* (*)(const char*, int, int, int, char*);
using pcap_close_fn = void (*)(pcap_t*);
using pcap_compile_fn = int (*)(pcap_t*, bpf_program_t*, const char*, int, uint32_t);
using pcap_setfilter_fn = int (*)(pcap_t*, bpf_program_t*);
using pcap_freecode_fn = void (*)(bpf_program_t*);
using pcap_next_ex_fn = int (*)(pcap_t*, pcap_pkthdr_t**, const uint8_t**);
using pcap_dispatch_handler = void (*)(uint8_t*, const pcap_pkthdr_t*, const uint8_t*);
using pcap_dispatch_fn = int (*)(pcap_t*, int, pcap_dispatch_handler, uint8_t*);
using pcap_geterr_fn = char* (*)(pcap_t*);

struct WpcapVtable {
    HMODULE dll = nullptr;
    pcap_lib_version_fn lib_version = nullptr;
    pcap_findalldevs_fn findalldevs = nullptr;
    pcap_freealldevs_fn freealldevs = nullptr;
    pcap_open_live_fn open_live = nullptr;
    pcap_close_fn close = nullptr;
    pcap_compile_fn compile = nullptr;
    pcap_setfilter_fn setfilter = nullptr;
    pcap_freecode_fn freecode = nullptr;
    pcap_next_ex_fn next_ex = nullptr;
    pcap_dispatch_fn dispatch = nullptr;
    pcap_geterr_fn geterr = nullptr;
    bool loaded_ok = false;
};

static std::once_flag g_wpcap_once;
static WpcapVtable g_wpcap{};

static void load_wpcap_once() {
#if defined(_WIN32)
    // Prefer %SystemRoot%\System32\Npcap\wpcap.dll (Npcap install path)
    // over a bare LoadLibrary — Npcap installs its DLLs under a
    // subdirectory to avoid colliding with WinPcap installs.
    HMODULE dll = nullptr;
    char sysroot[MAX_PATH];
    UINT n = GetSystemDirectoryA(sysroot, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string path(sysroot, n);
        path.append("\\Npcap\\wpcap.dll");
        dll = LoadLibraryA(path.c_str());
    }
    if (dll == nullptr) {
        dll = LoadLibraryA("wpcap.dll");
    }
    if (dll == nullptr) {
        g_wpcap.loaded_ok = false;
        return;
    }
    g_wpcap.dll = dll;
    g_wpcap.lib_version =
        reinterpret_cast<pcap_lib_version_fn>(GetProcAddress(dll, "pcap_lib_version"));
    g_wpcap.findalldevs =
        reinterpret_cast<pcap_findalldevs_fn>(GetProcAddress(dll, "pcap_findalldevs"));
    g_wpcap.freealldevs =
        reinterpret_cast<pcap_freealldevs_fn>(GetProcAddress(dll, "pcap_freealldevs"));
    g_wpcap.open_live =
        reinterpret_cast<pcap_open_live_fn>(GetProcAddress(dll, "pcap_open_live"));
    g_wpcap.close =
        reinterpret_cast<pcap_close_fn>(GetProcAddress(dll, "pcap_close"));
    g_wpcap.compile =
        reinterpret_cast<pcap_compile_fn>(GetProcAddress(dll, "pcap_compile"));
    g_wpcap.setfilter =
        reinterpret_cast<pcap_setfilter_fn>(GetProcAddress(dll, "pcap_setfilter"));
    g_wpcap.freecode =
        reinterpret_cast<pcap_freecode_fn>(GetProcAddress(dll, "pcap_freecode"));
    g_wpcap.next_ex =
        reinterpret_cast<pcap_next_ex_fn>(GetProcAddress(dll, "pcap_next_ex"));
    g_wpcap.dispatch =
        reinterpret_cast<pcap_dispatch_fn>(GetProcAddress(dll, "pcap_dispatch"));
    g_wpcap.geterr =
        reinterpret_cast<pcap_geterr_fn>(GetProcAddress(dll, "pcap_geterr"));

    // Minimum viable surface — everything except dispatch/freecode is
    // considered mandatory.
    g_wpcap.loaded_ok = g_wpcap.lib_version != nullptr &&
                        g_wpcap.findalldevs != nullptr &&
                        g_wpcap.freealldevs != nullptr &&
                        g_wpcap.open_live != nullptr &&
                        g_wpcap.close != nullptr &&
                        g_wpcap.compile != nullptr &&
                        g_wpcap.setfilter != nullptr &&
                        g_wpcap.next_ex != nullptr;
#else
    g_wpcap.loaded_ok = false;
#endif
}

static const WpcapVtable& wpcap() {
    std::call_once(g_wpcap_once, load_wpcap_once);
    return g_wpcap;
}

// Minimal version sanity check.  `pcap_lib_version` returns something
// like "Npcap version 1.79" or "libpcap version 1.10.4"; we require
// major >= 1.
static bool version_at_least_one(const char* v) {
    if (v == nullptr) return false;
    // Walk to first digit.
    const char* p = v;
    while (*p && (*p < '0' || *p > '9')) ++p;
    if (*p == 0) return false;
    // Parse leading integer.
    int major = 0;
    while (*p >= '0' && *p <= '9') {
        major = major * 10 + (*p - '0');
        ++p;
    }
    return major >= 1;
}

}  // namespace sao_npcap_ffi

// ───────────────────────────────────────────────────────────────────────
// Handle bookkeeping.
// ───────────────────────────────────────────────────────────────────────
struct sao_net_npcap_s {
    sao_npcap_ffi::pcap_t* pcap = nullptr;
    sao_npcap_ffi::bpf_program_t bpf{};
    bool bpf_active = false;
};

struct sao_net_capture_s {
    sao_net_npcap_handle_t npcap = nullptr;
    uint32_t snap_len = 65535;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::thread worker;
    sao_net_packet_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
};

// ───────────────────────────────────────────────────────────────────────
// Low-level Npcap primitive surface.
// ───────────────────────────────────────────────────────────────────────

extern "C" uint32_t SAO_NET_CALL sao_net_abi_version(void) {
    return SAO_NET_ABI_VERSION;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_npcap_available(
    bool* available_out) {
    if (available_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *available_out = false;
    const auto& v = sao_npcap_ffi::wpcap();
    if (!v.loaded_ok) return SAO_STATUS_OK;
    const char* ver = v.lib_version();
    *available_out = sao_npcap_ffi::version_at_least_one(ver);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_npcap_list_devices(
    SaoNpcapDevice* devices_out, size_t capacity, size_t* count_out) {
    if (count_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *count_out = 0;
    const auto& v = sao_npcap_ffi::wpcap();
    if (!v.loaded_ok) return SAO_STATUS_ERR_NOT_FOUND;

    sao_npcap_ffi::pcap_if_t* alldevs = nullptr;
    char errbuf[256] = {0};
    int rc = v.findalldevs(&alldevs, errbuf);
    if (rc != 0 || alldevs == nullptr) {
        if (alldevs != nullptr) v.freealldevs(alldevs);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    size_t written = 0;
    size_t seen = 0;
    for (auto* cur = alldevs; cur != nullptr; cur = cur->next) {
        ++seen;
        if (devices_out != nullptr && written < capacity) {
            auto& row = devices_out[written];
            std::memset(&row, 0, sizeof(row));
            if (cur->name != nullptr) {
                std::strncpy(row.name_utf8, cur->name,
                             sizeof(row.name_utf8) - 1);
            }
            if (cur->description != nullptr) {
                std::strncpy(row.description_utf8, cur->description,
                             sizeof(row.description_utf8) - 1);
            }
            ++written;
        }
    }
    v.freealldevs(alldevs);

    *count_out = seen;
    if (devices_out != nullptr && written < seen) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_npcap_open_live(
    const char* device_name, uint32_t snaplen, uint32_t timeout_ms,
    sao_net_npcap_handle_t* handle_out) {
    if (handle_out == nullptr || device_name == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *handle_out = nullptr;
    const auto& v = sao_npcap_ffi::wpcap();
    if (!v.loaded_ok) return SAO_STATUS_ERR_NOT_FOUND;

    // PCAP_OPENFLAG_PROMISCUOUS == 1 in libpcap.
    const int promisc = 1;
    if (snaplen == 0) snaplen = 65535;
    if (timeout_ms == 0) timeout_ms = 1;

    char errbuf[256] = {0};
    auto* pcap = v.open_live(device_name, static_cast<int>(snaplen), promisc,
                             static_cast<int>(timeout_ms), errbuf);
    if (pcap == nullptr) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    auto* h = new sao_net_npcap_s();
    h->pcap = pcap;
    *handle_out = h;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_npcap_set_filter(
    sao_net_npcap_handle_t handle, const char* filter_expr) {
    if (handle == nullptr || handle->pcap == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    const auto& v = sao_npcap_ffi::wpcap();
    if (!v.loaded_ok) return SAO_STATUS_ERR_NOT_FOUND;

    if (handle->bpf_active) {
        if (v.freecode != nullptr) v.freecode(&handle->bpf);
        handle->bpf_active = false;
        std::memset(&handle->bpf, 0, sizeof(handle->bpf));
    }

    if (filter_expr == nullptr || filter_expr[0] == 0) {
        return SAO_STATUS_OK;  // cleared
    }

    // netmask == PCAP_NETMASK_UNKNOWN (0xffffffff) is fine for BPF
    // without broadcast-address awareness.
    int rc = v.compile(handle->pcap, &handle->bpf, filter_expr, 1, 0xffffffffu);
    if (rc != 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    rc = v.setfilter(handle->pcap, &handle->bpf);
    if (rc != 0) {
        if (v.freecode != nullptr) v.freecode(&handle->bpf);
        std::memset(&handle->bpf, 0, sizeof(handle->bpf));
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    handle->bpf_active = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_npcap_next_packet(
    sao_net_npcap_handle_t handle, uint8_t* buf_out, size_t buf_capacity,
    uint64_t* ts_ms_out, size_t* size_out) {
    if (handle == nullptr || handle->pcap == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (size_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *size_out = 0;
    if (ts_ms_out != nullptr) *ts_ms_out = 0;

    const auto& v = sao_npcap_ffi::wpcap();
    if (!v.loaded_ok) return SAO_STATUS_ERR_NOT_FOUND;

    sao_npcap_ffi::pcap_pkthdr_t* hdr = nullptr;
    const uint8_t* data = nullptr;
    int rc = v.next_ex(handle->pcap, &hdr, &data);
    if (rc == 0) return SAO_STATUS_ERR_TIMEOUT;
    if (rc == 1) {
        if (hdr == nullptr || data == nullptr) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        uint32_t caplen = hdr->caplen;
        *size_out = caplen;
        if (ts_ms_out != nullptr) {
            uint64_t sec = static_cast<uint64_t>(
                static_cast<uint32_t>(hdr->ts.tv_sec));
            uint64_t usec = static_cast<uint64_t>(
                static_cast<uint32_t>(hdr->ts.tv_usec));
            *ts_ms_out = sec * 1000ull + usec / 1000ull;
        }
        if (buf_out == nullptr || buf_capacity < caplen) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(buf_out, data, caplen);
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

// Trampoline that bridges pcap's C callback to ours (types differ only
// in constness of the packet pointer).
struct DispatchCtx {
    sao_net_npcap_callback_t user_cb;
    void* user_data;
};

static void SAO_NET_CALL sao_npcap_dispatch_trampoline(
    uint8_t* userraw,
    const sao_npcap_ffi::pcap_pkthdr_t* hdr,
    const uint8_t* data) {
    if (userraw == nullptr || hdr == nullptr || data == nullptr) return;
    auto* ctx = reinterpret_cast<DispatchCtx*>(userraw);
    if (ctx->user_cb == nullptr) return;
    uint64_t sec = static_cast<uint64_t>(
        static_cast<uint32_t>(hdr->ts.tv_sec));
    uint64_t usec = static_cast<uint64_t>(
        static_cast<uint32_t>(hdr->ts.tv_usec));
    uint64_t ts_ms = sec * 1000ull + usec / 1000ull;
    ctx->user_cb(ctx->user_data, data, hdr->caplen, ts_ms);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_npcap_dispatch(
    sao_net_npcap_handle_t handle, int32_t max_count,
    sao_net_npcap_callback_t callback, void* user_data,
    int32_t* delivered_out) {
    if (delivered_out != nullptr) *delivered_out = 0;
    if (handle == nullptr || handle->pcap == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto& v = sao_npcap_ffi::wpcap();
    if (!v.loaded_ok) return SAO_STATUS_ERR_NOT_FOUND;
    if (v.dispatch == nullptr) return SAO_STATUS_ERR_NOT_IMPLEMENTED;

    DispatchCtx ctx{callback, user_data};
    int rc = v.dispatch(handle->pcap, static_cast<int>(max_count),
                        sao_npcap_dispatch_trampoline,
                        reinterpret_cast<uint8_t*>(&ctx));
    if (rc < 0) return SAO_STATUS_ERR_OS_CALL_FAILED;
    if (delivered_out != nullptr) *delivered_out = rc;
    return SAO_STATUS_OK;
}

extern "C" void SAO_NET_CALL sao_net_npcap_close(
    sao_net_npcap_handle_t handle) {
    if (handle == nullptr) return;
    const auto& v = sao_npcap_ffi::wpcap();
    if (v.loaded_ok) {
        if (handle->bpf_active && v.freecode != nullptr) {
            v.freecode(&handle->bpf);
        }
        if (handle->pcap != nullptr && v.close != nullptr) {
            v.close(handle->pcap);
        }
    }
    delete handle;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_enum_interfaces(
    SaoCaptureInterface* out_interfaces, size_t max_interfaces,
    size_t* out_count) {
    if (out_count == nullptr || (out_interfaces == nullptr && max_interfaces != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    size_t count = 0;
    auto status = sao_net_npcap_list_devices(nullptr, 0, &count);
    if (status != SAO_STATUS_OK) return status;
    *out_count = count;
    if (out_interfaces == nullptr) return SAO_STATUS_OK;
    if (max_interfaces < count) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;

    try {
        std::vector<SaoNpcapDevice> devices(count);
        status = sao_net_npcap_list_devices(devices.data(), devices.size(), &count);
        if (status != SAO_STATUS_OK) return status;
        for (size_t index = 0; index < count; ++index) {
            std::memset(&out_interfaces[index], 0, sizeof(out_interfaces[index]));
            std::strncpy(out_interfaces[index].name_utf8, devices[index].name_utf8,
                         sizeof(out_interfaces[index].name_utf8) - 1);
            std::strncpy(out_interfaces[index].description_utf8,
                         devices[index].description_utf8,
                         sizeof(out_interfaces[index].description_utf8) - 1);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        std::memset(out_interfaces, 0, max_interfaces * sizeof(*out_interfaces));
        *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_open(
    const char* interface_name_utf8, const char* bpf_utf8,
    uint32_t snap_len, uint32_t read_timeout_ms,
    sao_net_capture_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (interface_name_utf8 == nullptr || interface_name_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const uint32_t effective_snap_len = snap_len == 0 ? 65535 : snap_len;
    sao_net_npcap_handle_t npcap = nullptr;
    auto status = sao_net_npcap_open_live(interface_name_utf8, effective_snap_len,
                                           read_timeout_ms, &npcap);
    if (status != SAO_STATUS_OK) return status;
    status = sao_net_npcap_set_filter(npcap,
                                      bpf_utf8 == nullptr ? "tcp" : bpf_utf8);
    if (status != SAO_STATUS_OK) {
        sao_net_npcap_close(npcap);
        return status;
    }
    try {
        auto* capture = new sao_net_capture_s();
        capture->npcap = npcap;
        capture->snap_len = effective_snap_len;
        *out_handle = capture;
        return SAO_STATUS_OK;
    } catch (...) {
        sao_net_npcap_close(npcap);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_start(
    sao_net_capture_handle_t handle, sao_net_packet_callback_t callback,
    void* user_data) {
    if (handle == nullptr || handle->npcap == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (handle->running.exchange(true, std::memory_order_acq_rel)) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    handle->stop_requested.store(false, std::memory_order_release);
    handle->callback = callback;
    handle->callback_user_data = user_data;
    try {
        handle->worker = std::thread([handle] {
            std::vector<uint8_t> packet(handle->snap_len);
            while (!handle->stop_requested.load(std::memory_order_acquire)) {
                uint64_t timestamp_ms = 0;
                size_t packet_size = 0;
                const auto status = sao_net_npcap_next_packet(
                    handle->npcap, packet.data(), packet.size(),
                    &timestamp_ms, &packet_size);
                if (status == SAO_STATUS_ERR_TIMEOUT) continue;
                if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
                    try {
                        packet.resize(packet_size);
                    } catch (...) {
                        break;
                    }
                    continue;
                }
                if (status != SAO_STATUS_OK) break;
                try {
                    handle->callback(packet.data(), packet_size,
                                     timestamp_ms * 1'000'000ull,
                                     handle->callback_user_data);
                } catch (...) {
                    break;
                }
            }
            handle->running.store(false, std::memory_order_release);
        });
        return SAO_STATUS_OK;
    } catch (...) {
        handle->callback = nullptr;
        handle->callback_user_data = nullptr;
        handle->running.store(false, std::memory_order_release);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_stop(
    sao_net_capture_handle_t handle) {
    if (handle == nullptr || handle->npcap == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    handle->stop_requested.store(true, std::memory_order_release);
    if (handle->worker.joinable()) {
        if (handle->worker.get_id() == std::this_thread::get_id()) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        handle->worker.join();
    }
    handle->running.store(false, std::memory_order_release);
    handle->callback = nullptr;
    handle->callback_user_data = nullptr;
    return SAO_STATUS_OK;
}

extern "C" void SAO_NET_CALL sao_net_capture_close(
    sao_net_capture_handle_t handle) {
    if (handle == nullptr) return;
    (void)sao_net_capture_stop(handle);
    if (handle->worker.joinable() &&
        handle->worker.get_id() != std::this_thread::get_id()) {
        handle->worker.join();
    }
    sao_net_npcap_close(handle->npcap);
    handle->npcap = nullptr;
    delete handle;
}
