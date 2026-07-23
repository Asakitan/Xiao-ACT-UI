#include "cs_component_internal.h"

#include "cs_host_internal.h"

#include "sao/plugins/loader/loader_status.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace sao::plugins::csharp_host {
namespace {

namespace fs = std::filesystem;
using component_entry_fn = component_entry_point_fn;

constexpr int32_t kLoadAssemblyAndGetFunctionPointerDelegate = 5;
constexpr size_t kHookCount = 7;

const std::array<const wchar_t*, kHookCount> kHookNames = {
    L"InitSdkPointers", L"OnLoad", L"OnEnable",     L"OnDisable",
    L"OnUnload",        L"OnTick", L"GetTickCount",
};

#if defined(_WIN32)
using expected_hostfxr_close_fn = int32_t(__cdecl*)(hostfxr_handle_t);
using expected_component_entry_fn = int32_t(__stdcall*)(void*, int32_t);
static_assert(std::is_same_v<hostfxr_close_fn, expected_hostfxr_close_fn>);
static_assert(std::is_same_v<component_entry_fn, expected_component_entry_fn>);
#if defined(_M_IX86)
using cdecl_component_probe_fn = int32_t(__cdecl*)(void*, int32_t);
static_assert(!std::is_same_v<component_entry_fn, cdecl_component_probe_fn>);
#endif
#endif

std::mutex g_counter_mutex;
cs_sdk_counters g_counters{};
std::mutex g_resident_mutex;
std::unordered_set<std::string> g_resident_paths;
std::unordered_set<std::string> g_resident_identities;

std::string lower_ascii(std::string value);
std::string path_utf8(const fs::path& value);

std::string managed_assembly_identity(const std::string& managed_type,
                                      const fs::path& assembly_path) {
    const auto separator = managed_type.find(',');
    std::string identity = separator == std::string::npos ? path_utf8(assembly_path.stem())
                                                          : managed_type.substr(separator + 1);
    const auto first = identity.find_first_not_of(" \t\r\n");
    const auto last = identity.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
        return lower_ascii(path_utf8(assembly_path.stem()));
    return lower_ascii(identity.substr(first, last - first + 1));
}

void copy_bounded(const char* source, char* destination, size_t capacity) {
    if (destination == nullptr || capacity == 0)
        return;
    const size_t length = source == nullptr ? 0 : std::strlen(source);
    const size_t copied = std::min(length, capacity - 1);
    if (copied != 0)
        std::memcpy(destination, source, copied);
    destination[copied] = '\0';
}

extern "C" void SAO_PLUGINS_CALL csharp_sdk_log_info(const char* utf8) {
    try {
        std::lock_guard lock(g_counter_mutex);
        ++g_counters.log_info_calls;
        copy_bounded(utf8, g_counters.last_log_utf8, sizeof(g_counters.last_log_utf8));
    } catch (...) {
    }
}

extern "C" void SAO_PLUGINS_CALL csharp_sdk_register_ui_panel(const char* utf8) {
    try {
        std::lock_guard lock(g_counter_mutex);
        ++g_counters.register_ui_panel_calls;
        copy_bounded(utf8, g_counters.last_panel_id_utf8, sizeof(g_counters.last_panel_id_utf8));
    } catch (...) {
    }
}

extern "C" void SAO_PLUGINS_CALL csharp_sdk_register_hotkey(const char* id_utf8,
                                                            const char* key_utf8) {
    try {
        std::lock_guard lock(g_counter_mutex);
        ++g_counters.register_hotkey_calls;
        copy_bounded(id_utf8, g_counters.last_hotkey_id_utf8,
                     sizeof(g_counters.last_hotkey_id_utf8));
        copy_bounded(key_utf8, g_counters.last_hotkey_key_utf8,
                     sizeof(g_counters.last_hotkey_key_utf8));
    } catch (...) {
    }
}

size_t hook_index(managed_hook hook) {
    return static_cast<size_t>(hook);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool utf8_to_wide(std::string_view value, std::wstring& output) {
    output.clear();
#if !defined(_WIN32)
    output.assign(value.begin(), value.end());
    return !output.empty();
#else
    if (value.empty() || value.size() > static_cast<size_t>(INT32_MAX)) {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return false;
    output.resize(static_cast<size_t>(length));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), output.data(), length) == length;
#endif
}

std::string path_utf8(const fs::path& value) {
#if !defined(_WIN32)
    return value.u8string();
#else
    const auto& wide = value.native();
    if (wide.empty() || wide.size() > static_cast<size_t>(INT32_MAX))
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string output(static_cast<size_t>(length), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                               static_cast<int>(wide.size()), output.data(), length, nullptr,
                               nullptr) == length
               ? output
               : std::string{};
#endif
}

bool is_within(const fs::path& base, const fs::path& candidate) {
    const auto relative = candidate.lexically_relative(base);
    if (relative.empty() || relative.is_absolute())
        return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != fs::path("..");
}

int32_t resolve_file(const fs::path& base, const std::string& relative_utf8, fs::path& output,
                     std::string& error) {
    if (relative_utf8.empty()) {
        error = "managed file path is empty";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        const auto relative = fs::u8path(relative_utf8);
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
            error = "managed file path must be relative to the plugin directory";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        for (const auto& part : relative) {
            if (part == fs::path("..")) {
                error = "managed file path escapes the plugin directory";
                return SAO_ERR_INVALID_ARGUMENT;
            }
        }
        std::error_code file_error;
        const auto normalized_base = fs::weakly_canonical(base, file_error);
        if (file_error) {
            error = "plugin source_path is unavailable";
            return SAO_ERR_HANDLE_INVALID;
        }
        const auto candidate = fs::weakly_canonical(base / relative, file_error);
        if (file_error || !is_within(normalized_base, candidate) ||
            !fs::is_regular_file(candidate, file_error) || file_error) {
            error = "managed file is missing or outside the plugin directory: " + relative_utf8;
            return SAO_ERR_HANDLE_INVALID;
        }
        output = candidate;
        return SAO_OK;
    } catch (...) {
        error = "managed file path resolution failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

std::string inferred_runtime_config(const std::string& entry) {
    const auto path = fs::u8path(entry);
    auto runtime = path;
    runtime.replace_extension(".runtimeconfig.json");
    return path_utf8(runtime);
}

int32_t call_load_function_cpp(load_assembly_and_get_function_pointer_fn load,
                               const wchar_t* assembly, const wchar_t* type, const wchar_t* method,
                               void** output) noexcept {
    try {
        return load(assembly, type, method, nullptr, nullptr, output);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_load_function(load_assembly_and_get_function_pointer_fn load, const wchar_t* assembly,
                           const wchar_t* type, const wchar_t* method, void** output) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_load_function_cpp(load, assembly, type, method, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_load_function_cpp(load, assembly, type, method, output);
#endif
}

int32_t initialize_runtime_cpp(hostfxr_initialize_for_runtime_config_fn initialize,
                               const wchar_t* runtime_config, hostfxr_handle_t* context) noexcept {
    try {
        return initialize(runtime_config, nullptr, context);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t initialize_runtime_guarded(hostfxr_initialize_for_runtime_config_fn initialize,
                                   const wchar_t* runtime_config,
                                   hostfxr_handle_t* context) noexcept {
#if defined(_MSC_VER)
    __try {
        return initialize_runtime_cpp(initialize, runtime_config, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return initialize_runtime_cpp(initialize, runtime_config, context);
#endif
}

int32_t get_runtime_delegate_cpp(hostfxr_get_runtime_delegate_fn get_delegate,
                                 hostfxr_handle_t context, void** delegate) noexcept {
    try {
        return get_delegate(context, kLoadAssemblyAndGetFunctionPointerDelegate, delegate);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t get_runtime_delegate_guarded(hostfxr_get_runtime_delegate_fn get_delegate,
                                     hostfxr_handle_t context, void** delegate) noexcept {
#if defined(_MSC_VER)
    __try {
        return get_runtime_delegate_cpp(get_delegate, context, delegate);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return get_runtime_delegate_cpp(get_delegate, context, delegate);
#endif
}

int32_t invoke_cpp(component_entry_fn function, void* argument, int32_t argument_size,
                   int32_t* output) noexcept {
    try {
        *output = function(argument, argument_size);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_guarded(component_entry_fn function, void* argument, int32_t argument_size,
                       int32_t* output) noexcept {
#if defined(_MSC_VER)
    __try {
        return invoke_cpp(function, argument, argument_size, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return invoke_cpp(function, argument, argument_size, output);
#endif
}

int32_t close_runtime_cpp(hostfxr_close_fn close, hostfxr_handle_t context) noexcept {
    try {
        return close(context);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t close_runtime_guarded(hostfxr_close_fn close, hostfxr_handle_t context) noexcept {
#if defined(_MSC_VER)
    __try {
        return close_runtime_cpp(close, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return close_runtime_cpp(close, context);
#endif
}

} // namespace

struct managed_component_s {
    cshost_runtime_api runtime;
    hostfxr_handle_t context = nullptr;
    std::array<component_entry_fn, kHookCount> hooks{};
    fs::path assembly_path;
    fs::path runtime_config_path;
    std::wstring managed_type;
    std::string resident_key;
    std::string resident_identity;
    cs_sdk_bridge bridge{};
    cs_managed_plugin_context on_load_context{};
    void* sdk_context = nullptr;
    void* loader_context = nullptr;
    const cs_managed_sdk_table* sdk_table = nullptr;
    cs_managed_sdk_session_t sdk_session = nullptr;
    bool resident_key_reserved = false;
    bool assembly_process_resident = false;
    bool initialized = false;
};

int32_t cshost_component_attach_contexts(managed_component_s* component, void* sdk_context,
                                         void* loader_context) noexcept {
    if (component == nullptr || sdk_context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if ((component->sdk_context != nullptr && component->sdk_context != sdk_context) ||
        (loader_context != nullptr && component->loader_context != nullptr &&
         component->loader_context != loader_context)) {
        return SAO_ERR_HANDLE_INVALID;
    }
    component->sdk_context = sdk_context;
    component->loader_context = loader_context;
    return SAO_OK;
}

int32_t cshost_component_publish_sdk_session(managed_component_s* component,
                                             const cs_managed_sdk_table* table,
                                             cs_managed_sdk_session_t session) noexcept {
    // Accept any producer struct_size at or above the V1 prefix so binaries
    // compiled against the original 72-byte layout keep publishing. The ABI
    // version is fixed at 1 (append-only); appended slots are read only when
    // struct_size covers them via safe_readable_extent on the managed side.
    if (component == nullptr || table == nullptr || session == nullptr ||
        table->struct_size < SAO_CSHOST_SDK_TABLE_V1_SIZE ||
        table->abi_version != SAO_CSHOST_SDK_TABLE_ABI_VERSION) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (component->sdk_session != nullptr)
        return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    component->sdk_table = table;
    component->sdk_session = session;
    return SAO_OK;
}

void cshost_component_clear_sdk_session(managed_component_s* component,
                                        cs_managed_sdk_session_t session) noexcept {
    if (component == nullptr || component->sdk_session != session)
        return;
    component->sdk_table = nullptr;
    component->sdk_session = nullptr;
}

int32_t cshost_component_load(cs_host_handle_t host,
                              const sao::plugins::loader::plugin_manifest& manifest,
                              managed_component_s** out_component,
                              std::string& out_error) noexcept {
    if (out_component == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_component = nullptr;
    out_error.clear();
#if !defined(_WIN32)
    (void)host;
    (void)manifest;
    out_error = "hostfxr components require Windows";
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        if (host == nullptr || manifest.language != sao::plugins::loader::engine_kind::csharp ||
            manifest.source_path.empty() || manifest.entry.empty()) {
            out_error = "invalid C# component manifest";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (lower_ascii(fs::u8path(manifest.entry).extension().string()) != ".dll") {
            out_error = "C# generic loader accepts precompiled .dll entries only; source "
                        "compilation is not supported";
            return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }

        const std::string& managed_type_utf8 = manifest.managed_type;
        if (managed_type_utf8.empty() || managed_type_utf8.size() > 1024 ||
            managed_type_utf8.find('\0') != std::string::npos) {
            out_error = "manifest field managed_type is required for generic C# components";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::wstring managed_type;
        if (!utf8_to_wide(managed_type_utf8, managed_type)) {
            out_error = "manifest managed_type is not valid UTF-8";
            return SAO_ERR_INVALID_ARGUMENT;
        }

        const std::string runtime_config = manifest.runtimeconfig.empty()
                                               ? inferred_runtime_config(manifest.entry)
                                               : manifest.runtimeconfig;
        const fs::path plugin_dir = fs::u8path(manifest.source_path);
        fs::path assembly_path;
        int32_t status = resolve_file(plugin_dir, manifest.entry, assembly_path, out_error);
        if (status != SAO_OK)
            return status;
        fs::path runtime_config_path;
        status = resolve_file(plugin_dir, runtime_config, runtime_config_path, out_error);
        if (status != SAO_OK) {
            out_error = "runtimeconfig unavailable: " + out_error;
            return status;
        }

        auto component = std::unique_ptr<managed_component_s, decltype(&cshost_component_abandon)>(
            new managed_component_s(), &cshost_component_abandon);
        component->assembly_path = std::move(assembly_path);
        component->runtime_config_path = std::move(runtime_config_path);
        component->managed_type = std::move(managed_type);
        component->resident_key = lower_ascii(path_utf8(component->assembly_path));
        component->resident_identity =
            managed_assembly_identity(managed_type_utf8, component->assembly_path);
        {
            std::lock_guard lock(g_resident_mutex);
            const bool path_inserted = g_resident_paths.insert(component->resident_key).second;
            const bool identity_inserted =
                path_inserted && g_resident_identities.insert(component->resident_identity).second;
            if (!path_inserted || !identity_inserted) {
                if (path_inserted)
                    g_resident_paths.erase(component->resident_key);
                out_error = "managed assembly is already process-resident; collectible ALC is "
                            "unavailable, so reload requires process restart: " +
                            path_utf8(component->assembly_path);
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            component->resident_key_reserved = true;
        }
        status = cshost_acquire_runtime_api(host, component->runtime);
        if (status != SAO_OK) {
            out_error = status == SAO_ERR_NOT_INITIALIZED
                            ? "hostfxr runtime capability is unavailable"
                            : "hostfxr runtime API acquisition failed";
            return status;
        }

        const int32_t initialize_status =
            initialize_runtime_guarded(component->runtime.initialize_for_runtime_config,
                                       component->runtime_config_path.c_str(), &component->context);
        if (initialize_status < 0 || component->context == nullptr) {
            out_error = "hostfxr_initialize_for_runtime_config failed rc=" +
                        std::to_string(initialize_status) +
                        " path=" + path_utf8(component->runtime_config_path);
            return SAO_ERR_OS_CALL_FAILED;
        }

        void* delegate = nullptr;
        const int32_t delegate_status = get_runtime_delegate_guarded(
            component->runtime.get_runtime_delegate, component->context, &delegate);
        if (delegate_status < 0 || delegate == nullptr) {
            out_error = "hostfxr_get_runtime_delegate failed rc=" + std::to_string(delegate_status);
            return SAO_ERR_OS_CALL_FAILED;
        }
        const auto load = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(delegate);
        for (size_t index = 0; index < component->hooks.size(); ++index) {
            void* hook = nullptr;
            const int32_t hook_status =
                call_load_function(load, component->assembly_path.c_str(),
                                   component->managed_type.c_str(), kHookNames[index], &hook);
            component->assembly_process_resident = true;
            if (hook_status == 0 && hook != nullptr) {
                component->hooks[index] = reinterpret_cast<component_entry_fn>(hook);
            }
        }
        if (component->hooks[hook_index(managed_hook::on_load)] == nullptr) {
            out_error = "managed type or required OnLoad hook resolution failed: type='" +
                        managed_type_utf8 + "' assembly='" + path_utf8(component->assembly_path) +
                        "'";
            return SAO_ERR_HANDLE_INVALID;
        }
        *out_component = component.release();
        return SAO_OK;
    } catch (...) {
        out_error = "managed component load crossed the native exception boundary";
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

int32_t cshost_component_initialize(managed_component_s* component, void* sdk_context,
                                    void* loader_context, std::string& out_error) noexcept {
    out_error.clear();
    const int32_t attach_status =
        cshost_component_attach_contexts(component, sdk_context, loader_context);
    if (attach_status != SAO_OK)
        return attach_status;
    component->bridge = {
        csharp_sdk_log_info,
        csharp_sdk_register_ui_panel,
        csharp_sdk_register_hotkey,
        static_cast<uint32_t>(sizeof(cs_sdk_bridge)),
        SAO_CSHOST_MANAGED_ABI_VERSION,
        sdk_context,
        loader_context,
        component->sdk_table,
        component->sdk_session,
    };
    component->on_load_context = {
        static_cast<uint32_t>(sizeof(cs_managed_plugin_context)),
        SAO_CSHOST_MANAGED_ABI_VERSION,
        sdk_context,
        loader_context,
        component->sdk_table,
        component->sdk_session,
    };
    if (cshost_component_has_hook(component, managed_hook::init_sdk)) {
        int32_t managed_result = 0;
        const int32_t status = cshost_component_invoke(
            component, managed_hook::init_sdk, &component->bridge,
            static_cast<int32_t>(sizeof(component->bridge)), &managed_result, out_error);
        if (status != SAO_OK)
            return status;
        if (managed_result != 0) {
            out_error = "InitSdkPointers returned " + std::to_string(managed_result);
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    component->initialized = true;
    return SAO_OK;
}

int32_t cshost_component_on_load(managed_component_s* component, int32_t* out_managed_result,
                                 std::string& out_error) noexcept {
    if (component == nullptr || !component->initialized) {
        out_error = "managed component SDK bridge is not initialized";
        return SAO_ERR_NOT_INITIALIZED;
    }
    return cshost_component_invoke(component, managed_hook::on_load, &component->on_load_context,
                                   static_cast<int32_t>(sizeof(component->on_load_context)),
                                   out_managed_result, out_error);
}

bool cshost_component_has_hook(managed_component_s* component, managed_hook hook) noexcept {
    const size_t index = hook_index(hook);
    return component != nullptr && index < component->hooks.size() &&
           component->hooks[index] != nullptr;
}

int32_t cshost_component_invoke(managed_component_s* component, managed_hook hook, void* argument,
                                int32_t argument_size, int32_t* out_managed_result,
                                std::string& out_error) noexcept {
    if (out_managed_result != nullptr)
        *out_managed_result = 0;
    out_error.clear();
    if (component == nullptr || argument_size < 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const size_t index = hook_index(hook);
    if (index >= component->hooks.size() || component->hooks[index] == nullptr) {
        out_error = "required managed hook is missing";
        return SAO_ERR_HANDLE_INVALID;
    }
    int32_t managed_result = 0;
    const int32_t status =
        invoke_guarded(component->hooks[index], argument, argument_size, &managed_result);
    if (status != SAO_OK) {
        out_error = "managed hook crossed the native exception boundary";
        return status;
    }
    if (out_managed_result != nullptr)
        *out_managed_result = managed_result;
    return SAO_OK;
}

int32_t cshost_close_runtime_context(hostfxr_close_fn close, hostfxr_handle_t context,
                                     std::string& out_error) noexcept {
    out_error.clear();
    if (context == nullptr)
        return SAO_OK;
    if (close == nullptr) {
        out_error = "hostfxr_close is unavailable";
        return SAO_ERR_NOT_INITIALIZED;
    }
    const int32_t close_status = close_runtime_guarded(close, context);
    if (close_status == SAO_ERR_OS_CALL_FAILED) {
        out_error = "hostfxr_close crossed the native exception boundary";
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (close_status != 0) {
        out_error = "hostfxr_close failed rc=" + std::to_string(close_status);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

int32_t cshost_component_close(managed_component_s* component, std::string& out_error) noexcept {
    out_error.clear();
    if (component == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status =
        cshost_close_runtime_context(component->runtime.close, component->context, out_error);
    if (status != SAO_OK)
        return status;
    component->context = nullptr;
    cshost_release_runtime_api(component->runtime);
    delete component;
    return SAO_OK;
}

void cshost_component_abandon(managed_component_s* component) noexcept {
    if (component == nullptr)
        return;
    if (component->resident_key_reserved && !component->assembly_process_resident) {
        try {
            std::lock_guard lock(g_resident_mutex);
            g_resident_paths.erase(component->resident_key);
            g_resident_identities.erase(component->resident_identity);
        } catch (...) {
        }
    }
    std::string ignored_error;
    if (cshost_close_runtime_context(component->runtime.close, component->context, ignored_error) ==
        SAO_OK) {
        component->context = nullptr;
    }
    cshost_release_runtime_api(component->runtime);
    delete component;
}

void cshost_reset_sdk_counters() noexcept {
    try {
        std::lock_guard lock(g_counter_mutex);
        g_counters = {};
    } catch (...) {
    }
}

int32_t cshost_get_sdk_counters(cs_sdk_counters* out_counters) noexcept {
    if (out_counters == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_counter_mutex);
        *out_counters = g_counters;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::csharp_host
