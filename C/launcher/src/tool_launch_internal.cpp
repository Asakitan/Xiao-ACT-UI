#include "tool_launch_internal.h"
#include "tool_launch_package_internal.h"

#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_main_panel.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#endif

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#include "sao/core/logging.h"
#endif

#include <windows.h>
#include <process.h>
#include <tlhelp32.h>

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace sao::launcher::tool_launch {
namespace {

constexpr wchar_t kAiEditorLaunchMutex[] =
    L"Local\\SAO.Auto.AiEditor.Launch.v1";
constexpr std::uint32_t kAiEditorHandshakeTimeoutMs = 15000;
constexpr std::uint32_t kAiEditorLaunchAttempts = 2;
constexpr std::uint32_t kAiEditorShutdownTimeoutMs = 3000;
constexpr std::uint32_t kAiEditorPanelRetireAttempts = 64;
constexpr DWORD kAiEditorPanelRetireRetryDelayMs = 1;
constexpr std::int32_t kUnsetExitCode =
    (std::numeric_limits<std::int32_t>::min)();

void append_trace_line(const wchar_t* message) noexcept {
    wchar_t enabled[2]{};
    if (message == nullptr ||
        GetEnvironmentVariableW(L"SAO_AI_EDITOR_TRACE", enabled,
                                static_cast<DWORD>(std::size(enabled))) == 0u ||
        enabled[0] != L'1') {
        return;
    }
    wchar_t directory[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", directory,
                                static_cast<DWORD>(std::size(directory))) == 0u) {
        return;
    }
    const auto directory_path = std::filesystem::path(directory) / L"SAOAuto";
    std::error_code error;
    std::filesystem::create_directories(directory_path, error);
    if (error) {
        return;
    }
    const auto path = directory_path / L"ai-editor-trace.log";
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        return;
    }
    wchar_t line[1024]{};
    const int length = _snwprintf_s(line, std::size(line), _TRUNCATE,
                                    L"[pid=%lu tid=%lu] owner.%ls\r\n",
                                    static_cast<unsigned long>(GetCurrentProcessId()),
                                    static_cast<unsigned long>(GetCurrentThreadId()),
                                    message);
    if (length > 0) {
        DWORD written = 0;
        (void)WriteFile(file, line, static_cast<DWORD>(length * sizeof(wchar_t)), &written,
                        nullptr);
        OutputDebugStringW(line);
    }
    CloseHandle(file);
}

void trace_status(const wchar_t* stage, std::int32_t status = 0) noexcept {
    wchar_t message[768]{};
    (void)_snwprintf_s(message, std::size(message), _TRUNCATE, L"%ls status=%d",
                       stage != nullptr ? stage : L"", static_cast<int>(status));
    append_trace_line(message);
}

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
    case SAO_AI_EDITOR_ERR_BUSY:
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

sao_status_t validate_process_identity(std::uint32_t pid,
                                       std::uint64_t start_time_100ns) noexcept {
    OwnedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                    FALSE, static_cast<DWORD>(pid)));
    if (process.get() == nullptr) {
        const DWORD error = GetLastError();
        return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND
                   ? SAO_STATUS_ERR_PROCESS_GONE
                   : map_os_error(error);
    }
    if (GetProcessId(process.get()) != pid)
        return SAO_STATUS_ERR_PROCESS_GONE;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) == FALSE)
        return map_os_error(GetLastError());
    ULARGE_INTEGER created_100ns{};
    created_100ns.LowPart = created.dwLowDateTime;
    created_100ns.HighPart = created.dwHighDateTime;
    if (created_100ns.QuadPart != start_time_100ns)
        return SAO_STATUS_ERR_PROCESS_GONE;
    const DWORD wait = WaitForSingleObject(process.get(), 0);
    if (wait == WAIT_OBJECT_0)
        return SAO_STATUS_ERR_PROCESS_GONE;
    return wait == WAIT_TIMEOUT ? SAO_STATUS_OK : map_os_error(GetLastError());
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
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    sao_ai_editor_launcher_t launcher{};
    sao_ai_editor_main_panel_t panel_handle{};
    sao_ui_compositor_handle_t panel_compositor{};
#endif
    sao_rt_io_proxy_handle_t memory_proxy{};
    std::uint32_t memory_pid{};
    std::uint64_t memory_start_time_100ns{};
    std::uint64_t memory_selection_generation{};
    std::uint64_t memory_highest_selection_generation{};
    bool memory_bind_pending{};
    detail::PackageLease package_lease;
    std::filesystem::path executable;
    DWORD process_id{};
    DWORD exit_code{};
    bool has_exit_code{};
    AiEditorLaunchPhase phase{AiEditorLaunchPhase::idle};
    sao_status_t last_status{SAO_STATUS_OK};
    std::uint64_t generation{};
    bool owner_alive{true};
    bool show_requested{};
    bool ui_suppressed{};
    bool teardown_requested{};
    std::mutex mutex;
    std::mutex service_mutex;
    std::condition_variable service_cv;
    bool service_active{};
};

