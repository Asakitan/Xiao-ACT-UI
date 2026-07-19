#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"

#if defined(SAO_HAS_PYTHON_EMBED)
#define PY_SSIZE_T_CLEAN
#include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::python_host {

namespace fs = std::filesystem;
using json = nlohmann::json;
using loader_plugin_handle_t = sao::plugins::loader::plugin_handle_t;

struct adapter_plugin_record {
    py_plugin_handle_t python_plugin = nullptr;
    sao::plugins::loader::plugin_context_t* loader_context = nullptr;
    std::string requirements_report;
    enum class state {
        loading,
        ready,
        unloading,
    } lifecycle = state::loading;
    size_t active_calls = 0;
};

struct py_loader_adapter_owner_s {
    py_host_handle_t host = nullptr;
    bool active = false;
    bool lifecycle_adapter_registered = false;
#if defined(SAO_HAS_PYTHON_EMBED)
    DWORD registration_thread_id = 0;
    PyThreadState* released_thread_state = nullptr;
    PyGILState_STATE registration_gil_state = PyGILState_LOCKED;
    bool registration_gil_was_ensured = false;
#endif
    std::unordered_map<loader_plugin_handle_t, adapter_plugin_record> plugins;
    std::unordered_map<loader_plugin_handle_t, std::string> last_errors;
    std::unordered_map<loader_plugin_handle_t, std::string> requirements_reports;
};

namespace {

std::mutex g_adapter_mutex;
py_loader_adapter_owner_s* g_adapter_owner = nullptr;

#if defined(SAO_HAS_PYTHON_EMBED)
class gil_guard {
  public:
    gil_guard() : state_(PyGILState_Ensure()) {}
    ~gil_guard() {
        PyGILState_Release(state_);
    }

    gil_guard(const gil_guard&) = delete;
    gil_guard& operator=(const gil_guard&) = delete;

  private:
    PyGILState_STATE state_;
};

class existing_interpreter_gil {
  public:
    existing_interpreter_gil() {
        if (Py_IsInitialized() != 0) {
            state_ = PyGILState_Ensure();
            active_ = true;
        }
    }

    ~existing_interpreter_gil() {
        if (active_ && Py_IsInitialized() != 0) {
            PyGILState_Release(state_);
        }
    }

    existing_interpreter_gil(const existing_interpreter_gil&) = delete;
    existing_interpreter_gil& operator=(const existing_interpreter_gil&) = delete;

    bool active() const {
        return active_;
    }
    PyGILState_STATE state() const {
        return state_;
    }
    void dismiss() {
        active_ = false;
    }

  private:
    PyGILState_STATE state_ = PyGILState_LOCKED;
    bool active_ = false;
};
#else
class gil_guard {
  public:
    gil_guard() = default;
};
#endif

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int needed =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string result(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), needed, nullptr,
                            nullptr) != needed) {
        return {};
    }
    return result;
}

bool utf8_to_wide(const std::string& value, std::wstring& output) {
    output.clear();
    if (value.empty())
        return false;
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0)
        return false;
    output.resize(static_cast<size_t>(needed));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), output.data(), needed) == needed;
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
                          return std::isspace(ch) != 0;
                      }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string distribution_name(std::string line) {
    const auto comment = line.find('#');
    if (comment != std::string::npos)
        line.resize(comment);
    line = trim(std::move(line));
    if (line.empty() || line.front() == '-')
        return {};
    const auto extra = line.find('[');
    const auto version = line.find_first_of("<>=!~; \t");
    const auto stop = std::min(extra == std::string::npos ? line.size() : extra,
                               version == std::string::npos ? line.size() : version);
    line.resize(stop);
    return trim(std::move(line));
}

std::string import_name_for(const std::string& distribution) {
    std::string lower = distribution;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower == "pillow")
        return "PIL";
    if (lower == "pyyaml")
        return "yaml";
    if (lower == "beautifulsoup4")
        return "bs4";
    if (lower == "opencv-python-headless" || lower == "opencv-python") {
        return "cv2";
    }
    if (lower == "skia-python")
        return "skia";
    if (lower == "protobuf")
        return "google.protobuf";
    return distribution;
}

