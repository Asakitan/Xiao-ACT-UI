#include "tool_launch_internal.h"
#include "tool_launch_package_internal.h"

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#include "sao/core/logging.h"
#endif

#include <windows.h>
#include <dwmapi.h>
#include <process.h>

#include <algorithm>
#include <filesystem>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace sao::launcher::tool_launch {
namespace {

constexpr wchar_t kAiEditorWindowTitle[] = L"SAO AI Editor";
constexpr wchar_t kAiEditorLaunchMutex[] =
    L"Local\\SAO.Auto.AiEditor.Launch.v1";

sao_status_t map_os_error(DWORD error) noexcept {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return SAO_STATUS_ERR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    default:
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

class LaunchMutex final {
public:
    explicit LaunchMutex(HANDLE handle) noexcept : handle_(handle) {}
    ~LaunchMutex() {
        if (acquired_) {
            ReleaseMutex(handle_);
        }
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    LaunchMutex(const LaunchMutex&) = delete;
    LaunchMutex& operator=(const LaunchMutex&) = delete;

    void set_acquired() noexcept { acquired_ = true; }

private:
    HANDLE handle_{};
    bool acquired_{};
};

class OwnedHandle final {
public:
    explicit OwnedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~OwnedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    OwnedHandle(OwnedHandle&& other) noexcept : handle_(other.release()) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
private:
    HANDLE handle_{};
};

bool process_image_matches(HANDLE process,
                           const std::filesystem::path& expected) noexcept {
    if (process == nullptr) {
        return false;
    }
    wchar_t image[32768]{};
    DWORD length = static_cast<DWORD>(std::size(image));
    const bool matches = QueryFullProcessImageNameW(
                             process, 0, image, &length) != FALSE &&
                         _wcsicmp(image, expected.c_str()) == 0;
    return matches;
}

bool window_identity_matches(HWND hwnd, DWORD required_process_id) noexcept {
    if (!IsWindow(hwnd)) {
        return false;
    }
    wchar_t title[64]{};
    if (GetWindowTextW(hwnd, title, static_cast<int>(std::size(title))) <= 0 ||
        std::wcscmp(title, kAiEditorWindowTitle) != 0) {
        return false;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id == 0 || process_id != required_process_id) {
        return false;
    }
    return process_id != 0 && process_id == required_process_id;
}

bool window_is_cloaked(HWND hwnd) noexcept {
    DWORD cloaked = 0;
    const HRESULT cloak_status = DwmGetWindowAttribute(
        hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(cloak_status) && cloaked != 0;
}

bool window_responds(HWND hwnd) noexcept {
    DWORD_PTR response = 0;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, 200,
                               &response) != 0;
}

bool window_intersects_virtual_screen(const RECT& window) noexcept {
    constexpr LONG kMinimumVisible = 80;
    const LONG screen_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG screen_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG screen_right = screen_left +
        GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG screen_bottom = screen_top +
        GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return window.right >= screen_left + kMinimumVisible &&
           window.bottom >= screen_top + kMinimumVisible &&
           window.left <= screen_right - kMinimumVisible &&
           window.top <= screen_bottom - kMinimumVisible;
}

bool window_is_reusable(HWND hwnd, DWORD required_process_id) noexcept {
    return window_identity_matches(hwnd, required_process_id) &&
           !window_is_cloaked(hwnd) && window_responds(hwnd);
}

bool window_is_ready(HWND hwnd, DWORD required_process_id) noexcept {
    constexpr LONG kMinimumClientWidth = 320;
    constexpr LONG kMinimumClientHeight = 220;
    RECT client{};
    RECT window{};
    return IsWindowVisible(hwnd) &&
           window_is_reusable(hwnd, required_process_id) &&
           GetClientRect(hwnd, &client) &&
           client.right - client.left >= kMinimumClientWidth &&
           client.bottom - client.top >= kMinimumClientHeight &&
           GetWindowRect(hwnd, &window) &&
           window_intersects_virtual_screen(window) &&
           MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr;
}

struct WindowCandidate {
    HWND hwnd{};
    DWORD process_id{};
    OwnedHandle process;
};

struct WindowSearch {
    DWORD required_process_id{};
    const std::filesystem::path* expected_image{};
    bool require_ready{true};
    WindowCandidate candidate;
};

BOOL CALLBACK find_ai_editor_window(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id == 0 ||
        (search->required_process_id != 0 &&
         process_id != search->required_process_id) ||
        !(search->require_ready ? window_is_ready(hwnd, process_id)
                       : window_is_reusable(hwnd, process_id))) {
        return TRUE;
    }
    OwnedHandle process(OpenProcess(SYNCHRONIZE |
                                        PROCESS_QUERY_LIMITED_INFORMATION,
                                    FALSE, process_id));
    if (process.get() == nullptr ||
        WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT ||
        (search->expected_image != nullptr &&
         !process_image_matches(process.get(), *search->expected_image))) {
        return TRUE;
    }
    search->candidate.hwnd = hwnd;
    search->candidate.process_id = process_id;
    search->candidate.process = std::move(process);
    return FALSE;
}

HWND ai_editor_window(DWORD process_id) noexcept {
    WindowSearch search;
    search.required_process_id = process_id;
    (void)EnumWindows(&find_ai_editor_window,
                      reinterpret_cast<LPARAM>(&search));
    return search.candidate.hwnd;
}

bool existing_ai_editor_window(const std::filesystem::path& expected,
                               WindowCandidate& candidate) noexcept {
    WindowSearch search;
    search.expected_image = &expected;
    search.require_ready = false;
    (void)EnumWindows(&find_ai_editor_window,
                      reinterpret_cast<LPARAM>(&search));
    if (search.candidate.hwnd == nullptr) {
        return false;
    }
    candidate = std::move(search.candidate);
    return true;
}


bool activate_window(HWND hwnd, DWORD process_id) noexcept {
    if (hwnd == nullptr) {
        return false;
    }
    RECT window{};
    if (GetWindowRect(hwnd, &window) &&
        !window_intersects_virtual_screen(window)) {
        constexpr LONG kMinimumWidth = 600;
        constexpr LONG kMinimumHeight = 400;
        const LONG width = std::max(kMinimumWidth,
                                    window.right - window.left);
        const LONG height = std::max(kMinimumHeight,
                                     window.bottom - window.top);
        const LONG screen_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const LONG screen_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const LONG screen_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const LONG screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const LONG x = screen_left + std::max<LONG>(0, screen_width - width);
        const LONG y = screen_top + std::max<LONG>(0, screen_height - height);
        (void)SetWindowPos(hwnd, nullptr, x, y, width, height,
                           SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    (void)ShowWindowAsync(hwnd, SW_RESTORE);
    (void)SetForegroundWindow(hwnd);
    (void)BringWindowToTop(hwnd);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (window_is_ready(hwnd, process_id)) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

} // namespace

struct AiEditorProcessOwner::State {
    explicit State(std::wstring value) : base_dir(std::move(value)) {
        cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (cancel_event == nullptr) {
            throw std::runtime_error("CreateEventW failed");
        }
    }

    ~State() {
        if (worker != nullptr) {
            CloseHandle(worker);
        }
        if (cancel_event != nullptr) {
            CloseHandle(cancel_event);
        }
        if (process != nullptr) {
            CloseHandle(process);
        }
    }

    std::wstring base_dir;
    HANDLE cancel_event{};
    HANDLE worker{};
    DWORD worker_thread_id{};
    HANDLE process{};
    detail::PackageLease package_lease;
    DWORD process_id{};
    DWORD exit_code{};
    bool has_exit_code{};
    bool termination_required{};
    AiEditorLaunchPhase phase{AiEditorLaunchPhase::idle};
    sao_status_t last_status{SAO_STATUS_OK};
    std::uint64_t generation{};
    bool owner_alive{true};
    std::mutex mutex;
};

namespace {

void emit_launch_failure(sao_status_t status) noexcept {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    (void)sao_core_logf(SAO_LOG_ERROR, "launcher.ai_editor",
                        "Asynchronous AI Editor open failed: %d",
                        static_cast<int>(status));
#else
    (void)status;
#endif
}

void release_process_locked(AiEditorProcessOwner::State& state) noexcept {
    if (state.process != nullptr) {
        CloseHandle(state.process);
        state.process = nullptr;
    }
    state.package_lease = {};
    state.process_id = 0;
    state.termination_required = false;
}

bool terminate_and_confirm(HANDLE process, DWORD exit_code) noexcept {
    const DWORD initial = WaitForSingleObject(process, 0);
    if (initial == WAIT_OBJECT_0) {
        return true;
    }
    if (initial == WAIT_FAILED) {
        return false;
    }
    if (!TerminateProcess(process, exit_code)) {
        return WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    }
    return WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
}

void publish_failure(const std::shared_ptr<AiEditorProcessOwner::State>& state,
                     std::uint64_t generation,
                     sao_status_t status) noexcept {
    bool published = false;
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->owner_alive && state->generation == generation) {
            state->phase = AiEditorLaunchPhase::failed;
            state->last_status = status;
            state->process_id = 0;
            state->exit_code = 0;
            state->has_exit_code = false;
            published = true;
        }
    } catch (...) {
    }
    if (!published) {
        return;
    }
    emit_launch_failure(status);
}

bool force_new_ai_editor() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"SAO_AI_EDITOR_FORCE_NEW", value,
                                   static_cast<DWORD>(std::size(value))) > 0;
}

bool publish_existing(
    const std::shared_ptr<AiEditorProcessOwner::State>& state,
    std::uint64_t generation,
    const std::filesystem::path& executable,
    detail::PackageLease lease,
    WindowCandidate candidate) noexcept {
    if (candidate.process.get() == nullptr ||
        WaitForSingleObject(candidate.process.get(), 0) != WAIT_TIMEOUT ||
        !process_image_matches(candidate.process.get(), executable) ||
        !activate_window(candidate.hwnd, candidate.process_id)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->owner_alive || state->generation != generation) {
            return true;
        }
        if (WaitForSingleObject(candidate.process.get(), 0) != WAIT_TIMEOUT ||
            !process_image_matches(candidate.process.get(), executable) ||
            !window_is_ready(candidate.hwnd, candidate.process_id)) {
            return false;
        }
        state->process = candidate.process.release();
        state->package_lease = std::move(lease);
        state->phase = AiEditorLaunchPhase::started;
        state->last_status = SAO_STATUS_OK;
        state->process_id = candidate.process_id;
        state->exit_code = 0;
        state->has_exit_code = false;
    }
    return true;
}

