// SAO Auto — Npcap (wpcap.dll) capture wrapper.
//
// Implements the low-level primitive surface declared in
// `npcap_capture.h`.  wpcap.dll is loaded lazily via GetProcAddress; the
// build does NOT depend on wpcap.lib, so machines without the Npcap SDK
// can still link and machines without Npcap installed can still start
// (they gracefully receive SAO_STATUS_ERR_NOT_FOUND).

#include "sao/net/npcap_capture.h"

#include "sao_security/obfuscation/enc_str.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
    //
    // The DLL name and every exported symbol name are wrapped in
    // SAO_ENC_STR() so the .rdata segment does not advertise the wpcap
    // surface to static string scanners.  The lookup semantics are
    // unchanged.
    const auto enc_subdir = SAO_ENC_STR("\\Npcap\\wpcap.dll");
    const auto enc_bare   = SAO_ENC_STR("wpcap.dll");
    HMODULE dll = nullptr;
    char sysroot[MAX_PATH];
    UINT n = GetSystemDirectoryA(sysroot, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string path(sysroot, n);
        path.append(enc_subdir.decrypt());
        dll = LoadLibraryA(path.c_str());
    }
    if (dll == nullptr) {
        dll = LoadLibraryA(enc_bare.decrypt());
    }
    if (dll == nullptr) {
        g_wpcap.loaded_ok = false;
        return;
    }
    g_wpcap.dll = dll;

    const auto sym_lib_version = SAO_ENC_STR("pcap_lib_version");
    const auto sym_findalldevs = SAO_ENC_STR("pcap_findalldevs");
    const auto sym_freealldevs = SAO_ENC_STR("pcap_freealldevs");
    const auto sym_open_live   = SAO_ENC_STR("pcap_open_live");
    const auto sym_close       = SAO_ENC_STR("pcap_close");
    const auto sym_compile     = SAO_ENC_STR("pcap_compile");
    const auto sym_setfilter   = SAO_ENC_STR("pcap_setfilter");
    const auto sym_freecode    = SAO_ENC_STR("pcap_freecode");
    const auto sym_next_ex     = SAO_ENC_STR("pcap_next_ex");
    const auto sym_dispatch    = SAO_ENC_STR("pcap_dispatch");
    const auto sym_geterr      = SAO_ENC_STR("pcap_geterr");

    g_wpcap.lib_version =
        reinterpret_cast<pcap_lib_version_fn>(GetProcAddress(dll, sym_lib_version.decrypt()));
    g_wpcap.findalldevs =
        reinterpret_cast<pcap_findalldevs_fn>(GetProcAddress(dll, sym_findalldevs.decrypt()));
    g_wpcap.freealldevs =
        reinterpret_cast<pcap_freealldevs_fn>(GetProcAddress(dll, sym_freealldevs.decrypt()));
    g_wpcap.open_live =
        reinterpret_cast<pcap_open_live_fn>(GetProcAddress(dll, sym_open_live.decrypt()));
    g_wpcap.close =
        reinterpret_cast<pcap_close_fn>(GetProcAddress(dll, sym_close.decrypt()));
    g_wpcap.compile =
        reinterpret_cast<pcap_compile_fn>(GetProcAddress(dll, sym_compile.decrypt()));
    g_wpcap.setfilter =
        reinterpret_cast<pcap_setfilter_fn>(GetProcAddress(dll, sym_setfilter.decrypt()));
    g_wpcap.freecode =
        reinterpret_cast<pcap_freecode_fn>(GetProcAddress(dll, sym_freecode.decrypt()));
    g_wpcap.next_ex =
        reinterpret_cast<pcap_next_ex_fn>(GetProcAddress(dll, sym_next_ex.decrypt()));
    g_wpcap.dispatch =
        reinterpret_cast<pcap_dispatch_fn>(GetProcAddress(dll, sym_dispatch.decrypt()));
    g_wpcap.geterr =
        reinterpret_cast<pcap_geterr_fn>(GetProcAddress(dll, sym_geterr.decrypt()));

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
    struct TestPacket {
        std::vector<uint8_t> bytes;
        uint64_t ts_ms = 0;
    };

    std::mutex mutex;
    std::condition_variable condition;
    sao_net_npcap_handle_t npcap = nullptr;
    uint32_t snap_len = 65535;
    bool stop_requested = false;
    bool running = false;
    std::thread worker;
    std::thread::id worker_id{};
    sao_net_packet_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
    uint32_t lifetime_refs = 1;
    uint32_t active_callbacks = 0;
    bool admission_open = false;
    bool owner_released = false;
    bool close_requested = false;
    bool worker_finished = true;
    bool worker_exit_complete = true;
    bool worker_attached = false;
    bool cleanup_done = false;
    bool join_in_progress = false;
    bool test_backend = false;
    bool test_read_error = false;
    std::deque<TestPacket> test_packets;
};

