#include "tool_launch_internal.h"
#include "tool_launch_package_internal.h"

#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
#include "sao/ai_editor/ai_editor_launcher.h"
#endif

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#include "sao/core/logging.h"
#endif

#include <windows.h>
#include <dwmapi.h>
#include <process.h>
#include <tlhelp32.h>

#include <algorithm>
#include <climits>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace sao::launcher::tool_launch {
namespace {

constexpr wchar_t kAiEditorWindowTitle[] = L"SAO AI Editor";
constexpr wchar_t kAiEditorLaunchMutex[] =
    L"Local\\SAO.Auto.AiEditor.Launch.v1";
constexpr std::uint32_t kAiEditorHandshakeTimeoutMs = 5000;
constexpr std::uint32_t kAiEditorShutdownTimeoutMs = 3000;
constexpr std::int32_t kUnsetExitCode =
    (std::numeric_limits<std::int32_t>::min)();

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

#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
sao_status_t map_ai_editor_status(std::int32_t status) noexcept {
    switch (status) {
    case SAO_AI_EDITOR_OK:
        return SAO_STATUS_OK;
    case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
    case SAO_AI_EDITOR_ERR_CONFIG_MISSING:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case SAO_AI_EDITOR_ERR_NOT_INITIALIZED:
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    case SAO_AI_EDITOR_ERR_HANDLE_INVALID:
        return SAO_STATUS_ERR_HANDLE_INVALID;
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    case SAO_AI_EDITOR_ERR_ALREADY_RUNNING:
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    case SAO_AI_EDITOR_ERR_NOT_RUNNING:
    case SAO_AI_EDITOR_ERR_IPC_CLOSED:
        return SAO_STATUS_ERR_PROCESS_GONE;
    case SAO_AI_EDITOR_ERR_IPC_TIMEOUT:
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return SAO_STATUS_ERR_TIMEOUT;
    case SAO_AI_EDITOR_ERR_NOT_FOUND:
        return SAO_STATUS_ERR_NOT_FOUND;
    case SAO_AI_EDITOR_ERR_PERMISSION_DENIED:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    case SAO_AI_EDITOR_ERR_CANCELLED:
        return SAO_STATUS_ERR_CANCELLED;
    default:
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}
#endif

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
    search.require_ready = false;
    (void)EnumWindows(&find_ai_editor_window,
                      reinterpret_cast<LPARAM>(&search));
    return search.candidate.hwnd;
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
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        if (launcher != nullptr) {
            sao_ai_editor_destroy(launcher);
        }
#endif
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
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    sao_ai_editor_launcher_t launcher{};
#endif
    detail::PackageLease package_lease;
    std::filesystem::path executable;
    DWORD process_id{};
    DWORD exit_code{};
    bool has_exit_code{};
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
    state.executable.clear();
    state.process_id = 0;
}

void publish_failure(const std::shared_ptr<AiEditorProcessOwner::State>& state,
                     std::uint64_t generation,
                     sao_status_t status,
                     std::int32_t exit_code = kUnsetExitCode) noexcept {
    bool published = false;
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->owner_alive && state->generation == generation) {
            state->phase = AiEditorLaunchPhase::failed;
            state->last_status = status;
            state->process_id = 0;
            state->has_exit_code = exit_code != kUnsetExitCode;
            state->exit_code = state->has_exit_code
                ? static_cast<DWORD>(exit_code)
                : 0;
            published = true;
        }
    } catch (...) {
    }
    if (!published) {
        return;
    }
    emit_launch_failure(status);
}