DWORD wait_for_launch_mutex(
    const std::shared_ptr<AiEditorProcessOwner::State>& state,
    HANDLE mutex, HANDLE cancel_event) noexcept {
    const HANDLE handles[]{mutex, cancel_event};
    const ULONGLONG deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline) {
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 50);
        if (wait == WAIT_OBJECT_0 + 1) {
            SetLastError(ERROR_CANCELLED);
            return WAIT_FAILED;
        }
        if (wait != WAIT_TIMEOUT) {
            return wait;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->owner_alive) {
            return WAIT_FAILED;
        }
    }
    SetLastError(ERROR_TIMEOUT);
    return WAIT_TIMEOUT;
}

enum class WindowWaitResult {
    ready,
    process_gone,
    timeout,
    os_error,
    cancelled,
};

WindowWaitResult wait_for_ai_editor_window(HANDLE process, DWORD process_id,
                                           HANDLE cancel_event) noexcept {
    const HANDLE handles[]{process, cancel_event};
    const ULONGLONG deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline) {
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 25);
        if (wait == WAIT_OBJECT_0) {
            return WindowWaitResult::process_gone;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            return WindowWaitResult::cancelled;
        }
        if (wait == WAIT_FAILED) {
            return WindowWaitResult::os_error;
        }
        if (ai_editor_window(process_id) != nullptr) {
            return WindowWaitResult::ready;
        }
        Sleep(25);
    }
    return WindowWaitResult::timeout;
}