namespace {

class ServiceOperation final {
public:
    explicit ServiceOperation(AiEditorProcessOwner::State& state)
        : state_(state) {
        std::unique_lock lock(state_.service_mutex);
        state_.service_cv.wait(lock, [&] { return !state_.service_active; });
        state_.service_active = true;
    }

    ~ServiceOperation() {
        {
            std::lock_guard lock(state_.service_mutex);
            state_.service_active = false;
        }
        state_.service_cv.notify_one();
    }

    ServiceOperation(const ServiceOperation&) = delete;
    ServiceOperation& operator=(const ServiceOperation&) = delete;

private:
    AiEditorProcessOwner::State& state_;
};

void emit_launch_failure(sao_status_t status) noexcept {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    (void)sao_core_logf(SAO_LOG_ERROR, "launcher.ai_editor",
                        "Asynchronous AI Editor open failed: %d",
                        static_cast<int>(status));
#else
    (void)status;
#endif
}

void emit_launch_retry(std::uint32_t attempt, sao_status_t status) noexcept {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    (void)sao_core_logf(SAO_LOG_WARN, "launcher.ai_editor",
                        "AI Editor launch attempt %u failed transiently: %d",
                        static_cast<unsigned>(attempt), static_cast<int>(status));
#else
    (void)attempt;
    (void)status;
#endif
}

bool retryable_launch_status(sao_status_t status) noexcept {
    return status == SAO_STATUS_ERR_TIMEOUT ||
           status == SAO_STATUS_ERR_PROCESS_GONE ||
           status == SAO_STATUS_ERR_OS_CALL_FAILED;
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

struct ProcessCandidate {
    DWORD process_id{};
    OwnedHandle process;
};

bool find_ai_editor_process(const std::filesystem::path& executable,
                            ProcessCandidate& candidate) noexcept {
    OwnedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return false;
    }
    do {
        OwnedHandle process(OpenProcess(SYNCHRONIZE |
                                            PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, entry.th32ProcessID));
        if (process.get() == nullptr ||
            WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT ||
            !process_image_matches(process.get(), executable)) {
            continue;
        }
        candidate.process_id = entry.th32ProcessID;
        candidate.process = std::move(process);
        return true;
    } while (Process32NextW(snapshot.get(), &entry));
    return false;
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
sao_status_t shutdown_launcher(sao_ai_editor_launcher_t launcher) noexcept {
    if (launcher == nullptr) {
        return SAO_STATUS_OK;
    }
    bool running = false;
    std::int32_t exit_code = kUnsetExitCode;
    const std::int32_t status =
        sao_ai_editor_status(launcher, &running, &exit_code);
    if (status != SAO_AI_EDITOR_OK) {
        return map_ai_editor_status(status);
    }
    if (running) {
        const std::int32_t shutdown_status = sao_ai_editor_shutdown(
            launcher, kAiEditorShutdownTimeoutMs, &exit_code);
        if (shutdown_status != SAO_AI_EDITOR_OK) {
            return map_ai_editor_status(shutdown_status);
        }
    }
    return SAO_STATUS_OK;
}

sao_ui_compositor_handle_t borrow_platform_compositor() noexcept {
    void* raw = nullptr;
    if (sao_sdk_platform_get_ui_compositor(&raw) != SAO_SDK_OK) {
        return nullptr;
    }
    return static_cast<sao_ui_compositor_handle_t>(raw);
}

sao_status_t retire_panel(sao_ai_editor_main_panel_t panel,
                          sao_ui_compositor_handle_t compositor) noexcept {
    if (panel == nullptr) {
        return SAO_STATUS_OK;
    }
    const sao_status_t owner_status =
        sao_ui_compositor_require_owner_thread(compositor);
    if (owner_status != SAO_STATUS_OK) {
        return owner_status;
    }
    for (std::uint32_t attempt = 0;
         attempt < kAiEditorPanelRetireAttempts; ++attempt) {
        const std::int32_t status =
            sao_ai_editor_main_panel_try_destroy(panel);
        if (status == SAO_AI_EDITOR_OK ||
            status == SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
            return SAO_STATUS_OK;
        }
        if (status != SAO_AI_EDITOR_ERR_BUSY ||
            attempt + 1 == kAiEditorPanelRetireAttempts) {
            return map_ai_editor_status(status);
        }

        const std::int32_t tick_status =
            sao_ai_editor_main_panel_tick(panel);
        if (tick_status == SAO_AI_EDITOR_ERR_HANDLE_INVALID) {
            return SAO_STATUS_OK;
        }
        if (tick_status != SAO_AI_EDITOR_OK &&
            tick_status != SAO_AI_EDITOR_ERR_BUSY) {
            return map_ai_editor_status(tick_status);
        }
        Sleep(kAiEditorPanelRetireRetryDelayMs);
    }
    return map_ai_editor_status(SAO_AI_EDITOR_ERR_BUSY);
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
        trace_status(L"launch.resolve_package", status);
        if (status != SAO_STATUS_OK) {
            publish_failure(state, generation, status);
            return;
        }

        SetLastError(ERROR_SUCCESS);
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
        config.extra_args_utf8 = "--headless";
        config.handshake_timeout_ms = kAiEditorHandshakeTimeoutMs;
        config.request_timeout_ms = kAiEditorHandshakeTimeoutMs;

        sao_ai_editor_launcher_t launcher = nullptr;
        std::int32_t abi_status = sao_ai_editor_create(&config, &launcher);
        trace_status(L"launcher.create", abi_status);
        if (abi_status != SAO_AI_EDITOR_OK || launcher == nullptr) {
            publish_failure(state, generation,
                            map_ai_editor_status(abi_status));
            return;
        }

        std::int32_t exit_code = kUnsetExitCode;
        sao_status_t launch_status = SAO_STATUS_OK;
        bool handshake_completed = false;
        for (std::uint32_t attempt = 1;
             attempt <= kAiEditorLaunchAttempts; ++attempt) {
            exit_code = kUnsetExitCode;
            abi_status = sao_ai_editor_launch(launcher, &exit_code);
            launch_status = map_ai_editor_status(abi_status);
            trace_status(L"launcher.launch", abi_status);
            if (abi_status == SAO_AI_EDITOR_OK) {
                handshake_completed = true;
                break;
            }
            if (attempt < kAiEditorLaunchAttempts &&
                retryable_launch_status(launch_status)) {
                emit_launch_retry(attempt, launch_status);
                continue;
            }
            break;
        }
        if (!handshake_completed) {
            sao_ai_editor_destroy(launcher);
            publish_failure(state, generation, launch_status, exit_code);
            return;
        }

        bool running = false;
        exit_code = kUnsetExitCode;
        abi_status = sao_ai_editor_status(launcher, &running, &exit_code);
        if (abi_status != SAO_AI_EDITOR_OK || !running) {
            (void)shutdown_launcher(launcher);
            sao_ai_editor_destroy(launcher);
            publish_failure(
                state, generation,
                abi_status == SAO_AI_EDITOR_OK
                    ? SAO_STATUS_ERR_PROCESS_GONE
                    : map_ai_editor_status(abi_status),
                exit_code);
            return;
        }

        ProcessCandidate launched;
        (void)find_ai_editor_process(package.executable, launched);
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
            (void)shutdown_launcher(launcher);
            sao_ai_editor_destroy(launcher);
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

    // Normal owner-thread destruction follows the same retryable panel-first
    // path as production teardown. A foreign-thread failure is handled below
    // without destroying the launcher borrowed by the still-live panel.
    (void)take_offline();

    HANDLE worker = nullptr;
    DWORD worker_thread_id = 0;
    try {
        ServiceOperation service_operation(*state);
        std::unique_lock<std::mutex> lock(state->mutex);
        state->owner_alive = false;
        state->teardown_requested = true;
        state->show_requested = false;
        ++state->generation;
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
    sao_ai_editor_main_panel_t panel = nullptr;
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        panel = state->panel_handle;
        if (panel == nullptr) {
            launcher = state->launcher;
            state->launcher = nullptr;
            state->panel_compositor = nullptr;
            release_process_locked(*state);
        } else {
            launcher = state->launcher;
        }
    } catch (...) {
    }
    if (launcher != nullptr) {
        (void)shutdown_launcher(launcher);
        if (panel == nullptr) {
            sao_ai_editor_destroy(launcher);
        }
    }
#endif
}

sao_status_t AiEditorProcessOwner::open() noexcept {
    const auto state = state_;
    if (!state) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    try {
        ServiceOperation service_operation(*state);
        {
            std::lock_guard lock(state->mutex);
            const std::filesystem::path base(state->base_dir);
            if (state->base_dir.empty() || !base.is_absolute()) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if (state->teardown_requested || !state->owner_alive) {
                return SAO_STATUS_ERR_CANCELLED;
            }
            state->ui_suppressed = false;
        }
#if !defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        std::lock_guard lock(state->mutex);
        state->phase = AiEditorLaunchPhase::failed;
        state->last_status = SAO_STATUS_ERR_CAPABILITY_MISSING;
        return state->last_status;
#else
        HANDLE finished_worker = nullptr;
        {
            std::lock_guard lock(state->mutex);
            if (state->worker != nullptr) {
                const DWORD worker_wait = WaitForSingleObject(state->worker, 0);
                if (worker_wait == WAIT_TIMEOUT) {
                    state->show_requested = true;
                    return SAO_STATUS_OK;
                }
                if (worker_wait == WAIT_FAILED) {
                    return map_os_error(GetLastError());
                }
                finished_worker = state->worker;
                state->worker = nullptr;
                state->worker_thread_id = 0;
            }
        }
        if (finished_worker != nullptr) {
            CloseHandle(finished_worker);
        }

        sao_ai_editor_launcher_t launcher = nullptr;
        {
            std::lock_guard lock(state->mutex);
            launcher = state->launcher;
        }
        if (launcher != nullptr) {
            bool running = false;
            std::int32_t exit_code = kUnsetExitCode;
            const std::int32_t abi_status = sao_ai_editor_status(
                launcher, &running, &exit_code);
            if (abi_status == SAO_AI_EDITOR_OK && running) {
                std::lock_guard lock(state->mutex);
                if (state->launcher != launcher || state->teardown_requested) {
                    return SAO_STATUS_ERR_CANCELLED;
                }
                state->phase = AiEditorLaunchPhase::started;
                state->last_status = SAO_STATUS_OK;
                state->show_requested = true;
                return SAO_STATUS_OK;
            }

            sao_ai_editor_main_panel_t stale_panel = nullptr;
            sao_ui_compositor_handle_t stale_compositor = nullptr;
            {
                std::lock_guard lock(state->mutex);
                if (state->launcher != launcher) {
                    return SAO_STATUS_ERR_CANCELLED;
                }
                stale_panel = state->panel_handle;
                stale_compositor = state->panel_compositor;
            }
            if (stale_panel != nullptr) {
                const sao_status_t owner_status =
                    sao_ui_compositor_require_owner_thread(stale_compositor);
                if (owner_status != SAO_STATUS_OK) {
                    return owner_status;
                }
            }
            {
                std::lock_guard lock(state->mutex);
                if (state->launcher != launcher ||
                    state->panel_handle != stale_panel ||
                    state->panel_compositor != stale_compositor) {
                    return SAO_STATUS_ERR_CANCELLED;
                }
                state->has_exit_code = exit_code != kUnsetExitCode;
                state->exit_code = state->has_exit_code
                    ? static_cast<DWORD>(exit_code)
                    : 0;
                state->phase = AiEditorLaunchPhase::failed;
                state->last_status = abi_status == SAO_AI_EDITOR_OK
                    ? SAO_STATUS_ERR_PROCESS_GONE
                    : map_ai_editor_status(abi_status);
                state->show_requested = false;
            }

            const sao_status_t panel_status =
                retire_panel(stale_panel, stale_compositor);
            if (panel_status != SAO_STATUS_OK) {
                return panel_status;
            }

            bool destroy_launcher = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->launcher != launcher ||
                    state->panel_handle != stale_panel) {
                    return SAO_STATUS_ERR_CANCELLED;
                }
                state->panel_handle = nullptr;
                state->panel_compositor = nullptr;
                state->memory_bind_pending = state->memory_proxy != nullptr;
                state->launcher = nullptr;
                release_process_locked(*state);
                destroy_launcher = true;
            }
            if (destroy_launcher) {
                sao_ai_editor_destroy(launcher);
            }
        }

        std::unique_lock lock(state->mutex);
        if (state->teardown_requested || !state->owner_alive) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        if (state->process != nullptr) {
            const DWORD wait = WaitForSingleObject(state->process, 0);
            if (wait == WAIT_TIMEOUT) {
                return SAO_STATUS_ERR_CANCELLED;
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
        state->show_requested = true;
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

sao_status_t AiEditorProcessOwner::toggle() noexcept {
    trace_status(L"toggle.begin");
    try {
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    const auto state = state_;
    if (!state) {
        trace_status(L"toggle.no_state", SAO_STATUS_ERR_NOT_INITIALIZED);
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    const auto compositor = borrow_platform_compositor();
    trace_status(L"toggle.compositor", compositor == nullptr
                                              ? SAO_STATUS_ERR_NOT_INITIALIZED
                                              : SAO_STATUS_OK);
    if (!compositor) return SAO_STATUS_ERR_NOT_INITIALIZED;
    const auto owner_status = sao_ui_compositor_require_owner_thread(compositor);
    trace_status(L"toggle.owner", owner_status);
    if (owner_status != SAO_STATUS_OK) return owner_status;
    sao_ai_editor_main_panel_t panel{};
    bool pending = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->teardown_requested) return SAO_STATUS_ERR_CANCELLED;
        panel = state->panel_handle;
        pending = state->show_requested && state->phase != AiEditorLaunchPhase::failed;
        if (pending) {
            state->show_requested = false;
            state->ui_suppressed = true;
        }
    }
    bool visible = false;
    if (panel) {
        const auto status = sao_ai_editor_main_panel_is_visible(panel, &visible);
        if (status != SAO_AI_EDITOR_OK) return map_ai_editor_status(status);
    }
    if (pending || visible) {
        {
            std::lock_guard lock(state->mutex);
            state->ui_suppressed = true;
            state->show_requested = false;
        }
        return panel ? map_ai_editor_status(sao_ai_editor_main_panel_hide(panel)) : SAO_STATUS_OK;
    }
#endif
    return open();
    } catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}

sao_status_t AiEditorProcessOwner::service_ui() noexcept {
    const auto state = state_;
    if (!state) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
#if !defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    return SAO_STATUS_OK;
#else
    try {
        ServiceOperation service_operation(*state);
        sao_ai_editor_launcher_t launcher = nullptr;
        sao_ai_editor_main_panel_t panel = nullptr;
        sao_ui_compositor_handle_t panel_compositor = nullptr;
        bool show_requested = false;
        sao_rt_io_proxy_handle_t memory_proxy = nullptr;
        std::uint32_t memory_pid = 0u;
        std::uint64_t memory_start_time_100ns = 0u;
        std::uint64_t memory_selection_generation = 0u;
        bool memory_bind_pending = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->teardown_requested) {
                return SAO_STATUS_OK;
            }
            launcher = state->launcher;
            panel = state->panel_handle;
            panel_compositor = state->panel_compositor;
            show_requested = state->show_requested;
            memory_proxy = state->memory_proxy;
            memory_pid = state->memory_pid;
            memory_start_time_100ns = state->memory_start_time_100ns;
            memory_selection_generation = state->memory_selection_generation;
            memory_bind_pending = state->memory_bind_pending;
        }
        if (launcher == nullptr) {
            return SAO_STATUS_OK;
        }

        bool running = false;
        std::int32_t exit_code = kUnsetExitCode;
        const std::int32_t abi_status =
            sao_ai_editor_status(launcher, &running, &exit_code);
        if (abi_status != SAO_AI_EDITOR_OK || !running)
            trace_status(L"service.child_status", abi_status);
        if (abi_status != SAO_AI_EDITOR_OK || !running) {
            if (panel != nullptr) {
                const sao_status_t owner_status =
                    sao_ui_compositor_require_owner_thread(panel_compositor);
                if (owner_status != SAO_STATUS_OK) {
                    return owner_status;
                }
            }
            {
                std::lock_guard lock(state->mutex);
                if (state->launcher == launcher) {
                    state->phase = AiEditorLaunchPhase::failed;
                    state->last_status = abi_status == SAO_AI_EDITOR_OK
                                             ? SAO_STATUS_ERR_PROCESS_GONE
                                             : map_ai_editor_status(abi_status);
                    state->has_exit_code = exit_code != kUnsetExitCode;
                    state->exit_code = state->has_exit_code
                                           ? static_cast<DWORD>(exit_code)
                                           : 0;
                    state->show_requested = false;
                }
            }
            const sao_status_t panel_status =
                retire_panel(panel, panel_compositor);
            if (panel_status != SAO_STATUS_OK) {
                return panel_status;
            }
            bool destroy_launcher = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->panel_handle == panel) {
                    state->panel_handle = nullptr;
                    state->panel_compositor = nullptr;
                    state->memory_bind_pending = state->memory_proxy != nullptr;
                }
                if (state->launcher == launcher) {
                    state->launcher = nullptr;
                    release_process_locked(*state);
                    destroy_launcher = true;
                }
            }
            if (destroy_launcher) {
                sao_ai_editor_destroy(launcher);
            }
            return SAO_STATUS_OK;
        }

