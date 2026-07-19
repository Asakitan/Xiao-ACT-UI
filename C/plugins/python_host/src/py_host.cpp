// py_host.cpp — CPython 生命周期 + Wave 7 真加载/卸载/hook 调用
//
// 已实装 (Wave 2 → Wave 7 完整):
//   - sao_plugins_pyhost_init  : Py_InitializeEx(0) + 注入 sao_sdk + shim
//   - sao_plugins_pyhost_shutdown : Py_Finalize + 释放 handle
//   - sao_plugins_pyhost_version : Py_GetVersion 字符串直接透传
//   - sao_plugins_pyhost_available : PYTHON_HOME 探测
//   - sao_plugins_pyhost_load_plugin : 用 W1c compat 读 manifest → W4b sys.path
//     前插 libs/vendor → wrap_ctx → import 模块 → 抽 hook 函数
//   - sao_plugins_pyhost_call_on_load / on_enable / on_disable / on_unload
//   - sao_plugins_pyhost_unload_plugin : DECREF module + hook, 从 sys.modules 删
//   - sao_plugins_pyhost_call_hook : 任意 hook (未来 SDK 扩展)
//   - sao_plugins_pyhost_has_hook
//   - sao_plugins_pyhost_get_ctx_pyobject / get_module_pyobject / get_manifest
//     / get_last_error (内省 API, 供测试与 host 上层查)
//
// 关键设计:
//   - 每个 py_plugin_handle_t 拥有: manifest 副本 + ctx PyObject + module PyObject
//     + hook function PyObjects (on_load/on_enable/on_disable/on_unload)
//   - "老插件不改一个字" 靠 W1c + W4b + sao_sdk 内置模块 三层配合;
//     即使 on_load 抛异常 (缺依赖 / 缺游戏状态) 我们记 traceback 返回给宿主, 不 crash
//   - sys.path 修改用"记录旧值 → 前插 → unload 时 pop"的对称模式, 避免污染

#if defined(SAO_HAS_PYTHON_EMBED)
#define PY_SSIZE_T_CLEAN
#include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/compat/libs_vendor_bridge.h"
#include "sao/plugins/compat/py_v1_manifest.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"
#include "sao/sdk/sao_sdk.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace sao::plugins::python_host {

namespace {

struct py_host_state {
    bool initialized = false;
    bool owns_finalize = false;
    std::string python_home_utf8;
    // Wave 7: 是否已注入 sao_sdk 内置模块 (只需一次).
    bool sao_sdk_registered = false;
    std::wstring python_home;
    std::wstring platform_site_dir;
    bool controlled_test_shim = false;
    size_t active_plugins = 0;
};

std::mutex g_singleton_mu;
py_host_state* g_singleton = nullptr;
std::atomic<int32_t> g_ref_count{0};

#if defined(SAO_HAS_PYTHON_EMBED)

// 每插件持:
struct py_plugin_state {
    py_host_state* host = nullptr;
    std::string plugin_id;
    std::wstring plugin_dir_w;
    std::string entry_relative;
    // 加载时前插的 sys.path 条目 (unload 恢复用)
    std::vector<std::wstring> inserted_sys_paths;
    std::unordered_set<std::string> preexisting_modules;
    // PyObject 引用 (强引用, 需 DECREF)
    PyObject* module = nullptr;
    PyObject* ctx = nullptr;
    PyObject* hook_on_load = nullptr;
    PyObject* hook_on_enable = nullptr;
    PyObject* hook_on_disable = nullptr;
    PyObject* hook_on_unload = nullptr;
    // 上次错误
    std::string last_error;
    // manifest 副本 (compat 层解析出来的)
    sao::plugins::loader::plugin_manifest manifest;
    SaoSdkContext owned_sdk_context{};
    SaoSdkContext* sdk_context = nullptr;
    bool owns_sdk_context = false;
};

namespace fs = std::filesystem;

struct python_layout {
    std::wstring home;
    std::vector<std::wstring> module_search_paths;
};

// wchar_t → utf-8 (小工具, 复用).
std::string wchar_to_utf8(const wchar_t* w) {
    if (w == nullptr)
        return {};
    std::string out;
    while (*w != 0) {
        uint32_t cp = static_cast<uint32_t>(*w);
        ++w;
        if (cp >= 0xD800 && cp <= 0xDBFF && *w != 0) {
            uint32_t lo = static_cast<uint32_t>(*w);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++w;
            }
        }
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// UTF-8 → std::wstring (简易, 只处理 BMP + supplement)。
std::wstring utf8_to_wstring(const char* s) {
    std::wstring out;
    if (s == nullptr)
        return out;
    while (*s) {
        uint32_t cp = 0;
        unsigned char c = static_cast<unsigned char>(*s);
        if (c < 0x80) {
            cp = c;
            ++s;
        } else if ((c & 0xE0) == 0xC0) {
            cp = (c & 0x1F);
            ++s;
            if ((static_cast<unsigned char>(*s) & 0xC0) == 0x80) {
                cp = (cp << 6) | (static_cast<unsigned char>(*s) & 0x3F);
                ++s;
            }
        } else if ((c & 0xF0) == 0xE0) {
            cp = (c & 0x0F);
            for (int i = 0; i < 2 && *(s + 1) != 0; ++i) {
                ++s;
                cp = (cp << 6) | (static_cast<unsigned char>(*s) & 0x3F);
            }
            ++s;
        } else if ((c & 0xF8) == 0xF0) {
            cp = (c & 0x07);
            for (int i = 0; i < 3 && *(s + 1) != 0; ++i) {
                ++s;
                cp = (cp << 6) | (static_cast<unsigned char>(*s) & 0x3F);
            }
            ++s;
        } else {
            ++s;
            continue;
        }
        if (cp < 0x10000) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 | (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 | (cp & 0x3FF)));
        }
    }
    return out;
}

