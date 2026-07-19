// Wave 4 tests for the render worker first-implementable slice (G1.5b).
//
// Coverage:
//   * render_worker_submit_runs_task          - task_fn is invoked on the pool.
//   * render_worker_pool_size_2_runs_in_parallel — two 100ms tasks < 200ms wall.
//   * render_worker_flush_waits_for_all       - flush blocks until in_flight==0.
//   * render_worker_lane_compose_produces_frame — per-lane compose round trip.
//
#include <catch2/catch_test_macros.hpp>

#include "sao/ui/render_worker.h"
#include "sao/core/status.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

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

using namespace std::chrono_literals;

void SAO_UI_CALL bump_counter(void* user_data) {
    auto* c = reinterpret_cast<std::atomic<int>*>(user_data);
    c->fetch_add(1);
}

void SAO_UI_CALL sleep_100ms(void* user_data) {
    auto* c = reinterpret_cast<std::atomic<int>*>(user_data);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c->fetch_add(1);
}

SaoRenderWorkerConfig make_worker_config(int pool_size) {
    SaoRenderWorkerConfig cfg{};
    cfg.lane_count = 0;
    cfg.task_pool_size = pool_size;
    cfg.frame_buffer_pool_size = 0;
    cfg.queue_pending = true;
    return cfg;
}

struct BlockingProbe {
    std::mutex mutex;
    std::condition_variable condition;
    int entered = 0;
    int completed = 0;
    bool released = false;
};

void wait_in_probe(BlockingProbe& probe) {
    std::unique_lock lock(probe.mutex);
    ++probe.entered;
    probe.condition.notify_all();
    probe.condition.wait(lock, [&probe] { return probe.released; });
    ++probe.completed;
}

void SAO_UI_CALL blocking_task(void* user_data) {
    wait_in_probe(*static_cast<BlockingProbe*>(user_data));
}

struct DerivedFanoutProbe {
    sao_ui_render_worker_handle_t worker{};
    BlockingProbe parent;
    BlockingProbe child;
    std::atomic<sao_status_t> submit_status{SAO_STATUS_ERR_UNKNOWN};
};

sao_ui_frame_buffer_handle_t SAO_UI_CALL submit_derived_fanout(
    double, void* user_data) {
    auto* probe = static_cast<DerivedFanoutProbe*>(user_data);
    wait_in_probe(probe->parent);
    probe->submit_status.store(sao_ui_render_worker_submit(
        probe->worker, &blocking_task, &probe->child));
    return nullptr;
}

sao_ui_frame_buffer_handle_t SAO_UI_CALL blocking_compose(double, void* user_data) {
    wait_in_probe(*static_cast<BlockingProbe*>(user_data));
    return nullptr;
}

void wait_for_entries(BlockingProbe& probe, int expected) {
    std::unique_lock lock(probe.mutex);
    REQUIRE(probe.condition.wait_for(lock, std::chrono::seconds(2),
                                     [&probe, expected] { return probe.entered == expected; }));
}

void release_probe(BlockingProbe& probe) {
    {
        std::lock_guard lock(probe.mutex);
        probe.released = true;
    }
    probe.condition.notify_all();
}

void SAO_UI_CALL throwing_task(void*) {
    throw std::runtime_error("fan task fixture");
}

sao_ui_frame_buffer_handle_t SAO_UI_CALL throwing_compose(double, void*) {
    throw std::runtime_error("lane compose fixture");
}

sao_ui_frame_buffer_handle_t SAO_UI_CALL bump_compose(double, void* user_data) {
    bump_counter(user_data);
    return nullptr;
}

struct ReentryProbe {
    sao_ui_render_worker_handle_t worker = nullptr;
    std::atomic<sao_status_t> flush_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> destroy_status{SAO_STATUS_OK};
    std::atomic_int calls{0};
};

