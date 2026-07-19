// SAO Auto — render worker.  Wave 4 first-implementable slice.
//
// Python authoritative: `sao_auto/python/render/overlay_render_worker.py`.
//
// G1.5b — thread-pool + per-lane pinning.
//
// Design:
//   * A worker is `pool_size` std::threads sharing one MPMC job queue.
//   * `submit_compose` posts a job to the shared queue (fan-out mode).
//   * Per-overlay lanes still exist for thread-affine GL contexts; each
//     lane is one dedicated thread with its own queue, so a compose job
//     for lane X always runs on the same OS thread.
//   * `flush` waits for all in-flight (queue + running) jobs.
//
// The fan-out submit API used by tests is `sao_ui_render_worker_submit`
// (from the Wave 4 task).  Since the header is fixed and only exports
// per-lane submit, we mirror the fan-out contract with a stable-C ABI
// export declared in this .cpp -- the header prototype is added at the
// end of this file's local `extern "C"` block so the shared library
// exports it too.

#include "sao/ui/render_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <memory>
#include <limits>
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
#endif

// Fan-out submit task signature (added in Wave 4; not part of the fixed
// header per the plan, but exported by the DLL so tests can call it
// directly through GetProcAddress-style linkage.  The header could grow
// this as `sao_ui_render_worker_submit` in a follow-up.)
using sao_ui_render_worker_task_fn_t = void(SAO_UI_CALL*)(void* user_data);

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_submit(
    sao_ui_render_worker_handle_t handle,
    sao_ui_render_worker_task_fn_t task_fn,
    void* user_data);

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_render_worker_flush(
    sao_ui_render_worker_handle_t handle);

namespace {

constexpr int32_t kDefaultPoolSize = 4;

bool frame_byte_size(uint32_t width, uint32_t height, size_t& out_size) noexcept {
    out_size = 0;
    if (width == 0 || height == 0 || width > SIZE_MAX / 4u) return false;
    const size_t stride = static_cast<size_t>(width) * 4u;
    if (height > SIZE_MAX / stride) return false;
    out_size = stride * static_cast<size_t>(height);
    return true;
}

#if defined(_WIN32)
sao_status_t win32_failure_status(DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED ? SAO_STATUS_ERR_ACCESS_DENIED
                                        : SAO_STATUS_ERR_OS_CALL_FAILED;
}
#endif

int32_t auto_lane_count() {
    const unsigned int hw = std::thread::hardware_concurrency();
    if (hw <= 2) return 1;
    if (hw <= 4) return 2;
    if (hw <= 6) return 3;
    if (hw <= 8) return 4;
    if (hw >= 12) return static_cast<int32_t>(std::min<unsigned int>(6u, hw - 2u));
    return static_cast<int32_t>(std::max<unsigned int>(4u, hw / 2u));
}

struct FanTask {
    sao_ui_render_worker_task_fn_t fn;
    void* user_data;
};

struct LaneJob {
    sao_ui_compose_fn_t fn;
    void* user_data;
    double now_sec;
};

}  // namespace

struct sao_ui_frame_buffer_s {
    std::vector<uint8_t> bgra_bytes;
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
};

struct sao_ui_render_lane_s {
    std::string overlay_id;
    // The lane owns one dedicated thread that pulls jobs from its queue.
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<LaneJob> queue;
    std::atomic<bool> stopped{false};
    std::atomic<bool> busy{false};

    // Completed frame drop-slot (single-buffered).
    std::mutex frame_mtx;
    std::unique_ptr<sao_ui_frame_buffer_s> pending_frame;

    // Peak wall ms.
    std::atomic<double> peak_wall_ms_recent{0.0};

    // Backpointer to worker for stats/config.
    struct sao_ui_render_worker_s* worker = nullptr;

    void thread_main();
};

struct sao_ui_render_worker_s {
    // Fan-out pool: workers pull from shared queue.
    int32_t pool_size = kDefaultPoolSize;
    std::vector<std::thread> pool;
    std::mutex fan_mtx;
    std::condition_variable fan_cv;
    std::condition_variable idle_cv;
    std::deque<FanTask> fan_queue;
    std::atomic<int32_t> in_flight{0};
    std::atomic<bool> stop_requested{false};

