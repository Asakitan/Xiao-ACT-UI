#include <catch2/catch_test_macros.hpp>

#if defined(SAO_HAS_PYTHON_EMBED)
#  define PY_SSIZE_T_CLEAN
#  include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/sdk/sao_sdk.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#ifndef SAO_TEST_PYTHON_HOME
#  define SAO_TEST_PYTHON_HOME L""
#endif

using namespace sao::plugins::loader;
using namespace sao::plugins::python_host;
namespace fs = std::filesystem;
using json = nlohmann::json;

#if defined(SAO_HAS_PYTHON_EMBED)

namespace {

class GilGuard {
public:
    GilGuard() : state_(PyGILState_Ensure()) {}
    ~GilGuard() { PyGILState_Release(state_); }

    GilGuard(const GilGuard&) = delete;
    GilGuard& operator=(const GilGuard&) = delete;

private:
    PyGILState_STATE state_;
};

struct TempTree {
    fs::path root;

    explicit TempTree(const wchar_t* suffix) {
        root = fs::temp_directory_path() /
               (L"sao_pyhost_adapter_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + suffix);
        std::error_code error;
        fs::remove_all(root, error);
        REQUIRE(fs::create_directories(root));
    }

    ~TempTree() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

std::string path_utf8(const fs::path& path) {
    const std::wstring wide = path.native();
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    REQUIRE(needed > 0);
    std::string result(static_cast<size_t>(needed), '\0');
    REQUIRE(WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                static_cast<int>(wide.size()), result.data(), needed, nullptr,
                nullptr) == needed);
    return result;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(const TempTree& tree, const char* plugin_id) {
    const json document = {
        {"id", plugin_id},       {"name", plugin_id},
        {"version", "1.0.0"},   {"entry", "plugin.py"},
        {"language", "python"}, {"enabled", false},
    };
    write_text(tree.root / L"plugin.json", document.dump());

    plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    manifest.source_path = path_utf8(tree.root);
    return manifest;
}

py_host_config production_config() {
    py_host_config config{};
    config.python_home = SAO_TEST_PYTHON_HOME;
    config.isolated = true;
    config.no_site = true;
    config.ignore_pypath_env = true;
    config.register_sao_sdk = true;
    return config;
}

PyObject* loaded_module(const char* plugin_id) {
    const std::string name = std::string("act_plugin_") + plugin_id;
    PyObject* modules = PyImport_GetModuleDict();
    REQUIRE(modules != nullptr);
    return PyDict_GetItemString(modules, name.c_str());
}

std::string event_at(PyObject* module, Py_ssize_t index) {
    REQUIRE(module != nullptr);
    PyObject* events = PyObject_GetAttrString(module, "events");
    REQUIRE(events != nullptr);
    REQUIRE(PyList_Check(events));
    REQUIRE(index >= 0);
    REQUIRE(index < PyList_GET_SIZE(events));
    PyObject* item = PyList_GET_ITEM(events, index);
    REQUIRE(PyUnicode_Check(item));
    std::string result(PyUnicode_AsUTF8(item));
    Py_DECREF(events);
    return result;
}

bool sys_path_contains_in_order(const fs::path& engine, const fs::path& libs,
                                const fs::path& vendor) {
    PyObject* path = PySys_GetObject("path");
    if (path == nullptr || !PyList_Check(path)) return false;
    const std::wstring targets[] = {engine.native(), libs.native(), vendor.native()};
    Py_ssize_t previous = -1;
    for (const auto& target : targets) {
        Py_ssize_t found = -1;
        for (Py_ssize_t index = 0; index < PyList_GET_SIZE(path); ++index) {
            PyObject* item = PyList_GET_ITEM(path, index);
            if (!PyUnicode_Check(item)) continue;
            PyObject* expected = PyUnicode_FromWideChar(
                target.c_str(), static_cast<Py_ssize_t>(target.size()));
            REQUIRE(expected != nullptr);
            const int equal = PyObject_RichCompareBool(item, expected, Py_EQ);
            Py_DECREF(expected);
            if (equal == 1) {
                found = index;
                break;
            }
            if (equal < 0) PyErr_Clear();
        }
        if (found < 0 || found <= previous) return false;
        previous = found;
    }
    return true;
}

void require_runtime_flags() {
    PyObject* flags = PySys_GetObject("flags");
    REQUIRE(flags != nullptr);
    for (const char* name : {"isolated", "no_site", "no_user_site",
                             "ignore_environment"}) {
        PyObject* value = PyObject_GetAttrString(flags, name);
        REQUIRE(value != nullptr);
        CHECK(PyLong_AsLong(value) == 1);
        Py_DECREF(value);
    }
}

} // namespace

TEST_CASE("production Python loader adapter owns lifecycle and cleans failures",
          "[plugins][python][adapter]") {
    py_host_config invalid = production_config();
    invalid.python_home = nullptr;
    py_host_handle_t invalid_host = nullptr;
    CHECK(sao_plugins_pyhost_init(&invalid, &invalid_host) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK_FALSE(sao_plugins_pyhost_available(nullptr));
    REQUIRE(sao_plugins_pyhost_available(SAO_TEST_PYTHON_HOME));

    py_host_config config = production_config();
    py_host_handle_t direct_host = nullptr;
    REQUIRE(sao_plugins_pyhost_init(&config, &direct_host) == SAO_OK);
    REQUIRE(direct_host != nullptr);
    require_runtime_flags();

    py_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_pyhost_register_loader_adapter(&config, &owner) == SAO_OK);
    REQUIRE(owner != nullptr);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);