void invoke_reentry(ReentryProbe& probe) {
    probe.flush_status.store(sao_ui_render_worker_flush(probe.worker));
    probe.destroy_status.store(sao_ui_render_worker_destroy(probe.worker));
    probe.calls.fetch_add(1);
}

void SAO_UI_CALL reentrant_task(void* user_data) {
    invoke_reentry(*static_cast<ReentryProbe*>(user_data));
}

sao_ui_frame_buffer_handle_t SAO_UI_CALL reentrant_compose(double, void* user_data) {
    invoke_reentry(*static_cast<ReentryProbe*>(user_data));
    return nullptr;
}

}  // namespace

TEST_CASE("render_worker_submit_runs_task",
          "[ui][render_worker][wave4]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(2);
    sao_ui_render_worker_handle_t w = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &w) == SAO_STATUS_OK);
    REQUIRE(w != nullptr);

    std::atomic<int> counter{0};
    REQUIRE(sao_ui_render_worker_submit(w, &bump_counter, &counter) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_submit(w, &bump_counter, &counter) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_submit(w, &bump_counter, &counter) == SAO_STATUS_OK);

    REQUIRE(sao_ui_render_worker_flush(w) == SAO_STATUS_OK);
    CHECK(counter.load() == 3);

    REQUIRE(sao_ui_render_worker_destroy(w) == SAO_STATUS_OK);
}

TEST_CASE("render_worker_pool_size_2_runs_in_parallel",
          "[ui][render_worker][wave4]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(2);
    sao_ui_render_worker_handle_t w = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &w) == SAO_STATUS_OK);

    std::atomic<int> counter{0};

    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(sao_ui_render_worker_submit(w, &sleep_100ms, &counter) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_submit(w, &sleep_100ms, &counter) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_flush(w) == SAO_STATUS_OK);
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("elapsed_ms=" << elapsed_ms);
    // Serialized would be ~200ms; parallel on 2-thread pool must be < 200ms.
    // Give generous slack (180ms) to survive CI wobble but still catch
    // "pool is actually 1 thread" regressions.
    CHECK(elapsed_ms < 180.0);
    CHECK(counter.load() == 2);

    REQUIRE(sao_ui_render_worker_destroy(w) == SAO_STATUS_OK);
}

TEST_CASE("render_worker_flush_waits_for_all",
          "[ui][render_worker][wave4]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(4);
    sao_ui_render_worker_handle_t w = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &w) == SAO_STATUS_OK);

    std::atomic<int> counter{0};
    constexpr int kTaskCount = 32;
    for (int i = 0; i < kTaskCount; ++i) {
        REQUIRE(sao_ui_render_worker_submit(w, &bump_counter, &counter) == SAO_STATUS_OK);
    }
    REQUIRE(sao_ui_render_worker_flush(w) == SAO_STATUS_OK);
    // After flush, every task must have completed.
    CHECK(counter.load() == kTaskCount);

    // Second flush on an empty queue must be a no-op.
    REQUIRE(sao_ui_render_worker_flush(w) == SAO_STATUS_OK);

    REQUIRE(sao_ui_render_worker_destroy(w) == SAO_STATUS_OK);
}

TEST_CASE("render_worker_flush_waits_for_lane_derived_fanout",
          "[ui][render_worker][wave4][generation][concurrency]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(2);
    sao_ui_render_worker_handle_t worker = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &worker) == SAO_STATUS_OK);
    sao_ui_render_lane_handle_t lane = nullptr;
    REQUIRE(sao_ui_render_worker_get_lane(worker, "derived-fanout", &lane) ==
            SAO_STATUS_OK);

    BlockingProbe cutoff_probe;
    REQUIRE(sao_ui_render_worker_submit(
                worker, &blocking_task, &cutoff_probe) == SAO_STATUS_OK);
    wait_for_entries(cutoff_probe, 1);

    DerivedFanoutProbe probe{worker};
    REQUIRE(sao_ui_render_lane_submit_compose(
                lane, &submit_derived_fanout, &probe, 0.0) == SAO_STATUS_OK);
    wait_for_entries(probe.parent, 1);

    auto flush = std::async(std::launch::async, [worker] {
        return sao_ui_render_worker_flush(worker);
    });
    REQUIRE(flush.wait_for(20ms) == std::future_status::timeout);

    release_probe(cutoff_probe);
    release_probe(probe.parent);
    wait_for_entries(probe.child, 1);
    CHECK(flush.wait_for(50ms) == std::future_status::timeout);
    CHECK(probe.submit_status.load() == SAO_STATUS_OK);

    release_probe(probe.child);
    REQUIRE(flush.get() == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_destroy(worker) == SAO_STATUS_OK);
}

