#include "sao/ai_editor/ai_editor_launcher.h"

#include "sao/ai_editor/ai_editor_ipc.h"
#include "ai_editor_ipc_internal.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kDefaultHandshakeTimeoutMs = 5000U;
constexpr uint32_t kDefaultRequestTimeoutMs = 5000U;
constexpr char kHandshakeRequest[] = "SAO_AI_EDITOR_HELLO 1";
constexpr char kHandshakeResponse[] = "SAO_AI_EDITOR_READY 1";
constexpr char kShutdownRequest[] = "shutdown";
constexpr uint32_t kMaximumPayload = 1024U * 1024U;

bool copy_string(char* destination, size_t capacity, const std::string& source) {
    if (destination == nullptr || capacity == 0 || source.size() + 1 > capacity) {
        return false;
    }
    std::memcpy(destination, source.data(), source.size());
    destination[source.size()] = '\0';
    return true;
}

bool utf8_to_wide(const std::string& input, std::wstring& output) {
    if (input.empty()) {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            input.data(),
                                            static_cast<int>(input.size()),
                                            nullptr, 0);
    if (length <= 0) {
        return false;
    }
    output.assign(static_cast<size_t>(length), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               input.data(), static_cast<int>(input.size()),
                               output.data(), length) == length;
}

void append_quoted_argument(std::wstring& command_line,
                            const std::wstring& argument) {
    if (!command_line.empty()) {
        command_line.push_back(L' ');
    }
    command_line.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
            backslashes = 0;
            continue;
        }
        command_line.append(backslashes, L'\\');
        backslashes = 0;
        command_line.push_back(character);
    }
    command_line.append(backslashes * 2, L'\\');
    command_line.push_back(L'"');
}

