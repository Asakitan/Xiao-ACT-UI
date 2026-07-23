// SAO Auto — launcher/single_instance.cpp

#include "sao/launcher/single_instance.h"

#include "sao_security/obfuscation/enc_str.h"

#include <windows.h>
#include <cwchar>
#include <cstddef>
#include <cstdint>

namespace sao::launcher {

namespace {

// Compile-time opaque prefix seed. The four ASCII hex nibbles 4F5A survive
// obfuscation both as an mnemonic (for the operator) and as a stable but
// non-descriptive opaque identifier (for the observer via ObjectManager /
// FindWindow enumeration). The value itself is NOT persisted anywhere and
// is only reflected in the string prefixes generated below.
constexpr std::uint16_t kInstanceMutexSeed = 0x4F5A;

// Deterministic hash of a wide string.  We don't need cryptographic strength
// here — just enough to avoid two co-installed copies (e.g., one exe in
// C:\... and one in D:\...) fighting for the same mutex.
uint64_t fnv1a64(const wchar_t* s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= static_cast<uint64_t>(*s);
        h *= 1099511628211ULL;
    }
    return h;
}

// ASCII-only widening helper.  Every opaque identifier below is emitted as
// pure 7-bit ASCII, so we can widen byte-by-byte without going through
// MultiByteToWideChar (which would leave more decrypted plaintext on the
// stack for longer than necessary).
void widen_ascii(const char* src, wchar_t* dst, std::size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    std::size_t i = 0;
    if (src) {
        for (; src[i] != '\0' && i + 1 < dst_cap; ++i) {
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
        }
    }
    dst[i] = L'\0';
}

void buildMutexName(wchar_t* out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // The opaque prefix "Global\\4F5A.i." replaces the plaintext
    // "Global\\SaoAuto.Instance." pattern so ObjectManager scanners cannot
    // trivially pivot on the product name.  Reference kInstanceMutexSeed so
    // the derivation is visible to a reader without a second literal.
    static_assert(kInstanceMutexSeed == 0x4F5A,
                  "opaque prefix must match kInstanceMutexSeed");
    const auto prefix = SAO_ENC_STR("Global\\4F5A.i.");
    wchar_t wide_prefix[32]{};
    widen_ascii(prefix.decrypt(), wide_prefix, std::size(wide_prefix));

    const uint64_t hash = fnv1a64(exe);
    // Emit "<prefix>%016llx" without going through a runtime format string
    // so the CRT does not see a non-literal format specifier.
    wchar_t hex[17]{};
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        hex[15 - i] = kHex[(hash >> (i * 4)) & 0xFu];
    }
    hex[16] = L'\0';
    _snwprintf_s(out, out_cap, _TRUNCATE, L"%s%s", wide_prefix, hex);
}

} // namespace

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

bool acquireSingleInstance(HANDLE& mutex_out) noexcept {
    wchar_t name[128]{};
    buildMutexName(name, 128);

    HANDLE m = CreateMutexW(nullptr, FALSE, name);
    if (!m) {
        mutex_out = nullptr;
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        mutex_out = nullptr;
        return false;
    }
    mutex_out = m;
    return true;
}

void releaseSingleInstance(HANDLE mutex) noexcept {
    if (mutex) {
        CloseHandle(mutex);
    }
}

void forwardCommandLineToRunningInstance(const wchar_t* cmdline) noexcept {
    // Look for the running instance's hidden message-only window using an
    // opaque class name.  The platform/ui subsystem publishes the matching
    // class for exactly this purpose.  When we can't find it (e.g.,
    // platform/ui not built), we just silently fail — the second instance
    // will exit and the user will have to focus the first instance
    // manually.
    const auto msg_class = SAO_ENC_STR("4F5A.mh");
    wchar_t class_name[32]{};
    widen_ascii(msg_class.decrypt(), class_name, std::size(class_name));

    HWND target = FindWindowW(class_name, nullptr);
    if (!target) return;

    COPYDATASTRUCT cds{};
    cds.dwData = 0x5A051L;     // arbitrary tag, ui subsystem must recognise it
    cds.cbData = static_cast<DWORD>((wcslen(cmdline) + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(cmdline);
    SendMessageTimeoutW(target,
                        WM_COPYDATA,
                        0,
                        reinterpret_cast<LPARAM>(&cds),
                        SMTO_ABORTIFHUNG,
                        1000,
                        nullptr);
}

} // namespace sao::launcher
