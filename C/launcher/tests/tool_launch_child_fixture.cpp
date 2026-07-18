#include <windows.h>

#include <array>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool environment_present(const wchar_t* name) {
    return GetEnvironmentVariableW(name, nullptr, 0) > 1;
}

bool has_argument(int argc, wchar_t** argv, const wchar_t* expected) {
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], expected) == 0) {
            return true;
        }
    }
    return false;
}

const wchar_t* pipe_argument(int argc, wchar_t** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wcscmp(argv[index], L"--sao-ai-editor-pipe") == 0) {
            return argv[index + 1];
        }
    }
    return nullptr;
}

bool read_exact(HANDLE pipe, void* buffer, std::uint32_t size) {
    auto* cursor = static_cast<std::uint8_t*>(buffer);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD transferred = 0;
        if (!ReadFile(pipe, cursor, remaining, &transferred, nullptr) ||
            transferred == 0) {
            return false;
        }
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

bool write_exact(HANDLE pipe, const void* buffer, std::uint32_t size) {
    const auto* cursor = static_cast<const std::uint8_t*>(buffer);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD transferred = 0;
        if (!WriteFile(pipe, cursor, remaining, &transferred, nullptr) ||
            transferred == 0) {
            return false;
        }
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

bool receive_frame(HANDLE pipe, std::string& payload) {
    std::array<std::uint8_t, 4> header{};
    if (!read_exact(pipe, header.data(),
                    static_cast<std::uint32_t>(header.size()))) {
        return false;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U) |
        (static_cast<std::uint32_t>(header[2]) << 16U) |
        (static_cast<std::uint32_t>(header[3]) << 24U);
    if (size > 1024U * 1024U) {
        return false;
    }
    payload.assign(size, '\0');
    return size == 0 || read_exact(pipe, payload.data(), size);
}

bool send_frame(HANDLE pipe, const std::string& payload) {
    const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(size),
        static_cast<std::uint8_t>(size >> 8U),
        static_cast<std::uint8_t>(size >> 16U),
        static_cast<std::uint8_t>(size >> 24U),
    };
    return write_exact(pipe, header.data(),
                       static_cast<std::uint32_t>(header.size())) &&
           (payload.empty() || write_exact(pipe, payload.data(), size));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (has_argument(argc, argv, L"--ai-editor") ||
        environment_present(L"PYTHONPATH")) {
        return 7;
    }
    const wchar_t* pipe_name = pipe_argument(argc, argv);
    if (pipe_name == nullptr || pipe_name[0] == L'\0') {
        return 8;
    }

    std::ofstream output(L"ai_editor_fixture_marker.txt",
                         std::ios::binary | std::ios::app);
    output << "native\n";
    output.flush();
    if (!output.good()) {
        return 9;
    }
    if (environment_present(
            L"SAO_TOOL_LAUNCH_FIXTURE_EXIT_BEFORE_CONNECT")) {
        return 14;
    }
    if (environment_present(L"SAO_TOOL_LAUNCH_FIXTURE_NO_CONNECT")) {
        Sleep(15000);
        return 12;
    }

    if (!WaitNamedPipeW(pipe_name, 5000)) {
        return 10;
    }
    HANDLE pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return 11;
    }

    std::string message;
    if (!receive_frame(pipe, message) ||
        message != "SAO_AI_EDITOR_HELLO 1" ||
        !send_frame(pipe, "SAO_AI_EDITOR_READY 1")) {
        CloseHandle(pipe);
        return 13;
    }
    while (receive_frame(pipe, message)) {
        if (message == "shutdown") {
            (void)send_frame(pipe, "shutdown-ok");
            (void)FlushFileBuffers(pipe);
            CloseHandle(pipe);
            return 0;
        }
        if (!send_frame(pipe, "response:" + message)) {
            break;
        }
    }
    CloseHandle(pipe);
    return 15;
}