// 抓当前 Python error 并 format 成 utf-8 traceback 字符串 (供 last_error).
// 调用前必须持 GIL. 若无 error, 返 "" 且 PyErr 清空.
std::string capture_and_clear_pyerr() {
    if (!PyErr_Occurred())
        return {};
    PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    std::string out;
    // 试着用 traceback.format_exception
    PyObject* tb_mod = PyImport_ImportModule("traceback");
    if (tb_mod != nullptr) {
        PyObject* fmt_fn = PyObject_GetAttrString(tb_mod, "format_exception");
        Py_DECREF(tb_mod);
        if (fmt_fn != nullptr) {
            PyObject* lines = PyObject_CallFunctionObjArgs(
                fmt_fn, type ? type : Py_None, value ? value : Py_None, tb ? tb : Py_None, nullptr);
            Py_DECREF(fmt_fn);
            if (lines != nullptr && PyList_Check(lines)) {
                Py_ssize_t n = PyList_GET_SIZE(lines);
                for (Py_ssize_t i = 0; i < n; ++i) {
                    PyObject* item = PyList_GET_ITEM(lines, i);
                    const char* s = PyUnicode_AsUTF8(item);
                    if (s != nullptr)
                        out += s;
                }
            }
            Py_XDECREF(lines);
        }
    }
    if (out.empty() && value != nullptr) {
        PyObject* s = PyObject_Str(value);
        if (s != nullptr) {
            const char* c = PyUnicode_AsUTF8(s);
            if (c != nullptr)
                out = c;
            Py_DECREF(s);
        }
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    PyErr_Clear();
    return out;
}

// 把绝对目录 w 加到 sys.path 首位 (若已在则 no-op). 加入的路径记入 inserted.
void prepend_sys_path(const std::wstring& w, std::vector<std::wstring>& inserted) {
    PyObject* sys_mod = PyImport_ImportModule("sys");
    if (sys_mod == nullptr) {
        PyErr_Clear();
        return;
    }
    PyObject* sys_path = PyObject_GetAttrString(sys_mod, "path");
    Py_DECREF(sys_mod);
    if (sys_path == nullptr || !PyList_Check(sys_path)) {
        Py_XDECREF(sys_path);
        PyErr_Clear();
        return;
    }
    PyObject* p_obj = PyUnicode_FromWideChar(w.c_str(), static_cast<Py_ssize_t>(w.size()));
    if (p_obj == nullptr) {
        Py_DECREF(sys_path);
        PyErr_Clear();
        return;
    }

    // 检查是否已在
    Py_ssize_t n = PyList_GET_SIZE(sys_path);
    bool present = false;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PyList_GET_ITEM(sys_path, i);
        if (item != nullptr && PyUnicode_Check(item)) {
            if (PyUnicode_Compare(item, p_obj) == 0) {
                present = true;
                break;
            }
        }
    }
    if (!present) {
        PyList_Insert(sys_path, 0, p_obj);
        inserted.push_back(w);
    }
    Py_DECREF(p_obj);
    Py_DECREF(sys_path);
}

// 移除 sys.path 里我们加过的条目.
void remove_inserted_sys_paths(std::vector<std::wstring>& inserted) {
    if (inserted.empty())
        return;
    PyObject* sys_mod = PyImport_ImportModule("sys");
    if (sys_mod == nullptr) {
        PyErr_Clear();
        return;
    }
    PyObject* sys_path = PyObject_GetAttrString(sys_mod, "path");
    Py_DECREF(sys_mod);
    if (sys_path == nullptr || !PyList_Check(sys_path)) {
        Py_XDECREF(sys_path);
        PyErr_Clear();
        return;
    }
    for (auto& w : inserted) {
        PyObject* p_obj = PyUnicode_FromWideChar(w.c_str(), static_cast<Py_ssize_t>(w.size()));
        if (p_obj == nullptr)
            continue;
        Py_ssize_t n = PyList_GET_SIZE(sys_path);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* item = PyList_GET_ITEM(sys_path, i);
            if (item != nullptr && PyUnicode_Check(item) && PyUnicode_Compare(item, p_obj) == 0) {
                if (PySequence_DelItem(sys_path, i) == 0)
                    --n;
                --i; // 重扫本槽 (删完后 index shift)
            }
        }
        Py_DECREF(p_obj);
    }
    Py_DECREF(sys_path);
    inserted.clear();
}

void clear_module_context_refs(PyObject* module, PyObject* context) {
    if (module == nullptr || context == nullptr)
        return;
    PyObject* module_dict = PyModule_GetDict(module);
    if (module_dict == nullptr)
        return;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t position = 0;
    while (PyDict_Next(module_dict, &position, &key, &value)) {
        const bool retain = value == Py_None || PyBool_Check(value) || PyLong_Check(value) ||
                            PyFloat_Check(value) || PyUnicode_Check(value) ||
                            PyTuple_Check(value) || PyDict_Check(value) || PyList_Check(value) ||
                            PySet_Check(value) || PyFrozenSet_Check(value) ||
                            PyFunction_Check(value) || PyType_Check(value) || PyModule_Check(value);
        if (value == context || !retain) {
            (void)PyDict_SetItem(module_dict, key, Py_None);
        }
    }
}