TEST_CASE("render_worker_destroy_retires_handles_and_drains_every_accepted_job",
          "[ui][render_worker][wave4][destroy][concurrency]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(2);
    sao_ui_render_worker_handle_t worker = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &worker) == SAO_STATUS_OK);

    sao_ui_render_lane_handle_t lane = nullptr;
    REQUIRE(sao_ui_render_worker_get_lane(worker, "destroy-race", &lane) == SAO_STATUS_OK);

    BlockingProbe fan_probe;
    BlockingProbe lane_probe;
    int accepted_fan = 2;
    int accepted_lane = 1;
    REQUIRE(sao_ui_render_worker_submit(worker, blocking_task, &fan_probe) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_submit(worker, blocking_task, &fan_probe) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_lane_submit_compose(lane, blocking_compose, &lane_probe, 0.0) ==
            SAO_STATUS_OK);
    wait_for_entries(fan_probe, 2);
    wait_for_entries(lane_probe, 1);

    std::atomic_bool lane_lookup_started{false};
    auto lane_lookup = std::async(std::launch::async, [&]() -> sao_status_t {
        lane_lookup_started.store(true);
        for (;;) {
            sao_ui_render_lane_handle_t current = nullptr;
            const auto status =
                sao_ui_render_worker_get_lane(worker, "destroy-race", &current);
            if (status != SAO_STATUS_OK) return status;
            if (current != lane) return SAO_STATUS_ERR_UNKNOWN;
            std::this_thread::yield();
        }
    });
    std::atomic_bool take_started{false};
    auto take = std::async(std::launch::async, [&]() -> sao_status_t {
        take_started.store(true);
        for (;;) {
            sao_ui_frame_buffer_handle_t frame = nullptr;
            const auto status = sao_ui_render_lane_try_take_frame(lane, &frame);
            if (status == SAO_STATUS_ERR_HANDLE_INVALID) return status;
            if (status != SAO_STATUS_ERR_NOT_FOUND || frame != nullptr) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
            std::this_thread::yield();
        }
    });
    while (!lane_lookup_started.load() || !take_started.load()) std::this_thread::yield();

    auto flush = std::async(std::launch::async,
                            [worker] { return sao_ui_render_worker_flush(worker); });
    REQUIRE(flush.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    auto destroy = std::async(std::launch::async,
                              [worker] { return sao_ui_render_worker_destroy(worker); });

    for (;;) {
        const auto status = sao_ui_render_worker_submit(worker, blocking_task, &fan_probe);
        if (status == SAO_STATUS_OK) {
            ++accepted_fan;
            std::this_thread::yield();
            continue;
        }
        REQUIRE(status == SAO_STATUS_ERR_HANDLE_INVALID);
        break;
    }
    for (;;) {
        const auto status =
            sao_ui_render_lane_submit_compose(lane, blocking_compose, &lane_probe, 0.0);
        if (status == SAO_STATUS_OK) {
            ++accepted_lane;
            std::this_thread::yield();
            continue;
        }
        REQUIRE(status == SAO_STATUS_ERR_HANDLE_INVALID);
        break;
    }
    CHECK(lane_lookup.get() == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(take.get() == SAO_STATUS_ERR_HANDLE_INVALID);

    sao_ui_render_lane_handle_t stale_lane = reinterpret_cast<sao_ui_render_lane_handle_t>(1);
    CHECK(sao_ui_render_worker_get_lane(worker, "retired", &stale_lane) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(stale_lane == nullptr);
    sao_ui_frame_buffer_handle_t stale_frame = reinterpret_cast<sao_ui_frame_buffer_handle_t>(1);
    CHECK(sao_ui_render_lane_try_take_frame(lane, &stale_frame) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(stale_frame == nullptr);
    CHECK(sao_ui_render_worker_flush(worker) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(destroy.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

    release_probe(fan_probe);
    release_probe(lane_probe);
    REQUIRE(flush.get() == SAO_STATUS_OK);
    REQUIRE(destroy.get() == SAO_STATUS_OK);
    {
        std::lock_guard lock(fan_probe.mutex);
        CHECK(fan_probe.completed == accepted_fan);
    }
    {
        std::lock_guard lock(lane_probe.mutex);
        CHECK(lane_probe.completed == accepted_lane);
    }
    CHECK(sao_ui_render_worker_destroy(worker) == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("render_worker_callback_exceptions_restore_completion_state",
          "[ui][render_worker][wave4][exception]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(2);
    sao_ui_render_worker_handle_t worker = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &worker) == SAO_STATUS_OK);
    sao_ui_render_lane_handle_t lane = nullptr;
    REQUIRE(sao_ui_render_worker_get_lane(worker, "throwing-lane", &lane) == SAO_STATUS_OK);

    std::atomic_int fan_completed{0};
    std::atomic_int lane_completed{0};
    REQUIRE(sao_ui_render_worker_submit(worker, throwing_task, nullptr) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_worker_submit(worker, bump_counter, &fan_completed) == SAO_STATUS_OK);
    REQUIRE(sao_ui_render_lane_submit_compose(lane, throwing_compose, nullptr, 0.0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_render_lane_submit_compose(lane, bump_compose, &lane_completed, 0.0) ==
            SAO_STATUS_OK);

    REQUIRE(sao_ui_render_worker_flush(worker) == SAO_STATUS_OK);
    CHECK(fan_completed.load() == 1);
    CHECK(lane_completed.load() == 1);
    REQUIRE(sao_ui_render_worker_destroy(worker) == SAO_STATUS_OK);
}

TEST_CASE("render_worker_callbacks_do_not_wait_or_join_their_own_worker",
          "[ui][render_worker][wave4][reentry]") {
    SECTION("fan-out callback") {
        const SaoRenderWorkerConfig cfg = make_worker_config(2);
        sao_ui_render_worker_handle_t worker = nullptr;
        REQUIRE(sao_ui_render_worker_create(&cfg, &worker) == SAO_STATUS_OK);
        ReentryProbe probe;
        probe.worker = worker;

        REQUIRE(sao_ui_render_worker_submit(worker, reentrant_task, &probe) == SAO_STATUS_OK);
        REQUIRE(sao_ui_render_worker_flush(worker) == SAO_STATUS_OK);
        CHECK(probe.calls.load() == 1);
        CHECK(probe.flush_status.load() == SAO_UI_STATUS_ERR_BUSY);
        CHECK(probe.destroy_status.load() == SAO_UI_STATUS_ERR_BUSY);
        REQUIRE(sao_ui_render_worker_destroy(worker) == SAO_STATUS_OK);
    }

    SECTION("lane callback") {
        const SaoRenderWorkerConfig cfg = make_worker_config(2);
        sao_ui_render_worker_handle_t worker = nullptr;
        REQUIRE(sao_ui_render_worker_create(&cfg, &worker) == SAO_STATUS_OK);
        sao_ui_render_lane_handle_t lane = nullptr;
        REQUIRE(sao_ui_render_worker_get_lane(worker, "reentrant-lane", &lane) == SAO_STATUS_OK);
        ReentryProbe probe;
        probe.worker = worker;

        REQUIRE(sao_ui_render_lane_submit_compose(lane, reentrant_compose, &probe, 0.0) ==
                SAO_STATUS_OK);
        REQUIRE(sao_ui_render_worker_flush(worker) == SAO_STATUS_OK);
        CHECK(probe.calls.load() == 1);
        CHECK(probe.flush_status.load() == SAO_UI_STATUS_ERR_BUSY);
        CHECK(probe.destroy_status.load() == SAO_UI_STATUS_ERR_BUSY);
        REQUIRE(sao_ui_render_worker_destroy(worker) == SAO_STATUS_OK);
    }
}

namespace {

sao_ui_frame_buffer_handle_t SAO_UI_CALL make_16x16_frame(double now_sec, void* user_data) {
    auto* run_marker = reinterpret_cast<std::atomic<int>*>(user_data);
    run_marker->fetch_add(1);
    (void)now_sec;
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;
    uint8_t rgba[W * H * 4];
    for (uint32_t i = 0; i < W * H; ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 0;
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 200;
    }
    uint8_t* out = nullptr;
    size_t out_size = 0;
    if (sao_ui_render_worker_premultiply_rgba_to_bgra(rgba, W, H, &out, &out_size) != SAO_STATUS_OK) {
        return nullptr;
    }
    sao_ui_frame_buffer_handle_t frame = nullptr;
    const auto status = sao_ui_frame_buffer_create_bgra(
        out, out_size, W, H, 11, 22, &frame);
    std::free(out);
    return status == SAO_STATUS_OK ? frame : nullptr;
}

}  // namespace

TEST_CASE("render_worker_lane_compose_produces_frame",
          "[ui][render_worker][wave4]") {
    const SaoRenderWorkerConfig cfg = make_worker_config(2);
    sao_ui_render_worker_handle_t w = nullptr;
    REQUIRE(sao_ui_render_worker_create(&cfg, &w) == SAO_STATUS_OK);

    sao_ui_render_lane_handle_t lane = nullptr;
    REQUIRE(sao_ui_render_worker_get_lane(w, "overlay-A", &lane) == SAO_STATUS_OK);
    REQUIRE(lane != nullptr);

    std::atomic<int> run_marker{0};
    REQUIRE(sao_ui_render_lane_submit_compose(lane, &make_16x16_frame, &run_marker, 0.0) == SAO_STATUS_OK);

    REQUIRE(sao_ui_render_worker_flush(w) == SAO_STATUS_OK);
    CHECK(run_marker.load() == 1);

    sao_ui_frame_buffer_handle_t frame = nullptr;
    REQUIRE(sao_ui_render_lane_try_take_frame(lane, &frame) == SAO_STATUS_OK);
    REQUIRE(frame != nullptr);
    SaoFrameBufferView view{};
    REQUIRE(sao_ui_frame_buffer_view(frame, &view) == SAO_STATUS_OK);
    CHECK(view.width == 16);
    CHECK(view.height == 16);
    CHECK(view.x == 11);
    CHECK(view.y == 22);
    sao_ui_frame_buffer_release(frame);
    CHECK(sao_ui_render_lane_try_take_frame(lane, &frame) == SAO_STATUS_ERR_NOT_FOUND);

    REQUIRE(sao_ui_render_worker_destroy(w) == SAO_STATUS_OK);
}

TEST_CASE("render_worker_premultiply_helper_round_trip",
          "[ui][render_worker][wave4]") {
    // Straight from the header: RGBA in → premultiplied BGRA out.
    constexpr uint32_t W = 4;
    constexpr uint32_t H = 4;
    uint8_t rgba[W * H * 4];
    for (uint32_t i = 0; i < W * H; ++i) {
        rgba[i * 4 + 0] = 255;   // R
        rgba[i * 4 + 1] = 100;   // G
        rgba[i * 4 + 2] = 50;    // B
        rgba[i * 4 + 3] = 128;   // A ~ half
    }
    uint8_t* out = nullptr;
    size_t out_size = 0;
    REQUIRE(sao_ui_render_worker_premultiply_rgba_to_bgra(rgba, W, H, &out, &out_size) == SAO_STATUS_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_size == W * H * 4);

    // Channel order: R→2, G→1, B→0.
    // premul(c) = (c * a + 127) / 255 for a=128
    // R=255 → premul=(255*128+127)/255 = 128
    // G=100 → premul=(100*128+127)/255 = 50
    // B=50  → premul=(50*128+127)/255 = 25
    for (uint32_t i = 0; i < W * H; ++i) {
        CHECK(out[i * 4 + 0] == 25);   // B
        CHECK(out[i * 4 + 1] == 50);   // G
        CHECK(out[i * 4 + 2] == 128);  // R
        CHECK(out[i * 4 + 3] == 128);  // A untouched
    }

    std::free(out);
}

#if defined(_WIN32)

TEST_CASE("render_worker_ulw_commit_updates_a_real_layered_window",
          "[ui][render_worker][wave4][ulw]") {
    constexpr std::array<uint8_t, 16> pixels{
        0x00, 0x00, 0xFF, 0xFF, 0x00, 0x80, 0x00, 0x80,
        0x40, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    sao_ui_frame_buffer_handle_t frame = nullptr;
    REQUIRE(sao_ui_frame_buffer_create_bgra(pixels.data(), pixels.size(), 2, 2, 40, 50,
                                             &frame) == SAO_STATUS_OK);
    REQUIRE(frame != nullptr);

    const HWND layered = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                            L"STATIC", L"ULW commit fixture", WS_POPUP,
                                            0, 0, 1, 1, nullptr, nullptr,
                                            ::GetModuleHandleW(nullptr), nullptr);
    REQUIRE(layered != nullptr);
    REQUIRE(sao_ui_render_worker_ulw_commit(layered, frame) == SAO_STATUS_OK);

    RECT rect{};
    REQUIRE(::GetWindowRect(layered, &rect) != FALSE);
    CHECK(rect.left == 40);
    CHECK(rect.top == 50);
    CHECK(rect.right - rect.left == 2);
    CHECK(rect.bottom - rect.top == 2);

    REQUIRE(::DestroyWindow(layered) != FALSE);
    sao_ui_frame_buffer_release(frame);
}

TEST_CASE("render_worker_ulw_commit_rejects_missing_or_nonlayered_targets",
          "[ui][render_worker][wave4][ulw][failure]") {
    constexpr std::array<uint8_t, 4> pixel{0x20, 0x10, 0x08, 0x40};
    sao_ui_frame_buffer_handle_t frame = nullptr;
    REQUIRE(sao_ui_frame_buffer_create_bgra(pixel.data(), pixel.size(), 1, 1, 0, 0,
                                             &frame) == SAO_STATUS_OK);
    REQUIRE(frame != nullptr);
    CHECK(sao_ui_render_worker_ulw_commit(nullptr, frame) == SAO_STATUS_ERR_HANDLE_INVALID);

    const HWND nonlayered = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"ULW failure fixture",
                                               WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                               ::GetModuleHandleW(nullptr), nullptr);
    REQUIRE(nonlayered != nullptr);
    CHECK(sao_ui_render_worker_ulw_commit(nonlayered, frame) ==
          SAO_STATUS_ERR_SURFACE_INVALID);
    REQUIRE(::DestroyWindow(nonlayered) != FALSE);
    sao_ui_frame_buffer_release(frame);

    sao_ui_frame_buffer_handle_t invalid = reinterpret_cast<sao_ui_frame_buffer_handle_t>(1);
    CHECK(sao_ui_frame_buffer_create_bgra(pixel.data(), pixel.size() - 1, 1, 1, 0, 0,
                                          &invalid) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(invalid == nullptr);
}

#endif
