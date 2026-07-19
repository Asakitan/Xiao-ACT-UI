#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/dc_mutation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Forward-declare Wave 6 test-only helpers exported from
// dc_mutation.cpp with SAO_UI_API so the import symbols resolve
// against the DLL's export table when SAO_UI_USING_DLL is set for
// the test binary.
#include "sao/ui/abi.h"
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_dc_mut_test_dispatch_count(sao_ui_dc_mutation_coordinator_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_dc_mut_test_drain(sao_ui_dc_mutation_coordinator_handle_t handle, uint32_t timeout_ms);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mut_test_last_op(
    sao_ui_dc_mutation_coordinator_handle_t handle, char* out_op, size_t out_op_cap,
    char* out_method, size_t out_method_cap, char* out_args, size_t out_args_cap);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_dc_mut_test_failed_dispatch_count(sao_ui_dc_mutation_coordinator_handle_t handle);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dc_mut_test_last_dispatch_status(sao_ui_dc_mutation_coordinator_handle_t handle);
extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_dc_mut_test_worker_thread_id(sao_ui_dc_mutation_coordinator_handle_t handle);
extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_dc_mut_test_owner_execution_thread_id(sao_ui_dc_mutation_coordinator_handle_t handle);
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_dc_mut_test_owner_request_count(void);

namespace {

sao_status_t submit(sao_ui_dc_mutation_coordinator_handle_t coordinator, void* hwnd,
                    const char* operation, const char* method, const char* args) {
    return sao_ui_dc_mutation_coordinator_submit_dc(coordinator, hwnd, operation, method,
                                                    reinterpret_cast<const uint8_t*>(args),
                                                    std::strlen(args));
}

#if defined(_WIN32)

constexpr UINT kPauseOwnerThread = WM_APP + 0x245;

class PumpingWindow {
  public:
    PumpingWindow()
        : class_name_(L"SaoDcMutationWindow_" + std::to_wstring(reinterpret_cast<uintptr_t>(this))),
          thread_([this] { thread_main(); }) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_cv_.wait(lock, [this] { return ready_; });
    }

    ~PumpingWindow() {
        close();
    }

    PumpingWindow(const PumpingWindow&) = delete;
    PumpingWindow& operator=(const PumpingWindow&) = delete;

    HWND hwnd() const {
        return hwnd_.load(std::memory_order_acquire);
    }

    DWORD owner_thread_id() const {
        return owner_thread_id_.load(std::memory_order_acquire);
    }

    DWORD last_window_pos_thread_id() const {
        return last_window_pos_thread_id_.load(std::memory_order_acquire);
    }

    void clear_last_window_pos_thread_id() {
        last_window_pos_thread_id_.store(0, std::memory_order_release);
    }

    uint32_t non_client_calc_count() const {
        return non_client_calc_count_.load(std::memory_order_acquire);
    }

    void clear_non_client_calc_count() {
        non_client_calc_count_.store(0, std::memory_order_release);
    }

    bool pause() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pause_requested_ = true;
        }
        if (!::PostThreadMessageW(owner_thread_id(), kPauseOwnerThread, 0, 0)) {
            return false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        return pause_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return paused_; });
    }

    void resume() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pause_requested_ = false;
        }
        pause_cv_.notify_all();
    }

    void close() {
        resume();
        const HWND current = hwnd();
        if (current != nullptr)
            ::PostMessageW(current, WM_CLOSE, 0, 0);
        if (thread_.joinable())
            thread_.join();
    }

  private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PumpingWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<PumpingWindow*>(create->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr && message == WM_WINDOWPOSCHANGED) {
            self->last_window_pos_thread_id_.store(::GetCurrentThreadId(),
                                                   std::memory_order_release);
        }
        if (self != nullptr && message == WM_NCCALCSIZE) {
            self->non_client_calc_count_.fetch_add(1, std::memory_order_acq_rel);
        }
        if (message == WM_CLOSE) {
            ::DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            if (self != nullptr) {
                self->hwnd_.store(nullptr, std::memory_order_release);
            }
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void thread_main() {
        owner_thread_id_.store(::GetCurrentThreadId(), std::memory_order_release);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &PumpingWindow::window_proc;
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name_.c_str();
        const ATOM atom = ::RegisterClassExW(&window_class);
        HWND created = nullptr;
        if (atom != 0) {
            created = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                                        class_name_.c_str(), L"dc mutation test", WS_POPUP, 20, 30,
                                        80, 60, nullptr, nullptr, window_class.hInstance, this);
        }
        hwnd_.store(created, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = true;
        }
        ready_cv_.notify_all();

        if (created != nullptr) {
            MSG message{};
            while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (message.message == kPauseOwnerThread) {
                    std::unique_lock<std::mutex> lock(mutex_);
                    paused_ = true;
                    pause_cv_.notify_all();
                    pause_cv_.wait(lock, [this] { return !pause_requested_; });
                    paused_ = false;
                    continue;
                }
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }
        if (created != nullptr && ::IsWindow(created))
            ::DestroyWindow(created);
        if (atom != 0) {
            ::UnregisterClassW(class_name_.c_str(), window_class.hInstance);
        }
        hwnd_.store(nullptr, std::memory_order_release);
    }

    std::wstring class_name_;
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<DWORD> owner_thread_id_{0};
    std::atomic<DWORD> last_window_pos_thread_id_{0};
    std::atomic<uint32_t> non_client_calc_count_{0};
    std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::condition_variable pause_cv_;
    bool ready_ = false;
    bool pause_requested_ = false;
    bool paused_ = false;
};