    TempTree good(L"good");
    REQUIRE(fs::create_directories(good.root / L"engine"));
    REQUIRE(fs::create_directories(good.root / L"libs"));
    REQUIRE(fs::create_directories(good.root / L"vendor"));
    write_text(good.root / L"plugin.py", R"PY(
events = []
def on_load(ctx):
    events.append("load")
    ctx.ensure_requirements(install=True)
def on_enable():
    events.append("enable")
def on_disable():
    events.append("disable")
def on_unload():
    events.append("unload")
)PY");
    plugin_manifest good_manifest = make_manifest(good, "adapter_good");
    plugin_handle_t good_plugin = nullptr;
    registry_handle_t registry = sao_plugins_registry_instance();
    REQUIRE(sao_plugins_registry_add_plugin(registry, &good_manifest,
                                             &good_plugin) == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_load(good_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(good_plugin) ==
            lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 1);
    PyObject* good_module = nullptr;
    {
        GilGuard gil;
        good_module = loaded_module("adapter_good");
        REQUIRE(good_module != nullptr);
        CHECK(event_at(good_module, 0) == "load");
        CHECK(sys_path_contains_in_order(good.root / L"engine",
                                         good.root / L"libs",
                                         good.root / L"vendor"));
    }

    char* report = nullptr;
    REQUIRE(sao_plugins_pyhost_loader_adapter_get_requirements_report(
                owner, good_plugin, &report) == SAO_OK);
    REQUIRE(report != nullptr);
    const json parsed_report = json::parse(report);
    sao_plugins_pyhost_free_string(report);
    REQUIRE(parsed_report["added"].size() == 3);
    CHECK(parsed_report["deps"].empty());

    REQUIRE(sao_plugins_lifecycle_enable(good_plugin) == SAO_OK);
    {
        GilGuard gil;
        CHECK(event_at(good_module, 1) == "enable");
    }
    REQUIRE(sao_plugins_lifecycle_disable(good_plugin) == SAO_OK);
    {
        GilGuard gil;
        CHECK(event_at(good_module, 2) == "disable");
    }
    CHECK(sao_plugins_pyhost_unregister_loader_adapter(owner) ==
          SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_unload(good_plugin) == SAO_OK);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    {
        GilGuard gil;
        CHECK(loaded_module("adapter_good") == nullptr);
    }
    REQUIRE(sao_plugins_registry_remove(registry, good_plugin) == SAO_OK);

    TempTree failed(L"failed");
    write_text(failed.root / L"helper.py", "value = 7\n");
    write_text(failed.root / L"plugin.py",
               "import helper\nraise RuntimeError('adapter fixture failure')\n");
    plugin_manifest failed_manifest = make_manifest(failed, "adapter_failed");
    plugin_handle_t failed_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &failed_manifest,
                                             &failed_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_load(failed_plugin) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    {
        GilGuard gil;
        CHECK(loaded_module("adapter_failed") == nullptr);
        CHECK(PyDict_GetItemString(PyImport_GetModuleDict(), "helper") == nullptr);
    }

    char* error = nullptr;
    REQUIRE(sao_plugins_pyhost_loader_adapter_get_last_error(
                owner, failed_plugin, &error) == SAO_OK);
    REQUIRE(error != nullptr);
    CHECK(std::string(error).find("adapter fixture failure") != std::string::npos);
    sao_plugins_pyhost_free_string(error);
    REQUIRE(sao_plugins_registry_remove(registry, failed_plugin) == SAO_OK);

    TempTree dependencies(L"dependencies");
    REQUIRE(fs::create_directories(dependencies.root / L"engine" / L"dupmod"));
    REQUIRE(fs::create_directories(dependencies.root / L"libs" / L"dupmod"));
    REQUIRE(fs::create_directories(dependencies.root / L"vendor" / L"mido"));
    write_text(dependencies.root / L"requirements.txt",
               "dupmod>=1\nmido>=1.3\nmissing-dist==2\n");
    report = nullptr;
    REQUIRE(sao_plugins_pyhost_report_requirements(
                dependencies.root.c_str(), &report) == SAO_OK);
    REQUIRE(report != nullptr);
    const json dependency_report = json::parse(report);
    sao_plugins_pyhost_free_string(report);
    CHECK(dependency_report["deps"]["dupmod"] == "engine");
    CHECK(dependency_report["deps"]["mido"] == "vendor");
    CHECK(dependency_report["deps"]["missing-dist"] == "missing");

    TempTree blocked(L"blocked");
    write_text(blocked.root / L"plugin.py", R"PY(
unload_attempts = 0
def on_load(ctx):
    pass
def on_unload():
    global unload_attempts
    unload_attempts += 1
    return unload_attempts > 1
)PY");
    plugin_manifest blocked_manifest = make_manifest(blocked, "adapter_blocked");
    plugin_handle_t blocked_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &blocked_manifest,
                                             &blocked_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(blocked_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_unload(blocked_plugin) ==
          SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(blocked_plugin) ==
          lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 1);
    REQUIRE(sao_plugins_lifecycle_unload(blocked_plugin) == SAO_OK);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao_plugins_registry_remove(registry, blocked_plugin) == SAO_OK);