        const sao_ui_compositor_handle_t compositor = borrow_platform_compositor();
        if (compositor == nullptr) {
            trace_status(L"service.compositor", SAO_STATUS_ERR_NOT_INITIALIZED);
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const sao_status_t owner_status = sao_ui_compositor_require_owner_thread(compositor);
        if (owner_status != SAO_STATUS_OK) {
            trace_status(L"service.owner", owner_status);
            return owner_status;
        }

        if (panel == nullptr && !show_requested) return SAO_STATUS_OK;
        if (panel == nullptr) {
            sao_ai_editor_main_panel_t created = nullptr;
            trace_status(L"service.panel_create.begin");
            const std::int32_t create_status =
                sao_ai_editor_main_panel_create(compositor, launcher, &created);
            trace_status(L"service.panel_create", create_status);
            if (create_status != SAO_AI_EDITOR_OK || created == nullptr) {
                return create_status == SAO_AI_EDITOR_OK
                           ? SAO_STATUS_ERR_OS_CALL_FAILED
                           : map_ai_editor_status(create_status);
            }
            bool accepted = false;
            {
                std::lock_guard lock(state->mutex);
                if (!state->teardown_requested && state->launcher == launcher &&
                    state->panel_handle == nullptr) {
                    state->panel_handle = created;
                    state->panel_compositor = compositor;
                    state->memory_bind_pending = state->memory_proxy != nullptr;
                    panel = created;
                    memory_bind_pending = state->memory_bind_pending;
                    accepted = true;
                }
            }
            if (!accepted) {
                const sao_status_t retire_status =
                    retire_panel(created, compositor);
                return retire_status == SAO_STATUS_OK ? SAO_STATUS_OK : retire_status;
            }
        }

        if (panel != nullptr && memory_bind_pending && memory_proxy != nullptr &&
            memory_pid != 0u && memory_start_time_100ns != 0u &&
            memory_selection_generation != 0u) {
            const std::int32_t bind_status =
                sao_ai_editor_main_panel_bind_memory_target(
                    panel, memory_proxy, memory_pid,
                    memory_start_time_100ns,
                    memory_selection_generation);
            if (bind_status == SAO_AI_EDITOR_OK) {
                const sao_status_t identity_status =
                    validate_process_identity(memory_pid, memory_start_time_100ns);
                if (identity_status != SAO_STATUS_OK) {
                    const std::int32_t clear_status =
                        sao_ai_editor_main_panel_clear_memory_target(panel);
                    if (clear_status != SAO_AI_EDITOR_OK)
                        return map_ai_editor_status(clear_status);
                    const sao_status_t detach_status = sao_rt_io_proxy_detach(memory_proxy);
                    if (detach_status != SAO_STATUS_OK)
                        return detach_status;
                    std::lock_guard lock(state->mutex);
                    if (state->panel_handle == panel && state->memory_proxy == memory_proxy &&
                        state->memory_pid == memory_pid &&
                        state->memory_start_time_100ns == memory_start_time_100ns &&
                        state->memory_selection_generation == memory_selection_generation) {
                        state->memory_proxy = nullptr;
                        state->memory_pid = 0u;
                        state->memory_start_time_100ns = 0u;
                        state->memory_selection_generation = 0u;
                        state->memory_bind_pending = false;
                    }
                    return identity_status;
                }
                std::lock_guard lock(state->mutex);
                if (state->panel_handle == panel && state->memory_proxy == memory_proxy &&
                    state->memory_pid == memory_pid &&
                    state->memory_start_time_100ns == memory_start_time_100ns &&
                    state->memory_selection_generation == memory_selection_generation) {
                    state->memory_bind_pending = false;
                }
            } else {
                const sao_status_t mapped_status = map_ai_editor_status(bind_status);
                if (mapped_status == SAO_STATUS_ERR_NOT_FOUND ||
                    mapped_status == SAO_STATUS_ERR_PROCESS_GONE) {
                    const std::int32_t clear_status =
                        sao_ai_editor_main_panel_clear_memory_target(panel);
                    if (clear_status != SAO_AI_EDITOR_OK)
                        return map_ai_editor_status(clear_status);
                    const sao_status_t detach_status =
                        sao_rt_io_proxy_detach(memory_proxy);
                    if (detach_status != SAO_STATUS_OK)
                        return detach_status;
                    std::lock_guard lock(state->mutex);
                    if (state->panel_handle == panel &&
                        state->memory_proxy == memory_proxy &&
                        state->memory_pid == memory_pid &&
                        state->memory_start_time_100ns == memory_start_time_100ns &&
                        state->memory_selection_generation ==
                            memory_selection_generation) {
                        state->memory_proxy = nullptr;
                        state->memory_pid = 0u;
                        state->memory_start_time_100ns = 0u;
                        state->memory_selection_generation = 0u;
                        state->memory_bind_pending = false;
                    }
                }
            }
        }

        {
            std::lock_guard lock(state->mutex);
            show_requested = state->show_requested && !state->ui_suppressed;
        }
        if (show_requested) {
            const std::int32_t show_status = sao_ai_editor_main_panel_show(panel);
            trace_status(L"service.panel_show", show_status);
            if (show_status != SAO_AI_EDITOR_OK) {
                return map_ai_editor_status(show_status);
            }
            bool suppressed = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->panel_handle == panel) state->show_requested = false;
                suppressed = state->ui_suppressed;
            }
            if (suppressed) return map_ai_editor_status(sao_ai_editor_main_panel_hide(panel));
        }
        const std::int32_t tick_status = sao_ai_editor_main_panel_tick(panel);
        if (tick_status != SAO_AI_EDITOR_OK)
            trace_status(L"service.panel_tick", tick_status);
        return map_ai_editor_status(tick_status);
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#endif
}