bool extension_module_present(const fs::path& root, const std::string& import_name) {
    std::error_code error;
    fs::path relative;
    size_t start = 0;
    while (start < import_name.size()) {
        const auto dot = import_name.find('.', start);
        relative /= fs::u8path(
            import_name.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    const fs::path candidate = root / relative;
    if (fs::is_directory(candidate, error) ||
        fs::is_regular_file(candidate.wstring() + L".py", error) ||
        fs::is_regular_file(candidate.wstring() + L".pyd", error) ||
        fs::is_regular_file(candidate.wstring() + L".dll", error)) {
        return true;
    }
    error.clear();
    const auto parent = candidate.parent_path();
    if (!fs::is_directory(parent, error))
        return false;
    std::wstring prefix = candidate.filename().wstring() + L".";
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), towlower);
    for (fs::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error))
            continue;
        std::wstring name = it->path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        if (name.rfind(prefix, 0) == 0 && it->path().extension() == L".pyd") {
            return true;
        }
    }
    return false;
}

int32_t requirements_report(const wchar_t* plugin_dir, std::string& output) {
    output.clear();
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        const fs::path root(plugin_dir);
        std::error_code error;
        if (!fs::is_directory(root, error))
            return SAO_ERR_HANDLE_INVALID;

        struct search_root {
            const char* label;
            fs::path path;
        };
        std::vector<search_root> roots;
        json report = {{"added", json::array()}, {"deps", json::object()}};
        for (const auto* name : {L"engine", L"libs", L"vendor"}) {
            const fs::path candidate = root / name;
            if (!fs::is_directory(candidate, error)) {
                error.clear();
                continue;
            }
            const auto canonical = fs::weakly_canonical(candidate, error);
            const fs::path reported = error ? candidate.lexically_normal() : canonical;
            error.clear();
            const char* label = name == std::wstring(L"engine")
                                    ? "engine"
                                    : (name == std::wstring(L"libs") ? "libs" : "vendor");
            roots.push_back({label, reported});
            report["added"].push_back(wide_to_utf8(reported.native()));
        }

        const fs::path requirements = root / L"requirements.txt";
        if (fs::is_regular_file(requirements, error)) {
            std::ifstream input(requirements, std::ios::binary);
            if (!input)
                return SAO_ERR_HANDLE_INVALID;
            std::string line;
            while (std::getline(input, line)) {
                const std::string distribution = distribution_name(line);
                if (distribution.empty())
                    continue;
                const std::string import_name = import_name_for(distribution);
                const auto found = std::find_if(
                    roots.begin(), roots.end(), [&import_name](const search_root& item) {
                        return extension_module_present(item.path, import_name);
                    });
                report["deps"][distribution] = found == roots.end() ? "missing" : found->label;
            }
        }
        output = report.dump();
        return SAO_OK;
    } catch (...) {
        output.clear();
        return SAO_ERR_OS_CALL_FAILED;
    }
}

std::string copy_plugin_error(py_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return {};
    char* error = nullptr;
    if (sao_plugins_pyhost_get_last_error(plugin, &error) != SAO_OK || error == nullptr) {
        return {};
    }
    std::string result(error);
    sao_plugins_pyhost_free_string(error);
    return result;
}