    TempTree slow(L"slow");
    const fs::path slow_started = slow.root / L"started";
    const fs::path slow_finished = slow.root / L"finished";
    write_text(
        slow.root / L"plugin.py",
        "from pathlib import Path\n"
        "import time\n"
        "def on_load(ctx):\n"
        "    pass\n"
        "def on_enable():\n"
        "    Path(" + json(path_utf8(slow_started)).dump() +
            ").write_text('started', encoding='utf-8')\n"
        "    time.sleep(3.0)\n"
        "    Path(" + json(path_utf8(slow_finished)).dump() +
            ").write_text('finished', encoding='utf-8')\n");
    plugin_manifest slow_manifest = make_manifest(slow, "adapter_slow");
    plugin_handle_t slow_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &slow_manifest,
                                             &slow_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(slow_plugin) == SAO_OK);

    TempTree quick(L"quick");
    const fs::path quick_finished = quick.root / L"finished";
    write_text(
        quick.root / L"plugin.py",
        "from pathlib import Path\n"
        "def on_load(ctx):\n"
        "    pass\n"
        "def on_enable():\n"
        "    Path(" + json(path_utf8(quick_finished)).dump() +
            ").write_text('finished', encoding='utf-8')\n");
    plugin_manifest quick_manifest = make_manifest(quick, "adapter_quick");
    plugin_handle_t quick_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &quick_manifest,
                                             &quick_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(quick_plugin) == SAO_OK);

    std::atomic_int slow_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread slow_hook([&] {
        slow_status.store(sao_plugins_lifecycle_enable(slow_plugin));
    });
    for (int attempt = 0; attempt < 500 && !fs::exists(slow_started);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(fs::exists(slow_started));
    CHECK(sao_plugins_lifecycle_unload(slow_plugin) ==
          SAO_PLUGINS_ERR_BUSY);

    const auto quick_started_at = std::chrono::steady_clock::now();
    REQUIRE(sao_plugins_lifecycle_enable(quick_plugin) == SAO_OK);
    const auto quick_elapsed =
        std::chrono::steady_clock::now() - quick_started_at;
    CHECK(quick_elapsed < std::chrono::seconds(2));
    CHECK(fs::exists(quick_finished));
    CHECK_FALSE(fs::exists(slow_finished));
    slow_hook.join();
    CHECK(slow_status.load() == SAO_OK);
    CHECK(fs::exists(slow_finished));

    REQUIRE(sao_plugins_lifecycle_unload(quick_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(slow_plugin) == SAO_OK);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao_plugins_registry_remove(registry, quick_plugin) == SAO_OK);
    REQUIRE(sao_plugins_registry_remove(registry, slow_plugin) == SAO_OK);

    TempTree context_tree(L"context");
    write_text(context_tree.root / L"plugin.py",
               "def on_load(ctx):\n    ctx.log_info('external-context')\n");
    (void)make_manifest(context_tree, "adapter_context");
    SaoSdkContext external_context{};
    REQUIRE(sao_sdk_bind_context("adapter_context", "1.0.0",
                                 &external_context) == SAO_SDK_OK);
    py_plugin_handle_t direct_plugin = nullptr;
    {
        GilGuard gil;
        REQUIRE(sao_plugins_pyhost_load_plugin(
                    direct_host, context_tree.root.c_str(), "plugin.py",
                    "adapter_context", &external_context,
                    &direct_plugin) == SAO_OK);
        REQUIRE(direct_plugin != nullptr);
        CHECK(sao_plugins_pyhost_get_sdk_context(direct_plugin) ==
              &external_context);
        REQUIRE(sao_plugins_pyhost_call_on_load(direct_plugin) == SAO_OK);
        REQUIRE(sao_plugins_pyhost_unload_plugin(direct_plugin) == SAO_OK);
    }
    CHECK(external_context.ctx_impl != nullptr);
    CHECK(external_context.abi_version == SAO_SDK_ABI_VERSION);
    sao_sdk_context_destroy(&external_context);

    REQUIRE(sao_plugins_pyhost_unregister_loader_adapter(owner) == SAO_OK);
    REQUIRE(sao_plugins_pyhost_shutdown(direct_host) == SAO_OK);
}

#else

TEST_CASE("production Python loader adapter requires embedded CPython",
          "[plugins][python][adapter][.skip]") {
    SKIP("Python embed not found");
}

#endif
