// SAO Auto — overlay scheduler and monotonic frame clock.
//
// Python authoritative: `sao_auto/python/render/overlay_scheduler.py`.
//
// This implementation focuses on the 60 Hz monotonic clock (G1.5a):
//   * QueryPerformanceCounter perf-counter deadline
//   * winmm.timeBeginPeriod(1) engaged while running
//   * background std::thread drives per-frame tick fan-out
//   * per-tick stats (last_frame_ms / avg_frame_ms / frame_count)
//   * pressure floor + combat/menu signals feed idle_skip_n
//
// Job registration / animating / visibility fans out from one thread
// under a small mutex.  A jobs snapshot (std::shared_ptr<vector>) is
// swapped atomically so the tick hot path reads it lock-free.

#include "sao/ui/scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  pragma comment(lib, "winmm.lib")
extern "C" {
    // Forward declaration to avoid pulling in <mmsystem.h> and its aliases.
    __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int uPeriod);
    __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int uPeriod);
}
#endif

namespace {

constexpr int32_t kDefaultHz = 60;
constexpr int32_t kMinHz = 60;
constexpr int32_t kMaxHz = 240;
constexpr int32_t kDefaultMaxIdleSkipN = 4;

struct Job {
    std::string ident;
    sao_ui_scheduler_tick_fn_t tick_fn;
    sao_ui_scheduler_animating_fn_t animating_fn;
    sao_ui_scheduler_visibility_fn_t visibility_fn;
    void* user_data;
    uint32_t phase_offset;
};

uint32_t stable_phase_offset(const std::string& ident) {
    uint32_t acc = 0;
    for (size_t i = 0; i != ident.size(); ++i) {
        acc = (acc + static_cast<uint32_t>((i + 1) * static_cast<unsigned char>(ident[i]))) & 0x7FFFFFFFu;
    }
    return acc;
}

double perf_now_sec() {
#if defined(_WIN32)
    static thread_local double freq = 0.0;
    if (freq == 0.0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = static_cast<double>(f.QuadPart);
    }
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
#else
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
#endif
}

int32_t detect_refresh_hz_impl() {
#if defined(_WIN32)
    HDC hdc = ::GetDC(nullptr);
    if (hdc == nullptr) {
        return kDefaultHz;
    }
    const int rate = ::GetDeviceCaps(hdc, VREFRESH);
    ::ReleaseDC(nullptr, hdc);
    if (rate <= 1) {
        return kDefaultHz;
    }
    return std::max<int32_t>(kMinHz, std::min<int32_t>(kMaxHz, rate));
#else
    return kDefaultHz;
#endif
}

}  // namespace

struct sao_ui_scheduler_s {
    // Config-derived cadence.
    int32_t target_hz = kDefaultHz;
    double  frame_sec = 1.0 / kDefaultHz;
    int32_t max_idle_skip_n = kDefaultMaxIdleSkipN;
    bool    engage_time_period = true;
    bool    enable_pressure_floor = true;

    // Thread control.
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    std::condition_variable start_cv;
    std::mutex start_mutex;

    // Job registry.
    std::mutex jobs_mutex;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    std::atomic<std::shared_ptr<std::vector<std::shared_ptr<Job>>>> jobs_snapshot{
        std::make_shared<std::vector<std::shared_ptr<Job>>>()};

    // Pressure signals.
    std::atomic<bool> combat_load{false};
    std::atomic<bool> menu_open{false};
    std::atomic<int32_t> pressure_override{-1};
    std::atomic<int32_t> wall_pressure_floor{0};
    std::atomic<int32_t> current_idle_skip_n{1};

    // Stats.
    std::atomic<double> last_frame_ms{0.0};
    std::atomic<double> avg_frame_ms{0.0};
    std::atomic<uint64_t> frame_count{0};

    // winmm timer resolution engagement.
    bool time_period_engaged = false;

    void publish_snapshot() {
        auto snapshot = std::make_shared<std::vector<std::shared_ptr<Job>>>();
        snapshot->reserve(jobs.size());
        for (auto& kv : jobs) {
            snapshot->push_back(kv.second);
        }
        jobs_snapshot.store(std::move(snapshot));
    }