bool wait_for_inflight(sao_ui_dc_mutation_coordinator_handle_t coordinator) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        SaoDcMutationStats stats{};
        if (sao_ui_dc_mutation_coordinator_stats(coordinator, &stats) == SAO_STATUS_OK &&
            stats.inflight_operations != 0) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool wait_for_owner_request() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (sao_ui_dc_mut_test_owner_request_count() != 0)
            return true;
        std::this_thread::yield();
    }
    return false;
}

#endif

} // namespace

TEST_CASE("dc_mutation_create_destroy", "[ui][dc_mutation][wave6]") {
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(c != nullptr);

    SaoDcMutationStats stats{};
    REQUIRE(sao_ui_dc_mutation_coordinator_stats(c, &stats) == SAO_STATUS_OK);
    CHECK(stats.registered_hwnds == 0u);
    CHECK(stats.inflight_operations == 0u);
    CHECK(stats.queued_operations == 0u);
    CHECK(stats.total_invalidations == 0u);
    CHECK(stats.total_failed_invalidations == 0u);
    CHECK(stats.stale_blocks_active == 0u);

    sao_ui_dc_mutation_coordinator_destroy(c);
    // Also OK to destroy nullptr — no crash contract.
    sao_ui_dc_mutation_coordinator_destroy(nullptr);
}

#if defined(_WIN32)

