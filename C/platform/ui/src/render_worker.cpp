// SAO Auto — render worker.  Wave 4 first-implementable slice.
//
// Python authoritative: `sao_auto/python/render/overlay_render_worker.py`.
//
// G1.5b — thread-pool + per-lane pinning.
//
// Design:
//   * A worker is `pool_size` std::threads sharing one MPMC job queue.
//   * Per-overlay lanes own dedicated threads for thread-affine work.
//   * Public handles are stable shells looked up in active registries.
//   * Destroy retires handles, drains API leases, then joins threads.
//   * Flush uses completion conditions rather than polling lane state.

#include "sao/ui/render_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <limits>
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
#endif

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

extern thread_local sao_ui_render_worker_s* g_callback_worker;

class ApiActivity {
  public:
    bool try_acquire() noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (!accepting_) return false;
            ++active_;
            return true;
        } catch (...) {
            return false;
        }
    }

    void release() noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (active_ != 0) --active_;
            if (active_ == 0) idle_.notify_all();
        } catch (...) {
        }
    }

    void retire() noexcept {
        try {
            std::lock_guard lock(mutex_);
            accepting_ = false;
        } catch (...) {
        }
    }

    void wait() noexcept {
        try {
            std::unique_lock lock(mutex_);
            idle_.wait(lock, [this] { return active_ == 0; });
        } catch (...) {
        }
    }

  private:
    std::mutex mutex_;
    std::condition_variable idle_;
    size_t active_ = 0;
    bool accepting_ = true;
};

template <typename Callback> class ScopeExit {
  public:
    explicit ScopeExit(Callback callback) noexcept : callback_(std::move(callback)) {}

    ~ScopeExit() noexcept {
        try {
            callback_();
        } catch (...) {
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

  private:
    Callback callback_;
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
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::deque<LaneJob> queue;
    size_t in_flight = 0;
    bool stopped = false;
    bool busy = false;
    ApiActivity api_activity;

    std::mutex frame_mtx;
    std::unique_ptr<sao_ui_frame_buffer_s> pending_frame;

    std::atomic<double> peak_wall_ms_recent{0.0};

    struct sao_ui_render_worker_s* worker = nullptr;

    void thread_main();
};

struct sao_ui_render_worker_s {
    int32_t pool_size = kDefaultPoolSize;
    std::vector<std::thread> pool;
    std::mutex fan_mtx;
    std::condition_variable fan_cv;
    std::condition_variable idle_cv;
    std::deque<FanTask> fan_queue;
    size_t fan_in_flight = 0;
    bool stop_requested = false;
    ApiActivity api_activity;

    std::mutex lanes_mtx;
    std::unordered_map<std::string, std::shared_ptr<sao_ui_render_lane_s>> lanes;

    bool queue_pending = true;
    std::atomic<double> peak_wall_ms_recent{0.0};

    void fan_thread_main() {
        while (true) {
            FanTask task{};
            {
                std::unique_lock lock(fan_mtx);
                fan_cv.wait(lock, [this] { return stop_requested || !fan_queue.empty(); });
                if (stop_requested && fan_queue.empty()) return;
                task = fan_queue.front();
                fan_queue.pop_front();
            }
            ScopeExit complete([this] {
                std::lock_guard lock(fan_mtx);
                if (fan_in_flight != 0) --fan_in_flight;
                if (fan_in_flight == 0) idle_cv.notify_all();
            });
            auto* previous = g_callback_worker;
            g_callback_worker = this;
            ScopeExit restore_callback([previous] { g_callback_worker = previous; });
            try {
                if (task.fn != nullptr) task.fn(task.user_data);
            } catch (...) {
            }
        }
    }
};

namespace {

thread_local sao_ui_render_worker_s* g_callback_worker = nullptr;

struct WorkerRegistry {
    std::mutex mutex;
    std::unordered_map<sao_ui_render_worker_handle_t,
                       std::shared_ptr<sao_ui_render_worker_s>> active;
    std::vector<std::shared_ptr<sao_ui_render_worker_s>> all;
};

struct LaneRegistry {
    std::mutex mutex;
    std::unordered_map<sao_ui_render_lane_handle_t,
                       std::shared_ptr<sao_ui_render_lane_s>> active;
    std::vector<std::shared_ptr<sao_ui_render_lane_s>> all;
};

WorkerRegistry& worker_registry() {
    static WorkerRegistry registry;
    return registry;
}

LaneRegistry& lane_registry() {
    static LaneRegistry registry;
    return registry;
}

class WorkerOperationLease {
  public:
    explicit WorkerOperationLease(sao_ui_render_worker_handle_t handle) noexcept {
        try {
            auto& registry = worker_registry();
            std::lock_guard lock(registry.mutex);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end() ||
                !found->second->api_activity.try_acquire()) {
                return;
            }
            worker_ = found->second;
        } catch (...) {
        }
    }

    ~WorkerOperationLease() {
        if (worker_ != nullptr) worker_->api_activity.release();
    }

    WorkerOperationLease(const WorkerOperationLease&) = delete;
    WorkerOperationLease& operator=(const WorkerOperationLease&) = delete;

    explicit operator bool() const noexcept {
        return worker_ != nullptr;
    }

    sao_ui_render_worker_s* get() const noexcept {
        return worker_.get();
    }

  private:
    std::shared_ptr<sao_ui_render_worker_s> worker_;
};

class LaneOperationLease {
  public:
    explicit LaneOperationLease(sao_ui_render_lane_handle_t handle) noexcept {
        try {
            auto& registry = lane_registry();
            std::lock_guard lock(registry.mutex);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end() ||
                !found->second->api_activity.try_acquire()) {
                return;
            }
            lane_ = found->second;
        } catch (...) {
        }
    }

    ~LaneOperationLease() {
        if (lane_ != nullptr) lane_->api_activity.release();
    }

    LaneOperationLease(const LaneOperationLease&) = delete;
    LaneOperationLease& operator=(const LaneOperationLease&) = delete;

    explicit operator bool() const noexcept {
        return lane_ != nullptr;
    }

    sao_ui_render_lane_s* get() const noexcept {
        return lane_.get();
    }

  private:
    std::shared_ptr<sao_ui_render_lane_s> lane_;
};

void stop_and_join_lane(const std::shared_ptr<sao_ui_render_lane_s>& lane) noexcept {
    if (lane == nullptr) return;
    try {
        {
            std::lock_guard lock(lane->mtx);
            lane->stopped = true;
        }
        lane->cv.notify_all();
        if (lane->thread.joinable()) lane->thread.join();
        std::lock_guard frame_lock(lane->frame_mtx);
        lane->pending_frame.reset();
    } catch (...) {
    }
}

void retire_worker_lanes(sao_ui_render_worker_s& worker) noexcept {
    try {
        std::lock_guard lanes_lock(worker.lanes_mtx);
        auto& registry = lane_registry();
        std::lock_guard registry_lock(registry.mutex);
        for (const auto& [_, lane] : worker.lanes) {
            lane->api_activity.retire();
            registry.active.erase(lane.get());
        }
    } catch (...) {
    }
}

}  // namespace