bool publish_existing(
    const std::shared_ptr<AiEditorProcessOwner::State>& state,
    std::uint64_t generation,
    const std::filesystem::path& executable,
    detail::PackageLease lease,
    WindowCandidate candidate) noexcept {
    if (candidate.process.get() == nullptr ||
        WaitForSingleObject(candidate.process.get(), 0) != WAIT_TIMEOUT ||
        !process_image_matches(candidate.process.get(), executable)) {
        return false;
    }
    if (candidate.hwnd != nullptr) {
        (void)activate_window(candidate.hwnd, candidate.process_id);
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->owner_alive || state->generation != generation) {
            return true;
        }
        if (WaitForSingleObject(candidate.process.get(), 0) != WAIT_TIMEOUT ||
            !process_image_matches(candidate.process.get(), executable)) {
            return false;
        }
        state->process = candidate.process.release();
        state->package_lease = std::move(lease);
        state->executable = executable;
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

bool existing_ai_editor_process(const std::filesystem::path& executable,
                                WindowCandidate& candidate) noexcept {
    OwnedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return false;
    }
    WindowCandidate first;
    do {
        OwnedHandle process(OpenProcess(SYNCHRONIZE |
                                            PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, entry.th32ProcessID));
        if (process.get() == nullptr ||
            WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT ||
            !process_image_matches(process.get(), executable)) {
            continue;
        }
        WindowCandidate current;
        current.process_id = entry.th32ProcessID;
        current.hwnd = ai_editor_window(current.process_id);
        current.process = std::move(process);
        if (current.hwnd != nullptr) {
            candidate = std::move(current);
            return true;
        }
        if (first.process.get() == nullptr) {
            first = std::move(current);
        }
    } while (Process32NextW(snapshot.get(), &entry));
    if (first.process.get() == nullptr) {
        return false;
    }
    candidate = std::move(first);
    return true;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
        throw std::runtime_error("invalid path length");
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required, nullptr, nullptr) != required) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    return result;
}

#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
void shutdown_and_destroy(sao_ai_editor_launcher_t launcher) noexcept {
    if (launcher == nullptr) {
        return;
    }
    bool running = false;
    std::int32_t exit_code = 0;
    if (sao_ai_editor_status(launcher, &running, &exit_code) ==
            SAO_AI_EDITOR_OK &&
        running) {
        (void)sao_ai_editor_shutdown(launcher, kAiEditorShutdownTimeoutMs,
                                     &exit_code);
    }
    sao_ai_editor_destroy(launcher);
}
#endif

void launch_ai_editor(const std::shared_ptr<AiEditorProcessOwner::State>& state,
                      std::uint64_t generation) noexcept {
#if !defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    publish_failure(state, generation, SAO_STATUS_ERR_CAPABILITY_MISSING);
#else
    try {
        detail::PackagePaths package;
        sao_status_t status = detail::resolve_package(state->base_dir, package);
        if (status != SAO_STATUS_OK) {
            publish_failure(state, generation, status);
            return;
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

        WindowCandidate existing;
        if (existing_ai_editor_process(package.executable, existing)) {
            detail::PackageLease lease;
            status = detail::acquire_package_lease(
                state->base_dir, package, lease);
            if (status != SAO_STATUS_OK) {
                publish_failure(state, generation, status);
                return;
            }
            if (publish_existing(state, generation, package.executable,
                                 std::move(lease), std::move(existing))) {
                return;
            }
        }

        detail::PackageLease lease;
        status = detail::acquire_package_lease(
            state->base_dir, package, lease);
        if (status != SAO_STATUS_OK) {
            publish_failure(state, generation, status);
            return;
        }

        const std::string executable = path_to_utf8(package.executable);
        const std::string working_directory =
            path_to_utf8(package.working_directory);
        SaoAiEditorLaunchConfig config{};
        config.executable_utf8 = executable.c_str();
        config.base_dir_utf8 = working_directory.c_str();
        config.handshake_timeout_ms = kAiEditorHandshakeTimeoutMs;
        config.request_timeout_ms = kAiEditorHandshakeTimeoutMs;

        sao_ai_editor_launcher_t launcher = nullptr;
        std::int32_t abi_status = sao_ai_editor_create(&config, &launcher);
        if (abi_status != SAO_AI_EDITOR_OK || launcher == nullptr) {
            publish_failure(state, generation,
                            map_ai_editor_status(abi_status));
            return;
        }

        std::int32_t exit_code = kUnsetExitCode;
        abi_status = sao_ai_editor_launch(launcher, &exit_code);
        if (abi_status != SAO_AI_EDITOR_OK) {
            sao_ai_editor_destroy(launcher);
            publish_failure(state, generation,
                            map_ai_editor_status(abi_status), exit_code);
            return;
        }

        bool running = false;
        exit_code = kUnsetExitCode;
        abi_status = sao_ai_editor_status(launcher, &running, &exit_code);
        if (abi_status != SAO_AI_EDITOR_OK || !running) {
            shutdown_and_destroy(launcher);
            publish_failure(
                state, generation,
                abi_status == SAO_AI_EDITOR_OK
                    ? SAO_STATUS_ERR_PROCESS_GONE
                    : map_ai_editor_status(abi_status),
                exit_code);
            return;
        }

        WindowCandidate launched;
        (void)existing_ai_editor_process(package.executable, launched);
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->owner_alive && state->generation == generation) {
                state->launcher = launcher;
                state->process = launched.process.release();
                state->package_lease = std::move(lease);
                state->executable = package.executable;
                state->process_id = launched.process_id;
                state->phase = AiEditorLaunchPhase::started;
                state->last_status = SAO_STATUS_OK;
                state->exit_code = 0;
                state->has_exit_code = false;
                accepted = true;
            }
        }
        if (!accepted) {
            shutdown_and_destroy(launcher);
        }
    } catch (const std::bad_alloc&) {
        publish_failure(state, generation, SAO_STATUS_ERR_UNKNOWN);
    } catch (...) {
        publish_failure(state, generation, SAO_STATUS_ERR_OS_CALL_FAILED);
    }