namespace {

constexpr sao_status_t k_capture_busy_status = SAO_NET_STATUS_BUSY;

void release_capture_ref(sao_net_capture_s* handle);

bool retain_capture_ref(sao_net_capture_s* handle) {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> guard(handle->mutex);
    if (handle->lifetime_refs == 0) return false;
    ++handle->lifetime_refs;
    return true;
}

void close_capture_pcap(sao_net_capture_s* handle) {
    sao_net_npcap_handle_t npcap = nullptr;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (!handle->close_requested || !handle->worker_finished ||
            handle->cleanup_done) {
            return;
        }
        handle->cleanup_done = true;
        npcap = handle->npcap;
        handle->npcap = nullptr;
    }
    if (npcap != nullptr) sao_net_npcap_close(npcap);
}

void release_capture_ref(sao_net_capture_s* handle) {
    if (handle == nullptr) return;
    bool destroy = false;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->lifetime_refs == 0) return;
        --handle->lifetime_refs;
        destroy = handle->lifetime_refs == 0 && handle->owner_released &&
                  handle->worker_finished && handle->worker_exit_complete &&
                  handle->cleanup_done;
    }
    if (destroy) delete handle;
}

class CaptureOperationLease {
public:
    explicit CaptureOperationLease(sao_net_capture_s* handle)
        : handle_(retain_capture_ref(handle) ? handle : nullptr) {}
    ~CaptureOperationLease() { release_capture_ref(handle_); }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
private:
    sao_net_capture_s* handle_ = nullptr;
};

void finish_capture_worker(sao_net_capture_s* handle) {
    sao_net_npcap_handle_t npcap = nullptr;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        handle->running = false;
        handle->admission_open = false;
        handle->callback = nullptr;
        handle->callback_user_data = nullptr;
        handle->worker_finished = true;
        if (handle->close_requested && !handle->cleanup_done) {
            handle->cleanup_done = true;
            npcap = handle->npcap;
            handle->npcap = nullptr;
        }
        if (handle->close_requested && !handle->join_in_progress &&
            handle->worker.joinable()) {
            handle->worker.detach();
        }
        handle->condition.notify_all();
    }
    if (npcap != nullptr) sao_net_npcap_close(npcap);
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        handle->worker_exit_complete = true;
        handle->condition.notify_all();
    }
    release_capture_ref(handle);
}

bool enter_capture_callback(sao_net_capture_s* handle,
                            sao_net_packet_callback_t* callback_out,
                            void** user_data_out) {
    std::lock_guard<std::mutex> guard(handle->mutex);
    if (!handle->admission_open || handle->stop_requested ||
        handle->callback == nullptr) {
        return false;
    }
    ++handle->active_callbacks;
    ++handle->lifetime_refs;
    *callback_out = handle->callback;
    *user_data_out = handle->callback_user_data;
    return true;
}

void leave_capture_callback(sao_net_capture_s* handle) {
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->active_callbacks != 0) --handle->active_callbacks;
        handle->condition.notify_all();
    }
    release_capture_ref(handle);
}

void invoke_capture_callback(sao_net_capture_s* handle,
                             const uint8_t* bytes, size_t length,
                             uint64_t timestamp_ms) {
    sao_net_packet_callback_t callback = nullptr;
    void* user_data = nullptr;
    if (!enter_capture_callback(handle, &callback, &user_data)) return;
    try {
        callback(bytes, length, timestamp_ms * 1'000'000ull, user_data);
    } catch (...) {
    }
    leave_capture_callback(handle);
}