sao_status_t AiEditorProcessOwner::take_offline() noexcept {
    const auto state = state_;
    if (!state) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    try {
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        sao_ai_editor_main_panel_t initial_panel = nullptr;
        sao_ui_compositor_handle_t initial_compositor = nullptr;
        {
            std::lock_guard lock(state->mutex);
            initial_panel = state->panel_handle;
            initial_compositor = state->panel_compositor;
        }
        if (initial_panel != nullptr) {
            const sao_status_t owner_status =
                sao_ui_compositor_require_owner_thread(initial_compositor);
            if (owner_status != SAO_STATUS_OK) {
                return owner_status;
            }
        }
#endif
        ServiceOperation service_operation(*state);
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        sao_ai_editor_main_panel_t preflight_panel = nullptr;
        sao_ui_compositor_handle_t preflight_compositor = nullptr;
        {
            std::lock_guard lock(state->mutex);
            preflight_panel = state->panel_handle;
            preflight_compositor = state->panel_compositor;
        }
        if (preflight_panel != nullptr) {
            const sao_status_t owner_status =
                sao_ui_compositor_require_owner_thread(preflight_compositor);
            if (owner_status != SAO_STATUS_OK) {
                return owner_status;
            }
        }
#endif
        HANDLE worker = nullptr;
        DWORD worker_thread_id = 0;
        {
            std::lock_guard lock(state->mutex);
            state->teardown_requested = true;
            state->show_requested = false;
            ++state->generation;
            (void)SetEvent(state->cancel_event);
            worker = state->worker;
            worker_thread_id = state->worker_thread_id;
            state->worker = nullptr;
            state->worker_thread_id = 0;
        }
        if (worker != nullptr) {
            if (GetCurrentThreadId() == worker_thread_id) {
                std::lock_guard lock(state->mutex);
                state->worker = worker;
                state->worker_thread_id = worker_thread_id;
                return SAO_STATUS_ERR_CANCELLED;
            }
            (void)WaitForSingleObject(worker, INFINITE);
            CloseHandle(worker);
        }

#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        sao_ai_editor_main_panel_t panel = nullptr;
        sao_ui_compositor_handle_t panel_compositor = nullptr;
        sao_ai_editor_launcher_t launcher = nullptr;
        {
            std::lock_guard lock(state->mutex);
            panel = state->panel_handle;
            panel_compositor = state->panel_compositor;
            launcher = state->launcher;
        }
        const sao_status_t panel_status =
            retire_panel(panel, panel_compositor);
        if (panel_status != SAO_STATUS_OK) {
            return panel_status;
        }
        {
            std::lock_guard lock(state->mutex);
            if (state->panel_handle == panel) {
                state->panel_handle = nullptr;
                state->panel_compositor = nullptr;
                state->memory_bind_pending = state->memory_proxy != nullptr;
            }
        }

        const sao_status_t shutdown_status = shutdown_launcher(launcher);
        if (shutdown_status != SAO_STATUS_OK) {
            return shutdown_status;
        }
        bool destroy_launcher = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->launcher == launcher) {
                state->launcher = nullptr;
                release_process_locked(*state);
                state->phase = AiEditorLaunchPhase::idle;
                state->last_status = SAO_STATUS_OK;
                state->exit_code = 0;
                state->has_exit_code = false;
                state->show_requested = false;
                state->teardown_requested = false;
                destroy_launcher = launcher != nullptr;
            }
        }
        if (destroy_launcher) {
            sao_ai_editor_destroy(launcher);
        }
