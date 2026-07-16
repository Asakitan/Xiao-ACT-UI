#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kMaxPayload = 1024U * 1024U;

bool read_exact(HANDLE pipe, void* buffer, uint32_t size) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    uint32_t remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, remaining, &read, nullptr) || read == 0) {
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool write_exact(HANDLE pipe, const void* buffer, uint32_t size) {
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    uint32_t remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool recv_frame(HANDLE pipe, std::string& payload) {
    std::array<uint8_t, 4> header{};
    if (!read_exact(pipe, header.data(), static_cast<uint32_t>(header.size()))) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(header[0]) |
                          (static_cast<uint32_t>(header[1]) << 8U) |
                          (static_cast<uint32_t>(header[2]) << 16U) |
                          (static_cast<uint32_t>(header[3]) << 24U);
    if (size > kMaxPayload) {
        return false;
    }
    payload.assign(size, '\0');
    return size == 0 || read_exact(pipe, payload.data(), size);
}

bool send_frame(HANDLE pipe, const std::string& payload) {
    const uint32_t size = static_cast<uint32_t>(payload.size());
    const std::array<uint8_t, 4> header{
        static_cast<uint8_t>(size),
        static_cast<uint8_t>(size >> 8U),
        static_cast<uint8_t>(size >> 16U),
        static_cast<uint8_t>(size >> 24U),
    };
    return write_exact(pipe, header.data(), static_cast<uint32_t>(header.size())) &&
           (payload.empty() || write_exact(pipe, payload.data(), size));
}

const wchar_t* find_pipe_arg(int argc, wchar_t** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wcscmp(argv[index], L"--sao-ai-editor-pipe") == 0) {
            return argv[index + 1];
        }
    }
    return nullptr;
}

bool has_arg(int argc, wchar_t** argv, const wchar_t* expected) {
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], expected) == 0) {
            return true;
        }
    }
    return false;
}

const wchar_t* find_arg_value(int argc,
                              wchar_t** argv,
                              const wchar_t* expected) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wcscmp(argv[index], expected) == 0) {
            return argv[index + 1];
        }
    }
    return nullptr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* expected_argument = find_arg_value(
        argc, argv, L"--fixture-expect");
    if (expected_argument != nullptr &&
        std::wcscmp(expected_argument, L"value with spaces") != 0) {
        return 18;
    }
    if (has_arg(argc, argv, L"--fixture-exit-before-connect")) {
        return 19;
    }
    if (has_arg(argc, argv, L"--fixture-no-connect")) {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            return 20;
        }
        (void)WaitForSingleObject(event, 5000);
        CloseHandle(event);
        return 21;
    }
    const wchar_t* pipe_name = find_pipe_arg(argc, argv);
    if (pipe_name == nullptr || pipe_name[0] == L'\0') {
        return 2;
    }

    if (!WaitNamedPipeW(pipe_name, 5000)) {
        return 3;
    }
    HANDLE pipe = CreateFileW(pipe_name,
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return 4;
    }

    std::string message;
    if (!recv_frame(pipe, message) || message != "SAO_AI_EDITOR_HELLO 1") {
        CloseHandle(pipe);
        return 5;
    }
    if (!send_frame(pipe, "SAO_AI_EDITOR_READY 1")) {
        CloseHandle(pipe);
        return 6;
    }

    while (recv_frame(pipe, message)) {
        if (message == "shutdown") {
            (void)send_frame(pipe, "shutdown-ok");
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
            return 0;
        }
        if (message == "timeout") {
            continue;
        }
        if (message == "exit-now") {
            CloseHandle(pipe);
            return 23;
        }
        if (message == "pid") {
            if (!send_frame(pipe, std::to_string(GetCurrentProcessId()))) {
                CloseHandle(pipe);
                return 7;
            }
            continue;
        }
        if (!send_frame(pipe, "response:" + message)) {
            CloseHandle(pipe);
            return 7;
        }
    }

    CloseHandle(pipe);
    return 8;
}