void publish_wait_result(
    const std::shared_ptr<AiEditorProcessOwner::State>& state,
    std::uint64_t generation, HANDLE worker_process, DWORD process_id,
    WindowWaitResult result, DWORD wait_error) noexcept {
    if (result == WindowWaitResult::cancelled) {
        return;
    }
    if (result == WindowWaitResult::ready) {
        bool current_generation = false;
        bool published = false;
        std::lock_guard<std::mutex> lock(state->mutex);
        current_generation = state->owner_alive &&
            state->generation == generation && state->process != nullptr &&
            state->process_id == process_id;
        const HWND window = current_generation
            ? ai_editor_window(process_id)
            : nullptr;
        if (current_generation &&
            WaitForSingleObject(state->process, 0) == WAIT_TIMEOUT &&
            window != nullptr && window_is_ready(window, process_id)) {
            state->phase = AiEditorLaunchPhase::started;
            state->last_status = SAO_STATUS_OK;
            published = true;
        }
        if (published || !current_generation) {
            return;
        }
        const DWORD process_wait = WaitForSingleObject(worker_process, 0);
        if (process_wait == WAIT_OBJECT_0) {
            result = WindowWaitResult::process_gone;
        } else if (process_wait == WAIT_FAILED) {
            wait_error = GetLastError();
            result = WindowWaitResult::os_error;
        } else {
            result = WindowWaitResult::timeout;
        }
    }
    const sao_status_t status = result == WindowWaitResult::process_gone
        ? SAO_STATUS_ERR_PROCESS_GONE
        : result == WindowWaitResult::timeout
            ? SAO_STATUS_ERR_TIMEOUT
            : map_os_error(wait_error);
    const bool process_stopped = result == WindowWaitResult::process_gone ||
        terminate_and_confirm(worker_process,
                              result == WindowWaitResult::timeout
                                  ? ERROR_TIMEOUT
                                  : ERROR_CANCELLED);
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->owner_alive && state->generation == generation &&
            state->process != nullptr && state->process_id == process_id) {
            DWORD exit_code = 0;
            state->has_exit_code =
                GetExitCodeProcess(worker_process, &exit_code) != FALSE &&
                exit_code != STILL_ACTIVE;
            state->exit_code = state->has_exit_code ? exit_code : 0;
            if (process_stopped) {
                release_process_locked(*state);
            } else {
                state->termination_required = true;
            }
            state->phase = AiEditorLaunchPhase::failed;
            state->last_status = process_stopped
                ? status
                : SAO_STATUS_ERR_OS_CALL_FAILED;
            published = true;
        }
    }
    if (published) {
        emit_launch_failure(process_stopped ? status
                                            : SAO_STATUS_ERR_OS_CALL_FAILED);
    }
}