#else
        std::lock_guard lock(state->mutex);
        release_process_locked(*state);
        state->phase = AiEditorLaunchPhase::idle;
        state->last_status = SAO_STATUS_OK;
        state->exit_code = 0;
        state->has_exit_code = false;
        state->show_requested = false;
        state->teardown_requested = false;
#endif
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t AiEditorProcessOwner::bind_memory_target(
    sao_rt_io_proxy_handle_t proxy, std::uint32_t pid,
    std::uint64_t start_time_100ns,
    std::uint64_t selection_generation) noexcept {
    if (proxy == nullptr || pid == 0u || start_time_100ns == 0u ||
        selection_generation == 0u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto state = state_;
    if (!state)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t initial_identity_status =
        validate_process_identity(pid, start_time_100ns);
    if (initial_identity_status != SAO_STATUS_OK)
        return initial_identity_status;
    try {
        ServiceOperation service_operation(*state);
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        sao_ai_editor_main_panel_t panel = nullptr;
#endif
        {
            std::lock_guard lock(state->mutex);
            if (state->teardown_requested)
                return SAO_STATUS_ERR_CANCELLED;
            const bool same_binding =
                state->memory_proxy == proxy && state->memory_pid == pid &&
                state->memory_start_time_100ns == start_time_100ns &&
                state->memory_selection_generation == selection_generation;
            if (selection_generation < state->memory_highest_selection_generation ||
                (selection_generation == state->memory_highest_selection_generation &&
                 !same_binding)) {
                return SAO_STATUS_ERR_CANCELLED;
            }
            if (selection_generation > state->memory_highest_selection_generation) {
                state->memory_highest_selection_generation = selection_generation;
                state->memory_proxy = proxy;
                state->memory_pid = pid;
                state->memory_start_time_100ns = start_time_100ns;
                state->memory_selection_generation = selection_generation;
                state->memory_bind_pending = true;
            }
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
            panel = state->panel_handle;
#endif
        }
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        if (panel != nullptr) {
            const std::int32_t status =
                sao_ai_editor_main_panel_bind_memory_target(
                    panel, proxy, pid, start_time_100ns,
                    selection_generation);
            if (status != SAO_AI_EDITOR_OK)
                return map_ai_editor_status(status);
            std::lock_guard lock(state->mutex);
            if (state->panel_handle == panel && state->memory_proxy == proxy &&
                state->memory_pid == pid &&
                state->memory_start_time_100ns == start_time_100ns &&
                state->memory_selection_generation == selection_generation) {
                state->memory_bind_pending = false;
            }
        }
#endif
        const sao_status_t final_identity_status =
            validate_process_identity(pid, start_time_100ns);
        if (final_identity_status != SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
            if (panel != nullptr) {
                const std::int32_t clear_status =
                    sao_ai_editor_main_panel_clear_memory_target(panel);
                if (clear_status != SAO_AI_EDITOR_OK)
                    return map_ai_editor_status(clear_status);
            }
#endif
            std::lock_guard lock(state->mutex);
            if (state->memory_proxy == proxy && state->memory_pid == pid &&
                state->memory_start_time_100ns == start_time_100ns &&
                state->memory_selection_generation == selection_generation) {
                state->memory_proxy = nullptr;
                state->memory_pid = 0u;
                state->memory_start_time_100ns = 0u;
                state->memory_selection_generation = 0u;
                state->memory_bind_pending = false;
            }
            return final_identity_status;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t AiEditorProcessOwner::clear_memory_target() noexcept {
    const auto state = state_;
    if (!state)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    try {
        ServiceOperation service_operation(*state);
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        sao_ai_editor_main_panel_t panel = nullptr;
        {
            std::lock_guard lock(state->mutex);
            panel = state->panel_handle;
        }
        if (panel != nullptr) {
            const std::int32_t status =
                sao_ai_editor_main_panel_clear_memory_target(panel);
            if (status != SAO_AI_EDITOR_OK)
                return map_ai_editor_status(status);
        }
#endif
        std::lock_guard lock(state->mutex);
        state->memory_proxy = nullptr;
        state->memory_pid = 0u;
        state->memory_start_time_100ns = 0u;
        state->memory_selection_generation = 0u;
        state->memory_bind_pending = false;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
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
        ServiceOperation service_operation(*state);
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
        sao_ai_editor_launcher_t launcher = nullptr;
        {
            std::lock_guard lock(state->mutex);
            launcher = state->launcher;
        }
        if (launcher != nullptr) {
            bool running = false;
            std::int32_t exit_code = kUnsetExitCode;
            const std::int32_t abi_status = sao_ai_editor_status(
                launcher, &running, &exit_code);
            std::lock_guard lock(state->mutex);
            if (state->launcher == launcher) {
                if (abi_status == SAO_AI_EDITOR_OK && running) {
                    state->phase = AiEditorLaunchPhase::started;
                    state->last_status = SAO_STATUS_OK;
                } else {
                    state->has_exit_code = exit_code != kUnsetExitCode;
                    state->exit_code = state->has_exit_code
                        ? static_cast<DWORD>(exit_code)
                        : 0;
                    state->phase = AiEditorLaunchPhase::failed;
                    state->last_status = abi_status == SAO_AI_EDITOR_OK
                        ? SAO_STATUS_ERR_PROCESS_GONE
                        : map_ai_editor_status(abi_status);
                }
            }
        }
#endif
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->process != nullptr) {
            const DWORD wait = WaitForSingleObject(state->process, 0);
            if (wait == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                state->has_exit_code =
                    GetExitCodeProcess(state->process, &exit_code) != FALSE &&
                    exit_code != STILL_ACTIVE;
                state->exit_code = state->has_exit_code ? exit_code : 0;
                if (state->phase == AiEditorLaunchPhase::started ||
                    state->phase == AiEditorLaunchPhase::launching) {
                    state->phase = AiEditorLaunchPhase::failed;
                    state->last_status = SAO_STATUS_ERR_PROCESS_GONE;
                }
            } else if (wait == WAIT_FAILED) {
                const sao_status_t status = map_os_error(GetLastError());
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
    return owner == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : owner->toggle();
}

} // namespace sao::launcher::tool_launch