#endif
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
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    sao_ai_editor_launcher_t launcher = nullptr;
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        launcher = state->launcher;
        state->launcher = nullptr;
    } catch (...) {
    }
    shutdown_and_destroy(launcher);
#endif
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
#if !defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        state->phase = AiEditorLaunchPhase::failed;
        state->last_status = SAO_STATUS_ERR_CAPABILITY_MISSING;
        return state->last_status;
#else
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
        if (state->launcher != nullptr) {
            bool running = false;
            std::int32_t exit_code = kUnsetExitCode;
            const std::int32_t abi_status = sao_ai_editor_status(
                state->launcher, &running, &exit_code);
            if (abi_status == SAO_AI_EDITOR_OK && running) {
                state->phase = AiEditorLaunchPhase::started;
                state->last_status = SAO_STATUS_OK;
                const DWORD process_id = state->process_id;
                lock.unlock();
                (void)activate_window(ai_editor_window(process_id), process_id);
                return SAO_STATUS_OK;
            }
            sao_ai_editor_launcher_t launcher = state->launcher;
            state->launcher = nullptr;
            sao_ai_editor_destroy(launcher);
            state->has_exit_code = exit_code != kUnsetExitCode;
            state->exit_code = state->has_exit_code
                ? static_cast<DWORD>(exit_code)
                : 0;
            release_process_locked(*state);
            state->phase = AiEditorLaunchPhase::failed;
            state->last_status = abi_status == SAO_AI_EDITOR_OK
                ? SAO_STATUS_ERR_PROCESS_GONE
                : map_ai_editor_status(abi_status);
        }
        if (state->process != nullptr) {
            const DWORD wait = WaitForSingleObject(state->process, 0);
            if (wait == WAIT_TIMEOUT) {
                const HWND window = ai_editor_window(state->process_id);
                state->phase = AiEditorLaunchPhase::started;
                state->last_status = SAO_STATUS_OK;
                const DWORD process_id = state->process_id;
                lock.unlock();
                if (window != nullptr) {
                    (void)activate_window(window, process_id);
                }
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
#endif
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
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        if (state->launcher != nullptr) {
            bool running = false;
            std::int32_t exit_code = kUnsetExitCode;
            const std::int32_t abi_status = sao_ai_editor_status(
                state->launcher, &running, &exit_code);
            if (abi_status == SAO_AI_EDITOR_OK && running) {
                state->phase = AiEditorLaunchPhase::started;
                state->last_status = SAO_STATUS_OK;
            } else {
                sao_ai_editor_launcher_t launcher = state->launcher;
                state->launcher = nullptr;
                sao_ai_editor_destroy(launcher);
                state->has_exit_code = exit_code != kUnsetExitCode;
                state->exit_code = state->has_exit_code
                    ? static_cast<DWORD>(exit_code)
                    : 0;
                release_process_locked(*state);
                state->phase = AiEditorLaunchPhase::failed;
                state->last_status = abi_status == SAO_AI_EDITOR_OK
                    ? SAO_STATUS_ERR_PROCESS_GONE
                    : map_ai_editor_status(abi_status);
            }
        }
#endif
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
