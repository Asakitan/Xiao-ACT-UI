// SAO Auto — launcher boot-residency UX (implementation).  See the header
// for the state-machine contract.

#include "sao/launcher/boot_residency.h"

#include "sao/core/status.h"
#include "sao/rt_io/backend/helper_load_stack.h"
#include "sao_security/obfuscation/enc_str.h"

#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace sao::launcher {
namespace {

#if defined(_WIN32)

constexpr char kServicesPath[] = "SYSTEM\\CurrentControlSet\\Services";

bool reg_key_exists(HKEY root, const char* path) {
    HKEY key = nullptr;
    const LONG status = ::RegOpenKeyExA(root, path, 0u, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS) {
        ::RegCloseKey(key);
        return true;
    }
    return false;
}

bool reg_dword_nonzero(HKEY root, const char* path, const char* value,
                       bool default_when_missing) {
    HKEY key = nullptr;
    if (::RegOpenKeyExA(root, path, 0u, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return default_when_missing;
    DWORD data = 0u;
    DWORD type = 0u;
    DWORD size = sizeof(data);
    const LONG status =
        ::RegQueryValueExA(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(data))
        return default_when_missing;
    return data != 0u;
}

bool pending_file_rename_operations_present() {
    HKEY key = nullptr;
    const auto session_manager = SAO_ENC_STR("SYSTEM\\CurrentControlSet\\Control\\Session Manager");
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, session_manager.decrypt(), 0u, KEY_QUERY_VALUE,
                        &key) != ERROR_SUCCESS)
        return false;
    const auto value_name = SAO_ENC_STR("PendingFileRenameOperations");
    DWORD type = 0u;
    DWORD size = 0u;
    const LONG status =
        ::RegQueryValueExA(key, value_name.decrypt(), nullptr, &type, nullptr, &size);
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS && size > 0u;
}

bool latch_matches_current_boot(HKEY service_key) {
    const auto flag_name = SAO_ENC_STR("SaoRtIoRestartRequired");
    const auto boot_name = SAO_ENC_STR("SaoRtIoRestartBootId");
    DWORD flag = 0u;
    DWORD type = 0u;
    DWORD size = sizeof(flag);
    LONG status = ::RegQueryValueExA(service_key, flag_name.decrypt(), nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&flag), &size);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(flag) || flag == 0u)
        return false;
    char stored_boot[64]{};
    size = static_cast<DWORD>(sizeof(stored_boot));
    status = ::RegQueryValueExA(service_key, boot_name.decrypt(), nullptr, &type,
                                reinterpret_cast<LPBYTE>(stored_boot), &size);
    if (status != ERROR_SUCCESS || type != REG_SZ || size == 0u || size > sizeof(stored_boot))
        return true; // Malformed latch: fail closed as required.
    stored_boot[sizeof(stored_boot) - 1u] = '\0';
    char current_boot[64]{};
    size_t current_length = 0u;
    if (sao_rt_io_helper_boot_id(current_boot, sizeof(current_boot), &current_length) !=
            SAO_STATUS_OK ||
        current_length == 0u)
        return true; // Boot id unavailable: fail closed as required.
    return std::strcmp(stored_boot, current_boot) == 0;
}

#endif // _WIN32

} // namespace

