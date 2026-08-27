// cpu_tracker.cpp — per-process CPU% via GetProcessTimes.

#include "sao/mem_probe/cpu_tracker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
struct Sample {
    uint64_t creation_time_100ns = 0;
    uint64_t last_kernel_100ns = 0;
    uint64_t last_user_100ns = 0;
    uint64_t last_wall_100ns = 0;
};

std::mutex g_mu;
std::unordered_map<uint32_t, Sample> g_samples;

#if defined(_WIN32)
uint64_t filetime_value(FILETIME value) {
    return (static_cast<uint64_t>(value.dwHighDateTime) << 32) |
           static_cast<uint64_t>(value.dwLowDateTime);
}
#endif

uint64_t wall_time_100ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::duration<uint64_t, std::ratio<1, 10'000'000>>>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_cpu_tracker_sample(
    uint32_t pid, double* out_percent) {
    try {
    if (out_percent == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_percent = 0.0;
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return SAO_STATUS_ERR_ACCESS_DENIED;
    FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
    if (!GetProcessTimes(process, &creation_time, &exit_time, &kernel_time,
                         &user_time)) {
        CloseHandle(process);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    CloseHandle(process);

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uint32_t processor_count =
        system_info.dwNumberOfProcessors == 0 ? 1 : system_info.dwNumberOfProcessors;
    const uint64_t creation_100ns = filetime_value(creation_time);
    const uint64_t kernel_100ns = filetime_value(kernel_time);
    const uint64_t user_100ns = filetime_value(user_time);
    const uint64_t wall_100ns = wall_time_100ns();

    std::lock_guard lock(g_mu);
    auto it = g_samples.find(pid);
    if (it == g_samples.end() || it->second.creation_time_100ns != creation_100ns) {
        g_samples[pid] = Sample{creation_100ns, kernel_100ns, user_100ns,
                                wall_100ns};
        return SAO_STATUS_OK;
    }
    Sample& previous = it->second;
    if (kernel_100ns < previous.last_kernel_100ns ||
        user_100ns < previous.last_user_100ns ||
        wall_100ns <= previous.last_wall_100ns) {
        previous = Sample{creation_100ns, kernel_100ns, user_100ns, wall_100ns};
        return SAO_STATUS_OK;
    }

    const uint64_t delta_cpu = (kernel_100ns - previous.last_kernel_100ns) +
                               (user_100ns - previous.last_user_100ns);
    const uint64_t delta_wall = wall_100ns - previous.last_wall_100ns;
    const double capacity = static_cast<double>(delta_wall) * processor_count;
    if (capacity > 0.0) {
        *out_percent = std::clamp(
            100.0 * static_cast<double>(delta_cpu) / capacity, 0.0, 100.0);
    }
    previous = Sample{creation_100ns, kernel_100ns, user_100ns, wall_100ns};
    return SAO_STATUS_OK;
#else
        (void)pid;
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        if (out_percent != nullptr) *out_percent = 0.0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_CORE_CALL sao_memprobe_cpu_tracker_reset(uint32_t pid) {
    try {
        std::lock_guard lock(g_mu);
        g_samples.erase(pid);
    } catch (...) {
    }
}