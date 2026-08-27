// SAO Auto — launcher/single_instance.h
//
// Uses a named mutex "Global\SaoAuto.Instance.<file-identity>" to prevent
// two concurrent launches from stomping on each other.  When a second
// instance detects the mutex is already held, it forwards its command line
// to the running process via a WM_COPYDATA broadcast on the well-known
// SaoAuto message window class, then exits SAO_EXIT_ALREADY_RUNNING.
// The identity is the Windows volume serial plus file index, so hardlinks to
// the same executable intentionally share one instance.  Canonical paths are
// retained only for payload validation and command routing.

#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

namespace sao::launcher {

inline constexpr wchar_t kSingleInstanceWindowClassName[] = L"4F5A.mh";
inline constexpr ULONG_PTR kSingleInstanceCopyDataTag = 0x5A051u;
inline constexpr std::uint32_t kSingleInstancePayloadMagic = 0x53414F49u;
inline constexpr std::uint16_t kSingleInstancePayloadVersion = 1u;
struct SingleInstancePayloadHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reserved;
    std::uint64_t install_identity;
    std::uint32_t target_exe_chars;
    std::uint32_t command_line_chars;
};
static_assert(sizeof(SingleInstancePayloadHeader) == 24u);

enum class SingleInstanceAcquireResult : unsigned char {
    acquired,
    already_running,
    failed,
};

// Try to acquire the single-instance mutex.  On success, `mutex_out`
// receives a handle the caller must keep alive for the lifetime of the
// process.
//
// Returns:
//   true  — acquired, mutex_out valid
//   false — another instance already running, mutex_out == nullptr
SingleInstanceAcquireResult acquireSingleInstance(HANDLE& mutex_out) noexcept;
std::uint64_t singleInstanceInstallIdentity(const wchar_t* exe_path) noexcept;
bool singleInstanceCanonicalInstallPath(const wchar_t* exe_path,
                                        std::wstring& path_out) noexcept;

// Release the mutex.  Called by App::shutdown().
void releaseSingleInstance(HANDLE mutex) noexcept;

// When acquireSingleInstance() reports another instance, this forwards our
// command line to it (so double-clicking the icon focuses the running
// window, and a --file=X argument opens that file in the existing
// instance).  Best-effort; failure is silent.
void forwardCommandLineToRunningInstance(const wchar_t* cmdline) noexcept;

} // namespace sao::launcher