int32_t copy_string_result(const std::string& value, char** output) {
    if (output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    auto* buffer = static_cast<char*>(std::malloc(value.size() + 1));
    if (buffer == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    if (!value.empty())
        std::memcpy(buffer, value.data(), value.size());
    buffer[value.size()] = '\0';
    *output = buffer;
    return SAO_OK;
}

py_loader_adapter_owner_s* active_owner(void* host_user_data) {
    auto* owner = static_cast<py_loader_adapter_owner_s*>(host_user_data);
    return owner != nullptr && owner == g_adapter_owner && owner->active ? owner : nullptr;
}

class plugin_call_lease {
  public:
    plugin_call_lease() = default;
    ~plugin_call_lease() {
        reset();
    }

    plugin_call_lease(const plugin_call_lease&) = delete;
    plugin_call_lease& operator=(const plugin_call_lease&) = delete;

    py_plugin_handle_t python_plugin() const {
        return python_plugin_;
    }

    void assign(py_loader_adapter_owner_s* owner, loader_plugin_handle_t loader_plugin,
                py_plugin_handle_t python_plugin) {
        owner_ = owner;
        loader_plugin_ = loader_plugin;
        python_plugin_ = python_plugin;
    }

    void reset() {
        if (owner_ == nullptr)
            return;
        std::lock_guard lock(g_adapter_mutex);
        const auto found = owner_->plugins.find(loader_plugin_);
        if (found != owner_->plugins.end() && found->second.python_plugin == python_plugin_ &&
            found->second.active_calls > 0) {
            --found->second.active_calls;
        }
        owner_ = nullptr;
        loader_plugin_ = nullptr;
        python_plugin_ = nullptr;
    }

  private:
    py_loader_adapter_owner_s* owner_ = nullptr;
    loader_plugin_handle_t loader_plugin_ = nullptr;
    py_plugin_handle_t python_plugin_ = nullptr;
};

int32_t acquire_plugin(void* host_user_data, loader_plugin_handle_t plugin,
                       plugin_call_lease& lease) {
    std::lock_guard lock(g_adapter_mutex);
    auto* owner = active_owner(host_user_data);
    if (owner == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    const auto found = owner->plugins.find(plugin);
    if (found == owner->plugins.end() || found->second.python_plugin == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second.lifecycle != adapter_plugin_record::state::ready) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second.active_calls;
    lease.assign(owner, plugin, found->second.python_plugin);
    return SAO_OK;
}

void retain_last_error(py_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                       std::string error) {
    std::lock_guard lock(g_adapter_mutex);
    if (owner == g_adapter_owner && owner->active) {
        owner->last_errors[plugin] = std::move(error);
    }
}

void finish_failed_load(py_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                        std::string error) {
    std::lock_guard lock(g_adapter_mutex);
    if (owner != g_adapter_owner)
        return;
    owner->plugins.erase(plugin);
    owner->last_errors[plugin] = std::move(error);
}

int32_t SAO_PLUGINS_CALL adapter_load(loader_plugin_handle_t plugin,
                                      const sao::plugins::loader::plugin_manifest* manifest,
                                      void* host_user_data) {
    py_loader_adapter_owner_s* owner = nullptr;
    py_plugin_handle_t python_plugin = nullptr;
    sao::plugins::loader::plugin_context_t* loader_context = nullptr;
    bool load_reserved = false;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            if (plugin == nullptr || manifest == nullptr || manifest->source_path.empty()) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (owner->plugins.contains(plugin)) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            owner->last_errors.erase(plugin);
            owner->requirements_reports.erase(plugin);
            owner->plugins.emplace(plugin, adapter_plugin_record{});
            load_reserved = true;
        }

        int32_t status =
            sao::plugins::loader::sao_plugins_lifecycle_get_context(plugin, &loader_context);
        if (status != SAO_OK || loader_context == nullptr) {
            finish_failed_load(owner, plugin, "canonical loader context is unavailable");
            return status == SAO_OK ? SAO_ERR_NOT_INITIALIZED : status;
        }

        std::wstring plugin_dir;
        if (!utf8_to_wide(manifest->source_path, plugin_dir)) {
            finish_failed_load(owner, plugin, "plugin source_path is not valid UTF-8");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::string report;
        const int32_t report_status = requirements_report(plugin_dir.c_str(), report);
        if (report_status != SAO_OK) {
            finish_failed_load(owner, plugin, "requirements report failed");
            return report_status;
        }
        {
            std::lock_guard lock(g_adapter_mutex);
            owner->requirements_reports[plugin] = report;
        }

        status = SAO_OK;
        std::string error;
        {
            gil_guard gil;
            status = sao_plugins_pyhost_load_plugin(
                owner->host, plugin_dir.c_str(), manifest->entry.c_str(),
                manifest->plugin_id.c_str(), nullptr, &python_plugin);
            if (status == SAO_OK) {
                status = sao_plugins_pyhost_ctx_bind_loader_context(
                    sao_plugins_pyhost_get_ctx_pyobject(python_plugin), loader_context);
            }
            if (status != SAO_OK) {
                error = copy_plugin_error(python_plugin);
                if (python_plugin != nullptr) {
                    (void)sao_plugins_pyhost_unload_plugin(python_plugin);
                    python_plugin = nullptr;
                }
            }
        }
        if (status != SAO_OK) {
            finish_failed_load(owner, plugin,
                               error.empty() ? "Python plugin load failed" : std::move(error));
            return status;
        }
        {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() ||
                found->second.lifecycle != adapter_plugin_record::state::loading) {
                status = SAO_ERR_HANDLE_INVALID;
            } else {
                found->second.python_plugin = python_plugin;
                found->second.loader_context = loader_context;
                found->second.requirements_report = std::move(report);
                found->second.lifecycle = adapter_plugin_record::state::ready;
                python_plugin = nullptr;
                load_reserved = false;
            }
        }
        if (python_plugin != nullptr) {
            gil_guard gil;
            (void)sao_plugins_pyhost_unload_plugin(python_plugin);
        }
        if (status != SAO_OK) {
            finish_failed_load(owner, plugin, "Python plugin load was cancelled");
        }
        load_reserved = false;
        if (status != SAO_OK)
            return status;
        return SAO_OK;
    } catch (...) {
        if (python_plugin != nullptr) {
            gil_guard gil;
            (void)sao_plugins_pyhost_unload_plugin(python_plugin);
        }
        if (load_reserved && owner != nullptr) {
            finish_failed_load(owner, plugin, "Python plugin load failed");
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

template <typename Callback>
int32_t with_plugin(loader_plugin_handle_t plugin, void* host_user_data, Callback&& callback) {
    plugin_call_lease lease;
    int32_t status = acquire_plugin(host_user_data, plugin, lease);
    if (status != SAO_OK)
        return status;
    std::string error;
    {
        gil_guard gil;
        status = callback(lease.python_plugin());
        if (status != SAO_OK) {
            error = copy_plugin_error(lease.python_plugin());
        }
    }
    if (!error.empty()) {
        auto* owner = static_cast<py_loader_adapter_owner_s*>(host_user_data);
        retain_last_error(owner, plugin, std::move(error));
    }
    return status;
}

int32_t SAO_PLUGINS_CALL adapter_on_load(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        return with_plugin(plugin, host_user_data, [](py_plugin_handle_t python_plugin) {
            return sao_plugins_pyhost_has_hook(python_plugin, "on_load")
                       ? sao_plugins_pyhost_call_on_load(python_plugin)
                       : SAO_OK;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_enable(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        return with_plugin(plugin, host_user_data, [](py_plugin_handle_t python_plugin) {
            return sao_plugins_pyhost_has_hook(python_plugin, "on_enable")
                       ? sao_plugins_pyhost_call_on_enable(python_plugin)
                       : SAO_OK;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_disable(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        return with_plugin(plugin, host_user_data, [](py_plugin_handle_t python_plugin) {
            return sao_plugins_pyhost_has_hook(python_plugin, "on_disable")
                       ? sao_plugins_pyhost_call_on_disable(python_plugin)
                       : SAO_OK;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(loader_plugin_handle_t plugin, bool* allow_unload,
                                           void* host_user_data) {
    if (allow_unload != nullptr)
        *allow_unload = true;
    try {
        return with_plugin(
            plugin, host_user_data, [allow_unload](py_plugin_handle_t python_plugin) -> int32_t {
                if (!sao_plugins_pyhost_has_hook(python_plugin, "on_unload")) {
                    return SAO_OK;
                }
                char* result = nullptr;
                const int32_t status =
                    sao_plugins_pyhost_call_hook(python_plugin, "on_unload", nullptr, &result);
                if (status == SAO_OK && result != nullptr && allow_unload != nullptr) {
                    const json parsed = json::parse(result, nullptr, false);
                    if (parsed.is_boolean() && !parsed.get<bool>()) {
                        *allow_unload = false;
                    }
                }
                sao_plugins_pyhost_free_string(result);
                return status;
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        py_loader_adapter_owner_s* owner = nullptr;
        py_plugin_handle_t python_plugin = nullptr;
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || found->second.python_plugin == nullptr) {
                return SAO_ERR_HANDLE_INVALID;
            }
            if (found->second.lifecycle != adapter_plugin_record::state::ready ||
                found->second.active_calls != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second.lifecycle = adapter_plugin_record::state::unloading;
            python_plugin = found->second.python_plugin;
        }
        int32_t status = SAO_OK;
        std::string error;
        {
            gil_guard gil;
            status = sao_plugins_pyhost_unload_plugin(python_plugin);
            if (status != SAO_OK)
                error = copy_plugin_error(python_plugin);
        }
        std::lock_guard lock(g_adapter_mutex);
        const auto found = owner->plugins.find(plugin);
        if (status != SAO_OK) {
            owner->last_errors[plugin] = error.empty() ? "Python plugin unload failed" : error;
            if (found != owner->plugins.end()) {
                found->second.lifecycle = adapter_plugin_record::state::ready;
            }
            return status;
        }
        if (found != owner->plugins.end())
            owner->plugins.erase(found);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sao::plugins::loader::host_adapter_vtable adapter_vtable(py_loader_adapter_owner_s* owner) {
    sao::plugins::loader::host_adapter_vtable table{};
    table.load_plugin = adapter_load;
    table.call_on_load = adapter_on_load;
    table.call_on_enable = adapter_on_enable;
    table.call_on_disable = adapter_on_disable;
    table.call_on_unload = adapter_on_unload;
    table.unload_plugin = adapter_unload;
    table.host_user_data = owner;
    return table;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_register_loader_adapter(
    const py_host_config* cfg, py_loader_adapter_owner_t* out_owner) {
    if (out_owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard lock(g_adapter_mutex);
        if (g_adapter_owner != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
#if defined(SAO_HAS_PYTHON_EMBED)
        existing_interpreter_gil initial_gil;
#endif
        auto owner = std::make_unique<py_loader_adapter_owner_s>();
        int32_t status = sao_plugins_pyhost_init(cfg, &owner->host);
        if (status != SAO_OK)
            return status;
        const auto table = adapter_vtable(owner.get());
        status = sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
            sao::plugins::loader::engine_kind::python, &table);
        if (status != SAO_OK) {
            (void)sao_plugins_pyhost_shutdown(owner->host);
            return status;
        }
        owner->lifecycle_adapter_registered = true;
#if defined(SAO_HAS_PYTHON_EMBED)
        owner->registration_thread_id = GetCurrentThreadId();
        owner->registration_gil_was_ensured = initial_gil.active();
        owner->registration_gil_state = initial_gil.state();
        initial_gil.dismiss();
        owner->released_thread_state = PyEval_SaveThread();
#endif
        owner->active = true;
        g_adapter_owner = owner.get();
        *out_owner = owner.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unregister_loader_adapter(py_loader_adapter_owner_t owner) {
    if (owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(g_adapter_mutex);
        if (owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!owner->plugins.empty()) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
#if defined(SAO_HAS_PYTHON_EMBED)
        if (owner->registration_thread_id != GetCurrentThreadId() ||
            owner->released_thread_state == nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
#endif
        owner->active = false;
        lock.unlock();

        int32_t status = SAO_OK;
        if (owner->lifecycle_adapter_registered) {
            status = sao::plugins::loader::sao_plugins_lifecycle_unregister_host_adapter(
                sao::plugins::loader::engine_kind::python);
            if (status != SAO_OK) {
                lock.lock();
                owner->active = true;
                return status;
            }
            owner->lifecycle_adapter_registered = false;
        }

#if defined(SAO_HAS_PYTHON_EMBED)
        PyThreadState* released_thread_state = owner->released_thread_state;
        owner->released_thread_state = nullptr;
        PyEval_RestoreThread(released_thread_state);
#endif
        status = sao_plugins_pyhost_shutdown(owner->host);
        if (status != SAO_OK) {
#if defined(SAO_HAS_PYTHON_EMBED)
            owner->released_thread_state = PyEval_SaveThread();
#endif
            const auto table = adapter_vtable(owner);
            const int32_t restore =
                sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
                    sao::plugins::loader::engine_kind::python, &table);
            lock.lock();
            owner->lifecycle_adapter_registered = restore == SAO_OK;
            owner->active = true;
            return status;
        }
#if defined(SAO_HAS_PYTHON_EMBED)
        if (Py_IsInitialized() != 0) {
            if (owner->registration_gil_was_ensured) {
                PyGILState_Release(owner->registration_gil_state);
            } else {
                (void)PyEval_SaveThread();
            }
        }
#endif
        lock.lock();
        owner->host = nullptr;
        g_adapter_owner = nullptr;
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pyhost_loader_adapter_plugin_count(py_loader_adapter_owner_t owner) {
    try {
        std::lock_guard lock(g_adapter_mutex);
        return owner != nullptr && owner == g_adapter_owner && owner->active ? owner->plugins.size()
                                                                             : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_loader_adapter_get_last_error(py_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle, char** out_utf8) {
    if (out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_utf8 = nullptr;
    try {
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        std::string error;
        plugin_call_lease lease;
        if (acquire_plugin(owner, plugin, lease) == SAO_OK) {
            gil_guard gil;
            error = copy_plugin_error(lease.python_plugin());
        }
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            const auto retained = owner->last_errors.find(plugin);
            if (error.empty() && retained != owner->last_errors.end()) {
                error = retained->second;
            }
        }
        return copy_string_result(error, out_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_loader_adapter_get_requirements_report(py_loader_adapter_owner_t owner,
                                                          void* loader_plugin_handle,
                                                          char** out_json_utf8) {
    if (out_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_json_utf8 = nullptr;
    try {
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        std::string report;
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            const auto found = owner->requirements_reports.find(plugin);
            if (found == owner->requirements_reports.end()) {
                return SAO_ERR_HANDLE_INVALID;
            }
            report = found->second;
        }
        return copy_string_result(report, out_json_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_report_requirements(const wchar_t* plugin_dir, char** out_json_utf8) {
    if (out_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_json_utf8 = nullptr;
    std::string report;
    const int32_t status = requirements_report(plugin_dir, report);
    return status == SAO_OK ? copy_string_result(report, out_json_utf8) : status;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_pyhost_free_string(char* value) {
    std::free(value);
}

} // namespace sao::plugins::python_host