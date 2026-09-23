// csmini 原生源码与预编译托管 DLL 共用无执行预检，hostfxr 只在实际托管加载时初始化。
#include "sao/plugins/csmini/csmini_host.h"

#include "csmini_host_internal.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/script_ctx/ctx_surface.h"

#if defined(SAO_PLUGINS_ENABLE_CORECLR)
#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include "cs_component_internal.h"
#include "cs_host_internal.h"
#include "cs_sdk_bridge_internal.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sao::plugins::csmini {

using loader::host_adapter_vtable;
using loader::plugin_context_t;
using loader::plugin_handle_t;
using loader::plugin_manifest;

// adapter owner — concrete def of the opaque public handle
struct csmini_adapter_owner_s {
    // per-plugin record
    enum class route_e { csmini, coreclr };
    struct rec {
        route_e route = route_e::csmini;
        std::string plugin_id;
        std::string last_error;
        bool ready = false;
        bool native_owned = false;
        int32_t load_status = SAO_ERR_OS_CALL_FAILED;
        bool on_load_succeeded = false;
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        csharp_host::managed_component_s* component = nullptr;
        plugin_context_t* context = nullptr;
        SaoSdkContext* sdk_context = nullptr;
        SaoSdkContext* retired_sdk_context = nullptr;
        sdk_binding::plugin_binding_handle_t binding = nullptr;
        csharp_host::sdk_bridge_session* sdk_session = nullptr;
#endif
    };
    bool active = false;
    bool adapter_registered = false;
    std::wstring dotnet_root;
    std::vector<std::wstring> extra_dirs;
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    csharp_host::cs_host_handle_t host = nullptr;
    bool host_ready = false;
    bool binding_registered = false;
    bool binding_cleanup_pending = false;
#endif
    std::unordered_map<plugin_handle_t, rec> plugins;
    std::unordered_map<plugin_handle_t, std::string> last_errors;
    std::string last_error;
};