bool append_extra_arguments(const std::string& extra_args,
                            std::wstring& command_line) {
    if (extra_args.empty()) {
        return true;
    }
    std::wstring wide_args;
    if (!utf8_to_wide(extra_args, wide_args)) {
        return false;
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(wide_args.c_str(), &argc);
    if (argv == nullptr) {
        return false;
    }
    for (int index = 0; index < argc; ++index) {
        append_quoted_argument(command_line, argv[index]);
    }
    LocalFree(argv);
    return true;
}

bool is_existing_file(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_existing_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}  // namespace

struct SaoAiEditorLauncher {
    ~SaoAiEditorLauncher() {
        if (ipc != nullptr) {
            sao_ai_editor_ipc_destroy(ipc);
        }
    }

    std::mutex mutex;
    std::string executable;
    std::string base_dir;
    std::string extra_args;
    std::string ipc_pipe_name;
    std::wstring executable_wide;
    std::wstring base_dir_wide;
    bool inherit_stdio = false;
    uint32_t handshake_timeout_ms = kDefaultHandshakeTimeoutMs;
    uint32_t request_timeout_ms = kDefaultRequestTimeoutMs;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE job = nullptr;
    sao_ai_editor_ipc_t ipc = nullptr;
    bool launched = false;
    bool exited = false;
    bool response_pending = false;
    int32_t recorded_exit = 0;
};

namespace {

void close_process_handles(SaoAiEditorLauncher& launcher) {
    if (launcher.thread != nullptr) {
        CloseHandle(launcher.thread);
        launcher.thread = nullptr;
    }
    if (launcher.process != nullptr) {
        CloseHandle(launcher.process);
        launcher.process = nullptr;
    }
    if (launcher.job != nullptr) {
        CloseHandle(launcher.job);
        launcher.job = nullptr;
    }
}

void observe_process_exit(SaoAiEditorLauncher& launcher) {
    if (launcher.process == nullptr || launcher.exited) {
        return;
    }
    DWORD exit_code = STILL_ACTIVE;
    if (GetExitCodeProcess(launcher.process, &exit_code) && exit_code != STILL_ACTIVE) {
        launcher.exited = true;
        launcher.recorded_exit = static_cast<int32_t>(exit_code);
    }
}

int32_t recreate_ipc(SaoAiEditorLauncher& launcher) {
    if (launcher.ipc != nullptr) {
        sao_ai_editor_ipc_destroy(launcher.ipc);
        launcher.ipc = nullptr;
    }
    return sao_ai_editor_ipc_create(SAO_AI_EDITOR_IPC_NAMED_PIPE,
                                    launcher.ipc_pipe_name.c_str(),
                                    &launcher.ipc);
}

void fail_launch(SaoAiEditorLauncher& launcher, int32_t* out_exit_code) {
    if (launcher.process != nullptr) {
        DWORD exit_code = STILL_ACTIVE;
        if (GetExitCodeProcess(launcher.process, &exit_code) &&
            exit_code == STILL_ACTIVE) {
            (void)TerminateProcess(launcher.process, 1);
            (void)WaitForSingleObject(launcher.process, 5000);
            (void)GetExitCodeProcess(launcher.process, &exit_code);
        }
        launcher.recorded_exit = static_cast<int32_t>(exit_code);
        launcher.exited = true;
        if (out_exit_code != nullptr) {
            *out_exit_code = launcher.recorded_exit;
        }
    }
    close_process_handles(launcher);
    launcher.launched = false;
    (void)recreate_ipc(launcher);
}

int32_t create_job(SaoAiEditorLauncher& launcher) {
    launcher.job = CreateJobObjectW(nullptr, nullptr);
    if (launcher.job == nullptr) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
    information.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(launcher.job,
                                 JobObjectExtendedLimitInformation,
                                 &information,
                                 sizeof(information))) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t perform_handshake(SaoAiEditorLauncher& launcher) {
    int32_t status = sao::ai_editor::detail::ipc_accept_with_process(
        launcher.ipc, launcher.handshake_timeout_ms, launcher.process);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    status = sao_ai_editor_ipc_send(
        launcher.ipc, kHandshakeRequest,
        static_cast<uint32_t>(std::strlen(kHandshakeRequest)));
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::array<char, 128> response{};
    uint32_t response_length = 0;
    status = sao::ai_editor::detail::ipc_recv_with_process(
        launcher.ipc, response.data(), static_cast<uint32_t>(response.size()),
        &response_length, launcher.handshake_timeout_ms, launcher.process);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (response_length != std::strlen(kHandshakeResponse) ||
        std::memcmp(response.data(), kHandshakeResponse, response_length) != 0) {
        return SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL;
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

extern "C" SAO_AI_EDITOR_API uint32_t SAO_AI_EDITOR_CALL sao_ai_editor_abi_version(void) {
    return SAO_AI_EDITOR_ABI_VERSION;
}

extern "C" SAO_AI_EDITOR_API bool SAO_AI_EDITOR_CALL sao_ai_editor_launcher_available(void) {
    return true;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_create(
    const SaoAiEditorLaunchConfig* config,
    sao_ai_editor_launcher_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (config == nullptr || config->executable_utf8 == nullptr ||
        config->executable_utf8[0] == '\0' || config->base_dir_utf8 == nullptr ||
        config->base_dir_utf8[0] == '\0') {
        return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
    }

    try {
        auto implementation = std::unique_ptr<SaoAiEditorLauncher>(
            new (std::nothrow) SaoAiEditorLauncher());
        if (!implementation) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        implementation->executable = config->executable_utf8;
        implementation->base_dir = config->base_dir_utf8;
        implementation->extra_args = config->extra_args_utf8 != nullptr
            ? config->extra_args_utf8
            : "";
        implementation->inherit_stdio = config->inherit_stdio;
        implementation->handshake_timeout_ms = config->handshake_timeout_ms != 0
            ? config->handshake_timeout_ms
            : kDefaultHandshakeTimeoutMs;
        implementation->request_timeout_ms = config->request_timeout_ms != 0
            ? config->request_timeout_ms
            : kDefaultRequestTimeoutMs;

        if (!utf8_to_wide(implementation->executable,
                          implementation->executable_wide) ||
            !utf8_to_wide(implementation->base_dir,
                          implementation->base_dir_wide) ||
            !std::filesystem::path(implementation->executable_wide).is_absolute() ||
            !std::filesystem::path(implementation->base_dir_wide).is_absolute() ||
            !is_existing_file(implementation->executable_wide) ||
            !is_existing_directory(implementation->base_dir_wide)) {
            return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
        }

        int32_t status = sao_ai_editor_ipc_create(
            SAO_AI_EDITOR_IPC_NAMED_PIPE,
            config->ipc_pipe_name_utf8,
            &implementation->ipc);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        std::array<char, 512> pipe_name{};
        status = sao_ai_editor_ipc_get_pipe_name(
            implementation->ipc, pipe_name.data(), pipe_name.size());
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        implementation->ipc_pipe_name = pipe_name.data();

        *out_handle = implementation.release();
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_launch(
    sao_ai_editor_launcher_t handle,
    int32_t* out_exit_code) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->launched) {
        return SAO_AI_EDITOR_ERR_ALREADY_RUNNING;
    }
    if (!is_existing_file(handle->executable_wide) ||
        !is_existing_directory(handle->base_dir_wide) || handle->ipc == nullptr) {
        return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
    }

    try {
        std::wstring command_line;
        append_quoted_argument(command_line, handle->executable_wide);
        if (!append_extra_arguments(handle->extra_args, command_line)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        append_quoted_argument(command_line, L"--sao-ai-editor-pipe");
        std::wstring pipe_name_wide;
        if (!utf8_to_wide(handle->ipc_pipe_name, pipe_name_wide)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        append_quoted_argument(command_line, pipe_name_wide);
        std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back(L'\0');

        int32_t status = create_job(*handle);
        if (status != SAO_AI_EDITOR_OK) {
            close_process_handles(*handle);
            return status;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!handle->inherit_stdio) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = INVALID_HANDLE_VALUE;
            startup.hStdOutput = INVALID_HANDLE_VALUE;
            startup.hStdError = INVALID_HANDLE_VALUE;
        }
        PROCESS_INFORMATION process{};
        DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        if (!handle->inherit_stdio) {
            flags |= CREATE_NO_WINDOW;
        }
        const BOOL created = CreateProcessW(
            handle->executable_wide.c_str(), mutable_command.data(),
            nullptr, nullptr, FALSE, flags, nullptr,
            handle->base_dir_wide.c_str(), &startup, &process);
        if (!created) {
            close_process_handles(*handle);
            return SAO_AI_EDITOR_ERR_LAUNCH_FAILED;
        }
        handle->process = process.hProcess;
        handle->thread = process.hThread;
        handle->exited = false;
        handle->recorded_exit = 0;

        if (!AssignProcessToJobObject(handle->job, handle->process) ||
            ResumeThread(handle->thread) == static_cast<DWORD>(-1)) {
            fail_launch(*handle, out_exit_code);
            return SAO_AI_EDITOR_ERR_LAUNCH_FAILED;
        }

        status = perform_handshake(*handle);
        if (status != SAO_AI_EDITOR_OK) {
            fail_launch(*handle, out_exit_code);
            return status;
        }
        handle->launched = true;
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        fail_launch(*handle, out_exit_code);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_status(
    sao_ai_editor_launcher_t handle,
    bool* out_is_running,
    int32_t* out_exit_code) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (out_is_running == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    observe_process_exit(*handle);
    *out_is_running = handle->launched && !handle->exited;
    if (handle->exited && out_exit_code != nullptr) {
        *out_exit_code = handle->recorded_exit;
    }
    return SAO_AI_EDITOR_OK;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_request(
    sao_ai_editor_launcher_t handle,
    const void* request,
    uint32_t request_len,
    void* response,
    uint32_t response_cap,
    uint32_t* out_len,
    uint32_t timeout_ms) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (out_len == nullptr || (request == nullptr && request_len != 0) ||
        request_len > kMaximumPayload) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_len = 0;
    observe_process_exit(*handle);
    if (!handle->launched || handle->exited) {
        return SAO_AI_EDITOR_ERR_NOT_RUNNING;
    }
    int32_t status = SAO_AI_EDITOR_OK;
    if (!handle->response_pending) {
        status = sao_ai_editor_ipc_send(handle->ipc, request, request_len);
        if (status != SAO_AI_EDITOR_OK) {
            observe_process_exit(*handle);
            return status;
        }
    }
    const uint32_t effective_timeout = timeout_ms != 0
        ? timeout_ms
        : handle->request_timeout_ms;
    status = sao::ai_editor::detail::ipc_recv_with_process(
        handle->ipc, response, response_cap, out_len,
        effective_timeout, handle->process);
    handle->response_pending =
        status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    if (status == SAO_AI_EDITOR_ERR_IPC_CLOSED && handle->process != nullptr) {
        (void)WaitForSingleObject(handle->process, effective_timeout);
    }
    observe_process_exit(*handle);
    return status;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_shutdown(
    sao_ai_editor_launcher_t handle,
    uint32_t timeout_ms,
    int32_t* out_exit_code) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    observe_process_exit(*handle);
    if (!handle->launched) {
        return SAO_AI_EDITOR_ERR_NOT_RUNNING;
    }
    if (!handle->exited) {
        (void)sao_ai_editor_ipc_send(
            handle->ipc, kShutdownRequest,
            static_cast<uint32_t>(std::strlen(kShutdownRequest)));
        std::array<char, 64> acknowledgement{};
        uint32_t acknowledgement_length = 0;
        (void)sao::ai_editor::detail::ipc_recv_with_process(
            handle->ipc, acknowledgement.data(),
            static_cast<uint32_t>(acknowledgement.size()),
            &acknowledgement_length, timeout_ms, handle->process);
        const DWORD wait_result = WaitForSingleObject(handle->process, timeout_ms);
        if (wait_result == WAIT_TIMEOUT) {
            (void)TerminateProcess(handle->process, 1);
            (void)WaitForSingleObject(handle->process, 5000);
        } else if (wait_result == WAIT_FAILED) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        observe_process_exit(*handle);
    }
    if (out_exit_code != nullptr) {
        *out_exit_code = handle->recorded_exit;
    }
    return SAO_AI_EDITOR_OK;
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_destroy(
    sao_ai_editor_launcher_t handle) {
    if (handle == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        observe_process_exit(*handle);
        if (handle->process != nullptr && !handle->exited) {
            (void)TerminateProcess(handle->process, 1);
            (void)WaitForSingleObject(handle->process, 5000);
        }
        close_process_handles(*handle);
        if (handle->ipc != nullptr) {
            sao_ai_editor_ipc_destroy(handle->ipc);
            handle->ipc = nullptr;
        }
    }
    delete handle;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_get_config(
    sao_ai_editor_launcher_t handle,
    char* executable_out,
    size_t executable_cap,
    char* base_dir_out,
    size_t base_dir_cap,
    char* ipc_pipe_name_out,
    size_t ipc_pipe_cap) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (executable_out != nullptr &&
        !copy_string(executable_out, executable_cap, handle->executable)) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    if (base_dir_out != nullptr &&
        !copy_string(base_dir_out, base_dir_cap, handle->base_dir)) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    if (ipc_pipe_name_out != nullptr &&
        !copy_string(ipc_pipe_name_out, ipc_pipe_cap, handle->ipc_pipe_name)) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    return SAO_AI_EDITOR_OK;
}
