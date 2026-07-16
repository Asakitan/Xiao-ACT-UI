#include "sao/core/time.h"

#ifdef _WIN32
#include <windows.h>

namespace {

struct PerformanceClock {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER origin{};

    PerformanceClock() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&origin);
    }
};

const PerformanceClock& performance_clock() {
    static const PerformanceClock clock;
    return clock;
}

uint64_t counter_delta_to_ns(uint64_t delta, uint64_t frequency) {
    const uint64_t seconds = delta / frequency;
    const uint64_t remainder = delta % frequency;
    return seconds * 1'000'000'000ULL +
           remainder * 1'000'000'000ULL / frequency;
}

}  // namespace
#endif

extern "C" uint64_t SAO_CORE_CALL sao_core_time_now_ns(void) {
#ifdef _WIN32
    const auto& clock = performance_clock();
    LARGE_INTEGER current{};
    QueryPerformanceCounter(&current);
    const auto delta = static_cast<uint64_t>(current.QuadPart - clock.origin.QuadPart);
    return counter_delta_to_ns(delta, static_cast<uint64_t>(clock.frequency.QuadPart));
#else
    return 0;
#endif
}

extern "C" uint64_t SAO_CORE_CALL sao_core_time_now_ms(void) {
    return sao_core_time_now_ns() / 1'000'000ULL;
}

extern "C" uint64_t SAO_CORE_CALL sao_core_time_wall_ns(void) {
#ifdef _WIN32
    FILETIME file_time{};
    GetSystemTimePreciseAsFileTime(&file_time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    constexpr uint64_t k_windows_to_unix_epoch_100ns = 116'444'736'000'000'000ULL;
    if (ticks.QuadPart < k_windows_to_unix_epoch_100ns) {
        return 0;
    }
    return (ticks.QuadPart - k_windows_to_unix_epoch_100ns) * 100ULL;
#else
    return 0;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_time_sleep_ms(uint32_t millis) {
#ifdef _WIN32
    Sleep(millis);
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_core_time_scope_t SAO_CORE_CALL sao_core_time_scope_begin(void) {
    return sao_core_time_now_ns();
}

extern "C" uint64_t SAO_CORE_CALL sao_core_time_scope_end(
    sao_core_time_scope_t token) {
    const uint64_t now = sao_core_time_now_ns();
    return now >= token ? now - token : 0;
}