bool capture_stop_requested(sao_net_capture_s* handle) {
    std::lock_guard<std::mutex> guard(handle->mutex);
    return handle->stop_requested;
}

sao_status_t stop_capture_worker(sao_net_capture_s* handle) {
    if (!retain_capture_ref(handle)) return SAO_STATUS_ERR_HANDLE_INVALID;
    bool caller_is_worker = false;
    bool should_join = false;
    bool busy = false;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        handle->stop_requested = true;
        handle->admission_open = false;
        handle->callback = nullptr;
        handle->callback_user_data = nullptr;
        handle->condition.notify_all();
        caller_is_worker = handle->worker_id == std::this_thread::get_id() &&
                           handle->worker.joinable();
        if (!caller_is_worker && handle->worker.joinable()) {
            if (handle->join_in_progress) {
                busy = true;
            } else {
                handle->join_in_progress = true;
                should_join = true;
            }
        } else if (!caller_is_worker && !handle->worker_exit_complete) {
            busy = true;
        }
    }
    if (busy) {
        release_capture_ref(handle);
        return k_capture_busy_status;
    }
    if (caller_is_worker) {
        release_capture_ref(handle);
        return k_capture_busy_status;
    }
    if (should_join) {
        handle->worker.join();
        std::lock_guard<std::mutex> guard(handle->mutex);
        handle->join_in_progress = false;
        handle->condition.notify_all();
    }
    release_capture_ref(handle);
    return SAO_STATUS_OK;
}

}  // namespace

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
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    CaptureOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->owner_released ||
            (!handle->test_backend && handle->npcap == nullptr)) {
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        if (handle->running || handle->worker.joinable()) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        handle->stop_requested = false;
        handle->running = true;
        handle->worker_finished = false;
        handle->worker_exit_complete = false;
        handle->worker_attached = false;
        handle->admission_open = true;
        handle->callback = callback;
        handle->callback_user_data = user_data;
        ++handle->lifetime_refs;
    }
    try {
        std::thread worker([handle] {
            {
                std::unique_lock<std::mutex> lock(handle->mutex);
                handle->condition.wait(lock, [handle] {
                    return handle->worker_attached || handle->stop_requested;
                });
                if (handle->stop_requested && !handle->worker_attached) {
                    lock.unlock();
                    finish_capture_worker(handle);
                    return;
                }
                handle->worker_id = std::this_thread::get_id();
            }
            try {
                std::vector<uint8_t> packet(handle->snap_len);
                while (true) {
                    if (capture_stop_requested(handle)) break;
                    uint64_t timestamp_ms = 0;
                    size_t packet_size = 0;
                    sao_status_t status = SAO_STATUS_ERR_TIMEOUT;
                    if (handle->test_backend) {
                        sao_net_capture_s::TestPacket test_packet;
                        {
                            std::unique_lock<std::mutex> lock(handle->mutex);
                            handle->condition.wait(lock, [handle] {
                                return handle->stop_requested ||
                                       handle->test_read_error ||
                                       !handle->test_packets.empty();
                            });
                            if (handle->stop_requested) break;
                            if (handle->test_read_error) {
                                handle->test_read_error = false;
                                status = SAO_STATUS_ERR_OS_CALL_FAILED;
                            } else {
                                test_packet = std::move(handle->test_packets.front());
                                handle->test_packets.pop_front();
                                timestamp_ms = test_packet.ts_ms;
                                packet_size = test_packet.bytes.size();
                                packet = std::move(test_packet.bytes);
                                status = SAO_STATUS_OK;
                            }
                        }
                    } else {
                        status = sao_net_npcap_next_packet(
                            handle->npcap, packet.data(), packet.size(),
                            &timestamp_ms, &packet_size);
                    }
                    if (status == SAO_STATUS_ERR_TIMEOUT) continue;
                    if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
                        packet.resize(packet_size);
                        continue;
                    }
                    if (status != SAO_STATUS_OK) break;
                    invoke_capture_callback(handle, packet.data(), packet_size,
                                            timestamp_ms);
                }
            } catch (...) {
            }
            finish_capture_worker(handle);
        });
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            handle->worker = std::move(worker);
            handle->worker_id = handle->worker.get_id();
            handle->worker_attached = true;
            if (handle->worker_exit_complete && handle->close_requested &&
                handle->worker.joinable()) {
                handle->worker.detach();
            }
            handle->condition.notify_all();
        }
        return SAO_STATUS_OK;
    } catch (...) {
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            handle->callback = nullptr;
            handle->callback_user_data = nullptr;
            handle->admission_open = false;
            handle->running = false;
            handle->worker_finished = true;
            handle->worker_exit_complete = true;
            handle->worker_attached = false;
        }
        release_capture_ref(handle);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_stop(
    sao_net_capture_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return stop_capture_worker(handle);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_try_close(
    sao_net_capture_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_OK;
    if (!retain_capture_ref(handle)) return SAO_STATUS_ERR_HANDLE_INVALID;
    bool caller_is_worker = false;
    bool should_join = false;
    bool busy = false;
    bool release_owner = false;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (!handle->owner_released) release_owner = true;
        handle->owner_released = true;
        handle->close_requested = true;
        handle->stop_requested = true;
        handle->admission_open = false;
        handle->callback = nullptr;
        handle->callback_user_data = nullptr;
        handle->condition.notify_all();
        caller_is_worker = handle->worker_id == std::this_thread::get_id() &&
                           handle->worker.joinable();
        if (!caller_is_worker && handle->worker.joinable()) {
            if (handle->join_in_progress) {
                busy = true;
            } else {
                handle->join_in_progress = true;
                should_join = true;
            }
        } else if (!caller_is_worker && !handle->worker_exit_complete) {
            busy = true;
        }
    }
    if (release_owner) release_capture_ref(handle);
    if (busy || caller_is_worker) {
        release_capture_ref(handle);
        return k_capture_busy_status;
    }
    if (should_join) {
        handle->worker.join();
        std::lock_guard<std::mutex> guard(handle->mutex);
        handle->join_in_progress = false;
        handle->condition.notify_all();
    }
    close_capture_pcap(handle);
    release_capture_ref(handle);
    return SAO_STATUS_OK;
}