    int32_t compute_idle_skip_n() {
        int32_t base = 1;
        const int32_t explicit_level = pressure_override.load();
        if (explicit_level >= 0) {
            switch (explicit_level) {
                case 0: base = 1; break;
                case 1: base = 2; break;
                case 2: base = 3; break;
                case 3: base = 4; break;
                default: base = 4; break;
            }
        } else {
            if (menu_open.load()) base = std::max(base, 3);
            if (combat_load.load()) base = std::max(base, 2);
            if (enable_pressure_floor) {
                base = std::max(base, wall_pressure_floor.load() + 1);
            }
        }
        base = std::min<int32_t>(base, std::max<int32_t>(1, max_idle_skip_n));
        return base;
    }
};

namespace {

void scheduler_thread_main(sao_ui_scheduler_s* self) {
    // Wait for start signal to fully publish before running.
    {
        std::unique_lock<std::mutex> lk(self->start_mutex);
        self->start_cv.wait(lk, [self] { return self->running.load() || self->stop_requested.load(); });
    }

    if (self->stop_requested.load()) return;

#if defined(_WIN32)
    if (self->engage_time_period && !self->time_period_engaged) {
        if (timeBeginPeriod(1) == 0) {
            self->time_period_engaged = true;
        }
    }
#endif

    double next_deadline = perf_now_sec() + self->frame_sec;
    double avg_ms_ewma = 0.0;

    while (!self->stop_requested.load()) {
        // Precise wait until next deadline.
        double now = perf_now_sec();
        while (now < next_deadline && !self->stop_requested.load()) {
            const double remaining = next_deadline - now;
            if (remaining > 0.003) {
                // Coarse sleep with 1 ms resolution (timeBeginPeriod engaged).
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else if (remaining > 0.0005) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            } else {
                // Spin the final <0.5 ms with a yield hint.
                std::this_thread::yield();
            }
            now = perf_now_sec();
        }

        if (self->stop_requested.load()) break;

        const double frame_start = now;

        // Roll deadline forward to catch up if we ran late.
        next_deadline += self->frame_sec;
        if (frame_start > next_deadline + self->frame_sec * 4.0) {
            // Fell more than 4 frames behind; realign to now.
            next_deadline = frame_start + self->frame_sec;
        }

        // Snapshot jobs lock-free.
        auto snap = self->jobs_snapshot.load();
        if (snap) {
            const int32_t skip_n = self->compute_idle_skip_n();
            self->current_idle_skip_n.store(skip_n);
            const uint64_t frame_idx = self->frame_count.load() + 1;
            for (const auto& job : *snap) {
                if (job == nullptr || job->tick_fn == nullptr) continue;
                if (job->visibility_fn != nullptr && !job->visibility_fn(job->user_data)) {
                    continue;
                }
                const bool animating = (job->animating_fn != nullptr)
                    ? job->animating_fn(job->user_data)
                    : true;
                if (!animating && skip_n > 1) {
                    const uint32_t phase = (job->phase_offset % static_cast<uint32_t>(skip_n));
                    if ((frame_idx + phase) % static_cast<uint32_t>(skip_n) != 0) {
                        continue;
                    }
                }
                job->tick_fn(frame_start, job->user_data);
            }
        }

        const double frame_end = perf_now_sec();
        const double ms = (frame_end - frame_start) * 1000.0;
        self->last_frame_ms.store(ms);
        avg_ms_ewma = avg_ms_ewma == 0.0 ? ms : (avg_ms_ewma * 0.9 + ms * 0.1);
        self->avg_frame_ms.store(avg_ms_ewma);
        self->frame_count.fetch_add(1);

        // Update pressure floor from ms observation.
        if (self->enable_pressure_floor) {
            int32_t floor = 0;
            if (ms > 50.0) floor = 3;
            else if (ms > 25.0) floor = 2;
            else if (ms > 12.0) floor = 1;
            self->wall_pressure_floor.store(floor);
        }
    }

#if defined(_WIN32)
    if (self->time_period_engaged) {
        timeEndPeriod(1);
        self->time_period_engaged = false;
    }
#endif
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_create(
    const SaoSchedulerConfig* config, sao_ui_scheduler_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    auto* s = new (std::nothrow) sao_ui_scheduler_s();
    if (s == nullptr) return SAO_STATUS_ERR_UNKNOWN;

    int32_t hz = kDefaultHz;
    int32_t max_idle_skip_n = kDefaultMaxIdleSkipN;
    bool engage_period = true;
    bool enable_floor = true;
    if (config != nullptr) {
        if (config->target_hz > 0) {
            hz = std::max<int32_t>(kMinHz, std::min<int32_t>(kMaxHz, config->target_hz));
        } else {
            hz = detect_refresh_hz_impl();
        }
        if (config->max_idle_skip_n > 0) {
            max_idle_skip_n = config->max_idle_skip_n;
        }
        engage_period = config->engage_time_period;
        enable_floor = config->enable_pressure_floor;
    } else {
        hz = detect_refresh_hz_impl();
    }

    s->target_hz = hz;
    s->frame_sec = 1.0 / static_cast<double>(hz);
    s->max_idle_skip_n = max_idle_skip_n;
    s->engage_time_period = engage_period;
    s->enable_pressure_floor = enable_floor;

    *out_handle = s;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_scheduler_destroy(
    sao_ui_scheduler_handle_t handle) {
    if (handle == nullptr) return;
    sao_ui_scheduler_stop(handle);
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_start(
    sao_ui_scheduler_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->running.load()) return SAO_STATUS_ERR_ALREADY_EXISTS;
    handle->stop_requested.store(false);
    handle->worker = std::thread(&scheduler_thread_main, handle);
    {
        std::lock_guard<std::mutex> lk(handle->start_mutex);
        handle->running.store(true);
    }
    handle->start_cv.notify_all();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_stop(
    sao_ui_scheduler_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!handle->running.load() && !handle->worker.joinable()) {
        return SAO_STATUS_OK;
    }
    {
        std::lock_guard<std::mutex> lk(handle->start_mutex);
        handle->stop_requested.store(true);
    }
    handle->start_cv.notify_all();
    if (handle->worker.joinable()) {
        handle->worker.join();
    }
    handle->running.store(false);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_register(
    sao_ui_scheduler_handle_t handle,
    const char* ident_utf8,
    sao_ui_scheduler_tick_fn_t tick_fn,
    sao_ui_scheduler_animating_fn_t animating_fn,
    sao_ui_scheduler_visibility_fn_t visibility_fn,
    void* user_data) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (ident_utf8 == nullptr || tick_fn == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto job = std::make_shared<Job>();
    job->ident = ident_utf8;
    job->tick_fn = tick_fn;
    job->animating_fn = animating_fn;
    job->visibility_fn = visibility_fn;
    job->user_data = user_data;
    job->phase_offset = stable_phase_offset(job->ident);

    std::lock_guard<std::mutex> lk(handle->jobs_mutex);
    if (handle->jobs.count(job->ident) != 0) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    handle->jobs.emplace(job->ident, job);
    handle->publish_snapshot();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_unregister(
    sao_ui_scheduler_handle_t handle, const char* ident_utf8) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (ident_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->jobs_mutex);
    const auto it = handle->jobs.find(ident_utf8);
    if (it == handle->jobs.end()) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    handle->jobs.erase(it);
    handle->publish_snapshot();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_set_combat_load(
    sao_ui_scheduler_handle_t handle, bool active) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    handle->combat_load.store(active);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_set_menu_open(
    sao_ui_scheduler_handle_t handle, bool active) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    handle->menu_open.store(active);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_set_render_pressure(
    sao_ui_scheduler_handle_t handle, int32_t level) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    handle->pressure_override.store(level < 0 ? -1 : level);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_scheduler_get_stats(
    sao_ui_scheduler_handle_t handle, SaoSchedulerStats* out_stats) {
    if (out_stats == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_stats, 0, sizeof(*out_stats));
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    out_stats->target_hz = handle->target_hz;
    out_stats->current_idle_skip_n = handle->current_idle_skip_n.load();
    out_stats->wall_pressure_floor = handle->wall_pressure_floor.load();
    {
        auto snap = handle->jobs_snapshot.load();
        out_stats->job_count = snap ? static_cast<uint32_t>(snap->size()) : 0u;
    }
    out_stats->last_frame_ms = handle->last_frame_ms.load();
    out_stats->avg_frame_ms = handle->avg_frame_ms.load();
    out_stats->frame_count = handle->frame_count.load();
    return SAO_STATUS_OK;
}

extern "C" int32_t SAO_UI_CALL sao_ui_scheduler_detect_refresh_hz(void) {
    return detect_refresh_hz_impl();
}