TEST_CASE("dc mutation executor applies bounds on the HWND owner thread",
          "[ui][dc_mutation][wave6][win32]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);

    void* token = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), &token) == SAO_STATUS_OK);
    REQUIRE(token != nullptr);

    window.clear_last_window_pos_thread_id();
    const char* args = "{\"x\":120,\"y\":140,\"width\":300,\"height\":180}";
    REQUIRE(submit(c, window.hwnd(), "host-rect", "set_window_rect", args) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mut_test_drain(c, 2000));
    REQUIRE(sao_ui_dc_mut_test_dispatch_count(c) == 1u);
    CHECK(sao_ui_dc_mut_test_failed_dispatch_count(c) == 0u);
    CHECK(sao_ui_dc_mut_test_last_dispatch_status(c) == SAO_STATUS_OK);

    RECT rect{};
    REQUIRE(::GetWindowRect(window.hwnd(), &rect));
    CHECK(rect.left == 120);
    CHECK(rect.top == 140);
    CHECK(rect.right - rect.left == 300);
    CHECK(rect.bottom - rect.top == 180);
    REQUIRE(window.last_window_pos_thread_id() != 0);
    CHECK(window.last_window_pos_thread_id() == window.owner_thread_id());
    REQUIRE(sao_ui_dc_mut_test_worker_thread_id(c) != 0);
    CHECK(sao_ui_dc_mut_test_worker_thread_id(c) != window.owner_thread_id());

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation executor clears only requested exstyle bits",
          "[ui][dc_mutation][wave6][win32]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), nullptr) == SAO_STATUS_OK);
    window.clear_non_client_calc_count();

    const uint32_t mask = WS_EX_TOPMOST | WS_EX_LAYERED;
    const std::string args = "{\"mask\":" + std::to_string(mask) + "}";
    REQUIRE(submit(c, window.hwnd(), "host-exstyle", "hide_exstyle", args.c_str()) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mut_test_drain(c, 2000));
    REQUIRE(sao_ui_dc_mut_test_dispatch_count(c) == 1u);

    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR exstyle = ::GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE);
    REQUIRE((exstyle != 0 || ::GetLastError() == ERROR_SUCCESS));
    CHECK((exstyle & static_cast<LONG_PTR>(mask)) == 0);
    CHECK((exstyle & WS_EX_TOOLWINDOW) != 0);
    CHECK(window.non_client_calc_count() != 0u);
    CHECK(sao_ui_dc_mut_test_owner_execution_thread_id(c) == window.owner_thread_id());
    CHECK(sao_ui_dc_mut_test_owner_execution_thread_id(c) !=
          sao_ui_dc_mut_test_worker_thread_id(c));

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation rejects unknown and malformed work before enqueue",
          "[ui][dc_mutation][wave6][validation]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), nullptr) == SAO_STATUS_OK);

    const char* valid = "{\"x\":1,\"y\":2,\"width\":3,\"height\":4}";
    CHECK(submit(c, window.hwnd(), "unknown-op", "set_window_rect", valid) ==
          SAO_STATUS_ERR_NOT_IMPLEMENTED);
    const char* malformed = "{\"x\":1,\"y\":2,\"width\":3}";
    CHECK(submit(c, window.hwnd(), "host-rect", "set_window_rect", malformed) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_dc_mutation_coordinator_submit_dc(c, window.hwnd(), "host-exstyle", "hide_exstyle",
                                                   nullptr, 0) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_dc_mut_test_dispatch_count(c) == 0u);
    CHECK(sao_ui_dc_mut_test_failed_dispatch_count(c) == 0u);

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation execution failure never records success",
          "[ui][dc_mutation][wave6][fail_closed]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    const HWND stale_hwnd = window.hwnd();
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, stale_hwnd, nullptr) == SAO_STATUS_OK);
    window.close();

    const char* args = "{\"x\":10,\"y\":20,\"width\":30,\"height\":40}";
    REQUIRE(submit(c, stale_hwnd, "host-rect", "set_bounds", args) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mut_test_drain(c, 2000));
    CHECK(sao_ui_dc_mut_test_dispatch_count(c) == 0u);
    CHECK(sao_ui_dc_mut_test_failed_dispatch_count(c) == 1u);
    CHECK(sao_ui_dc_mut_test_last_dispatch_status(c) == SAO_STATUS_ERR_HANDLE_INVALID);

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation pending work coalesces behind one inflight operation",
          "[ui][dc_mutation][wave6][coalesce]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), nullptr) == SAO_STATUS_OK);
    REQUIRE(window.pause());

    const char* first = "{\"x\":40,\"y\":50,\"width\":100,\"height\":110}";
    const char* second = "{\"x\":60,\"y\":70,\"width\":120,\"height\":130}";
    const char* last = "{\"x\":80,\"y\":90,\"width\":140,\"height\":150}";
    REQUIRE(submit(c, window.hwnd(), "host-rect", "set_bounds", first) == SAO_STATUS_OK);
    REQUIRE(wait_for_inflight(c));
    REQUIRE(submit(c, window.hwnd(), "host-rect", "set_bounds", second) == SAO_STATUS_OK);
    REQUIRE(submit(c, window.hwnd(), "host-rect", "set_bounds", last) == SAO_STATUS_OK);
    SaoDcMutationStats stats{};
    REQUIRE(sao_ui_dc_mutation_coordinator_stats(c, &stats) == SAO_STATUS_OK);
    CHECK(stats.inflight_operations == 1u);
    CHECK(stats.queued_operations == 1u);
    window.resume();

    REQUIRE(sao_ui_dc_mut_test_drain(c, 2000));
    CHECK(sao_ui_dc_mut_test_dispatch_count(c) == 2u);

    char op_buf[64]{};
    char method_buf[64]{};
    char args_buf[128]{};
    REQUIRE(sao_ui_dc_mut_test_last_op(c, op_buf, sizeof(op_buf), method_buf, sizeof(method_buf),
                                       args_buf, sizeof(args_buf)));
    CHECK(std::string(op_buf) == "host-rect");
    CHECK(std::string(method_buf) == "set_bounds");
    CHECK(std::string(args_buf) == last);

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation destroy drains admitted work and joins its worker",
          "[ui][dc_mutation][wave6][shutdown]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), nullptr) == SAO_STATUS_OK);
    REQUIRE(window.pause());

    const char* args = "{\"x\":160,\"y\":170,\"width\":210,\"height\":220}";
    REQUIRE(submit(c, window.hwnd(), "host-rect", "set_bounds", args) == SAO_STATUS_OK);
    REQUIRE(wait_for_inflight(c));

    std::atomic<bool> destroy_finished{false};
    std::thread destroy_thread([&] {
        sao_ui_dc_mutation_coordinator_destroy(c);
        destroy_finished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(destroy_finished.load(std::memory_order_acquire));

    window.resume();
    destroy_thread.join();
    REQUIRE(destroy_finished.load(std::memory_order_acquire));

    RECT rect{};
    REQUIRE(::GetWindowRect(window.hwnd(), &rect));
    CHECK(rect.left == 160);
    CHECK(rect.top == 170);
    CHECK(rect.right - rect.left == 210);
    CHECK(rect.bottom - rect.top == 220);
}

TEST_CASE("dc mutation invalidate revokes generation and allows reregister",
          "[ui][dc_mutation][wave6][invalidate]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);

    void* token = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), &token) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_invalidate(c, window.hwnd(), 1.0));

    SaoDcMutationStats stats{};
    REQUIRE(sao_ui_dc_mutation_coordinator_stats(c, &stats) == SAO_STATUS_OK);
    CHECK(stats.total_invalidations == 1u);
    CHECK(stats.stale_blocks_active == 0u);
    CHECK(stats.registered_hwnds == 0u);

    const char* args = "{\"mask\":8}";
    REQUIRE(submit(c, window.hwnd(), "host-exstyle", "hide_exstyle", args) ==
            SAO_STATUS_ERR_ACCESS_DENIED);

    void* token2 = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), &token2) == SAO_STATUS_OK);
    REQUIRE(token2 != nullptr);
    CHECK(token2 != token);
    CHECK_FALSE(sao_ui_dc_mutation_coordinator_clear_failed(c, window.hwnd()));

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation timeout tombstones the old generation until it exits",
          "[ui][dc_mutation][wave6][generation][timeout]") {
    PumpingWindow window;
    REQUIRE(window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), nullptr) == SAO_STATUS_OK);
    REQUIRE(window.pause());

    const char* args = "{\"x\":210,\"y\":220,\"width\":230,\"height\":240}";
    REQUIRE(submit(c, window.hwnd(), "host-rect", "set_bounds", args) == SAO_STATUS_OK);
    REQUIRE(wait_for_inflight(c));
    REQUIRE(wait_for_owner_request());
    CHECK_FALSE(sao_ui_dc_mutation_coordinator_invalidate(c, window.hwnd(), 0.0));

    SaoDcMutationStats stats{};
    REQUIRE(sao_ui_dc_mutation_coordinator_stats(c, &stats) == SAO_STATUS_OK);
    CHECK(stats.stale_blocks_active == 1u);
    CHECK(stats.total_failed_invalidations == 1u);
    CHECK_FALSE(sao_ui_dc_mutation_coordinator_clear_failed(c, window.hwnd()));
    CHECK_FALSE(sao_ui_dc_mutation_coordinator_invalidate(c, window.hwnd(), 0.0));
    void* replacement_token = nullptr;
    CHECK(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), &replacement_token) ==
          SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(replacement_token == nullptr);

    window.resume();
    REQUIRE(sao_ui_dc_mut_test_drain(c, 2000));
    CHECK(sao_ui_dc_mutation_coordinator_clear_failed(c, window.hwnd()));
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window.hwnd(), &replacement_token) ==
            SAO_STATUS_OK);
    CHECK(replacement_token != nullptr);

    sao_ui_dc_mutation_coordinator_destroy(c);
}

