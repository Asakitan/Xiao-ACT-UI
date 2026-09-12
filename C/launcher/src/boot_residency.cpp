// SAO Auto — launcher boot-residency UX (implementation).  See the header
// for the state-machine contract.

#include "sao/launcher/boot_residency.h"

#include "sao/core/status.h"
#include "sao_security/obfuscation/enc_str.h"
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/dialog.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
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

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
sao_ui_dialog_handle_t restart_dialog{};
sao_ui_compositor_handle_t restart_compositor{};
int32_t restart_result = BOOT_RESIDENCY_NOT_REQUIRED;
std::chrono::steady_clock::time_point restart_tick{};
void SAO_UI_CALL restart_answer(SaoUiDialogButton answer, const char*, size_t, void*) {
    restart_result = answer == SAO_UI_DIALOG_BTN_YES
        ? BOOT_RESIDENCY_RESTART_ACCEPTED : BOOT_RESIDENCY_RESTART_DECLINED;
}
#endif

#if defined(_WIN32)

constexpr char kServicesPath[] = "SYSTEM\\CurrentControlSet\\Services";

bool current_boot_id(char out[64]) {
    if (out == nullptr)
        return false;
    out[0] = '\0';
    using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll != nullptr
        ? reinterpret_cast<NtQuerySystemInformationFn>(
              ::GetProcAddress(ntdll, "NtQuerySystemInformation"))
        : nullptr;
    if (query == nullptr)
        return false;
    uint8_t raw[32]{};
    ULONG returned = 0u;
    if (query(90u, raw, static_cast<ULONG>(sizeof(raw)), &returned) < 0 ||
        returned < 16u) {
        return false;
    }
    uint32_t d1 = 0u;
    uint16_t d2 = 0u;
    uint16_t d3 = 0u;
    std::memcpy(&d1, raw, sizeof(d1));
    std::memcpy(&d2, raw + 4u, sizeof(d2));
    std::memcpy(&d3, raw + 6u, sizeof(d3));
    const int written = std::snprintf(
        out, 64u, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        d1, d2, d3, raw[8], raw[9], raw[10], raw[11], raw[12], raw[13],
        raw[14], raw[15]);
    return written == 36;
}

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
    if (!current_boot_id(current_boot))
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
#if defined(_WIN32) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    (void)owner_hwnd;
    if (restart_dialog != nullptr) return BOOT_RESIDENCY_PROMPT_PENDING;
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

    void* raw = nullptr;
    if (sao_sdk_platform_get_ui_compositor(&raw) != SAO_SDK_OK || raw == nullptr)
        return BOOT_RESIDENCY_ERROR;
    restart_compositor = static_cast<sao_ui_compositor_handle_t>(raw);
    if (sao_ui_compositor_require_owner_thread(restart_compositor) != SAO_STATUS_OK)
        return BOOT_RESIDENCY_ERROR;
    char message[1024]{};
    std::snprintf(message, sizeof(message),
        "SAO 启动驻留已部署，需要一次重启完成引导加载。\n环境：%s\n"
        "请先保存其他应用中的工作；快速启动下的关机不会替代重启。\n是否在 30 秒后重启？", gate_note);
    const SaoUiDialogButtonSpec buttons[] = {
        {SAO_UI_DIALOG_BTN_CANCEL, "暂不重启", 0},
        {SAO_UI_DIALOG_BTN_YES, "30秒后重启", 0}
    };
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_ASK;
    spec.title_utf8 = "需要重启";
    spec.message_utf8 = message;
    spec.buttons = buttons;
    spec.button_count = 2;
    spec.width = 620;
    spec.height = 340;
    spec.theme_override = SAO_UI_THEME_COUNT;
    spec.dismiss_on_esc = true;
    if (sao_ui_dialog_create(restart_compositor, nullptr, &restart_dialog) != SAO_STATUS_OK)
        return BOOT_RESIDENCY_ERROR;
    restart_result = BOOT_RESIDENCY_PROMPT_PENDING;
    restart_tick = std::chrono::steady_clock::now();
    if (sao_ui_dialog_show(restart_dialog, &spec, restart_answer, nullptr) != SAO_STATUS_OK) {
        sao_ui_dialog_destroy(restart_dialog);
        restart_dialog = nullptr;
        restart_compositor = nullptr;
        restart_result = BOOT_RESIDENCY_ERROR;
        return BOOT_RESIDENCY_ERROR;
    }
    return BOOT_RESIDENCY_PROMPT_PENDING;
#else
    (void)owner_hwnd;
    return BOOT_RESIDENCY_NOT_REQUIRED;
#endif
}

int32_t boot_residency_take_prompt_result() noexcept {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    if (!restart_dialog) return BOOT_RESIDENCY_NOT_REQUIRED;
    if (sao_ui_compositor_require_owner_thread(restart_compositor) != SAO_STATUS_OK)
        return BOOT_RESIDENCY_ERROR;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - restart_tick).count();
    restart_tick = now;
    const auto status = sao_ui_dialog_tick(restart_dialog, static_cast<int32_t>(std::clamp<int64_t>(elapsed, 0, 250)));
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_INITIALIZED)
        restart_result = BOOT_RESIDENCY_ERROR;
    if (restart_result == BOOT_RESIDENCY_PROMPT_PENDING) return BOOT_RESIDENCY_NOT_REQUIRED;
    const int32_t result = restart_result;
    sao_ui_dialog_destroy(restart_dialog);
    restart_dialog = nullptr;
    restart_compositor = nullptr;
    restart_result = BOOT_RESIDENCY_NOT_REQUIRED;
    if (result == BOOT_RESIDENCY_RESTART_ACCEPTED &&
        request_machine_restart(L"SAO 启动驻留需要重启完成引导加载。", 30u) != SAO_STATUS_OK)
        return BOOT_RESIDENCY_ERROR;
    return result;
#else
    return BOOT_RESIDENCY_NOT_REQUIRED;
#endif
}

bool boot_residency_close_prompt() noexcept {
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    if (!restart_dialog) return true;
    if (sao_ui_compositor_require_owner_thread(restart_compositor) != SAO_STATUS_OK) return false;
    if (sao_ui_dialog_hide(restart_dialog) != SAO_STATUS_OK) return false;
    sao_ui_dialog_destroy(restart_dialog);
    restart_dialog = nullptr;
    restart_compositor = nullptr;
    restart_result = BOOT_RESIDENCY_NOT_REQUIRED;
#endif
    return true;
}

} // namespace sao::launcher