void launch_ai_editor(const std::shared_ptr<AiEditorProcessOwner::State>& state,
                      std::uint64_t generation) noexcept {
    try {
        detail::PackagePaths package;
        sao_status_t status = detail::resolve_package(state->base_dir, package);
        if (status != SAO_STATUS_OK) {
            publish_failure(state, generation, status);
            return;
        }

        const bool force_new = force_new_ai_editor();
        if (!force_new) {
            WindowCandidate candidate;
            if (existing_ai_editor_window(package.executable, candidate)) {
                detail::PackageLease lease;
                status = detail::acquire_package_lease(
                    state->base_dir, package, lease);
                if (status != SAO_STATUS_OK) {
                    publish_failure(state, generation, status);
                    return;
                }
                if (publish_existing(state, generation, package.executable,
                                     std::move(lease),
                                     std::move(candidate))) {
                    return;
                }
            }
        }

        HANDLE launch_mutex_handle = CreateMutexW(nullptr, FALSE,
                                                  kAiEditorLaunchMutex);
        if (launch_mutex_handle == nullptr) {
            publish_failure(state, generation, map_os_error(GetLastError()));
            return;
        }
        LaunchMutex launch_mutex(launch_mutex_handle);
        const DWORD mutex_wait = wait_for_launch_mutex(state,
                                                       launch_mutex_handle,
                                                       state->cancel_event);
        if (mutex_wait != WAIT_OBJECT_0 && mutex_wait != WAIT_ABANDONED) {
            if (GetLastError() == ERROR_CANCELLED) {
                return;
            }
            const sao_status_t failure = mutex_wait == WAIT_TIMEOUT
                ? SAO_STATUS_ERR_TIMEOUT
                : map_os_error(GetLastError());
            publish_failure(state, generation, failure);
            return;
        }
        launch_mutex.set_acquired();

        if (!force_new) {
            WindowCandidate candidate;
            if (existing_ai_editor_window(package.executable, candidate)) {
                detail::PackageLease lease;
                status = detail::acquire_package_lease(
                    state->base_dir, package, lease);
                if (status != SAO_STATUS_OK) {
                    publish_failure(state, generation, status);
                    return;
                }
                if (publish_existing(state, generation, package.executable,
                                     std::move(lease),
                                     std::move(candidate))) {
                    return;
                }
            }
        }

        PROCESS_INFORMATION process{};
        detail::PackageLease lease;
        const sao_status_t create_status = detail::create_ai_editor_process(
            state->base_dir, process, package, lease);
        if (create_status != SAO_STATUS_OK) {
            publish_failure(state, generation, create_status);
            return;
        }
        OwnedHandle worker_process(process.hProcess);
        OwnedHandle worker_thread(process.hThread);
        HANDLE state_process = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), worker_process.get(),
                             GetCurrentProcess(), &state_process, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            const sao_status_t duplicate_status = map_os_error(GetLastError());
            const bool stopped = terminate_and_confirm(worker_process.get(), 1);
            if (stopped) {
                publish_failure(state, generation, duplicate_status);
            } else {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->generation == generation &&
                    state->process == nullptr) {
                    state->process = worker_process.release();
                    state->package_lease = std::move(lease);
                    state->process_id = process.dwProcessId;
                    state->termination_required = true;
                    state->phase = AiEditorLaunchPhase::failed;
                    state->last_status = SAO_STATUS_ERR_OS_CALL_FAILED;
                }
            }
            return;
        }
        OwnedHandle state_process_owner(state_process);
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->generation != generation ||
                state->process != nullptr) {
                cancelled = true;
            } else {
                state->process = state_process_owner.release();
                state->package_lease = std::move(lease);
                state->process_id = process.dwProcessId;
                state->phase = AiEditorLaunchPhase::launching;
                state->last_status = SAO_STATUS_OK;
                state->exit_code = 0;
                state->has_exit_code = false;
                state->termination_required = false;
                cancelled = !state->owner_alive;
            }
        }
        if (cancelled) {
            const bool stopped = terminate_and_confirm(
                worker_process.get(), ERROR_CANCELLED);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->generation == generation &&
                state->process_id == process.dwProcessId) {
                if (stopped) {
                    release_process_locked(*state);
                } else {
                    state->termination_required = true;
                }
            }
            return;
        }
        const WindowWaitResult wait_result =
            wait_for_ai_editor_window(worker_process.get(), process.dwProcessId,
                                      state->cancel_event);
        if (wait_result == WindowWaitResult::cancelled) {
            const bool stopped = terminate_and_confirm(
                worker_process.get(), ERROR_CANCELLED);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (stopped && state->generation == generation &&
                state->process_id == process.dwProcessId) {
                release_process_locked(*state);
            } else if (!stopped && state->generation == generation &&
                       state->process_id == process.dwProcessId) {
                state->termination_required = true;
            }
            return;
        }
        const DWORD wait_error = GetLastError();
        publish_wait_result(state, generation, worker_process.get(),
                            process.dwProcessId, wait_result, wait_error);
    } catch (const std::bad_alloc&) {
        publish_failure(state, generation, SAO_STATUS_ERR_UNKNOWN);
    } catch (...) {
        publish_failure(state, generation, SAO_STATUS_ERR_OS_CALL_FAILED);
    }
}

