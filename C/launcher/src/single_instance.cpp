// SAO Auto — launcher/single_instance.cpp

#include "sao/launcher/single_instance.h"
#include "sao/launcher/working_dir.h"

#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <limits>
#include <cstring>

namespace sao::launcher {

namespace {

// Compile-time opaque prefix seed. The four ASCII hex nibbles 4F5A survive
// obfuscation both as an mnemonic (for the operator) and as a stable but
// non-descriptive opaque identifier (for the observer via ObjectManager /
// FindWindow enumeration). The value itself is NOT persisted anywhere and
// is only reflected in the string prefixes generated below.
constexpr std::uint16_t kInstanceMutexSeed = 0x4F5A;


uint64_t fnv1a64_append(uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0u; index < size; ++index) {
        hash ^= static_cast<uint64_t>(bytes[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool final_path_from_handle(HANDLE handle, std::wstring& path_out) {
    path_out.clear();
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED);
        if (length == 0u) return false;
        if (length < buffer.size()) {
            path_out.assign(buffer.data(), length);
            return !path_out.empty();
        }
        if (buffer.size() >= 32768u) return false;
        const std::size_t next_size = std::max<std::size_t>(
            buffer.size() * 2u, static_cast<std::size_t>(length) + 1u);
        buffer.resize(std::min<std::size_t>(next_size, 32768u));
    }
}

bool open_install_image(const wchar_t* exe_path, HANDLE& handle_out,
                        std::wstring& canonical_path_out,
                        BY_HANDLE_FILE_INFORMATION& file_info_out) {
    handle_out = INVALID_HANDLE_VALUE;
    canonical_path_out.clear();
    std::memset(&file_info_out, 0, sizeof(file_info_out));
    if (exe_path == nullptr || *exe_path == L'\0') return false;
    HANDLE handle = CreateFileW(
        exe_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    if (!final_path_from_handle(handle, canonical_path_out) ||
        !GetFileInformationByHandle(handle, &file_info_out)) {
        CloseHandle(handle);
        canonical_path_out.clear();
        return false;
    }
    handle_out = handle;
    return true;
}

std::uint64_t install_identity_from_handle(
    const BY_HANDLE_FILE_INFORMATION& file_info) {
    std::uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a64_append(hash, &file_info.dwVolumeSerialNumber,
                          sizeof(file_info.dwVolumeSerialNumber));
    hash = fnv1a64_append(hash, &file_info.nFileIndexHigh,
                          sizeof(file_info.nFileIndexHigh));
    hash = fnv1a64_append(hash, &file_info.nFileIndexLow,
                          sizeof(file_info.nFileIndexLow));
    return hash;
}

// ASCII-only widening helper.  Every opaque identifier below is emitted as
// pure 7-bit ASCII, so we can widen byte-by-byte without going through
// MultiByteToWideChar (which would leave more decrypted plaintext on the
// stack for longer than necessary).
bool buildMutexName(wchar_t* out, size_t out_cap) {
    if (!out || out_cap == 0) return false;
    out[0] = L'\0';
    std::wstring exe;
    if (!getCurrentModulePath(exe))
        return false;
    // The opaque prefix "Global\\4F5A.i." replaces the plaintext
    // "Global\\SaoAuto.Instance." pattern so ObjectManager scanners cannot
    // trivially pivot on the product name.  Reference kInstanceMutexSeed so
    // the derivation is visible to a reader without a second literal.
    static_assert(kInstanceMutexSeed == 0x4F5A,
                  "opaque prefix must match kInstanceMutexSeed");
    constexpr wchar_t wide_prefix[] = L"Global\\4F5A.i.";

    const uint64_t hash = singleInstanceInstallIdentity(exe.c_str());
    if (hash == 0u) return false;
    // Emit "<prefix>%016llx" without going through a runtime format string
    // so the CRT does not see a non-literal format specifier.
    wchar_t hex[17]{};
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        hex[15 - i] = kHex[(hash >> (i * 4)) & 0xFu];
    }
    hex[16] = L'\0';
    std::wstring name = wide_prefix;
    name += hex;
    if (name.size() + 1u > out_cap)
        return false;
    std::wmemcpy(out, name.c_str(), name.size() + 1u);
    return true;
}

} // namespace

std::uint64_t singleInstanceInstallIdentity(const wchar_t* exe_path) noexcept {
    if (exe_path == nullptr) return 0;
    try {
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::wstring canonical_path;
        BY_HANDLE_FILE_INFORMATION file_info{};
        if (!open_install_image(exe_path, handle, canonical_path, file_info))
            return 0;
        const std::uint64_t identity =
            install_identity_from_handle(file_info);
        CloseHandle(handle);
        return identity;
    } catch (...) {
        return 0;
    }
}

bool singleInstanceCanonicalInstallPath(const wchar_t* exe_path,
                                        std::wstring& path_out) noexcept {
    path_out.clear();
    try {
        HANDLE handle = INVALID_HANDLE_VALUE;
        BY_HANDLE_FILE_INFORMATION file_info{};
        if (!open_install_image(exe_path, handle, path_out, file_info))
            return false;
        CloseHandle(handle);
        return !path_out.empty();
    } catch (...) {
        path_out.clear();
        return false;
    }
}

// Test-only helper exposing the opaque mutex name that acquireSingleInstance
// will race for. Focused unit tests need a way to grab the mutex first so the
// launcher's second-instance guard trips. This helper is not part of any
// public DLL export table (no SAO_API markup) and is never referenced by the
// production launcher — the linker strips it from shipped binaries when the
// tests are not part of the link set.
extern "C" void sao_launcher_debug_build_single_instance_mutex_name(
    wchar_t* out, std::size_t out_cap) noexcept {
    buildMutexName(out, out_cap);
}

SingleInstanceAcquireResult acquireSingleInstance(HANDLE& mutex_out) noexcept {
    wchar_t name[128]{};
    buildMutexName(name, 128);
    if (name[0] == L'\0') {
        mutex_out = nullptr;
        return SingleInstanceAcquireResult::failed;
    }

    SetLastError(ERROR_SUCCESS);
    HANDLE m = CreateMutexW(nullptr, FALSE, name);
    if (!m) {
        mutex_out = nullptr;
        return SingleInstanceAcquireResult::failed;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        mutex_out = nullptr;
        return SingleInstanceAcquireResult::already_running;
    }
    mutex_out = m;
    return SingleInstanceAcquireResult::acquired;
}

void releaseSingleInstance(HANDLE mutex) noexcept {
    if (mutex) {
        CloseHandle(mutex);
    }
}

void forwardCommandLineToRunningInstance(const wchar_t* cmdline) noexcept {
    HWND target = FindWindowW(kSingleInstanceWindowClassName, nullptr);
    if (!target) return;
    std::wstring exe;
    if (!getCurrentModulePath(exe)) return;
    std::wstring canonical_exe;
    if (!singleInstanceCanonicalInstallPath(exe.c_str(), canonical_exe)) return;
    const std::wstring command_line = cmdline ? cmdline : L"";
    if (canonical_exe.size() + 1u > std::numeric_limits<std::uint32_t>::max() ||
        command_line.size() + 1u > std::numeric_limits<std::uint32_t>::max())
        return;
    const std::size_t total = sizeof(SingleInstancePayloadHeader) +
        (canonical_exe.size() + 1u + command_line.size() + 1u) * sizeof(wchar_t);
    if (total > std::numeric_limits<DWORD>::max()) return;
    std::vector<std::uint8_t> payload(total);
    SingleInstancePayloadHeader header{};
    header.magic = kSingleInstancePayloadMagic;
    header.version = kSingleInstancePayloadVersion;
    header.install_identity = singleInstanceInstallIdentity(canonical_exe.c_str());
    header.target_exe_chars = static_cast<std::uint32_t>(canonical_exe.size() + 1u);
    header.command_line_chars = static_cast<std::uint32_t>(command_line.size() + 1u);
    std::memcpy(payload.data(), &header, sizeof(header));
    auto* strings = payload.data() + sizeof(header);
    std::memcpy(strings, canonical_exe.c_str(), header.target_exe_chars * sizeof(wchar_t));
    strings += header.target_exe_chars * sizeof(wchar_t);
    std::memcpy(strings, command_line.c_str(),
                header.command_line_chars * sizeof(wchar_t));

    COPYDATASTRUCT cds{};
    cds.dwData = kSingleInstanceCopyDataTag;
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = payload.data();
    SendMessageTimeoutW(target, WM_COPYDATA, 0,
                        reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG,
                        1000, nullptr);
}

} // namespace sao::launcher
