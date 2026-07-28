// cpu_tracker.cpp — Phase 14.
// Uses GetProcessTimes for a simple 250ms delta.

#include "sao/mem_probe/cpu_tracker.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
struct Sample {
    uint64_t last_kernel_ns = 0;
    uint64_t last_user_ns = 0;
    uint64_t last_wall_ns = 0;
};
std::mutex g_mu;
std::unordered_map<uint32_t, Sample> g_samples;

uint64_t ft_to_ns(FILETIME ft) {
#if defined(_WIN32)
    uint64_t v = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return v * 100; // 100-ns units → ns
#else
    (void)ft;
    return 0;
#endif
}
} // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_cpu_tracker_sample(
    uint32_t pid, double* out_percent) {
    if (out_percent == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_percent = 0.0;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return SAO_STATUS_ERR_ACCESS_DENIED;
    FILETIME ct, et, kt, ut;
    if (!GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        CloseHandle(h);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    CloseHandle(h);
    uint64_t kernel_ns = ft_to_ns(kt);
    uint64_t user_ns = ft_to_ns(ut);
    uint64_t wall_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::lock_guard lock(g_mu);
    auto& s = g_samples[pid];
    if (s.last_wall_ns == 0) {
        s.last_kernel_ns = kernel_ns;
        s.last_user_ns = user_ns;
        s.last_wall_ns = wall_ns;
        return SAO_STATUS_OK;
    }
    uint64_t d_kernel = kernel_ns - s.last_kernel_ns;
    uint64_t d_user = user_ns - s.last_user_ns;
    uint64_t d_wall = wall_ns - s.last_wall_ns;
    if (d_wall > 0) {
        *out_percent = 100.0 * static_cast<double>(d_kernel + d_user) /
                       static_cast<double>(d_wall);
    }
    s.last_kernel_ns = kernel_ns;
    s.last_user_ns = user_ns;
    s.last_wall_ns = wall_ns;
    return SAO_STATUS_OK;
#else
    (void)pid;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" void SAO_CORE_CALL sao_memprobe_cpu_tracker_reset(uint32_t pid) {
    std::lock_guard lock(g_mu);
    g_samples.erase(pid);
}