void sao_ui_render_lane_s::thread_main() {
    while (true) {
        LaneJob job{};
        {
            std::unique_lock lock(mtx);
            cv.wait(lock, [this] { return stopped || !queue.empty(); });
            if (stopped && queue.empty()) return;
            job = queue.front();
            queue.pop_front();
            busy = true;
        }
        ScopeExit complete([this] {
            std::lock_guard lock(mtx);
            busy = false;
            if (in_flight != 0) --in_flight;
            if (in_flight == 0) idle_cv.notify_all();
        });
        auto* previous = g_callback_worker;
        g_callback_worker = worker;
        ScopeExit restore_callback([previous] { g_callback_worker = previous; });
        const auto t0 = std::chrono::steady_clock::now();
        sao_ui_frame_buffer_handle_t frame = nullptr;
        try {
            if (job.fn != nullptr) frame = job.fn(job.now_sec, job.user_data);
        } catch (...) {
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
            std::lock_guard frame_lock(frame_mtx);
            pending_frame.reset(frame);
        }
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_create(
    const SaoRenderWorkerConfig* config, sao_ui_render_worker_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    std::shared_ptr<sao_ui_render_worker_s> worker;
    try {
        worker = std::make_shared<sao_ui_render_worker_s>();
        int32_t pool_size = 0;
        if (config != nullptr) {
            pool_size = config->task_pool_size > 0 ? config->task_pool_size : 0;
            worker->queue_pending = config->queue_pending;
        }
        if (pool_size <= 0) {
            const int32_t lanes = auto_lane_count();
            pool_size = std::max<int32_t>(2, lanes);
        }
        worker->pool_size = pool_size;
        worker->pool.reserve(static_cast<size_t>(pool_size));

        auto& registry = worker_registry();
        {
            std::lock_guard lock(registry.mutex);
            registry.all.push_back(worker);
        }
        for (int32_t index = 0; index < pool_size; ++index) {
            worker->pool.emplace_back(&sao_ui_render_worker_s::fan_thread_main, worker.get());
        }

        std::lock_guard lock(registry.mutex);
        const auto handle = worker.get();
        registry.active.emplace(handle, worker);
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        if (worker != nullptr) {
            {
                std::lock_guard lock(worker->fan_mtx);
                worker->stop_requested = true;
            }
            worker->fan_cv.notify_all();
            for (auto& thread : worker->pool) {
                if (thread.joinable()) thread.join();
            }
            worker->pool.clear();
        }
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_destroy(
    sao_ui_render_worker_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (g_callback_worker == handle) return SAO_UI_STATUS_ERR_BUSY;

    std::shared_ptr<sao_ui_render_worker_s> worker;
    {
        auto& registry = worker_registry();
        std::lock_guard lock(registry.mutex);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end()) return SAO_STATUS_ERR_HANDLE_INVALID;
        worker = found->second;
        worker->api_activity.retire();
        registry.active.erase(found);
    }

    retire_worker_lanes(*worker);
    worker->api_activity.wait();
    retire_worker_lanes(*worker);

    for (const auto& [_, lane] : worker->lanes) lane->api_activity.wait();

    {
        std::lock_guard lock(worker->fan_mtx);
        worker->stop_requested = true;
    }
    worker->fan_cv.notify_all();
    for (auto& thread : worker->pool) {
        if (thread.joinable()) thread.join();
    }
    worker->pool.clear();

    for (const auto& [_, lane] : worker->lanes) stop_and_join_lane(lane);
    {
        std::lock_guard lock(worker->lanes_mtx);
        worker->lanes.clear();
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_get_lane(
    sao_ui_render_worker_handle_t handle, const char* overlay_id_utf8,
    sao_ui_render_lane_handle_t* out_lane) {
    if (overlay_id_utf8 == nullptr || out_lane == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_lane = nullptr;
    WorkerOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;

    try {
        auto* worker = operation.get();
        std::lock_guard lock(worker->lanes_mtx);
        const std::string key = overlay_id_utf8;
        const auto found = worker->lanes.find(key);
        if (found != worker->lanes.end()) {
            *out_lane = found->second.get();
            return SAO_STATUS_OK;
        }
        auto lane = std::make_shared<sao_ui_render_lane_s>();
        lane->overlay_id = key;
        lane->worker = worker;
        {
            auto& registry = lane_registry();
            std::lock_guard registry_lock(registry.mutex);
            registry.all.push_back(lane);
        }
        worker->lanes.emplace(key, lane);
        try {
            lane->thread = std::thread(&sao_ui_render_lane_s::thread_main, lane.get());
            auto& registry = lane_registry();
            std::lock_guard registry_lock(registry.mutex);
            registry.active.emplace(lane.get(), lane);
        } catch (...) {
            stop_and_join_lane(lane);
            auto& registry = lane_registry();
            std::lock_guard registry_lock(registry.mutex);
            registry.active.erase(lane.get());
            worker->lanes.erase(key);
            return SAO_STATUS_ERR_UNKNOWN;
        }
        *out_lane = lane.get();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_lane_submit_compose(
    sao_ui_render_lane_handle_t lane, sao_ui_compose_fn_t fn,
    void* user_data, double now_sec) {
    if (fn == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    LaneOperationLease operation(lane);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* active_lane = operation.get();

    try {
        std::lock_guard lock(active_lane->mtx);
        if (active_lane->stopped) return SAO_STATUS_ERR_CANCELLED;
        if (active_lane->in_flight != 0 && active_lane->worker != nullptr &&
            !active_lane->worker->queue_pending) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        active_lane->queue.push_back(LaneJob{fn, user_data, now_sec});
        ++active_lane->in_flight;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    active_lane->cv.notify_one();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_lane_try_take_frame(
    sao_ui_render_lane_handle_t lane, sao_ui_frame_buffer_handle_t* out_frame) {
    if (out_frame == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_frame = nullptr;
    LaneOperationLease operation(lane);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;

    std::lock_guard frame_lock(operation.get()->frame_mtx);
    if (operation.get()->pending_frame == nullptr) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    *out_frame = operation.get()->pending_frame.release();
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
    WorkerOperationLease operation(handle);
    return operation ? operation.get()->peak_wall_ms_recent.load() : 0.0;
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

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_submit(
    sao_ui_render_worker_handle_t handle,
    sao_ui_render_worker_task_fn_t task_fn,
    void* user_data) {
    if (task_fn == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    WorkerOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* worker = operation.get();
    try {
        std::lock_guard lock(worker->fan_mtx);
        if (worker->stop_requested) return SAO_STATUS_ERR_CANCELLED;
        worker->fan_queue.push_back(FanTask{task_fn, user_data});
        ++worker->fan_in_flight;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    worker->fan_cv.notify_one();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_render_worker_flush(
    sao_ui_render_worker_handle_t handle) {
    if (g_callback_worker == handle) return SAO_UI_STATUS_ERR_BUSY;
    WorkerOperationLease operation(handle);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* worker = operation.get();
    try {
        {
            std::unique_lock lock(worker->fan_mtx);
            worker->idle_cv.wait(lock, [worker] { return worker->fan_in_flight == 0; });
        }

        std::vector<std::shared_ptr<sao_ui_render_lane_s>> lanes;
        {
            std::lock_guard lock(worker->lanes_mtx);
            lanes.reserve(worker->lanes.size());
            for (const auto& [_, lane] : worker->lanes) lanes.push_back(lane);
        }
        for (const auto& lane : lanes) {
            std::unique_lock lock(lane->mtx);
            lane->idle_cv.wait(lock, [&lane] { return lane->in_flight == 0; });
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