void boot_environment_probe(BootEnvironmentGates* out_gates) noexcept {
    if (out_gates == nullptr)
        return;
    *out_gates = {};
    out_gates->struct_size = sizeof(*out_gates);
#if defined(_WIN32)
    out_gates->safe_mode = ::GetSystemMetrics(SM_CLEANBOOT) != 0 ? 1u : 0u;
    out_gates->remote_session = ::GetSystemMetrics(SM_REMOTESESSION) != 0 ? 1u : 0u;
    const auto power_path = SAO_ENC_STR(
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power");
    const auto hiberboot_name = SAO_ENC_STR("HiberbootEnabled");
    out_gates->fast_startup =
        reg_dword_nonzero(HKEY_LOCAL_MACHINE, power_path.decrypt(), hiberboot_name.decrypt(),
                          false)
            ? 1u
            : 0u;
    const auto system_policies = SAO_ENC_STR(
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System");
    const auto enable_lua = SAO_ENC_STR("EnableLUA");
    out_gates->uac_enabled =
        reg_dword_nonzero(HKEY_LOCAL_MACHINE, system_policies.decrypt(), enable_lua.decrypt(),
                          true)
            ? 1u
            : 0u;
    const auto wu_reboot = SAO_ENC_STR(
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired");
    const auto cbs_reboot = SAO_ENC_STR(
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending");
    out_gates->update_pending_reboot =
        (reg_key_exists(HKEY_LOCAL_MACHINE, wu_reboot.decrypt()) ||
         reg_key_exists(HKEY_LOCAL_MACHINE, cbs_reboot.decrypt()) ||
         pending_file_rename_operations_present())
            ? 1u
            : 0u;
#endif
}

void boot_residency_restart_required(uint32_t* out_required) noexcept {
    if (out_required == nullptr)
        return;
    *out_required = 0u;
#if defined(_WIN32)
    HKEY services = nullptr;
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, kServicesPath, 0u,
                        KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                        &services) != ERROR_SUCCESS)
        return;
    bool required = false;
    for (DWORD index = 0u;; ++index) {
        char service_name[256]{};
        DWORD service_name_length = static_cast<DWORD>(std::size(service_name));
        const LONG enum_status = ::RegEnumKeyExA(
            services, index, service_name, &service_name_length, nullptr, nullptr, nullptr,
            nullptr);
        if (enum_status == ERROR_NO_MORE_ITEMS)
            break;
        if (enum_status != ERROR_SUCCESS)
            break;
        HKEY service = nullptr;
        if (::RegOpenKeyExA(services, service_name, 0u, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                            &service) != ERROR_SUCCESS)
            continue;
        const bool matched = latch_matches_current_boot(service);
        ::RegCloseKey(service);
        if (matched) {
            required = true;
            break;
        }
    }
    ::RegCloseKey(services);
    *out_required = required ? 1u : 0u;
#endif
}

int request_machine_restart(const wchar_t* message_utf16, uint32_t timeout_seconds) noexcept {
#if defined(_WIN32)
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    TOKEN_PRIVILEGES privileges{};
    if (!::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid)) {
        ::CloseHandle(token);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    privileges.PrivilegeCount = 1u;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr,
                                 nullptr)) {
        ::CloseHandle(token);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const bool privilege_held =
        ::GetLastError() != ERROR_NOT_ALL_ASSIGNED; // May already be held.
    ::CloseHandle(token);
    if (!privilege_held)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (!::InitiateSystemShutdownExW(nullptr, const_cast<LPWSTR>(message_utf16),
                                     timeout_seconds, FALSE, TRUE,
                                     SHTDN_REASON_MAJOR_APPLICATION |
                                         SHTDN_REASON_MINOR_INSTALLATION |
                                         SHTDN_REASON_FLAG_PLANNED))
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    return SAO_STATUS_OK;
#else
    (void)message_utf16;
    (void)timeout_seconds;
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

void abort_machine_restart() noexcept {
#if defined(_WIN32)
    ::AbortSystemShutdownW(nullptr);
#endif
}

int32_t boot_residency_prompt_if_required(void* owner_hwnd) noexcept {
#if defined(_WIN32)
    BootEnvironmentGates gates{};
    boot_environment_probe(&gates);
    // Safe mode never loads boot-start drivers; prompting is pointless there.
    if (gates.safe_mode != 0u)
        return BOOT_RESIDENCY_NOT_REQUIRED;
    uint32_t required = 0u;
    boot_residency_restart_required(&required);
    if (required == 0u)
        return BOOT_RESIDENCY_NOT_REQUIRED;

    char gate_note[192]{};
    size_t offset = 0u;
    const auto append = [&](const char* text) {
        const size_t len = std::strlen(text);
        if (offset + len < sizeof(gate_note)) {
            std::memcpy(gate_note + offset, text, len);
            offset += len;
            gate_note[offset] = '\0';
        }
    };
    if (gates.fast_startup != 0u)
        append("[Fast Startup] ");
    if (gates.update_pending_reboot != 0u)
        append("[OS update reboot pending] ");
    if (gates.remote_session != 0u)
        append("[RDP session] ");
    if (offset == 0u)
        append("(none)");

    wchar_t message[512]{};
    // Same concise copy for the countdown dialog shown by the executor.
    const wchar_t* text =
        L"SAO 驱动启动驻留已部署，需要一次重启完成引导加载。\n"
        L"环境: %hs\n"
        L"请使用「重启」而不是「关机」——快速启动不会重新加载引导驱动。\n\n"
        L"立即重启（30 秒后执行，可再次运行本程序取消）？";
    _snwprintf_s(message, _countof(message), _TRUNCATE, text, gate_note);

    const HWND owner = static_cast<HWND>(owner_hwnd);
    const int choice = ::MessageBoxW(owner, message, L"SAO — Boot residency restart required",
                                     MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    if (choice != IDYES)
        return BOOT_RESIDENCY_RESTART_DECLINED;
    const int status = request_machine_restart(
        L"SAO 驱动启动驻留需要重启完成引导加载。", 30u);
    return status == SAO_STATUS_OK ? BOOT_RESIDENCY_RESTART_ACCEPTED
                                   : BOOT_RESIDENCY_ERROR;
#else
    (void)owner_hwnd;
    return BOOT_RESIDENCY_NOT_REQUIRED;
#endif
}

} // namespace sao::launcher