struct WorkerRequest {
    std::shared_ptr<AiEditorProcessOwner::State> state;
    std::uint64_t generation{};
};

unsigned __stdcall launch_ai_editor_thread(void* parameter) noexcept {
    std::unique_ptr<WorkerRequest> request(
        static_cast<WorkerRequest*>(parameter));
    launch_ai_editor(request->state, request->generation);
    return 0;
}

} // namespace

AiEditorProcessOwner::AiEditorProcessOwner(std::wstring base_dir)
    : state_(std::make_shared<State>(std::move(base_dir))) {}

AiEditorProcessOwner::~AiEditorProcessOwner() {
    const auto state = state_;
    if (!state) {
        return;
    }
    HANDLE worker = nullptr;
    DWORD worker_thread_id = 0;
    try {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->owner_alive = false;
        (void)SetEvent(state->cancel_event);
        worker = state->worker;
        worker_thread_id = state->worker_thread_id;
        state->worker = nullptr;
        state->worker_thread_id = 0;
    } catch (...) {
    }
    if (worker != nullptr) {
        if (GetCurrentThreadId() != worker_thread_id) {
            (void)WaitForSingleObject(worker, INFINITE);
        }
        CloseHandle(worker);
    }
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->termination_required && state->process != nullptr &&
            terminate_and_confirm(state->process, ERROR_CANCELLED)) {
            release_process_locked(*state);
        }
    } catch (...) {
    }
}