TEST_CASE("dc mutation shutdown cancels queued generations and waits only for current work",
          "[ui][dc_mutation][wave6][shutdown][bounded]") {
    PumpingWindow blocked_window;
    REQUIRE(blocked_window.hwnd() != nullptr);
    sao_ui_dc_mutation_coordinator_handle_t c = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create(&c) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dc_mutation_coordinator_register(c, blocked_window.hwnd(), nullptr) ==
            SAO_STATUS_OK);
    REQUIRE(blocked_window.pause());

    std::vector<std::unique_ptr<PumpingWindow>> queued_windows;
    for (int index = 0; index < 4; ++index) {
        auto window = std::make_unique<PumpingWindow>();
        REQUIRE(window->hwnd() != nullptr);
        REQUIRE(sao_ui_dc_mutation_coordinator_register(c, window->hwnd(), nullptr) ==
                SAO_STATUS_OK);
        REQUIRE(window->pause());
        queued_windows.push_back(std::move(window));
    }

    const char* blocked_args = "{\"x\":310,\"y\":320,\"width\":330,\"height\":340}";
    REQUIRE(submit(c, blocked_window.hwnd(), "host-rect", "set_bounds", blocked_args) ==
            SAO_STATUS_OK);
    REQUIRE(wait_for_inflight(c));

    const uint32_t mask = WS_EX_LAYERED;
    const std::string queued_args = "{\"mask\":" + std::to_string(mask) + "}";
    for (const auto& window : queued_windows) {
        REQUIRE(submit(c, window->hwnd(), "host-exstyle", "hide_exstyle", queued_args.c_str()) ==
                SAO_STATUS_OK);
    }

    const auto started = std::chrono::steady_clock::now();
    sao_ui_dc_mutation_coordinator_destroy(c);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(900));

    for (const auto& window : queued_windows) {
        ::SetLastError(ERROR_SUCCESS);
        const LONG_PTR exstyle = ::GetWindowLongPtrW(window->hwnd(), GWL_EXSTYLE);
        REQUIRE((exstyle != 0 || ::GetLastError() == ERROR_SUCCESS));
        CHECK((exstyle & WS_EX_LAYERED) != 0);
        window->resume();
    }
    blocked_window.resume();
}

#else

TEST_CASE("dc mutation executor reports missing non-Windows capability",
          "[ui][dc_mutation][wave6]") {
    SUCCEED("Win32 executor coverage runs on Windows");
}

#endif