std::wstring normalized_path(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (!value.empty() && value.back() == L'\\')
        value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return std::towlower(ch); });
    return value;
}

bool path_is_inside(const std::wstring& path, const std::wstring& root) {
    const std::wstring normalized_file = normalized_path(path);
    const std::wstring normalized_root = normalized_path(root);
    return !normalized_root.empty() && normalized_file.size() > normalized_root.size() &&
           normalized_file.compare(0, normalized_root.size(), normalized_root) == 0 &&
           normalized_file[normalized_root.size()] == L'\\';
}

void snapshot_module_names(std::unordered_set<std::string>& output) {
    output.clear();
    PyObject* modules = PyImport_GetModuleDict();
    if (modules == nullptr)
        return;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t position = 0;
    while (PyDict_Next(modules, &position, &key, &value)) {
        if (!PyUnicode_Check(key))
            continue;
        const char* name = PyUnicode_AsUTF8(key);
        if (name != nullptr)
            output.emplace(name);
        else
            PyErr_Clear();
    }
}

void remove_plugin_modules(py_plugin_state* plugin) {
    if (plugin == nullptr)
        return;
    PyObject* modules = PyImport_GetModuleDict();
    if (modules == nullptr)
        return;
    std::vector<std::string> remove_names;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t position = 0;
    const std::string main_module = "act_plugin_" + plugin->plugin_id;
    const std::string local_prefix = "sao_local_" + plugin->plugin_id + "_";
    while (PyDict_Next(modules, &position, &key, &value)) {
        if (!PyUnicode_Check(key))
            continue;
        const char* name_utf8 = PyUnicode_AsUTF8(key);
        if (name_utf8 == nullptr) {
            PyErr_Clear();
            continue;
        }
        const std::string name(name_utf8);
        bool remove = name == main_module || name.rfind(local_prefix, 0) == 0;
        if (!remove && !plugin->preexisting_modules.contains(name) && value != nullptr) {
            PyObject* file = PyObject_GetAttrString(value, "__file__");
            if (file != nullptr && PyUnicode_Check(file)) {
                Py_ssize_t size = 0;
                const wchar_t* path = PyUnicode_AsWideCharString(file, &size);
                if (path != nullptr) {
                    remove = path_is_inside(std::wstring(path, static_cast<size_t>(size)),
                                            plugin->plugin_dir_w);
                    PyMem_Free(const_cast<wchar_t*>(path));
                } else {
                    PyErr_Clear();
                }
            } else {
                PyErr_Clear();
            }
            Py_XDECREF(file);
        }
        if (remove)
            remove_names.push_back(name);
    }
    for (const auto& name : remove_names) {
        if (PyDict_DelItemString(modules, name.c_str()) != 0)
            PyErr_Clear();
    }
    plugin->preexisting_modules.clear();
}

bool py_status_ok(PyStatus status) {
    return !PyStatus_Exception(status);
}

bool append_module_search_path(PyConfig* config, const std::wstring& path) {
    if (path.empty())
        return true;
    return py_status_ok(PyWideStringList_Append(&config->module_search_paths, path.c_str()));
}

bool is_python_zip_name(const fs::path& path) {
    std::wstring name = path.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t ch) { return std::towlower(ch); });
    if (name.size() < 12 || name.rfind(L"python3", 0) != 0 || path.extension() != L".zip") {
        return false;
    }
    return std::all_of(name.begin() + 7, name.end() - 4,
                       [](wchar_t ch) { return std::iswdigit(ch) != 0; });
}

bool discover_python_layout(const wchar_t* python_home, python_layout& layout) {
    layout = {};
    if (python_home == nullptr || python_home[0] == L'\0')
        return false;
    try {
        std::error_code error;
        fs::path home = fs::weakly_canonical(fs::path(python_home), error);
        if (error || !fs::is_directory(home, error))
            return false;

        const std::wstring runtime_stem =
            L"python" + std::to_wstring(PY_MAJOR_VERSION) + std::to_wstring(PY_MINOR_VERSION);
        if (!fs::is_regular_file(home / (runtime_stem + L".dll"), error)) {
            return false;
        }

        std::vector<fs::path> zip_candidates;
        for (fs::directory_iterator it(home, error), end; !error && it != end;
             it.increment(error)) {
            if (it->is_regular_file(error) && is_python_zip_name(it->path())) {
                zip_candidates.push_back(it->path());
            }
        }
        error.clear();
        std::sort(zip_candidates.begin(), zip_candidates.end());
        const auto preferred = std::find_if(
            zip_candidates.begin(), zip_candidates.end(), [&runtime_stem](const fs::path& path) {
                std::wstring name = path.filename().wstring();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](wchar_t ch) { return std::towlower(ch); });
                return name == runtime_stem + L".zip";
            });
        if (preferred != zip_candidates.end() && preferred != zip_candidates.begin()) {
            std::rotate(zip_candidates.begin(), preferred, preferred + 1);
        }

        const fs::path lib = home / L"Lib";
        const bool has_lib = fs::is_directory(lib / L"encodings", error);
        error.clear();
        if (zip_candidates.empty() && !has_lib)
            return false;

        layout.home = home.native();
        for (const auto& zip : zip_candidates) {
            layout.module_search_paths.push_back(zip.native());
        }
        layout.module_search_paths.push_back(layout.home);
        if (has_lib)
            layout.module_search_paths.push_back(lib.native());
        const fs::path dlls = home / L"DLLs";
        if (fs::is_directory(dlls, error)) {
            layout.module_search_paths.push_back(dlls.native());
        }
        error.clear();
        const fs::path site_packages = lib / L"site-packages";
        if (fs::is_directory(site_packages, error)) {
            layout.module_search_paths.push_back(site_packages.native());
        }
        return true;
    } catch (...) {
        layout = {};
        return false;
    }
}