// ── utf8 ↔ wide helpers ──────────────────────────────────────────────
namespace {

std::wstring widen_u8(const std::string& u8) {
    if (u8.empty())
        return {};
    if (u8.size() > INT32_MAX || u8.find('\0') != std::string::npos)
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(),
                                      static_cast<int>(u8.size()), nullptr,
                                      0);
    if (n <= 0)
        return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()),
                        w.data(), n);
    return w;
}
std::string narrow_w(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// error-string helper owned by free via sao_plugins_csmini_free_string
char* dup_err(const std::string& s) {
    if (s.empty())
        return nullptr;
    char* b = static_cast<char*>(std::malloc(s.size() + 1));
    if (!b)
        return nullptr;
    std::memcpy(b, s.data(), s.size() + 1);
    return b;
}

using rec = csmini_adapter_owner_s::rec;
using route_t = csmini_adapter_owner_s::route_e;

std::mutex g_mu;
csmini_adapter_owner_s* g_owner = nullptr;

csmini_adapter_owner_s* active_owner(void* ud) {
    return static_cast<csmini_adapter_owner_s*>(ud);
}

void set_err(csmini_adapter_owner_s* o, std::string e) {
    if (o)
        o->last_error = std::move(e);
}
void retain_error(csmini_adapter_owner_s* o, plugin_handle_t plugin,
                  std::string e) {
    set_err(o, e);
    if (o)
        o->last_errors[plugin] = std::move(e);
}
void clear_error(csmini_adapter_owner_s* o, plugin_handle_t plugin) {
    if (o)
        o->last_errors.erase(plugin);
}

// ── coreclr delegate wrappers (only under SAO_PLUGINS_ENABLE_CORECLR) ──
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
using csharp_host::managed_hook;

int32_t ensure_coreclr_host(csmini_adapter_owner_s* o, std::string* err) {
    if (o->binding_cleanup_pending) {
        const int32_t status = csharp_host::cshost_unregister_sdk_binding_provider();
        if (status != SAO_OK && status != loader::SAO_PLUGINS_ERR_NOT_FOUND &&
            status != SAO_ERR_HANDLE_INVALID) {
            *err = "C# SDK binding registration rollback pending";
            return status;
        }
        o->binding_cleanup_pending = false;
    }
    if (o->host && !o->host_ready) {
        const int32_t status = csharp_host::sao_plugins_cshost_shutdown(o->host);
        if (status != SAO_OK) {
            *err = "C# host initialization rollback pending";
            return status;
        }
        o->host = nullptr;
    }
    if (!o->host) {
        const auto fxr = csharp_host::cshost_resolve_hostfxr_path(o->dotnet_root);
        if (fxr.empty()) {
            *err = o->dotnet_root.empty() ? "hostfxr.dll not found in default .NET layout"
                                        : "hostfxr.dll not found in configured dotnet_root";
            return SAO_ERR_NOT_INITIALIZED;
        }
        csharp_host::cs_host_config config{};
        config.hostfxr_path = fxr.c_str();
        const int32_t status = csharp_host::sao_plugins_cshost_init(&config, &o->host);
        if (status != SAO_OK) {
            *err = "hostfxr initialization failed: status=" + std::to_string(status);
            return status;
        }
        o->host_ready = true;
    }
    if (!o->binding_registered) {
        const int32_t status = csharp_host::cshost_register_sdk_binding_provider();
        if (status != SAO_OK) {
            o->binding_cleanup_pending = status != loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            *err = "C# SDK binding provider registration failed: status=" + std::to_string(status);
            return status;
        }
        o->binding_registered = true;
    }
    return SAO_OK;
}

// replicate the cs_loader_adapter assembly sequence for one plugin.
int32_t csh_load(csmini_adapter_owner_s* o, plugin_context_t* lctx,
                 const plugin_manifest* manifest, rec* r,
                 std::string* err) {
    auto*& component = r->component;
    auto*& sdk_context = r->sdk_context;
    auto& binding = r->binding;
    auto*& sdk_session = r->sdk_session;
    r->context = lctx;
    int32_t status =
        csharp_host::cshost_component_load(o->host, *manifest, &component,
                                         *err);
    if (status == SAO_OK) {
        status = sao_sdk_context_create(manifest->source_path.c_str(),
                                        manifest->plugin_id.c_str(),
                                        &sdk_context);
        if (status == SAO_OK)
            status = sao_sdk_context_bind_platform_services(sdk_context);
        if (status != SAO_OK && err)
            *err = "C# SDK context initialization failed";
    }
    if (status == SAO_OK) {
        status = csharp_host::cshost_component_attach_contexts(
            component, sdk_context, lctx);
        if (status == SAO_OK)
            status = csharp_host::cshost_sdk_bridge_prepare(
                component, lctx, sdk_context, component);
        if (status == SAO_OK) {
            status = sdk_binding::sao_plugins_binding_csharp_activate(
                reinterpret_cast<sdk_binding::plugin_context_ptr>(lctx),
                reinterpret_cast<sdk_binding::csharp_domain_ptr>(component),
                &binding);
        }
        csharp_host::cshost_sdk_bridge_cancel(component);
        sdk_session = csharp_host::cshost_sdk_session_find(component);
        if (status == SAO_OK) {
            status = sdk_session == nullptr
                         ? SAO_ERR_NOT_INITIALIZED
                         : csharp_host::cshost_sdk_bridge_set_binding(
                               sdk_session, binding);
        }
        if (status != SAO_OK && err)
            *err = "C# SDK binding provider activation failed";
    }
    if (status == SAO_OK) {
        status = csharp_host::cshost_component_initialize(
            component, sdk_context, lctx, *err);
    }
    return status;
}

// generic managed hook invoke — optional hooks skip when absent.
int32_t csh_invoke(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                   managed_hook hook, const char* hook_name, bool optional,
                   int32_t* managed_result) {
    if (!r.component)
        return SAO_ERR_HANDLE_INVALID;
    if (!csharp_host::cshost_component_has_hook(r.component, hook)) {
        if (optional) {
            if (managed_result)
                *managed_result = 0;
            return SAO_OK;
        }
        retain_error(o, ph, std::string("required managed hook missing: ") +
                                hook_name);
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string error;
    const int32_t status = csharp_host::cshost_component_invoke(
        r.component, hook, nullptr, 0, managed_result, error);
    if (status != SAO_OK)
        retain_error(o, ph,
                     error.empty()
                         ? std::string("managed hook failed: ") + hook_name
                         : std::string(hook_name) + ": " + error);
    return status;
}

int32_t csh_hook(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                 const char* which, std::string* err) {
    if (!r.component)
        return SAO_ERR_HANDLE_INVALID;
    int32_t managed_result = 0;
    int32_t status = SAO_OK;
    if (std::strcmp(which, "on_load") == 0) {
        std::string error;
        status = csharp_host::cshost_component_on_load(
            r.component, &managed_result, error);
        if (status == SAO_OK && managed_result != 0) {
            status = SAO_ERR_OS_CALL_FAILED;
            error = "OnLoad returned " + std::to_string(managed_result);
        }
        if (status != SAO_OK) {
            retain_error(o, ph, error.empty() ? "OnLoad failed" : error);
            return status;
        }
        r.on_load_succeeded = true;
        clear_error(o, ph);
        return SAO_OK;
    }
    if (std::strcmp(which, "on_enable") == 0)
        status = csh_invoke(o, ph, r, managed_hook::on_enable, "OnEnable",
                            true, &managed_result);
    else if (std::strcmp(which, "on_disable") == 0)
        status = csh_invoke(o, ph, r, managed_hook::on_disable, "OnDisable",
                            true, &managed_result);
    else
        return SAO_ERR_INVALID_ARGUMENT;
    if (status != SAO_OK)
        return status;
    if (managed_result != 0) {
        retain_error(o, ph, std::string(which) + " returned " +
                                std::to_string(managed_result));
        return SAO_ERR_OS_CALL_FAILED;
    }
    clear_error(o, ph);
    return SAO_OK;
}

int32_t csh_on_unload(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                      bool* allow, std::string* err) {
    if (!r.component || !r.on_load_succeeded) {
        *allow = true;
        return SAO_OK;
    }
    if (!csharp_host::cshost_component_has_hook(r.component,
                                              managed_hook::on_unload)) {
        *allow = true;
        clear_error(o, ph);
        return SAO_OK;
    }
    int32_t managed_result = 0;
    std::string error;
    const int32_t status = csharp_host::cshost_component_invoke(
        r.component, managed_hook::on_unload, nullptr, 0, &managed_result,
        error);
    if (status != SAO_OK) {
        retain_error(o, ph,
                     error.empty() ? "OnUnload failed" : error);
        return status;
    }
    if (managed_result == 0) {
        *allow = true;
        clear_error(o, ph);
        return SAO_OK;
    }
    if (managed_result == 1) {
        retain_error(o, ph, "OnUnload vetoed unload");
        *allow = false;
        return SAO_OK;
    }
    retain_error(o, ph, "OnUnload returned invalid result " +
                            std::to_string(managed_result));
    return SAO_ERR_OS_CALL_FAILED;
}

// teardown mirroring adapter_unload's managed-chain sequence.
int32_t csh_unload(csmini_adapter_owner_s* o, plugin_handle_t ph, rec& r,
                   std::string* err) {
    csharp_host::managed_component_s* component = r.component;
    SaoSdkContext* sdk_context = r.sdk_context;
    sdk_binding::plugin_binding_handle_t binding = r.binding;
    csharp_host::sdk_bridge_session* sdk_session = r.sdk_session;
    if (sdk_session != nullptr) {
        const int32_t status =
            csharp_host::cshost_sdk_session_quiesce(sdk_session);
        if (status != SAO_OK) {
            if (err)
                *err = "C# managed SDK session quiesce failed";
            return status;
        }
    }
    if (sdk_context != nullptr) {
        int32_t status = sao_sdk_context_try_destroy(sdk_context);
        if (status == SAO_SDK_ERR_BUSY)
            status = loader::SAO_PLUGINS_ERR_BUSY;
        if (status != SAO_OK) {
            if (sdk_session != nullptr)
                (void)csharp_host::cshost_sdk_session_resume(sdk_session);
            if (err)
                *err = "C# SDK context teardown failed";
            return status;
        }
        r.sdk_context = nullptr;
        if (sdk_session != nullptr)
            r.retired_sdk_context = sdk_context;
    }
    if (r.retired_sdk_context != nullptr) {
        const int32_t status = csharp_host::cshost_sdk_session_clear_sdk_context(
            sdk_session, r.retired_sdk_context);
        if (status != SAO_OK) {
            *err = "C# SDK context detach pending";
            return status;
        }
        r.retired_sdk_context = nullptr;
    }
    if (binding != nullptr) {
        int32_t status = sdk_session ?
            csharp_host::cshost_sdk_session_release_callbacks(sdk_session) : SAO_OK;
        if (status == SAO_OK)
            status = sdk_binding::sao_plugins_binding_csharp_deactivate(
                binding);
        if (status != SAO_OK) {
            if (err)
                *err = "C# SDK binding provider teardown failed";
            return status;
        }
        r.binding = nullptr;
    }
    if (sdk_session != nullptr) {
        const int32_t detach_status = csharp_host::cshost_sdk_bridge_set_binding(sdk_session, nullptr);
        if (detach_status != SAO_OK) {
            *err = "C# SDK binding detach pending";
            return detach_status;
        }
        const int32_t status =
            csharp_host::cshost_sdk_bridge_finish(sdk_session);
        if (status != SAO_OK) {
            if (err)
                *err = "C# managed callback release failed";
            return status;
        }
        r.sdk_session = nullptr;
    }
    if (component == nullptr)
        return SAO_OK;
    std::string close_err;
    const int32_t close_status =
        csharp_host::cshost_component_close(component, close_err);
    if (close_status != SAO_OK) {
        if (err)
            *err = std::move(close_err);
        return close_status;
    }
    r.component = nullptr;
    return SAO_OK;
}
#endif // SAO_PLUGINS_ENABLE_CORECLR

// ── route decision ───────────────────────────────────────────────────
int32_t resolve_plugin_file(const std::wstring& root, const std::string& relative,
                            std::filesystem::path* result, std::string* reason) {
    namespace fs = std::filesystem;
    const auto wide = widen_u8(relative);
    const fs::path rel(wide);
    if (wide.empty() || rel.has_root_path() || wide.find(L':') != std::wstring::npos) {
        *reason = "plugin file must be a valid UTF-8 relative path: " + relative;
        return SAO_ERR_INVALID_ARGUMENT;
    }
    for (const auto& part : rel) {
        if (part == L"..") {
            *reason = "plugin file escapes source_path: " + relative;
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    std::error_code error;
    const auto base = fs::canonical(root, error);
    if (error || !fs::is_directory(base, error)) {
        *reason = "plugin source_path is unavailable";
        return SAO_ERR_OS_CALL_FAILED;
    }
    const auto file = fs::canonical(base / rel, error);
    if (error || !fs::is_regular_file(file, error) || error) {
        *reason = "plugin file is missing or not a regular file: " + relative;
        return SAO_ERR_OS_CALL_FAILED;
    }
    const auto within = file.lexically_relative(base);
    if (within.empty() || within.has_root_path() || *within.begin() == L"..") {
        *reason = "plugin file resolves outside source_path: " + relative;
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *result = file;
    return SAO_OK;
}

template<class T>
bool read_image_value(std::string_view image, size_t offset, T* value) {
    if (offset > image.size() || sizeof(T) > image.size() - offset)
        return false;
    std::memcpy(value, image.data() + offset, sizeof(T));
    return true;
}

bool managed_metadata_layout(std::string_view metadata) {
    DWORD signature = 0;
    DWORD version_size = 0;
    if (metadata.size() < 20 || !read_image_value(metadata, 0, &signature) ||
        signature != 0x424A5342 || !read_image_value(metadata, 12, &version_size) ||
        version_size == 0 || version_size % 4 != 0 || version_size > metadata.size() - 20 ||
        metadata.substr(16, version_size).find('\0') == std::string_view::npos)
        return false;
    size_t cursor = 16 + static_cast<size_t>(version_size);
    WORD stream_count = 0;
    if (!read_image_value(metadata, cursor + 2, &stream_count) || stream_count == 0)
        return false;
    cursor += 4;
    size_t first_data = metadata.size();
    bool tables = false;
    for (size_t index = 0; index < stream_count; ++index) {
        DWORD offset = 0;
        DWORD size = 0;
        if (!read_image_value(metadata, cursor, &offset) ||
            !read_image_value(metadata, cursor + 4, &size))
            return false;
        cursor += 8;
        const auto end = metadata.find('\0', cursor);
        if (end == std::string_view::npos || end == cursor)
            return false;
        const auto name = metadata.substr(cursor, end - cursor);
        cursor = (end + 4) & ~size_t{3};
        if (cursor > metadata.size() || offset > metadata.size() ||
            size > metadata.size() - offset)
            return false;
        if (size != 0)
            first_data = (std::min)(first_data, static_cast<size_t>(offset));
        if (name == "#~" || name == "#-") {
            if (size < 24)
                return false;
            tables = true;
        }
    }
    return tables && cursor <= first_data;
}

bool managed_image_layout(const std::string& image) {
    IMAGE_DOS_HEADER dos{};
    if (!read_image_value(image, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)))
        return false;
    const size_t nt = static_cast<size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!read_image_value(image, nt, &signature) || signature != IMAGE_NT_SIGNATURE ||
        !read_image_value(image, nt + sizeof(signature), &header) ||
        (header.Characteristics & IMAGE_FILE_DLL) == 0 || header.NumberOfSections == 0)
        return false;
    const size_t optional = nt + sizeof(signature) + sizeof(header);
    WORD magic = 0;
    if (!read_image_value(image, optional, &magic))
        return false;
    IMAGE_DATA_DIRECTORY cli{};
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 value{};
        if (header.SizeOfOptionalHeader < sizeof(value) ||
            !read_image_value(image, optional, &value) ||
            value.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
            return false;
        cli = value.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 value{};
        if (header.SizeOfOptionalHeader < sizeof(value) ||
            !read_image_value(image, optional, &value) ||
            value.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
            return false;
        cli = value.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
    } else {
        return false;
    }
    const size_t sections = optional + header.SizeOfOptionalHeader;
    if (sections > image.size() ||
        header.NumberOfSections > (image.size() - sections) / sizeof(IMAGE_SECTION_HEADER))
        return false;
    const auto file_offset = [&](DWORD rva, DWORD length, size_t* output) {
        for (size_t index = 0; index < header.NumberOfSections; ++index) {
            IMAGE_SECTION_HEADER section{};
            if (!read_image_value(image, sections + index * sizeof(section), &section))
                return false;
            if (rva < section.VirtualAddress)
                continue;
            const uint64_t delta = uint64_t{rva} - section.VirtualAddress;
            const uint64_t offset = uint64_t{section.PointerToRawData} + delta;
            if (delta + length <= section.SizeOfRawData && offset <= image.size() &&
                length <= image.size() - offset) {
                *output = static_cast<size_t>(offset);
                return true;
            }
        }
        return false;
    };
    size_t cli_offset = 0;
    DWORD cli_size = 0;
    IMAGE_DATA_DIRECTORY metadata{};
    size_t metadata_offset = 0;
    return cli.VirtualAddress != 0 && cli.Size >= 72 &&
           file_offset(cli.VirtualAddress, cli.Size, &cli_offset) &&
           read_image_value(image, cli_offset, &cli_size) && cli_size >= 72 && cli_size <= cli.Size &&
           read_image_value(image, cli_offset + 8, &metadata) &&
           metadata.VirtualAddress != 0 && metadata.Size >= 20 &&
           file_offset(metadata.VirtualAddress, metadata.Size, &metadata_offset) &&
           managed_metadata_layout(std::string_view(image).substr(metadata_offset, metadata.Size));
}

int32_t classify_plugin(const plugin_manifest* m, route_t* route, std::string* reason) {
    *route = route_t::csmini;
    reason->clear();
    if (!m || m->language != loader::engine_kind::csharp || !m->native_entry.empty() ||
        m->plugin_id.empty() || m->plugin_id.find('\0') != std::string::npos ||
        m->source_path.empty() || m->entry.empty()) {
        *reason = "expected a C# manifest with plugin_id, source_path and entry";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!m->cs_runtime.empty() && m->cs_runtime != "auto" &&
        m->cs_runtime != "csmini" && m->cs_runtime != "coreclr") {
        *reason = "invalid cs_runtime: " + m->cs_runtime;
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        const auto root = widen_u8(m->source_path);
        if (root.empty()) {
            *reason = "invalid UTF-8 in source_path";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::filesystem::path entry;
        int32_t status = resolve_plugin_file(root, m->entry, &entry, reason);
        if (status != SAO_OK)
            return status;
        auto extension = std::filesystem::path(widen_u8(m->entry)).extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t ch) { return ch >= L'A' && ch <= L'Z'
                                                  ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch; });
        if (extension == L".cs") {
            const auto source = csmini_read_file(entry.wstring());
            std::string feature;
            const bool supported = csmini_preflight_subset(source, &feature);
            if (supported && m->cs_runtime != "coreclr")
                return SAO_OK;
            *reason = (feature.empty() ? "cs_runtime:coreclr cannot execute .cs source"
                                      : "unsupported csmini source feature: " + feature) +
                      std::string("; precompile to a managed .dll and provide runtimeconfig and "
                                  "managed_type; no Roslyn/source compiler is provided");
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        if (extension != L".dll" || m->cs_runtime == "csmini") {
            *reason = "csmini accepts .cs source; managed .dll entries require cs_runtime:auto/coreclr";
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        if (m->managed_type.size() > 1024 || widen_u8(m->managed_type).empty() ||
            m->managed_type.find_first_not_of(" \t\r\n") == std::string::npos) {
            *reason = "managed .dll entry requires a nonempty valid UTF-8 managed_type";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::string runtimeconfig = m->runtimeconfig;
        if (runtimeconfig.empty()) {
            auto inferred = std::filesystem::path(widen_u8(m->entry));
            inferred.replace_extension(L".runtimeconfig.json");
            runtimeconfig = narrow_w(inferred.wstring());
        }
        std::filesystem::path config;
        status = resolve_plugin_file(root, runtimeconfig, &config, reason);
        if (status != SAO_OK) {
            *reason = "runtimeconfig: " + *reason;
            return status;
        }
        const auto json = nlohmann::json::parse(csmini_read_file(config.wstring()), nullptr, false);
        if (!json.is_object() || !json.contains("runtimeOptions") ||
            !json["runtimeOptions"].is_object()) {
            *reason = "invalid runtimeconfig JSON: expected a runtimeOptions object";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (!managed_image_layout(csmini_read_file(entry.wstring()))) {
            *reason = "entry is not a valid managed PE DLL with CLR metadata";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        *route = route_t::coreclr;
        return SAO_OK;
    } catch (const cs_error& error) {
        *reason = (error.file.empty() || error.file == "<preflight>" ? m->entry : error.file) + ":" +
                  std::to_string(error.pos.line) + ":" + std::to_string(error.pos.col) +
                  ": " + error.kind + ": " + error.message;
        return error.code != SAO_OK && error.code != loader::SAO_PLUGINS_ERR_UNSUPPORTED
                   ? error.code : SAO_ERR_OS_CALL_FAILED;
    } catch (const std::exception& error) {
        *reason = error.what();
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        *reason = "C# plugin preflight failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool coreclr_layout_available(const csmini_adapter_owner_s* owner, std::string* reason) {
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    const bool available = !csharp_host::cshost_resolve_hostfxr_path(owner->dotnet_root).empty();
    *reason = available ? "hostfxr file layout found; runtime loading and framework compatibility unverified"
                        : owner->dotnet_root.empty() ? "hostfxr.dll not found in default .NET layout"
                                                    : "hostfxr.dll not found in configured dotnet_root";
    return available;
#else
    (void)owner;
    *reason = "CoreCLR delegate not built (SAO_PLUGINS_ENABLE_CORECLR off)";
    return false;
#endif
}

// ── vtable functions ─────────────────────────────────────────────────
int32_t load_failure(csmini_adapter_owner_s* owner, plugin_handle_t plugin,
                     int32_t status, const std::string& error) {
    auto found = owner->plugins.find(plugin);
    bool retained = false;
    if (found != owner->plugins.end()) {
        auto& record = found->second;
        retained = record.native_owned;
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        retained = retained || record.component || record.sdk_context || record.binding || record.sdk_session;
#endif
        if (retained) {
            record.load_status = status;
            record.last_error = error;
        } else {
            owner->plugins.erase(found);
        }
    }
    retain_error(owner, plugin, error);
    // Lifecycle calls on_load only after a successful load, then owns rollback of retained state.
    return retained ? SAO_OK : status;
}

int32_t SAO_PLUGINS_CALL adapter_load(
    plugin_handle_t plugin, const plugin_manifest* manifest,
    void* host_user_data) try {
    csmini_adapter_owner_s* o = active_owner(host_user_data);
    std::lock_guard<std::mutex> g(g_mu);
    if (!o || o != g_owner || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    if (!plugin || !manifest)
        return SAO_ERR_INVALID_ARGUMENT;
    if (o->plugins.count(plugin))
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    try {
        clear_error(o, plugin);
        o->last_error.clear();
        route_t route;
        std::string error;
        int32_t status = classify_plugin(manifest, &route, &error);
        if (status != SAO_OK)
            return load_failure(o, plugin, status, error);
        if (route == route_t::coreclr) {
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
            status = ensure_coreclr_host(o, &error);
#else
            status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
            error = "CoreCLR delegate not built (SAO_PLUGINS_ENABLE_CORECLR off)";
#endif
            if (status != SAO_OK)
                return load_failure(o, plugin, status, error);
        }
        plugin_context_t* lctx = nullptr;
        status = loader::sao_plugins_lifecycle_get_context(plugin, &lctx);
        if (status != SAO_OK || !lctx)
            return load_failure(o, plugin, status == SAO_OK ? SAO_ERR_NOT_INITIALIZED : status,
                                "loader plugin context unavailable");
        sao::plugins::script_ctx::ctx_surface_advisory_check(
            loader::engine_kind::csharp, lctx, manifest);
        rec candidate;
        candidate.route = route;
        candidate.plugin_id = manifest->plugin_id;
        rec& slot = o->plugins.emplace(plugin, std::move(candidate)).first->second;
        if (route == route_t::csmini) {
            const auto dir = widen_u8(manifest->source_path);
            const int rc = csmini_host_load_plugin(
                lctx, manifest->plugin_id.c_str(), dir.c_str(),
                widen_u8(manifest->entry).c_str(), o->extra_dirs, &error, &slot.native_owned);
            status = rc == 0 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
        } else {
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
            status = csh_load(o, lctx, manifest, &slot, &error);
#endif
        }
        if (status != SAO_OK)
            return load_failure(o, plugin, status, error.empty() ? "C# plugin load failed" : error);
        slot.ready = true;
        return SAO_OK;
    } catch (const cs_error& error) {
        return load_failure(o, plugin, SAO_ERR_OS_CALL_FAILED, error.kind + ": " + error.message);
    } catch (const std::exception& error) {
        return load_failure(o, plugin, SAO_ERR_OS_CALL_FAILED, error.what());
    } catch (...) {
        return load_failure(o, plugin, SAO_ERR_OS_CALL_FAILED, "C# plugin load failed before completion");
    }
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t call_named(plugin_handle_t plugin, void* ud, const char* hook,
                   bool* allow_unload = nullptr) try {
    csmini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    if (o != g_owner || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end()) {
        if (allow_unload && o->last_errors.count(plugin)) {
            *allow_unload = true;
            return SAO_OK;
        }
        return SAO_ERR_HANDLE_INVALID;
    }
    rec& r = it->second;
    if (!r.ready) {
        if (allow_unload) {
            *allow_unload = true;
            return SAO_OK;
        }
        return r.load_status;
    }
    std::string err;
    if (r.route == route_t::csmini) {
        if (allow_unload) {
            CsRef ret;
            const int rc = csmini_host_call_hook_ret(
                r.plugin_id.c_str(), hook, nullptr, &ret, &err);
            if (rc == 0) {
                *allow_unload =
                    !r.on_load_succeeded || (ret && as_bool(ret) ? as_bool(ret)->v : true);
            } else if (rc == 1) {
                *allow_unload = true;      // hook absent → allow
            } else if (!r.on_load_succeeded) {
                *allow_unload = true;
            } else {
                retain_error(o, plugin, err);
                return SAO_ERR_OS_CALL_FAILED;
            }
            return SAO_OK;
        }
        const int rc =
            csmini_host_call_hook(r.plugin_id.c_str(), hook, nullptr, &err);
        if (rc == 0 || rc == 1) {
            if (std::strcmp(hook, "on_load") == 0)
                r.on_load_succeeded = true;
            return SAO_OK;
        }
        if (std::strcmp(hook, "on_load") == 0)
            r.last_error = err;
        retain_error(o, plugin, err);
        return SAO_ERR_OS_CALL_FAILED;
    }
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    if (allow_unload)
        return csh_on_unload(o, plugin, r, allow_unload, &err);
    const int32_t status = csh_hook(o, plugin, r, hook, &err);
    if (status != SAO_OK && std::strcmp(hook, "on_load") == 0)
        r.last_error = o->last_error;
    return status;
#else
    (void)allow_unload;
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t SAO_PLUGINS_CALL adapter_on_load(plugin_handle_t plugin,
                                         void* ud) {
    return call_named(plugin, ud, "on_load");
}
int32_t SAO_PLUGINS_CALL adapter_on_enable(plugin_handle_t plugin,
                                           void* ud) {
    return call_named(plugin, ud, "on_enable");
}
int32_t SAO_PLUGINS_CALL adapter_on_disable(plugin_handle_t plugin,
                                            void* ud) {
    return call_named(plugin, ud, "on_disable");
}
int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t plugin,
                                           bool* allow, void* ud) {
    if (!allow)
        return SAO_ERR_INVALID_ARGUMENT;
    *allow = false;
    return call_named(plugin, ud, "on_unload", allow);
}

int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t plugin,
                                        void* ud) try {
    csmini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::mutex> g(g_mu);
    if (o != g_owner || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return o->last_errors.count(plugin) ? SAO_OK : SAO_ERR_HANDLE_INVALID;
    rec& r = it->second;
    std::string err;
    int32_t status = SAO_OK;
    if (r.route == route_t::csmini) {
        if (r.native_owned) {
            status = csmini_host_unload_plugin(r.plugin_id.c_str(), &err);
            if (status == SAO_OK)
                r.native_owned = false;
        }
    }
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
    else
        status = csh_unload(o, plugin, r, &err);
#endif
    if (status != SAO_OK) {
        if (err.empty())
            err = "C# plugin teardown failed: status=" + std::to_string(status);
        retain_error(o, plugin, r.last_error.empty() ? err : r.last_error + "; cleanup: " + err);
        return status;
    }
    if (!r.last_error.empty())
        retain_error(o, plugin, r.last_error);
    else
        clear_error(o, plugin);
    o->plugins.erase(it);
    return SAO_OK;
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t SAO_PLUGINS_CALL adapter_last_error(void* user_data,
                                            loader::plugin_handle_t plugin, char** out_utf8) {
    return sao_plugins_csmini_adapter_get_last_error(
        static_cast<csmini_adapter_owner_t>(user_data), plugin, out_utf8);
}

host_adapter_vtable make_vtable(csmini_adapter_owner_s* o) {
    host_adapter_vtable t{};
    t.load_plugin = adapter_load;
    t.call_on_load = adapter_on_load;
    t.call_on_enable = adapter_on_enable;
    t.call_on_disable = adapter_on_disable;
    t.call_on_unload = adapter_on_unload;
    t.unload_plugin = adapter_unload;
    t.host_user_data = o;
    t.get_last_error = adapter_last_error;
    t.free_error_string = sao_plugins_csmini_free_string;
    return t;
}

} // namespace

// ═══ public C-ABI exports ═══════════════════════════════════════════
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_register_loader_adapter(
    const csmini_adapter_config* cfg,
    csmini_adapter_owner_t* out_owner) {
    if (!out_owner)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_owner)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        auto o = std::make_unique<csmini_adapter_owner_s>();
        if (cfg && cfg->dotnet_root)
            o->dotnet_root = cfg->dotnet_root;
        if (cfg && cfg->extra_assembly_dirs)
            for (uint32_t k = 0; k < cfg->extra_assembly_dirs_count; ++k)
                if (cfg->extra_assembly_dirs[k])
                    o->extra_dirs.emplace_back(
                        cfg->extra_assembly_dirs[k]);

        const auto table = make_vtable(o.get());
        const int32_t st =
            loader::sao_plugins_lifecycle_register_host_adapter(
                loader::engine_kind::csharp, &table);
        if (st != SAO_OK)
            return st;
        o->adapter_registered = true;
        o->active = true;
        g_owner = o.get();
        *out_owner = o.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_unregister_loader_adapter(
    csmini_adapter_owner_t owner) {
    if (!owner)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (owner != g_owner)
            return SAO_ERR_HANDLE_INVALID;
        if (!owner->plugins.empty())
            return loader::SAO_PLUGINS_ERR_BUSY;
        owner->active = false;
        int32_t st = SAO_OK;
        if (owner->adapter_registered) {
            st = loader::sao_plugins_lifecycle_unregister_host_adapter(
                loader::engine_kind::csharp);
            if (st != SAO_OK) {
                owner->active = true;
                return st;
            }
            owner->adapter_registered = false;
        }
#if defined(SAO_PLUGINS_ENABLE_CORECLR)
        if (owner->binding_registered || owner->binding_cleanup_pending) {
            st = csharp_host::cshost_unregister_sdk_binding_provider();
            if (st != SAO_OK && st != loader::SAO_PLUGINS_ERR_NOT_FOUND &&
                st != SAO_ERR_HANDLE_INVALID) {
                set_err(owner, "C# SDK binding provider unregister pending: status=" + std::to_string(st));
                return st;
            }
            owner->binding_registered = false;
            owner->binding_cleanup_pending = false;
        }
        if (owner->host) {
            st = csharp_host::sao_plugins_cshost_shutdown(owner->host);
            if (st != SAO_OK) {
                set_err(owner, "C# host shutdown pending: status=" + std::to_string(st));
                return st;
            }
            owner->host = nullptr;
            owner->host_ready = false;
        }
#endif
        g_owner = nullptr;
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_plugin_count(csmini_adapter_owner_t owner) try {
    if (!owner)
        return 0;
    std::lock_guard<std::mutex> g(g_mu);
    if (owner != g_owner)
        return 0;
    return owner->plugins.size();
} catch (...) {
    return 0;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_requires_dotnet(
    csmini_adapter_owner_t owner, const loader::plugin_manifest* manifest,
    bool* out_required, bool* out_available, char** out_reason) {
    if (out_required)
        *out_required = false;
    if (out_available)
        *out_available = false;
    if (out_reason)
        *out_reason = nullptr;
    if (!owner || !manifest || !out_required || !out_available || !out_reason)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        if (owner != g_owner || !owner->active)
            return SAO_ERR_HANDLE_INVALID;
        route_t route;
        std::string reason;
        const int32_t status = classify_plugin(manifest, &route, &reason);
        const bool required = status == SAO_OK && route == route_t::coreclr;
        const bool available = required && coreclr_layout_available(owner, &reason);
        char* copy = dup_err(reason);
        if (!reason.empty() && !copy)
            return SAO_ERR_OS_CALL_FAILED;
        *out_required = required;
        *out_available = available;
        *out_reason = copy;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_adapter_get_last_error(
    csmini_adapter_owner_t owner, void* loader_plugin_handle,
    char** out_utf8) {
    if (out_utf8)
        *out_utf8 = nullptr;
    if (!owner || !out_utf8)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> g(g_mu);
        if (owner != g_owner)
            return SAO_ERR_HANDLE_INVALID;
        std::string e = owner->last_error;
        auto it = owner->last_errors.find(
            static_cast<plugin_handle_t>(loader_plugin_handle));
        if (it != owner->last_errors.end())
            e = it->second;
        if (e.empty())
            return SAO_ERR_HANDLE_INVALID;
        *out_utf8 = dup_err(e);
        return *out_utf8 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_csmini_free_string(char* value) {
    std::free(value);
}

// script_ctx bridge hookup — forward to the TU that owns the ops struct.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_register_script_engine(void) {
    return csmini_register_script_engine();
}
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_csmini_unregister_script_engine(void) {
    return csmini_unregister_script_engine();
}

} // namespace sao::plugins::csmini