extern "C" void SAO_NET_CALL sao_net_capture_close(
    sao_net_capture_handle_t handle) {
    (void)sao_net_capture_try_close(handle);
}

extern "C" bool sao_net_capture_retain_internal(
    sao_net_capture_handle_t handle) {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> guard(handle->mutex);
    if (handle->owner_released || handle->lifetime_refs == 0) return false;
    ++handle->lifetime_refs;
    return true;
}

extern "C" void sao_net_capture_release_internal(
    sao_net_capture_handle_t handle) {
    release_capture_ref(handle);
}

#if defined(SAO_NET_TESTING)
extern "C" sao_status_t SAO_NET_CALL sao_net_capture_test_open(
    sao_net_capture_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        auto* handle = new sao_net_capture_s();
        handle->test_backend = true;
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_test_submit(
    sao_net_capture_handle_t handle, const uint8_t* bytes, size_t length,
    uint64_t ts_ms) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (bytes == nullptr && length != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!retain_capture_ref(handle)) return SAO_STATUS_ERR_HANDLE_INVALID;
    bool accepted = false;
    try {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->test_backend && handle->running &&
            !handle->close_requested && handle->admission_open) {
            sao_net_capture_s::TestPacket packet;
            if (length != 0) packet.bytes.assign(bytes, bytes + length);
            packet.ts_ms = ts_ms;
            handle->test_packets.push_back(std::move(packet));
            handle->condition.notify_all();
            accepted = true;
        }
    } catch (...) {
        release_capture_ref(handle);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    release_capture_ref(handle);
    return accepted ? SAO_STATUS_OK : SAO_STATUS_ERR_CANCELLED;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_capture_test_fail(
    sao_net_capture_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!retain_capture_ref(handle)) return SAO_STATUS_ERR_HANDLE_INVALID;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->test_backend && handle->running) {
            handle->test_read_error = true;
            handle->condition.notify_all();
            accepted = true;
        }
    }
    release_capture_ref(handle);
    return accepted ? SAO_STATUS_OK : SAO_STATUS_ERR_NOT_INITIALIZED;
}
#endif