sao_status_t AiEditorProcessOwner::open() noexcept {
    const auto state = state_;
    if (!state) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    try {
        std::unique_lock<std::mutex> lock(state->mutex);
        const std::filesystem::path base(state->base_dir);
        if (state->base_dir.empty() || !base.is_absolute()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (state->worker != nullptr) {
            const DWORD worker_wait = WaitForSingleObject(state->worker, 0);
            if (worker_wait == WAIT_TIMEOUT) {
                return SAO_STATUS_OK;
            }
            if (worker_wait == WAIT_FAILED) {
                return map_os_error(GetLastError());
            }
            CloseHandle(state->worker);
            state->worker = nullptr;
            state->worker_thread_id = 0;
        }
        if (state->phase == AiEditorLaunchPhase::launching) {
            return SAO_STATUS_OK;
        }
        if (state->process != nullptr) {
            const DWORD wait = WaitForSingleObject(state->process, 0);
            if (wait == WAIT_TIMEOUT) {
                const HWND window = ai_editor_window(state->process_id);
                if (state->phase == AiEditorLaunchPhase::failed) {
                    return state->last_status;
                }
                if (window == nullptr) {
                    state->phase = AiEditorLaunchPhase::failed;
                    state->last_status = SAO_STATUS_ERR_TIMEOUT;
                    return state->last_status;
                }
                state->phase = AiEditorLaunchPhase::started;
                state->last_status = SAO_STATUS_OK;
                const DWORD process_id = state->process_id;
                lock.unlock();
                (void)activate_window(window, process_id);
                return SAO_STATUS_OK;
            }
            if (wait == WAIT_FAILED) {
                const sao_status_t status = map_os_error(GetLastError());
                release_process_locked(*state);
                state->phase = AiEditorLaunchPhase::failed;
                state->last_status = status;
                return status;
            }
            DWORD exit_code = 0;
            state->has_exit_code =
                GetExitCodeProcess(state->process, &exit_code) != FALSE &&
                exit_code != STILL_ACTIVE;
            state->exit_code = state->has_exit_code ? exit_code : 0;
            release_process_locked(*state);
        }
        const std::uint64_t generation = ++state->generation;
        auto parameter = std::make_unique<WorkerRequest>(
            WorkerRequest{state, generation});
        (void)ResetEvent(state->cancel_event);
        state->phase = AiEditorLaunchPhase::launching;
        state->last_status = SAO_STATUS_OK;
        state->exit_code = 0;
        state->has_exit_code = false;
        unsigned thread_id = 0;
        const uintptr_t thread = _beginthreadex(
            nullptr, 0, &launch_ai_editor_thread, parameter.get(), 0,
            &thread_id);
        if (thread == 0) {
            state->phase = AiEditorLaunchPhase::failed;
            state->last_status = SAO_STATUS_ERR_OS_CALL_FAILED;
            return state->last_status;
        }
        parameter.release();
        state->worker = reinterpret_cast<HANDLE>(thread);
        state->worker_thread_id = thread_id;
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t AiEditorProcessOwner::snapshot(
    AiEditorLaunchSnapshot& out) const noexcept {
    out = {};
    const auto state = state_;
    if (!state) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->process != nullptr) {
            const DWORD wait = WaitForSingleObject(state->process, 0);
            if (wait == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                state->has_exit_code =
                    GetExitCodeProcess(state->process, &exit_code) != FALSE &&
                    exit_code != STILL_ACTIVE;
                state->exit_code = state->has_exit_code ? exit_code : 0;
                release_process_locked(*state);
                if (state->phase == AiEditorLaunchPhase::started ||
                    state->phase == AiEditorLaunchPhase::launching) {
                    state->phase = AiEditorLaunchPhase::failed;
                    state->last_status = SAO_STATUS_ERR_PROCESS_GONE;
                }
            } else if (wait == WAIT_FAILED) {
                const sao_status_t status = map_os_error(GetLastError());
                release_process_locked(*state);
                state->phase = AiEditorLaunchPhase::failed;
                state->last_status = status;
            } else if (state->phase == AiEditorLaunchPhase::started &&
                       ai_editor_window(state->process_id) == nullptr) {
                state->phase = AiEditorLaunchPhase::failed;
                state->last_status = SAO_STATUS_ERR_TIMEOUT;
            }
        }
        out.phase = state->phase;
        out.last_status = state->last_status;
        out.process_id = state->process_id;
        out.exit_code = state->exit_code;
        out.has_exit_code = state->has_exit_code;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t open_ai_editor(AiEditorProcessOwner* owner) noexcept {
    return owner == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : owner->open();
}

} // namespace sao::launcher::tool_launch