    // Per-lane state.
    std::mutex lanes_mtx;
    std::unordered_map<std::string, std::unique_ptr<sao_ui_render_lane_s>> lanes;

    // Config
    bool queue_pending = true;

    // Peak sampling.
    std::atomic<double> peak_wall_ms_recent{0.0};

    void fan_thread_main() {
        while (true) {
            FanTask task{};
            {
                std::unique_lock<std::mutex> lk(fan_mtx);
                fan_cv.wait(lk, [this] { return stop_requested.load() || !fan_queue.empty(); });
                if (stop_requested.load() && fan_queue.empty()) return;
                task = fan_queue.front();
                fan_queue.pop_front();
            }
            if (task.fn) {
                task.fn(task.user_data);
            }
            const int32_t left = in_flight.fetch_sub(1) - 1;
            if (left == 0) {
                std::lock_guard<std::mutex> lk(fan_mtx);
                idle_cv.notify_all();
            }
        }
    }
};

void sao_ui_render_lane_s::thread_main() {
    while (true) {
        LaneJob job{};
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [this] { return stopped.load() || !queue.empty(); });
            if (stopped.load() && queue.empty()) return;
            job = queue.front();
            queue.pop_front();
        }
        busy.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        sao_ui_frame_buffer_handle_t frame = nullptr;
        if (job.fn) {
            frame = job.fn(job.now_sec, job.user_data);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double prev = peak_wall_ms_recent.load();
        if (ms > prev) peak_wall_ms_recent.store(ms);
        if (worker != nullptr) {
            const double gprev = worker->peak_wall_ms_recent.load();
            if (ms > gprev) worker->peak_wall_ms_recent.store(ms);
        }
        if (frame != nullptr) {
            std::lock_guard<std::mutex> flk(frame_mtx);
            pending_frame.reset(frame);
        }
        busy.store(false);
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_create(
    const SaoRenderWorkerConfig* config, sao_ui_render_worker_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* w = new (std::nothrow) sao_ui_render_worker_s();
    if (w == nullptr) return SAO_STATUS_ERR_UNKNOWN;

    int32_t pool_size = 0;
    if (config != nullptr) {
        pool_size = config->task_pool_size > 0 ? config->task_pool_size : 0;
        w->queue_pending = config->queue_pending;
    }
    if (pool_size <= 0) {
        // Auto lane sizing heuristic doubles for fan-out pool (min 2).
        const int32_t lanes = auto_lane_count();
        pool_size = std::max<int32_t>(2, lanes);
    }
    w->pool_size = pool_size;

    for (int32_t i = 0; i < pool_size; ++i) {
        w->pool.emplace_back(&sao_ui_render_worker_s::fan_thread_main, w);
    }

    *out_handle = w;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_render_worker_destroy(
    sao_ui_render_worker_handle_t handle) {
    if (handle == nullptr) return;

    // Stop fan-out pool.
    {
        std::lock_guard<std::mutex> lk(handle->fan_mtx);
        handle->stop_requested.store(true);
    }
    handle->fan_cv.notify_all();
    for (auto& t : handle->pool) {
        if (t.joinable()) t.join();
    }
    handle->pool.clear();

    // Stop lanes.
    {
        std::lock_guard<std::mutex> lk(handle->lanes_mtx);
        for (auto& kv : handle->lanes) {
            auto& lane = *kv.second;
            {
                std::lock_guard<std::mutex> llk(lane.mtx);
                lane.stopped.store(true);
            }
            lane.cv.notify_all();
        }
        for (auto& kv : handle->lanes) {
            auto& lane = *kv.second;
            if (lane.thread.joinable()) lane.thread.join();
        }
        handle->lanes.clear();
    }

    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_get_lane(
    sao_ui_render_worker_handle_t handle, const char* overlay_id_utf8,
    sao_ui_render_lane_handle_t* out_lane) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (overlay_id_utf8 == nullptr || out_lane == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_lane = nullptr;

    std::lock_guard<std::mutex> lk(handle->lanes_mtx);
    const std::string key = overlay_id_utf8;
    auto it = handle->lanes.find(key);
    if (it != handle->lanes.end()) {
        *out_lane = it->second.get();
        return SAO_STATUS_OK;
    }
    auto lane = std::make_unique<sao_ui_render_lane_s>();
    lane->overlay_id = key;
    lane->worker = handle;
    lane->thread = std::thread(&sao_ui_render_lane_s::thread_main, lane.get());
    *out_lane = lane.get();
    handle->lanes.emplace(key, std::move(lane));
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_lane_submit_compose(
    sao_ui_render_lane_handle_t lane, sao_ui_compose_fn_t fn,
    void* user_data, double now_sec) {
    if (lane == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (fn == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    {
        std::lock_guard<std::mutex> lk(lane->mtx);
        if (lane->stopped.load()) return SAO_STATUS_ERR_CANCELLED;
        if (!lane->queue.empty() && lane->worker != nullptr && !lane->worker->queue_pending) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        lane->queue.push_back(LaneJob{fn, user_data, now_sec});
    }
    lane->cv.notify_one();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_lane_try_take_frame(
    sao_ui_render_lane_handle_t lane, sao_ui_frame_buffer_handle_t* out_frame) {
    if (out_frame == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_frame = nullptr;
    if (lane == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    std::lock_guard<std::mutex> flk(lane->frame_mtx);
    if (lane->pending_frame == nullptr) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    *out_frame = lane->pending_frame.release();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_frame_buffer_create_bgra(
    const uint8_t* bgra_bytes, size_t bgra_size, uint32_t width, uint32_t height,
    int32_t x, int32_t y, sao_ui_frame_buffer_handle_t* out_frame) {
    if (out_frame == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_frame = nullptr;
    size_t expected_size = 0;
    if (bgra_bytes == nullptr || !frame_byte_size(width, height, expected_size) ||
        bgra_size != expected_size) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto frame = std::make_unique<sao_ui_frame_buffer_s>();
        frame->bgra_bytes.assign(bgra_bytes, bgra_bytes + bgra_size);
        frame->width = width;
        frame->height = height;
        frame->x = x;
        frame->y = y;
        *out_frame = frame.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_frame_buffer_view(
    sao_ui_frame_buffer_handle_t handle, SaoFrameBufferView* out_view) {
    if (out_view == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_view, 0, sizeof(*out_view));
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    out_view->bgra_bytes = handle->bgra_bytes.data();
    out_view->width = handle->width;
    out_view->height = handle->height;
    out_view->x = handle->x;
    out_view->y = handle->y;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_frame_buffer_release(
    sao_ui_frame_buffer_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_ulw_commit(
    void* hwnd, sao_ui_frame_buffer_handle_t frame) {
    if (frame == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (hwnd == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    size_t expected_size = 0;
    if (!frame_byte_size(frame->width, frame->height, expected_size) ||
        frame->bgra_bytes.size() != expected_size) {
        return SAO_STATUS_ERR_SURFACE_INVALID;
    }
#if defined(_WIN32)
    if (frame->width > static_cast<uint32_t>(std::numeric_limits<LONG>::max()) ||
        frame->height > static_cast<uint32_t>(std::numeric_limits<LONG>::max())) {
        return SAO_STATUS_ERR_SURFACE_INVALID;
    }
    const HWND target = static_cast<HWND>(hwnd);
    if (!::IsWindow(target)) return SAO_STATUS_ERR_HANDLE_INVALID;
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR ex_style = ::GetWindowLongPtrW(target, GWL_EXSTYLE);
    if (ex_style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        return win32_failure_status(::GetLastError());
    }
    if ((ex_style & WS_EX_LAYERED) == 0) return SAO_STATUS_ERR_SURFACE_INVALID;

    HDC screen_dc = ::GetDC(nullptr);
    if (screen_dc == nullptr) return win32_failure_status(::GetLastError());
    HDC memory_dc = ::CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
        const DWORD error = ::GetLastError();
        ::ReleaseDC(nullptr, screen_dc);
        return win32_failure_status(error);
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = static_cast<LONG>(frame->width);
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(frame->height);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(memory_dc, &bitmap_info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        const DWORD error = ::GetLastError();
        if (bitmap != nullptr) ::DeleteObject(bitmap);
        ::DeleteDC(memory_dc);
        ::ReleaseDC(nullptr, screen_dc);
        return win32_failure_status(error);
    }
    const HGDIOBJ previous_bitmap = ::SelectObject(memory_dc, bitmap);
    if (previous_bitmap == nullptr || previous_bitmap == HGDI_ERROR) {
        const DWORD error = ::GetLastError();
        ::DeleteObject(bitmap);
        ::DeleteDC(memory_dc);
        ::ReleaseDC(nullptr, screen_dc);
        return win32_failure_status(error);
    }

    std::memcpy(bits, frame->bgra_bytes.data(), expected_size);
    POINT destination{frame->x, frame->y};
    SIZE size{static_cast<LONG>(frame->width), static_cast<LONG>(frame->height)};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO update{};
    update.cbSize = sizeof(update);
    update.hdcDst = screen_dc;
    update.pptDst = &destination;
    update.psize = &size;
    update.hdcSrc = memory_dc;
    update.pptSrc = &source;
    update.pblend = &blend;
    update.dwFlags = ULW_ALPHA;
    const BOOL committed = ::UpdateLayeredWindowIndirect(target, &update);
    const DWORD commit_error = committed ? ERROR_SUCCESS : ::GetLastError();

    const bool selection_restored = ::SelectObject(memory_dc, previous_bitmap) != nullptr;
    const bool bitmap_deleted = ::DeleteObject(bitmap) != FALSE;
    const bool memory_dc_deleted = ::DeleteDC(memory_dc) != FALSE;
    const bool screen_dc_released = ::ReleaseDC(nullptr, screen_dc) != 0;
    if (!committed) return win32_failure_status(commit_error);
    return selection_restored && bitmap_deleted && memory_dc_deleted && screen_dc_released
               ? SAO_STATUS_OK
               : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" double SAO_UI_CALL sao_ui_render_worker_peak_wall_ms(
    sao_ui_render_worker_handle_t handle, double /*window_sec*/) {
    if (handle == nullptr) return 0.0;
    return handle->peak_wall_ms_recent.load();
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_premultiply_rgba_to_bgra(
    const uint8_t* rgba_in, uint32_t width, uint32_t height,
    uint8_t** out_bgra, size_t* out_size) {
    if (out_bgra == nullptr || out_size == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_bgra = nullptr;
    *out_size = 0;
    if (rgba_in == nullptr || width == 0 || height == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels > (SIZE_MAX / 4u)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const size_t bytes = pixels * 4u;
    auto* buf = static_cast<uint8_t*>(std::malloc(bytes));
    if (buf == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t r = rgba_in[i * 4 + 0];
        const uint8_t g = rgba_in[i * 4 + 1];
        const uint8_t b = rgba_in[i * 4 + 2];
        const uint8_t a = rgba_in[i * 4 + 3];
        // premultiply: c' = c * a / 255, using classic rounded fixed math.
        auto premul = [a](uint8_t c) -> uint8_t {
            return static_cast<uint8_t>((static_cast<uint32_t>(c) * a + 127u) / 255u);
        };
        buf[i * 4 + 0] = premul(b);
        buf[i * 4 + 1] = premul(g);
        buf[i * 4 + 2] = premul(r);
        buf[i * 4 + 3] = a;
    }
    *out_bgra = buf;
    *out_size = bytes;
    return SAO_STATUS_OK;
}

// ── Wave 4 fan-out API (not yet in header; exported for tests) ─────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_submit(
    sao_ui_render_worker_handle_t handle,
    sao_ui_render_worker_task_fn_t task_fn,
    void* user_data) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (task_fn == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (handle->stop_requested.load()) return SAO_STATUS_ERR_CANCELLED;
    {
        std::lock_guard<std::mutex> lk(handle->fan_mtx);
        handle->in_flight.fetch_add(1);
        handle->fan_queue.push_back(FanTask{task_fn, user_data});
    }
    handle->fan_cv.notify_one();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_flush(
    sao_ui_render_worker_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::unique_lock<std::mutex> lk(handle->fan_mtx);
    handle->idle_cv.wait(lk, [handle] {
        return handle->in_flight.load() == 0 && handle->fan_queue.empty();
    });

    // Also wait for all lanes to drain.
    lk.unlock();
    {
        std::lock_guard<std::mutex> llk(handle->lanes_mtx);
        for (auto& kv : handle->lanes) {
            auto& lane = *kv.second;
            while (true) {
                {
                    std::lock_guard<std::mutex> lqk(lane.mtx);
                    if (lane.queue.empty() && !lane.busy.load()) break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
    return SAO_STATUS_OK;
}