bool initialize_isolated_python(const py_host_config* cfg) {
    if (cfg == nullptr || !cfg->isolated || !cfg->no_site || !cfg->ignore_pypath_env) {
        return false;
    }
    python_layout layout;
    if (!discover_python_layout(cfg->python_home, layout))
        return false;

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.isolated = 1;
    config.use_environment = 0;
    config.site_import = 0;
    config.user_site_directory = 0;
    config.parse_argv = 0;
    config.install_signal_handlers = 0;
    config.write_bytecode = 0;
    config.pathconfig_warnings = 0;

    bool configured = py_status_ok(PyConfig_SetString(&config, &config.home, layout.home.c_str()));
    config.module_search_paths_set = 1;
    for (const auto& path : layout.module_search_paths) {
        if (configured)
            configured = append_module_search_path(&config, path);
    }

    const PyStatus init_status = configured
                                     ? Py_InitializeFromConfig(&config)
                                     : PyStatus_Error("invalid isolated Python configuration");
    PyConfig_Clear(&config);
    return configured && py_status_ok(init_status) && Py_IsInitialized() != 0;
}

#endif // SAO_HAS_PYTHON_EMBED

} // namespace

// ── init / shutdown / version / available (W2c 原实装保留, 加 sao_sdk 挂载) ──

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_init(const py_host_config* cfg, py_host_handle_t* out_host) {
    if (out_host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;

#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (cfg == nullptr || cfg->python_home == nullptr || cfg->python_home[0] == L'\0' ||
        !cfg->isolated || !cfg->no_site || !cfg->ignore_pypath_env) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_singleton_mu);

    if (g_singleton != nullptr && g_singleton->initialized) {
        python_layout requested;
        if (!discover_python_layout(cfg->python_home, requested) ||
            normalized_path(requested.home) != normalized_path(g_singleton->python_home)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        *out_host = reinterpret_cast<py_host_handle_t>(g_singleton);
        g_ref_count.fetch_add(1);
        return SAO_OK;
    }

    bool externally_initialized = (Py_IsInitialized() != 0);

    // 若 Py 未 init: 在 init 前先注入 sao_sdk (PyImport_AppendInittab).
    // 若已 init: 稍后在建 handle 后走 sys.modules 直接挂.
    bool want_register = (cfg == nullptr || cfg->register_sao_sdk);
    if (!externally_initialized && want_register) {
        (void)sao_plugins_pyhost_register_native_module();
    }

    if (!externally_initialized) {
        if (!initialize_isolated_python(cfg)) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    PyObject* sys_mod = PyImport_ImportModule("sys");
    if (sys_mod == nullptr) {
        PyErr_Clear();
        if (!externally_initialized) {
            Py_FinalizeEx();
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
    Py_DECREF(sys_mod);

    // 已 init 状态下需要手动注入 sao_sdk (init 前 append_inittab 不生效).
    if (externally_initialized && want_register) {
        (void)sao_plugins_pyhost_register_native_module();
    }
    // 无论如何再挂 shim (幂等).
    if (want_register) {
        (void)sao_plugins_pyhost_register_shim_module();
    }

    g_singleton = new py_host_state();
    g_singleton->initialized = true;
    g_singleton->owns_finalize = !externally_initialized;
    g_singleton->sao_sdk_registered = want_register;
    if (cfg != nullptr && cfg->python_home != nullptr) {
        g_singleton->python_home_utf8 = wchar_to_utf8(cfg->python_home);
        g_singleton->python_home = cfg->python_home;
    }
    if (cfg != nullptr && cfg->platform_site_dir != nullptr) {
        g_singleton->platform_site_dir = cfg->platform_site_dir;
    }
    g_singleton->controlled_test_shim = cfg != nullptr && cfg->controlled_test_shim;
    g_ref_count.store(1);

    *out_host = reinterpret_cast<py_host_handle_t>(g_singleton);
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_shutdown(py_host_handle_t host) {
    if (host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;

#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    std::lock_guard<std::mutex> lock(g_singleton_mu);

    py_host_state* s = reinterpret_cast<py_host_state*>(host);
    if (s != g_singleton)
        return SAO_ERR_HANDLE_INVALID;
    if (!s->initialized)
        return SAO_ERR_NOT_INITIALIZED;
    if (s->active_plugins != 0) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }

    int32_t remaining = g_ref_count.fetch_sub(1) - 1;
    if (remaining > 0) {
        return SAO_OK;
    }

    if (s->owns_finalize) {
        (void)Py_FinalizeEx();
    }
    s->initialized = false;

    delete s;
    g_singleton = nullptr;
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_pyhost_version(py_host_handle_t host) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)host;
    return "";
#else
    if (host == nullptr)
        return "";
    py_host_state* s = reinterpret_cast<py_host_state*>(host);
    if (!s->initialized)
        return "";
    return Py_GetVersion();
#endif
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_pyhost_available(const wchar_t* python_home) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)python_home;
    return false;
#else
    python_layout layout;
    return discover_python_layout(python_home, layout);
#endif
}

// ── Wave 7: 加载 / 卸载 / hook 调用 ──────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_load_plugin(
    py_host_handle_t host, const wchar_t* plugin_dir, const char* entry_relative,
    const char* plugin_id_utf8, void* ctx_ptr, py_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;

#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)host;
    (void)plugin_dir;
    (void)entry_relative;
    (void)plugin_id_utf8;
    (void)ctx_ptr;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (host == nullptr || plugin_dir == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    py_host_state* s = reinterpret_cast<py_host_state*>(host);
    if (s != g_singleton || !s->initialized)
        return SAO_ERR_NOT_INITIALIZED;

    auto* pl = new py_plugin_state();
    pl->host = s;
    ++s->active_plugins;
    pl->plugin_dir_w = plugin_dir;

    // 1. 读 manifest (compat W1c).
    std::wstring manifest_path = pl->plugin_dir_w;
    if (!manifest_path.empty() && manifest_path.back() != L'/' && manifest_path.back() != L'\\') {
        manifest_path.push_back(L'\\');
    }
    manifest_path += L"plugin.json";
    char* err_utf8 = nullptr;
    int32_t rc = sao::plugins::compat::sao_plugins_compat_load_manifest_file(
        manifest_path.c_str(), &pl->manifest, &err_utf8);
    if (rc != SAO_OK) {
        if (err_utf8 != nullptr) {
            pl->last_error = err_utf8;
            sao::plugins::compat::sao_plugins_compat_free_string(err_utf8);
        } else {
            pl->last_error = "cannot open plugin.json";
        }
        // 若调用方给了 plugin_id 显式覆盖, 且没给 entry, 我们放弃—因为 manifest
        // 读不出说明连 fallback 都不能靠。仍返回 handle (供 last_error 内省).
        if (plugin_id_utf8 != nullptr && *plugin_id_utf8 != '\0') {
            pl->plugin_id = plugin_id_utf8;
        }
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_HANDLE_INVALID;
    }
    // normalize (补默认字段).
    (void)sao::plugins::compat::sao_plugins_compat_normalize_v1_manifest(&pl->manifest, nullptr);

    // 2. 决定 plugin_id 与 entry_relative.
    if (plugin_id_utf8 != nullptr && *plugin_id_utf8 != '\0') {
        pl->plugin_id = plugin_id_utf8;
    } else {
        pl->plugin_id = pl->manifest.plugin_id;
    }
    if (entry_relative != nullptr && *entry_relative != '\0') {
        pl->entry_relative = entry_relative;
    } else {
        pl->entry_relative = pl->manifest.entry;
    }
    if (pl->entry_relative.empty())
        pl->entry_relative = "plugin.py";

    // ctx_ptr 可传入已绑定的 SaoSdkContext。有效上下文只借用，由调用方持有；
    // 其他值保持兼容并回退为本 plugin 唯一的 host-owned SaoSdkContext。
    if (ctx_ptr != nullptr) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(ctx_ptr, &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT && (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            reinterpret_cast<uintptr_t>(ctx_ptr) + sizeof(SaoSdkContext) <=
                reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize) {
            auto* supplied = static_cast<SaoSdkContext*>(ctx_ptr);
            if (supplied->abi_version == SAO_SDK_ABI_VERSION && supplied->ctx_impl != nullptr) {
                pl->sdk_context = supplied;
            }
        }
    }
    if (pl->sdk_context == nullptr) {
        rc = sao_sdk_bind_context(pl->plugin_id.c_str(), pl->manifest.version.c_str(),
                                  &pl->owned_sdk_context);
        if (rc != SAO_SDK_OK) {
            pl->last_error = "sao_sdk_bind_context failed";
            *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
            return rc;
        }
        pl->sdk_context = &pl->owned_sdk_context;
        pl->owns_sdk_context = true;
        rc = sao_sdk_context_bind_platform_services(pl->sdk_context);
        if (rc != SAO_SDK_OK) {
            pl->last_error = "sao_sdk_context_bind_platform_services failed";
            *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
            return rc;
        }
    }

    // 3. 前插 sys.path: plugin_dir + libs/ + vendor/ + engine/ (W4b probe).
    prepend_sys_path(pl->plugin_dir_w, pl->inserted_sys_paths);
    sao::plugins::compat::discovered_deps_dirs deps;
    (void)sao::plugins::compat::sao_plugins_compat_libs_vendor_probe(pl->plugin_dir_w.c_str(),
                                                                     &deps);
    // deps.ordered 是 engine → libs → vendor 顺序; 我们逆序前插以让 engine 在最前.
    for (auto it = deps.ordered.rbegin(); it != deps.ordered.rend(); ++it) {
        prepend_sys_path(*it, pl->inserted_sys_paths);
    }
    sao::plugins::compat::sao_plugins_compat_free_deps_dirs(&deps);

    // 4. wrap ctx: 用 sao_sdk.PluginContext (记账 shim).
    {
        PyObject* sao_sdk = PyImport_ImportModule("sao_sdk");
        if (sao_sdk == nullptr) {
            pl->last_error = capture_and_clear_pyerr();
            if (pl->last_error.empty())
                pl->last_error = "sao_sdk not importable";
            *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
            return SAO_ERR_OS_CALL_FAILED;
        }
        PyObject* pc_cls = PyObject_GetAttrString(sao_sdk, "PluginContext");
        Py_DECREF(sao_sdk);
        if (pc_cls == nullptr) {
            pl->last_error = capture_and_clear_pyerr();
            if (pl->last_error.empty())
                pl->last_error = "sao_sdk.PluginContext not found";
            *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
            return SAO_ERR_OS_CALL_FAILED;
        }
        std::string dir_utf8 = wchar_to_utf8(pl->plugin_dir_w.c_str());
        Py_ssize_t hnd = static_cast<Py_ssize_t>(reinterpret_cast<intptr_t>(pl->sdk_context));
        PyObject* args = Py_BuildValue("(ssni)", pl->plugin_id.c_str(), dir_utf8.c_str(), hnd,
                                       s->controlled_test_shim ? 1 : 0);
        if (args == nullptr) {
            Py_DECREF(pc_cls);
            pl->last_error = capture_and_clear_pyerr();
            *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
            return SAO_ERR_OS_CALL_FAILED;
        }
        pl->ctx = PyObject_CallObject(pc_cls, args);
        Py_DECREF(args);
        Py_DECREF(pc_cls);
        if (pl->ctx == nullptr) {
            pl->last_error = capture_and_clear_pyerr();
            if (pl->last_error.empty())
                pl->last_error = "PluginContext() failed";
            *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    // 5. spec_from_file_location(f"act_plugin_{id}", <plugin_dir>/<entry>).
    std::string module_name = "act_plugin_" + pl->plugin_id;
    snapshot_module_names(pl->preexisting_modules);
    // 老代码可能自己 import (相对), 我们用完整绝对路径避免混淆.
    std::wstring entry_path = pl->plugin_dir_w;
    if (!entry_path.empty() && entry_path.back() != L'/' && entry_path.back() != L'\\') {
        entry_path.push_back(L'\\');
    }
    entry_path += utf8_to_wstring(pl->entry_relative.c_str());
    std::string entry_utf8 = wchar_to_utf8(entry_path.c_str());

    PyObject* importlib_util = PyImport_ImportModule("importlib.util");
    if (importlib_util == nullptr) {
        pl->last_error = capture_and_clear_pyerr();
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* spec_fn = PyObject_GetAttrString(importlib_util, "spec_from_file_location");
    if (spec_fn == nullptr) {
        Py_DECREF(importlib_util);
        pl->last_error = capture_and_clear_pyerr();
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* spec = PyObject_CallFunction(spec_fn, "ss", module_name.c_str(), entry_utf8.c_str());
    Py_DECREF(spec_fn);
    if (spec == nullptr || spec == Py_None) {
        Py_XDECREF(spec);
        Py_DECREF(importlib_util);
        pl->last_error = capture_and_clear_pyerr();
        if (pl->last_error.empty())
            pl->last_error = "spec_from_file_location returned None";
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* mod_from_spec_fn = PyObject_GetAttrString(importlib_util, "module_from_spec");
    Py_DECREF(importlib_util);
    if (mod_from_spec_fn == nullptr) {
        Py_DECREF(spec);
        pl->last_error = capture_and_clear_pyerr();
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    pl->module = PyObject_CallOneArg(mod_from_spec_fn, spec);
    Py_DECREF(mod_from_spec_fn);
    if (pl->module == nullptr) {
        Py_DECREF(spec);
        pl->last_error = capture_and_clear_pyerr();
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    // 挂 sys.modules[module_name] 让相对 import / 二次导入命中同一实例.
    PyObject* sys_modules = PyImport_GetModuleDict();
    if (sys_modules != nullptr) {
        PyDict_SetItemString(sys_modules, module_name.c_str(), pl->module);
    }
    // exec_module.
    PyObject* loader = PyObject_GetAttrString(spec, "loader");
    Py_DECREF(spec);
    if (loader == nullptr) {
        pl->last_error = capture_and_clear_pyerr();
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* exec_fn = PyObject_GetAttrString(loader, "exec_module");
    Py_DECREF(loader);
    if (exec_fn == nullptr) {
        pl->last_error = capture_and_clear_pyerr();
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* exec_res = PyObject_CallOneArg(exec_fn, pl->module);
    Py_DECREF(exec_fn);
    if (exec_res == nullptr) {
        // 模块 top-level 代码抛异常 (可能是 import cv2 等缺依赖) —— 记 traceback,
        // 保 module 引用 (供 last_error 内省), 但视为加载失败.
        pl->last_error = capture_and_clear_pyerr();
        if (pl->last_error.empty())
            pl->last_error = "exec_module failed";
        *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
        return SAO_ERR_OS_CALL_FAILED;
    }
    Py_DECREF(exec_res);

    // 6. 抽 hook (可选; 缺失不视为错误).
    auto grab_hook = [&](const char* name) -> PyObject* {
        PyObject* fn = PyObject_GetAttrString(pl->module, name);
        if (fn == nullptr) {
            PyErr_Clear();
            return nullptr;
        }
        if (!PyCallable_Check(fn)) {
            Py_DECREF(fn);
            return nullptr;
        }
        return fn;
    };
    pl->hook_on_load = grab_hook("on_load");
    pl->hook_on_enable = grab_hook("on_enable");
    pl->hook_on_disable = grab_hook("on_disable");
    pl->hook_on_unload = grab_hook("on_unload");

    *out_plugin = reinterpret_cast<py_plugin_handle_t>(pl);
    return SAO_OK;
#endif
}

#if defined(SAO_HAS_PYTHON_EMBED)
namespace {
// 通用 hook 调用: 已持 GIL, plugin != nullptr, hook == 可调用.
int32_t invoke_hook(py_plugin_state* pl, PyObject* hook, bool pass_ctx) {
    if (pl == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (hook == nullptr)
        return SAO_ERR_NOT_IMPLEMENTED; // hook 缺失
    PyObject* args = nullptr;
    if (pass_ctx) {
        if (pl->ctx == nullptr)
            return SAO_ERR_NOT_INITIALIZED;
        args = PyTuple_Pack(1, pl->ctx);
    } else {
        args = PyTuple_New(0);
    }
    if (args == nullptr) {
        pl->last_error = capture_and_clear_pyerr();
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* res = PyObject_CallObject(hook, args);
    Py_DECREF(args);
    if (res == nullptr) {
        pl->last_error = capture_and_clear_pyerr();
        return SAO_ERR_OS_CALL_FAILED;
    }
    Py_DECREF(res);
    return SAO_OK;
}
} // namespace
#endif

extern "C" SAO_PLUGINS_API
    int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_call_on_load(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return invoke_hook(pl, pl->hook_on_load, /*pass_ctx=*/true);
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_enable(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    // on_enable() 老插件不传 ctx (see hide_seek: on_disable(), on_unload()).
    return invoke_hook(pl, pl->hook_on_enable, /*pass_ctx=*/false);
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_disable(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return invoke_hook(pl, pl->hook_on_disable, /*pass_ctx=*/false);
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_on_unload(py_plugin_handle_t plugin, bool* out_allow_unload) {
    if (out_allow_unload != nullptr)
        *out_allow_unload = true;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return invoke_hook(pl, pl->hook_on_unload, /*pass_ctx=*/false);
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unload_plugin(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);

    // Py 可能已被 finalize (host_shutdown 之后二次 unload). 只有 initialized
    // 时才走 Py 侧清理.
    if (Py_IsInitialized() != 0) {
        const int32_t teardown_status = sao_plugins_pyhost_ctx_try_teardown_native(pl->ctx);
        if (teardown_status != SAO_OK)
            return teardown_status;

        if (pl->owns_sdk_context && pl->sdk_context != nullptr) {
            int32_t status = sao_sdk_context_try_destroy(pl->sdk_context);
            if (status == SAO_SDK_ERR_BUSY)
                status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            if (status != SAO_OK)
                return status;
            pl->sdk_context = nullptr;
            pl->owns_sdk_context = false;
        }

        // 从 sys.modules 撕主模块以及本次加载新增且位于插件目录内的模块。
        remove_plugin_modules(pl);
        // DECREF hook + module + ctx.
        Py_CLEAR(pl->hook_on_load);
        Py_CLEAR(pl->hook_on_enable);
        Py_CLEAR(pl->hook_on_disable);
        Py_CLEAR(pl->hook_on_unload);
        clear_module_context_refs(pl->module, pl->ctx);
        Py_CLEAR(pl->module);
        Py_CLEAR(pl->ctx);

        // 恢复 sys.path.
        remove_inserted_sys_paths(pl->inserted_sys_paths);
    } else {
        // Python 已 shutdown; PyObject 指针无效但内存已归还.
        pl->hook_on_load = pl->hook_on_enable = pl->hook_on_disable = pl->hook_on_unload = nullptr;
        pl->module = nullptr;
        pl->ctx = nullptr;
        pl->inserted_sys_paths.clear();
        pl->preexisting_modules.clear();
    }
    if (pl->owns_sdk_context && pl->sdk_context != nullptr) {
        int32_t status = sao_sdk_context_try_destroy(pl->sdk_context);
        if (status == SAO_SDK_ERR_BUSY)
            status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        if (status != SAO_OK)
            return status;
        pl->sdk_context = nullptr;
        pl->owns_sdk_context = false;
    }
    if (pl->host != nullptr && pl->host->active_plugins > 0) {
        --pl->host->active_plugins;
    }
    delete pl;
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_pyhost_has_hook(py_plugin_handle_t plugin, const char* hook_name) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    (void)hook_name;
    return false;
#else
    if (plugin == nullptr || hook_name == nullptr)
        return false;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    if (std::strcmp(hook_name, "on_load") == 0)
        return pl->hook_on_load != nullptr;
    if (std::strcmp(hook_name, "on_enable") == 0)
        return pl->hook_on_enable != nullptr;
    if (std::strcmp(hook_name, "on_disable") == 0)
        return pl->hook_on_disable != nullptr;
    if (std::strcmp(hook_name, "on_unload") == 0)
        return pl->hook_on_unload != nullptr;
    // 任意 hook: 查 module attr.
    if (pl->module == nullptr)
        return false;
    PyObject* attr = PyObject_GetAttrString(pl->module, hook_name);
    if (attr == nullptr) {
        PyErr_Clear();
        return false;
    }
    bool ok = PyCallable_Check(attr) != 0;
    Py_DECREF(attr);
    return ok;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_hook(py_plugin_handle_t plugin, const char* hook_name,
                             const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr)
        *out_result_json_utf8 = nullptr;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    (void)hook_name;
    (void)args_json_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr || hook_name == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    if (pl->module == nullptr)
        return SAO_ERR_NOT_INITIALIZED;

    // 特殊 hook 名: 落到已 cached 的 hook.
    PyObject* hook = nullptr;
    if (std::strcmp(hook_name, "on_load") == 0)
        hook = pl->hook_on_load;
    else if (std::strcmp(hook_name, "on_enable") == 0)
        hook = pl->hook_on_enable;
    else if (std::strcmp(hook_name, "on_disable") == 0)
        hook = pl->hook_on_disable;
    else if (std::strcmp(hook_name, "on_unload") == 0)
        hook = pl->hook_on_unload;

    bool hook_needs_decref = false;
    if (hook == nullptr) {
        hook = PyObject_GetAttrString(pl->module, hook_name);
        if (hook == nullptr) {
            PyErr_Clear();
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!PyCallable_Check(hook)) {
            Py_DECREF(hook);
            return SAO_ERR_HANDLE_INVALID;
        }
        hook_needs_decref = true;
    } else {
        Py_INCREF(hook);
        hook_needs_decref = true;
    }

    // 若 hook 是 on_load, 传 ctx; 否则传 args_json 反序列化 (或空 tuple).
    PyObject* args_tuple = nullptr;
    bool pass_ctx = (std::strcmp(hook_name, "on_load") == 0);
    if (pass_ctx) {
        if (pl->ctx == nullptr) {
            Py_DECREF(hook);
            return SAO_ERR_NOT_INITIALIZED;
        }
        args_tuple = PyTuple_Pack(1, pl->ctx);
    } else if (args_json_utf8 != nullptr && *args_json_utf8 != '\0') {
        // JSON → PyObject (用 json.loads).
        PyObject* json_mod = PyImport_ImportModule("json");
        if (json_mod == nullptr) {
            Py_DECREF(hook);
            return SAO_ERR_OS_CALL_FAILED;
        }
        PyObject* loads = PyObject_GetAttrString(json_mod, "loads");
        Py_DECREF(json_mod);
        if (loads == nullptr) {
            Py_DECREF(hook);
            return SAO_ERR_OS_CALL_FAILED;
        }
        PyObject* parsed = PyObject_CallFunction(loads, "s", args_json_utf8);
        Py_DECREF(loads);
        if (parsed == nullptr) {
            PyErr_Clear();
            args_tuple = PyTuple_New(0);
        } else if (PyTuple_Check(parsed)) {
            args_tuple = parsed; // steal
        } else if (PyList_Check(parsed)) {
            args_tuple = PyList_AsTuple(parsed);
            Py_DECREF(parsed);
        } else {
            args_tuple = PyTuple_Pack(1, parsed);
            Py_DECREF(parsed);
        }
    } else {
        args_tuple = PyTuple_New(0);
    }
    if (args_tuple == nullptr) {
        Py_DECREF(hook);
        pl->last_error = capture_and_clear_pyerr();
        return SAO_ERR_OS_CALL_FAILED;
    }

    PyObject* res = PyObject_CallObject(hook, args_tuple);
    Py_DECREF(args_tuple);
    if (hook_needs_decref)
        Py_DECREF(hook);
    if (res == nullptr) {
        pl->last_error = capture_and_clear_pyerr();
        return SAO_ERR_OS_CALL_FAILED;
    }
    // 若调用方要 JSON: 用 json.dumps repr (fallback str).
    if (out_result_json_utf8 != nullptr) {
        PyObject* json_mod = PyImport_ImportModule("json");
        std::string json_out;
        bool ok = false;
        if (json_mod != nullptr) {
            PyObject* dumps = PyObject_GetAttrString(json_mod, "dumps");
            Py_DECREF(json_mod);
            if (dumps != nullptr) {
                PyObject* kw = PyDict_New();
                if (kw != nullptr)
                    PyDict_SetItemString(kw, "default", Py_NewRef(Py_None));
                PyObject* dumps_args = PyTuple_Pack(1, res);
                PyObject* s = nullptr;
                if (dumps_args != nullptr) {
                    // 忽略非法 obj (default=None fallback 会抛; 我们改用直接调 dumps(res))
                    s = PyObject_CallOneArg(dumps, res);
                    if (s == nullptr)
                        PyErr_Clear();
                    Py_DECREF(dumps_args);
                }
                Py_XDECREF(kw);
                Py_DECREF(dumps);
                if (s != nullptr && PyUnicode_Check(s)) {
                    const char* c = PyUnicode_AsUTF8(s);
                    if (c != nullptr) {
                        json_out = c;
                        ok = true;
                    }
                }
                Py_XDECREF(s);
            }
        }
        if (!ok) {
            // fallback: str(res)
            PyObject* s = PyObject_Str(res);
            if (s != nullptr) {
                const char* c = PyUnicode_AsUTF8(s);
                if (c != nullptr)
                    json_out = c;
                Py_DECREF(s);
            }
        }
        char* buf = static_cast<char*>(std::malloc(json_out.size() + 1));
        if (buf != nullptr) {
            std::memcpy(buf, json_out.data(), json_out.size());
            buf[json_out.size()] = '\0';
            *out_result_json_utf8 = buf;
        }
    }
    Py_DECREF(res);
    return SAO_OK;
#endif
}

// ── 内省 API ─────────────────────────────────────────────

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_ctx_pyobject(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return nullptr;
#else
    if (plugin == nullptr)
        return nullptr;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return pl->ctx;
#endif
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_module_pyobject(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return nullptr;
#else
    if (plugin == nullptr)
        return nullptr;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return pl->module;
#endif
}

extern "C" SAO_PLUGINS_API const void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_manifest(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return nullptr;
#else
    if (plugin == nullptr)
        return nullptr;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return &pl->manifest;
#endif
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_get_sdk_context(py_plugin_handle_t plugin) {
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return nullptr;
#else
    if (plugin == nullptr)
        return nullptr;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    return pl->sdk_context;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_get_last_error(py_plugin_handle_t plugin, char** out_utf8) {
    if (out_utf8 != nullptr)
        *out_utf8 = nullptr;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (plugin == nullptr || out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* pl = reinterpret_cast<py_plugin_state*>(plugin);
    const std::string& s = pl->last_error;
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (buf == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *out_utf8 = buf;
    return SAO_OK;
#endif
}

} // namespace sao::plugins::python_host
