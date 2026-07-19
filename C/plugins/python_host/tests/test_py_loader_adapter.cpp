#include <catch2/catch_test_macros.hpp>

#if defined(SAO_HAS_PYTHON_EMBED)
#define PY_SSIZE_T_CLEAN
#include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef SAO_TEST_PYTHON_HOME
#define SAO_TEST_PYTHON_HOME L""
#endif

using namespace sao::plugins::loader;
using namespace sao::plugins::python_host;
namespace fs = std::filesystem;
using json = nlohmann::json;

#if defined(SAO_HAS_PYTHON_EMBED)

namespace {

struct test_dependency_provider_state {
    std::atomic_int retain_calls{0};
    std::atomic_int release_calls{0};
};

struct test_dependency_session {
    std::mutex mutex;
    std::vector<std::wstring> paths;
};

void SAO_PLUGINS_CALL test_dependency_retain(void* user_data) {
    ++static_cast<test_dependency_provider_state*>(user_data)->retain_calls;
}

void SAO_PLUGINS_CALL test_dependency_release(void* user_data) {
    ++static_cast<test_dependency_provider_state*>(user_data)->release_calls;
}

int32_t SAO_PLUGINS_CALL test_dependency_create(void*, const deps_session_spec* spec,
                                                void** out_provider_session) {
    if (spec == nullptr || out_provider_session == nullptr ||
        spec->struct_size < sizeof(deps_session_spec) || spec->plugin_id_utf8 == nullptr ||
        spec->plugin_dir == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto session = std::make_unique<test_dependency_session>();
    *out_provider_session = session.release();
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL test_dependency_attach(void*, void* provider_session,
                                                const wchar_t* absolute_dir) {
    if (provider_session == nullptr || absolute_dir == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto& session = *static_cast<test_dependency_session*>(provider_session);
    const fs::path path(absolute_dir);
    std::error_code error;
    if (!path.is_absolute() || !fs::is_directory(path, error) || error)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(session.mutex);
    session.paths.push_back(path.lexically_normal().wstring());
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL test_dependency_restore(void*, void* provider_session,
                                                 const wchar_t* absolute_dir) {
    if (provider_session == nullptr || absolute_dir == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto& session = *static_cast<test_dependency_session*>(provider_session);
    const std::wstring normalized = fs::path(absolute_dir).lexically_normal().wstring();
    std::lock_guard lock(session.mutex);
    const auto found = std::find(session.paths.rbegin(), session.paths.rend(), normalized);
    if (found == session.paths.rend())
        return SAO_ERR_HANDLE_INVALID;
    session.paths.erase(std::prev(found.base()));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL test_dependency_close(void*, void* provider_session) {
    if (provider_session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    std::unique_ptr<test_dependency_session> session(
        static_cast<test_dependency_session*>(provider_session));
    std::lock_guard lock(session->mutex);
    if (!session->paths.empty()) {
        session.release();
        return SAO_PLUGINS_ERR_BUSY;
    }
    return SAO_OK;
}

deps_provider test_dependency_provider(test_dependency_provider_state& state) {
    deps_provider provider{};
    provider.abi_version = SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &state;
    provider.retain = test_dependency_retain;
    provider.release = test_dependency_release;
    provider.create_session = test_dependency_create;
    provider.attach_path = test_dependency_attach;
    provider.restore_path = test_dependency_restore;
    provider.close_session = test_dependency_close;
    return provider;
}

class GilGuard {
  public:
    GilGuard() : state_(PyGILState_Ensure()) {}
    ~GilGuard() {
        PyGILState_Release(state_);
    }

    GilGuard(const GilGuard&) = delete;
    GilGuard& operator=(const GilGuard&) = delete;

  private:
    PyGILState_STATE state_;
};

struct busy_gpu_provider {
    bool busy = true;

    static void SAO_SDK_CALL retain(void*) {}
    static void SAO_SDK_CALL release(void*) {}
    static sao_sdk_status_t SAO_SDK_CALL open(void* user_data, const char*, void** out_session) {
        if (user_data == nullptr || out_session == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        *out_session = user_data;
        return SAO_SDK_OK;
    }
    static sao_sdk_status_t SAO_SDK_CALL close(void* user_data, void*) {
        return static_cast<busy_gpu_provider*>(user_data)->busy ? SAO_SDK_ERR_BUSY : SAO_SDK_OK;
    }
    static sao_sdk_status_t SAO_SDK_CALL attach(void*, void*, uint32_t) {
        return SAO_SDK_OK;
    }
    static sao_sdk_status_t SAO_SDK_CALL detach(void*, void*) {
        return SAO_SDK_OK;
    }
    static sao_sdk_status_t SAO_SDK_CALL enum_regions(void*, void*, SaoSdkGpuHuntRegion*, size_t,
                                                      size_t* out_count) {
        if (out_count == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        *out_count = 0;
        return SAO_SDK_OK;
    }
    static sao_sdk_status_t SAO_SDK_CALL read(void*, void*, uint64_t, uint8_t*, size_t,
                                              size_t* out_read) {
        if (out_read == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        *out_read = 0;
        return SAO_SDK_ERR_READ_FAULT;
    }

    SaoSdkProviderVTable table() {
        SaoSdkProviderVTable provider{};
        provider.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
        provider.struct_size = sizeof(provider);
        provider.user_data = this;
        provider.retain = retain;
        provider.release = release;
        provider.gpu_hunt_open_session = open;
        provider.gpu_hunt_close_session = close;
        provider.gpu_hunt_attach = attach;
        provider.gpu_hunt_detach = detach;
        provider.gpu_hunt_enum_regions = enum_regions;
        provider.gpu_hunt_read = read;
        return provider;
    }
};

struct TempTree {
    fs::path root;

    explicit TempTree(const wchar_t* suffix) {
        root = fs::temp_directory_path() /
               (L"sao_pyhost_adapter_" + std::to_wstring(GetCurrentProcessId()) + L"_" + suffix);
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
    const int needed =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(needed > 0);
    std::string result(static_cast<size_t>(needed), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
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
        {"id", plugin_id},      {"name", plugin_id},    {"version", "1.0.0"},
        {"entry", "plugin.py"}, {"language", "python"}, {"enabled", false},
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
    if (path == nullptr || !PyList_Check(path))
        return false;
    const std::wstring targets[] = {engine.native(), libs.native(), vendor.native()};
    Py_ssize_t previous = -1;
    for (const auto& target : targets) {
        Py_ssize_t found = -1;
        for (Py_ssize_t index = 0; index < PyList_GET_SIZE(path); ++index) {
            PyObject* item = PyList_GET_ITEM(path, index);
            if (!PyUnicode_Check(item))
                continue;
            PyObject* expected =
                PyUnicode_FromWideChar(target.c_str(), static_cast<Py_ssize_t>(target.size()));
            REQUIRE(expected != nullptr);
            const int equal = PyObject_RichCompareBool(item, expected, Py_EQ);
            Py_DECREF(expected);
            if (equal == 1) {
                found = index;
                break;
            }
            if (equal < 0)
                PyErr_Clear();
        }
        if (found < 0 || found <= previous)
            return false;
        previous = found;
    }
    return true;
}

void require_runtime_flags() {
    PyObject* flags = PySys_GetObject("flags");
    REQUIRE(flags != nullptr);
    for (const char* name : {"isolated", "no_site", "no_user_site", "ignore_environment"}) {
        PyObject* value = PyObject_GetAttrString(flags, name);
        REQUIRE(value != nullptr);
        CHECK(PyLong_AsLong(value) == 1);
        Py_DECREF(value);
    }
}

struct CapabilityProbe;

struct CapabilitySession {
    CapabilityProbe* owner = nullptr;
    std::string plugin_id;
};

struct TimerRegistration {
    timer_callback_fn callback = nullptr;
    void* user_data = nullptr;
    bool one_shot = false;
};

struct CapabilityProbe {
    std::mutex mutex;
    std::uint64_t next_token = 100;
    int retain_calls = 0;
    int release_calls = 0;
    int create_session_calls = 0;
    int quiesce_session_calls = 0;
    int destroy_session_calls = 0;
    bool quiesced = false;
    std::unordered_map<std::uint64_t, TimerRegistration> timers;
    std::unordered_map<std::uint64_t, std::string> notifications;

    std::vector<TimerRegistration> timer_callbacks() {
        std::lock_guard lock(mutex);
        std::vector<TimerRegistration> result;
        result.reserve(timers.size());
        for (const auto& [_, timer] : timers)
            result.push_back(timer);
        return result;
    }

    void fire_timers_once() {
        std::vector<TimerRegistration> callbacks;
        {
            std::lock_guard lock(mutex);
            callbacks.reserve(timers.size());
            for (auto iterator = timers.begin(); iterator != timers.end();) {
                callbacks.push_back(iterator->second);
                if (iterator->second.one_shot)
                    iterator = timers.erase(iterator);
                else
                    ++iterator;
            }
        }
        for (const auto& timer : callbacks)
            timer.callback(timer.user_data);
    }

    std::size_t timer_count() {
        std::lock_guard lock(mutex);
        return timers.size();
    }

    std::size_t notification_count() {
        std::lock_guard lock(mutex);
        return notifications.size();
    }
};

CapabilitySession* capability_session(plugin_context_platform_session_t session) {
    return static_cast<CapabilitySession*>(session);
}

void SAO_PLUGINS_CALL capability_retain(void* user_data) {
    auto* probe = static_cast<CapabilityProbe*>(user_data);
    std::lock_guard lock(probe->mutex);
    ++probe->retain_calls;
}

void SAO_PLUGINS_CALL capability_release(void* user_data) {
    auto* probe = static_cast<CapabilityProbe*>(user_data);
    std::lock_guard lock(probe->mutex);
    ++probe->release_calls;
}

int32_t SAO_PLUGINS_CALL capability_create_session(void* user_data,
                                                   const plugin_context_platform_session_spec* spec,
                                                   plugin_context_platform_session_t* out_session) {
    if (user_data == nullptr || spec == nullptr || out_session == nullptr ||
        spec->struct_size < sizeof(plugin_context_platform_session_spec) ||
        spec->plugin_id_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto session = std::make_unique<CapabilitySession>();
    session->owner = static_cast<CapabilityProbe*>(user_data);
    session->plugin_id = spec->plugin_id_utf8;
    {
        std::lock_guard lock(session->owner->mutex);
        ++session->owner->create_session_calls;
    }
    *out_session = session.release();
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL capability_quiesce_session(void*,
                                                    plugin_context_platform_session_t session) {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* state = capability_session(session);
    std::lock_guard lock(state->owner->mutex);
    ++state->owner->quiesce_session_calls;
    state->owner->quiesced = true;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL capability_destroy_session(void*,
                                                    plugin_context_platform_session_t session) {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    std::unique_ptr<CapabilitySession> state(capability_session(session));
    std::lock_guard lock(state->owner->mutex);
    ++state->owner->destroy_session_calls;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL
capability_register_timer(void*, plugin_context_platform_session_t session, double, bool one_shot,
                          timer_callback_fn callback, void* callback_user_data,
                          plugin_context_platform_token_t* out_provider_token) {
    if (session == nullptr || callback == nullptr || out_provider_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* probe = capability_session(session)->owner;
    std::lock_guard lock(probe->mutex);
    const std::uint64_t token = ++probe->next_token;
    probe->timers.emplace(token, TimerRegistration{callback, callback_user_data, one_shot});
    *out_provider_token = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL
capability_unregister_timer(void*, plugin_context_platform_session_t session,
                            plugin_context_platform_token_t provider_token) {
    if (session == nullptr || provider_token == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* probe = capability_session(session)->owner;
    std::lock_guard lock(probe->mutex);
    return probe->timers.erase(provider_token) == 1 ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

int32_t SAO_PLUGINS_CALL capability_show_notify(
    void*, plugin_context_platform_session_t session, const char*, const char* message, double,
    const char*, plugin_context_platform_token_t* out_provider_token) {
    if (session == nullptr || message == nullptr || out_provider_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* probe = capability_session(session)->owner;
    std::lock_guard lock(probe->mutex);
    const std::uint64_t token = ++probe->next_token;
    probe->notifications.emplace(token, message);
    *out_provider_token = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL capability_dismiss_notify(void*, plugin_context_platform_session_t session,
                                                   plugin_context_platform_token_t provider_token) {
    if (session == nullptr || provider_token == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* probe = capability_session(session)->owner;
    std::lock_guard lock(probe->mutex);
    return probe->notifications.erase(provider_token) == 1 ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

plugin_context_platform_provider capability_provider(CapabilityProbe& probe) {
    plugin_context_platform_provider provider{};
    provider.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &probe;
    provider.retain = capability_retain;
    provider.release = capability_release;
    provider.create_session = capability_create_session;
    provider.quiesce_session = capability_quiesce_session;
    provider.destroy_session = capability_destroy_session;
    provider.register_timer = capability_register_timer;
    provider.unregister_timer = capability_unregister_timer;
    provider.show_notify = capability_show_notify;
    provider.dismiss_notify = capability_dismiss_notify;
    return provider;
}

void* g_teardown_context = nullptr;
std::atomic_int g_teardown_status{SAO_ERR_OS_CALL_FAILED};

PyObject* teardown_from_callback(PyObject*, PyObject*) {
    g_teardown_status.store(sao_plugins_pyhost_ctx_try_teardown_native(g_teardown_context));
    Py_RETURN_NONE;
}

PyMethodDef g_teardown_method = {
    "teardown_from_callback",
    reinterpret_cast<PyCFunction>(teardown_from_callback),
    METH_O,
    nullptr,
};

#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
struct MenuRowSnapshot {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload;
    bool can_activate = false;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const MenuRowSnapshot&) const = default;
};

struct MenuProviderSnapshot {
    std::string provider_id;
    std::string owner_id;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::vector<MenuRowSnapshot> rows;

    bool operator==(const MenuProviderSnapshot&) const = default;
};

struct MenuRootSnapshot {
    std::string owner_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::vector<std::pair<std::string, std::string>> actions;

    bool operator==(const MenuRootSnapshot&) const = default;
};

struct MenuCatalogSnapshot {
    std::vector<MenuProviderSnapshot> providers;
    std::vector<MenuRootSnapshot> roots;
};

int32_t SAO_PLUGINS_CALL copy_menu_catalog(const entity_provider_catalog_view* view,
                                           void* user_data) {
    if (view == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    MenuCatalogSnapshot candidate;
    candidate.providers.reserve(view->provider_count);
    for (std::uint32_t provider_index = 0; provider_index < view->provider_count;
         ++provider_index) {
        const auto& source = view->providers[provider_index];
        MenuProviderSnapshot provider;
        provider.provider_id = source.provider_id_utf8;
        provider.owner_id = source.owner_plugin_id_utf8;
        provider.generation = source.generation;
        provider.revision = source.revision;
        provider.rows.reserve(source.row_count);
        for (std::uint32_t row_index = 0; row_index < source.row_count; ++row_index) {
            const auto& row = source.rows[row_index];
            provider.rows.push_back({
                row.row_label_utf8,
                row.row_icon_utf8,
                row.action_id_utf8,
                row.payload_json_utf8,
                row.can_activate != 0,
                row.keep_menu_open != 0,
                row.close_menu_before != 0,
            });
        }
        candidate.providers.push_back(std::move(provider));
    }
    candidate.roots.reserve(view->root_contribution_count);
    for (std::uint32_t root_index = 0; root_index < view->root_contribution_count; ++root_index) {
        const auto& source = view->root_contributions[root_index];
        MenuRootSnapshot root;
        root.owner_id = source.owner_plugin_id_utf8;
        root.contribution_id = source.contribution_id_utf8;
        root.root_id = source.root_id_utf8;
        root.name = source.name_utf8;
        root.icon = source.icon_utf8;
        root.priority = source.priority;
        root.actions.reserve(source.action_count);
        for (std::uint32_t action_index = 0; action_index < source.action_count; ++action_index) {
            root.actions.emplace_back(source.actions[action_index].provider_id_utf8,
                                      source.actions[action_index].action_id_utf8);
        }
        candidate.roots.push_back(std::move(root));
    }
    *static_cast<MenuCatalogSnapshot*>(user_data) = std::move(candidate);
    return SAO_OK;
}

int32_t snapshot_menu_catalog(MenuCatalogSnapshot& out) {
    return sao_plugins_entity_provider_snapshot(copy_menu_catalog, &out);
}
#endif

} // namespace

TEST_CASE("Python owned SDK context BUSY preserves unload ownership for retry",
          "[plugins][python][sdk][busy][retry][.sdk-retry]") {
    REQUIRE(sao_plugins_pyhost_available(SAO_TEST_PYTHON_HOME));
    py_host_config config = production_config();
    py_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_pyhost_init(&config, &host) == SAO_OK);
    REQUIRE(host != nullptr);

    busy_gpu_provider provider_state;
    auto provider = provider_state.table();
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(&provider) == SAO_SDK_OK);
    TempTree tree(L"sdk_retry");
    write_text(tree.root / L"plugin.py", "def on_load(ctx):\n    return None\n");
    (void)make_manifest(tree, "python_sdk_retry");
    py_plugin_handle_t plugin = nullptr;
    {
        GilGuard gil;
        REQUIRE(sao_plugins_pyhost_load_plugin(host, tree.root.c_str(), "plugin.py",
                                               "python_sdk_retry", nullptr, &plugin) == SAO_OK);
        auto* sdk_context = static_cast<SaoSdkContext*>(sao_plugins_pyhost_get_sdk_context(plugin));
        REQUIRE(sdk_context != nullptr);
        sao_sdk_gpu_tracker_t tracker = 0;
        REQUIRE(sdk_context->gpu_hunt->create_tracker(sdk_context->ctx_impl, &tracker) ==
                SAO_SDK_OK);
        REQUIRE(tracker != 0);
        CHECK(sao_plugins_pyhost_unload_plugin(plugin) == SAO_PLUGINS_ERR_BUSY);
        CHECK(sao_plugins_pyhost_get_sdk_context(plugin) == sdk_context);
        provider_state.busy = false;
        REQUIRE(sao_plugins_pyhost_unload_plugin(plugin) == SAO_OK);
    }
    REQUIRE(sao_sdk_platform_gpu_hunt_configure_provider(nullptr) == SAO_SDK_OK);
    REQUIRE(sao_plugins_pyhost_shutdown(host) == SAO_OK);
}

TEST_CASE("production Python loader adapter owns lifecycle and cleans failures",
          "[plugins][python][adapter]") {
    py_host_config invalid = production_config();
    invalid.python_home = nullptr;
    py_host_handle_t invalid_host = nullptr;
    CHECK(sao_plugins_pyhost_init(&invalid, &invalid_host) == SAO_ERR_INVALID_ARGUMENT);
    CHECK_FALSE(sao_plugins_pyhost_available(nullptr));
    REQUIRE(sao_plugins_pyhost_available(SAO_TEST_PYTHON_HOME));

    py_host_config config = production_config();
    py_host_handle_t direct_host = nullptr;
    REQUIRE(sao_plugins_pyhost_init(&config, &direct_host) == SAO_OK);
    REQUIRE(direct_host != nullptr);
    require_runtime_flags();

    CapabilityProbe capability_probe;
    auto platform_provider = capability_provider(capability_probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&platform_provider) == SAO_OK);
    test_dependency_provider_state dependency_state;
    const auto dependency_provider = test_dependency_provider(dependency_state);
    REQUIRE(sao_plugins_deps_register_provider(&dependency_provider) == SAO_OK);

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
    REQUIRE(sao_plugins_registry_add_plugin(registry, &good_manifest, &good_plugin) == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_load(good_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(good_plugin) == lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 1);
    PyObject* good_module = nullptr;
    {
        GilGuard gil;
        good_module = loaded_module("adapter_good");
        REQUIRE(good_module != nullptr);
        CHECK(event_at(good_module, 0) == "load");
        CHECK(sys_path_contains_in_order(good.root / L"engine", good.root / L"libs",
                                         good.root / L"vendor"));
    }

    char* report = nullptr;
    REQUIRE(sao_plugins_pyhost_loader_adapter_get_requirements_report(owner, good_plugin,
                                                                      &report) == SAO_OK);
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
    CHECK(sao_plugins_pyhost_unregister_loader_adapter(owner) == SAO_PLUGINS_ERR_BUSY);
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
    REQUIRE(sao_plugins_registry_add_plugin(registry, &failed_manifest, &failed_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_load(failed_plugin) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    {
        GilGuard gil;
        CHECK(loaded_module("adapter_failed") == nullptr);
        CHECK(PyDict_GetItemString(PyImport_GetModuleDict(), "helper") == nullptr);
    }

    char* error = nullptr;
    REQUIRE(sao_plugins_pyhost_loader_adapter_get_last_error(owner, failed_plugin, &error) ==
            SAO_OK);
    REQUIRE(error != nullptr);
    CHECK(std::string(error).find("adapter fixture failure") != std::string::npos);
    sao_plugins_pyhost_free_string(error);
    REQUIRE(sao_plugins_registry_remove(registry, failed_plugin) == SAO_OK);

    TempTree dependencies(L"dependencies");
    REQUIRE(fs::create_directories(dependencies.root / L"engine" / L"dupmod"));
    REQUIRE(fs::create_directories(dependencies.root / L"libs" / L"dupmod"));
    REQUIRE(fs::create_directories(dependencies.root / L"vendor" / L"mido"));
    write_text(dependencies.root / L"requirements.txt", "dupmod>=1\nmido>=1.3\nmissing-dist==2\n");
    report = nullptr;
    REQUIRE(sao_plugins_pyhost_report_requirements(dependencies.root.c_str(), &report) == SAO_OK);
    REQUIRE(report != nullptr);
    const json dependency_report = json::parse(report);
    sao_plugins_pyhost_free_string(report);
    CHECK(dependency_report["deps"]["dupmod"] == "engine");
    CHECK(dependency_report["deps"]["mido"] == "vendor");
    CHECK(dependency_report["deps"]["missing-dist"] == "missing");

    TempTree blocked(L"blocked");
    write_text(blocked.root / L"plugin.py", R"PY(
unload_attempts = 0
context = None
def on_load(ctx):
    global context
    context = ctx
def on_unload():
    global unload_attempts
    unload_attempts += 1
    return unload_attempts > 1
)PY");
    plugin_manifest blocked_manifest = make_manifest(blocked, "adapter_blocked");
    plugin_handle_t blocked_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &blocked_manifest, &blocked_plugin) ==
            SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(blocked_plugin) == SAO_OK);
    const int blocked_quiesce_baseline = capability_probe.quiesce_session_calls;
    CHECK(sao_plugins_lifecycle_unload(blocked_plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(blocked_plugin) == lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 1);
    CHECK(capability_probe.quiesce_session_calls == blocked_quiesce_baseline);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_blocked");
        REQUIRE(module != nullptr);
        PyObject* context = PyObject_GetAttrString(module, "context");
        REQUIRE(context != nullptr);
        PyObject* notify = PyObject_GetAttrString(context, "notify");
        Py_DECREF(context);
        REQUIRE(notify != nullptr);
        PyObject* notify_result =
            PyObject_CallFunction(notify, "ssds", "veto", "still-live", 5.0, "test");
        Py_DECREF(notify);
        REQUIRE(notify_result != nullptr);
        Py_DECREF(notify_result);
    }
    CHECK(capability_probe.notification_count() == 1);
    REQUIRE(sao_plugins_lifecycle_unload(blocked_plugin) == SAO_OK);
    CHECK(capability_probe.quiesce_session_calls == blocked_quiesce_baseline + 1);
    CHECK(capability_probe.notification_count() == 0);
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao_plugins_registry_remove(registry, blocked_plugin) == SAO_OK);

    TempTree slow(L"slow");
    const fs::path slow_started = slow.root / L"started";
    const fs::path slow_finished = slow.root / L"finished";
    write_text(slow.root / L"plugin.py", "from pathlib import Path\n"
                                         "import time\n"
                                         "def on_load(ctx):\n"
                                         "    pass\n"
                                         "def on_enable():\n"
                                         "    Path(" +
                                             json(path_utf8(slow_started)).dump() +
                                             ").write_text('started', encoding='utf-8')\n"
                                             "    time.sleep(3.0)\n"
                                             "    Path(" +
                                             json(path_utf8(slow_finished)).dump() +
                                             ").write_text('finished', encoding='utf-8')\n");
    plugin_manifest slow_manifest = make_manifest(slow, "adapter_slow");
    plugin_handle_t slow_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &slow_manifest, &slow_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(slow_plugin) == SAO_OK);

    TempTree quick(L"quick");
    const fs::path quick_finished = quick.root / L"finished";
    write_text(quick.root / L"plugin.py", "from pathlib import Path\n"
                                          "def on_load(ctx):\n"
                                          "    pass\n"
                                          "def on_enable():\n"
                                          "    Path(" +
                                              json(path_utf8(quick_finished)).dump() +
                                              ").write_text('finished', encoding='utf-8')\n");
    plugin_manifest quick_manifest = make_manifest(quick, "adapter_quick");
    plugin_handle_t quick_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &quick_manifest, &quick_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(quick_plugin) == SAO_OK);

    std::atomic_int slow_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread slow_hook([&] { slow_status.store(sao_plugins_lifecycle_enable(slow_plugin)); });
    for (int attempt = 0; attempt < 500 && !fs::exists(slow_started); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(fs::exists(slow_started));
    CHECK(sao_plugins_lifecycle_unload(slow_plugin) == SAO_PLUGINS_ERR_BUSY);

    const auto quick_started_at = std::chrono::steady_clock::now();
    REQUIRE(sao_plugins_lifecycle_enable(quick_plugin) == SAO_OK);
    const auto quick_elapsed = std::chrono::steady_clock::now() - quick_started_at;
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
    write_text(context_tree.root / L"plugin.py", R"PY(
results = {}

def unsupported(name, operation):
    try:
        operation()
    except NotImplementedError:
        results[name] = True
    else:
        results[name] = False

def on_load(ctx):
    ctx.log_info("external-context")
    unsupported("should_stop", lambda: ctx.should_stop)
    unsupported("engine", lambda: ctx.register_engine("direct", object()))
    unsupported("menu", lambda: ctx.register_menu_category(
        "direct", "D", lambda: []))
    status = ctx.open_window("panel", 640, 480)
    results["open_window"] = not status["ok"]
)PY");
    (void)make_manifest(context_tree, "adapter_context");
    SaoSdkContext external_context{};
    REQUIRE(sao_sdk_bind_context("adapter_context", "1.0.0", &external_context) == SAO_SDK_OK);
    py_plugin_handle_t direct_plugin = nullptr;
    {
        GilGuard gil;
        REQUIRE(sao_plugins_pyhost_load_plugin(direct_host, context_tree.root.c_str(), "plugin.py",
                                               "adapter_context", &external_context,
                                               &direct_plugin) == SAO_OK);
        REQUIRE(direct_plugin != nullptr);
        CHECK(sao_plugins_pyhost_get_sdk_context(direct_plugin) == &external_context);
        REQUIRE(sao_plugins_pyhost_call_on_load(direct_plugin) == SAO_OK);
        PyObject* direct_module = loaded_module("adapter_context");
        REQUIRE(direct_module != nullptr);
        PyObject* results = PyObject_GetAttrString(direct_module, "results");
        REQUIRE(results != nullptr);
        REQUIRE(PyDict_Check(results));
        for (const char* key : {"should_stop", "engine", "menu", "open_window"}) {
            CAPTURE(key);
            PyObject* value = PyDict_GetItemString(results, key);
            REQUIRE(value != nullptr);
            CHECK(PyObject_IsTrue(value) == 1);
        }
        Py_DECREF(results);
        REQUIRE(sao_plugins_pyhost_unload_plugin(direct_plugin) == SAO_OK);
    }
    CHECK(external_context.ctx_impl != nullptr);
    CHECK(external_context.abi_version == SAO_SDK_ABI_VERSION);
    REQUIRE(sao_sdk_context_try_destroy(&external_context) == SAO_SDK_OK);

    TempTree capabilities(L"capabilities");
    const fs::path stop_observation = capabilities.root / L"stop.txt";
    write_text(capabilities.root / L"plugin.py",
               "from pathlib import Path\n"
               "events = []\n"
               "hits = []\n"
               "state = {}\n"
               "context = None\n"
               "event_token = ''\n"
               "interval_token = ''\n"
               "def on_event(event):\n"
               "    events.append(event)\n"
               "    try:\n"
               "        context.unsubscribe(event_token)\n"
               "    except RuntimeError:\n"
               "        state['event_reentry_busy'] = True\n"
               "def on_interval():\n"
               "    hits.append('interval')\n"
               "    try:\n"
               "        context.clear_timer(interval_token)\n"
               "    except RuntimeError:\n"
               "        state['timer_reentry_busy'] = True\n"
               "def on_timeout():\n"
               "    hits.append('timeout')\n"
               "def clear_interval():\n"
               "    return (context.clear_timer(interval_token), "
               "context.clear_timer(interval_token))\n"
               "def unsubscribe_event():\n"
               "    return (context.unsubscribe(event_token), "
               "context.unsubscribe(event_token))\n"
               "def on_load(ctx):\n"
               "    global context, event_token, interval_token\n"
               "    context = ctx\n"
               "    state['should_stop'] = ctx.should_stop is False\n"
               "    engine = {'value': 7}\n"
               "    ctx.engine.register('Demo-Engine', engine)\n"
               "    state['engine'] = ctx.get_engine('demo_engine') is engine\n"
               "    state['engine_default'] = ctx.get_engine('missing', 19) == 19\n"
               "    event_token = ctx.subscribe('capability.event', on_event)\n"
               "    state['event_token'] = event_token.startswith('event_')\n"
               "    interval_token = ctx.set_interval(on_interval, 60.0)\n"
               "    timeout_token = ctx.set_timeout(on_timeout, 60.0)\n"
               "    state['timer_tokens'] = (interval_token.startswith('timer_') and "
               "timeout_token.startswith('timer_') and interval_token != timeout_token)\n"
               "    state['notify'] = ctx.notify('title', 'message', 30.0, 'test')\n"
               "    state['dismiss'] = ctx.dismiss_notify()\n"
               "    state['dismiss_again'] = not ctx.dismiss_notify()\n"
               "    state['toast'] = ctx.toast('toast-message')\n"
               "    window = ctx.open_window('panel', 640, 480)\n"
               "    state['open_window'] = not window['ok']\n"
               "def on_unload():\n"
               "    Path(" +
                   json(path_utf8(stop_observation)).dump() +
                   ").write_text('true' if context.should_stop else 'false', "
                   "encoding='utf-8')\n");
    plugin_manifest capabilities_manifest = make_manifest(capabilities, "adapter_capabilities");
    plugin_handle_t capabilities_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &capabilities_manifest,
                                            &capabilities_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(capabilities_plugin) == SAO_OK);
    CHECK(capability_probe.timer_count() == 2);
    CHECK(capability_probe.notification_count() == 1);
    PyObject* timeout_weakref = nullptr;
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_capabilities");
        REQUIRE(module != nullptr);
        PyObject* weakref_module = PyImport_ImportModule("weakref");
        REQUIRE(weakref_module != nullptr);
        PyObject* weakref_ref = PyObject_GetAttrString(weakref_module, "ref");
        Py_DECREF(weakref_module);
        REQUIRE(weakref_ref != nullptr);
        PyObject* timeout_callback = PyObject_GetAttrString(module, "on_timeout");
        REQUIRE(timeout_callback != nullptr);
        timeout_weakref = PyObject_CallOneArg(weakref_ref, timeout_callback);
        Py_DECREF(timeout_callback);
        Py_DECREF(weakref_ref);
        REQUIRE(timeout_weakref != nullptr);
        REQUIRE(PyObject_DelAttrString(module, "on_timeout") == 0);
    }

    plugin_context_t* capabilities_context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(capabilities_plugin, &capabilities_context) ==
            SAO_OK);
    REQUIRE(capabilities_context != nullptr);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_capabilities");
        REQUIRE(module != nullptr);
        PyObject* context = PyObject_GetAttrString(module, "context");
        REQUIRE(context != nullptr);
        CHECK(sao_plugins_pyhost_ctx_bind_loader_context(context, capabilities_context) ==
              SAO_PLUGINS_ERR_ALREADY_EXISTS);
        Py_DECREF(context);
    }
    std::atomic_int event_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread event_worker([&] {
        event_status.store(
            sao_plugins_ctx_emit(capabilities_context, "capability.event", R"({"value":7})"));
    });
    event_worker.join();
    CHECK(event_status.load() == SAO_OK);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_capabilities");
        REQUIRE(module != nullptr);
        PyObject* state = PyObject_GetAttrString(module, "state");
        REQUIRE(state != nullptr);
        REQUIRE(PyDict_Check(state));
        for (const char* key :
             {"should_stop", "engine", "engine_default", "event_token", "timer_tokens", "notify",
              "dismiss", "dismiss_again", "toast", "open_window", "event_reentry_busy"}) {
            CAPTURE(key);
            PyObject* value = PyDict_GetItemString(state, key);
            REQUIRE(value != nullptr);
            CHECK(PyObject_IsTrue(value) == 1);
        }
        Py_DECREF(state);
        PyObject* events = PyObject_GetAttrString(module, "events");
        REQUIRE(events != nullptr);
        REQUIRE(PyList_Check(events));
        REQUIRE(PyList_GET_SIZE(events) == 1);
        PyObject* event = PyList_GET_ITEM(events, 0);
        REQUIRE(PyDict_Check(event));
        PyObject* payload = PyDict_GetItemString(event, "payload");
        REQUIRE(payload != nullptr);
        REQUIRE(PyDict_Check(payload));
        PyObject* value = PyDict_GetItemString(payload, "value");
        REQUIRE(value != nullptr);
        CHECK(PyLong_AsLong(value) == 7);
        Py_DECREF(events);
        PyObject* unsubscribe = PyObject_GetAttrString(module, "unsubscribe_event");
        REQUIRE(unsubscribe != nullptr);
        PyObject* unsubscribe_result = PyObject_CallNoArgs(unsubscribe);
        Py_DECREF(unsubscribe);
        REQUIRE(unsubscribe_result != nullptr);
        REQUIRE(PyTuple_Check(unsubscribe_result));
        REQUIRE(PyTuple_GET_SIZE(unsubscribe_result) == 2);
        CHECK(PyObject_IsTrue(PyTuple_GET_ITEM(unsubscribe_result, 0)) == 1);
        CHECK(PyObject_IsTrue(PyTuple_GET_ITEM(unsubscribe_result, 1)) == 0);
        Py_DECREF(unsubscribe_result);
    }

    REQUIRE(capability_probe.timer_callbacks().size() == 2);
    std::jthread timer_worker([&capability_probe] { capability_probe.fire_timers_once(); });
    timer_worker.join();
    CHECK(capability_probe.timer_count() == 1);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_capabilities");
        PyObject* hits = PyObject_GetAttrString(module, "hits");
        REQUIRE(hits != nullptr);
        REQUIRE(PyList_Check(hits));
        CHECK(PyList_GET_SIZE(hits) == 2);
        Py_DECREF(hits);
        PyObject* state = PyObject_GetAttrString(module, "state");
        REQUIRE(state != nullptr);
        PyObject* timer_reentry_busy = PyDict_GetItemString(state, "timer_reentry_busy");
        REQUIRE(timer_reentry_busy != nullptr);
        CHECK(PyObject_IsTrue(timer_reentry_busy) == 1);
        Py_DECREF(state);
        PyObject* context = PyObject_GetAttrString(module, "context");
        REQUIRE(context != nullptr);
        void* timer_tokens = sao_plugins_pyhost_ctx_get_records(context, "timer_tokens");
        REQUIRE(timer_tokens != nullptr);
        CHECK(PyDict_Size(reinterpret_cast<PyObject*>(timer_tokens)) == 1);
        Py_DECREF(reinterpret_cast<PyObject*>(timer_tokens));
        void* callback_refs = sao_plugins_pyhost_ctx_get_records(context, "callback_refs");
        REQUIRE(callback_refs != nullptr);
        CHECK(PyList_Size(reinterpret_cast<PyObject*>(callback_refs)) == 1);
        Py_DECREF(reinterpret_cast<PyObject*>(callback_refs));
        Py_DECREF(context);
        (void)PyGC_Collect();
        CHECK(PyWeakref_GetObject(timeout_weakref) == Py_None);
        Py_DECREF(timeout_weakref);
        PyObject* clear = PyObject_GetAttrString(module, "clear_interval");
        REQUIRE(clear != nullptr);
        PyObject* result = PyObject_CallNoArgs(clear);
        Py_DECREF(clear);
        REQUIRE(result != nullptr);
        REQUIRE(PyTuple_Check(result));
        REQUIRE(PyTuple_GET_SIZE(result) == 2);
        CHECK(PyObject_IsTrue(PyTuple_GET_ITEM(result, 0)) == 1);
        CHECK(PyObject_IsTrue(PyTuple_GET_ITEM(result, 1)) == 0);
        Py_DECREF(result);
    }
    CHECK(capability_probe.timer_count() == 0);

    g_teardown_status.store(SAO_ERR_OS_CALL_FAILED);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_capabilities");
        REQUIRE(module != nullptr);
        PyObject* context = PyObject_GetAttrString(module, "context");
        REQUIRE(context != nullptr);
        g_teardown_context = context;
        PyObject* callback = PyCFunction_NewEx(&g_teardown_method, nullptr, nullptr);
        REQUIRE(callback != nullptr);
        PyObject* subscribe = PyObject_GetAttrString(context, "subscribe");
        REQUIRE(subscribe != nullptr);
        PyObject* token = PyObject_CallFunction(subscribe, "sO", "teardown.reentry", callback);
        Py_DECREF(subscribe);
        Py_DECREF(callback);
        REQUIRE(token != nullptr);
        Py_DECREF(token);
        Py_DECREF(context);
    }
    REQUIRE(sao_plugins_ctx_emit(capabilities_context, "teardown.reentry", "{}") == SAO_OK);
    CHECK(g_teardown_status.load() == SAO_PLUGINS_ERR_BUSY);
    g_teardown_context = nullptr;

    REQUIRE(sao_plugins_lifecycle_unload(capabilities_plugin) == SAO_OK);
    CHECK(capability_probe.timer_count() == 0);
    CHECK(capability_probe.notification_count() == 0);
    REQUIRE(fs::exists(stop_observation));
    {
        std::ifstream input(stop_observation, std::ios::binary);
        std::string observed;
        input >> observed;
        CHECK(observed == "true");
    }
    REQUIRE(sao_plugins_registry_remove(registry, capabilities_plugin) == SAO_OK);

#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
    TempTree menu(L"menu_provider");
    write_text(menu.root / L"plugin.py", R"PY(
events = []
mode = 0
action_fail = False
build_calls = 0
context = None
duplicate_rejected = False

def run_primary():
    if action_fail:
        raise RuntimeError("action fixture failure")
    events.append("primary")

def run_secondary():
    events.append("secondary")

def build_menu():
    global build_calls
    build_calls += 1
    if mode == 2:
        raise RuntimeError("builder fixture failure")
    primary = {
        "action_id": "primary",
        "icon": "界",
        "label": "主要" if mode == 0 else "主要更新",
        "command": run_primary,
        "payload": {"值": "雪"},
        "keep_menu_open": True,
    }
    secondary = {
        "action_id": "secondary",
        "icon": "关",
        "label": "关闭前",
        "command": run_secondary,
        "close_menu_before": True,
    }
    return [primary, secondary] if mode == 0 else [secondary, primary]

def on_load(ctx):
    global context, duplicate_rejected
    context = ctx
    extension_id = ctx.register_menu_category(
        "工具 α", "⚙", build_menu, priority=10.5)
    try:
        ctx.register_menu_category(
            "工具 α", "⚙", build_menu, priority=10.5)
    except RuntimeError:
        duplicate_rejected = True
    else:
        raise RuntimeError("duplicate menu registration was accepted")
    events.append("menu:" + extension_id)
)PY");
    plugin_manifest menu_manifest = make_manifest(menu, "adapter_menu");
    plugin_handle_t menu_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &menu_manifest, &menu_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(menu_plugin) == SAO_OK);
    PyObject* builder_weakref = nullptr;
    PyObject* action_weakref = nullptr;
    MenuCatalogSnapshot menu_catalog;
    REQUIRE(snapshot_menu_catalog(menu_catalog) == SAO_OK);
    CHECK(menu_catalog.providers.empty());
    CHECK(menu_catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_enable(menu_plugin) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(menu_catalog) == SAO_OK);
    REQUIRE(menu_catalog.providers.size() == 1);
    REQUIRE(menu_catalog.roots.size() == 1);
    const auto first_menu = menu_catalog;
    const auto& first_provider = first_menu.providers[0];
    const auto& first_root = first_menu.roots[0];
    CHECK(first_provider.owner_id == "adapter_menu");
    CHECK(first_provider.provider_id.rfind("adapter_menu/", 0) == 0);
    CHECK(first_provider.generation > 0);
    CHECK(first_provider.revision == 1);
    REQUIRE(first_provider.rows.size() == 2);
    CHECK(first_provider.rows[0].label == "主要");
    CHECK(first_provider.rows[0].icon == "界");
    CHECK(first_provider.rows[0].can_activate);
    CHECK(first_provider.rows[0].keep_menu_open);
    CHECK_FALSE(first_provider.rows[0].close_menu_before);
    CHECK(first_provider.rows[0].payload.find("\\u503c") != std::string::npos);
    CHECK(first_provider.rows[1].close_menu_before);
    CHECK(first_root.owner_id == "adapter_menu");
    CHECK(first_root.name == "工具 α");
    CHECK(first_root.icon == "⚙");
    CHECK(first_root.priority == 10.5);
    REQUIRE(first_root.actions.size() == 2);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_menu");
        PyObject* build_calls = PyObject_GetAttrString(module, "build_calls");
        REQUIRE(build_calls != nullptr);
        CHECK(PyLong_AsLong(build_calls) == 1);
        Py_DECREF(build_calls);
        PyObject* duplicate_rejected = PyObject_GetAttrString(module, "duplicate_rejected");
        REQUIRE(duplicate_rejected != nullptr);
        CHECK(PyObject_IsTrue(duplicate_rejected) == 1);
        Py_DECREF(duplicate_rejected);
        PyObject* context = PyObject_GetAttrString(module, "context");
        REQUIRE(context != nullptr);
        void* menus = sao_plugins_pyhost_ctx_get_records(context, "menus");
        REQUIRE(menus != nullptr);
        CHECK(PyList_GET_SIZE(reinterpret_cast<PyObject*>(menus)) == 1);
        Py_DECREF(reinterpret_cast<PyObject*>(menus));
        Py_DECREF(context);
        PyObject* weakref_module = PyImport_ImportModule("weakref");
        REQUIRE(weakref_module != nullptr);
        PyObject* weakref_ref = PyObject_GetAttrString(weakref_module, "ref");
        Py_DECREF(weakref_module);
        REQUIRE(weakref_ref != nullptr);
        PyObject* builder = PyObject_GetAttrString(module, "build_menu");
        PyObject* action = PyObject_GetAttrString(module, "run_primary");
        REQUIRE(builder != nullptr);
        REQUIRE(action != nullptr);
        builder_weakref = PyObject_CallOneArg(weakref_ref, builder);
        action_weakref = PyObject_CallOneArg(weakref_ref, action);
        Py_DECREF(builder);
        Py_DECREF(action);
        Py_DECREF(weakref_ref);
        REQUIRE(builder_weakref != nullptr);
        REQUIRE(action_weakref != nullptr);
    }

    const std::uint64_t menu_generation = first_provider.generation;
    const std::string primary_action = first_provider.rows[0].action_id;
    const std::string secondary_action = first_provider.rows[1].action_id;
    std::atomic_int worker_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread action_worker([&] {
        worker_status.store(sao_plugins_entity_provider_invoke(
            first_provider.provider_id.c_str(), menu_generation, primary_action.c_str(),
            first_provider.rows[0].payload.c_str()));
    });
    action_worker.join();
    CHECK(worker_status.load() == SAO_OK);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_menu");
        CHECK(event_at(module, 1) == "primary");
        PyObject* one = PyLong_FromLong(1);
        REQUIRE(one != nullptr);
        REQUIRE(PyObject_SetAttrString(module, "mode", one) == 0);
        Py_DECREF(one);
    }

    MenuCatalogSnapshot reordered;
    REQUIRE(snapshot_menu_catalog(reordered) == SAO_OK);
    REQUIRE(reordered.providers.size() == 1);
    REQUIRE(reordered.providers[0].rows.size() == 2);
    CHECK(reordered.providers[0].revision == 2);
    CHECK(reordered.providers[0].rows[0].action_id == secondary_action);
    CHECK(reordered.providers[0].rows[1].action_id == primary_action);
    CHECK(reordered.providers[0].rows[1].label == "主要更新");

    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_menu");
        PyObject* two = PyLong_FromLong(2);
        REQUIRE(two != nullptr);
        REQUIRE(PyObject_SetAttrString(module, "mode", two) == 0);
        Py_DECREF(two);
    }
    const MenuCatalogSnapshot retained = reordered;
    CHECK(snapshot_menu_catalog(reordered) == SAO_ERR_OS_CALL_FAILED);
    CHECK(reordered.providers == retained.providers);
    CHECK(reordered.roots == retained.roots);
    REQUIRE(sao_plugins_entity_provider_invoke(first_provider.provider_id.c_str(), menu_generation,
                                               primary_action.c_str(), "{}") == SAO_OK);

    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_menu");
        REQUIRE(PyObject_SetAttrString(module, "action_fail", Py_True) == 0);
    }
    CHECK(sao_plugins_entity_provider_invoke(first_provider.provider_id.c_str(), menu_generation,
                                             primary_action.c_str(),
                                             "{}") == SAO_ERR_OS_CALL_FAILED);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_menu");
        REQUIRE(PyObject_SetAttrString(module, "action_fail", Py_False) == 0);
        PyObject* zero = PyLong_FromLong(0);
        REQUIRE(zero != nullptr);
        REQUIRE(PyObject_SetAttrString(module, "mode", zero) == 0);
        Py_DECREF(zero);
        REQUIRE(PyObject_DelAttrString(module, "build_menu") == 0);
        REQUIRE(PyObject_DelAttrString(module, "run_primary") == 0);
    }

    REQUIRE(sao_plugins_lifecycle_disable(menu_plugin) == SAO_OK);
    menu_catalog = {};
    REQUIRE(snapshot_menu_catalog(menu_catalog) == SAO_OK);
    CHECK(menu_catalog.providers.empty());
    CHECK(menu_catalog.roots.empty());
    CHECK(sao_plugins_entity_provider_invoke(first_provider.provider_id.c_str(), menu_generation,
                                             primary_action.c_str(), "{}") == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(menu_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(menu_plugin) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke(first_provider.provider_id.c_str(), menu_generation,
                                             primary_action.c_str(),
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_registry_remove(registry, menu_plugin) == SAO_OK);
    {
        GilGuard gil;
        (void)PyGC_Collect();
        CHECK(PyWeakref_GetObject(builder_weakref) == Py_None);
        CHECK(PyWeakref_GetObject(action_weakref) == Py_None);
        Py_DECREF(builder_weakref);
        Py_DECREF(action_weakref);
    }

    TempTree menu_rollback(L"menu_rollback");
    write_text(menu_rollback.root / L"plugin.py", R"PY(
def build_menu():
    return [{"label": "rollback", "icon": "R", "command": lambda: None}]
def on_load(ctx):
    ctx.register_menu_category("回滚", "R", build_menu, priority=1.25)
    raise RuntimeError("menu rollback fixture")
)PY");
    plugin_manifest rollback_manifest = make_manifest(menu_rollback, "adapter_menu_rollback");
    plugin_handle_t rollback_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &rollback_manifest, &rollback_plugin) ==
            SAO_OK);
    CHECK(sao_plugins_lifecycle_load(rollback_plugin) == SAO_ERR_OS_CALL_FAILED);
    menu_catalog = {};
    REQUIRE(snapshot_menu_catalog(menu_catalog) == SAO_OK);
    CHECK(menu_catalog.providers.empty());
    CHECK(menu_catalog.roots.empty());
    CHECK(sao_plugins_pyhost_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao_plugins_registry_remove(registry, rollback_plugin) == SAO_OK);

    TempTree empty_menu(L"empty_menu");
    write_text(empty_menu.root / L"plugin.py", R"PY(
build_calls = 0
def build_menu():
    global build_calls
    build_calls += 1
    return []
def on_load(ctx):
    ctx.register_menu_category("空菜单", "E", build_menu)
)PY");
    plugin_manifest empty_menu_manifest = make_manifest(empty_menu, "adapter_empty_menu");
    plugin_handle_t empty_menu_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &empty_menu_manifest, &empty_menu_plugin) ==
            SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(empty_menu_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(empty_menu_plugin) == SAO_OK);
    MenuCatalogSnapshot empty_catalog;
    REQUIRE(snapshot_menu_catalog(empty_catalog) == SAO_OK);
    REQUIRE(empty_catalog.providers.size() == 1);
    CHECK(empty_catalog.providers[0].rows.empty());
    REQUIRE(empty_catalog.roots.size() == 1);
    CHECK(empty_catalog.roots[0].actions.empty());
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_empty_menu");
        REQUIRE(module != nullptr);
        PyObject* build_calls = PyObject_GetAttrString(module, "build_calls");
        REQUIRE(build_calls != nullptr);
        CHECK(PyLong_AsLong(build_calls) == 1);
        Py_DECREF(build_calls);
    }
    REQUIRE(sao_plugins_lifecycle_unload(empty_menu_plugin) == SAO_OK);
    REQUIRE(sao_plugins_registry_remove(registry, empty_menu_plugin) == SAO_OK);

    TempTree implicit_menu(L"implicit_menu");
    write_text(implicit_menu.root / L"plugin.py", R"PY(
mode = 0
def run_shared():
    pass
def build_menu():
    shared_a = {
        "label": "共享 A",
        "icon": "A",
        "command": run_shared,
        "payload": {"slot": "a"},
        "keep_menu_open": True,
    }
    duplicate_a = dict(shared_a)
    shared_b = {
        "label": "共享 B",
        "icon": "B",
        "command": run_shared,
        "payload": {"slot": "b"},
        "close_menu_before": True,
    }
    inserted = {
        "label": "插入项",
        "icon": "I",
        "command": run_shared,
        "payload": {"slot": "inserted"},
    }
    if mode == 0:
        return [shared_a, duplicate_a, shared_b]
    if mode == 1:
        return [inserted, shared_a, duplicate_a, shared_b]
    return [shared_b, shared_a, duplicate_a]
def on_load(ctx):
    ctx.register_menu_category("隐式标识", "I", build_menu)
)PY");
    plugin_manifest implicit_manifest = make_manifest(implicit_menu, "adapter_implicit_menu");
    plugin_handle_t implicit_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &implicit_manifest, &implicit_plugin) ==
            SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(implicit_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(implicit_plugin) == SAO_OK);

    MenuCatalogSnapshot implicit_first;
    REQUIRE(snapshot_menu_catalog(implicit_first) == SAO_OK);
    REQUIRE(implicit_first.providers.size() == 1);
    REQUIRE(implicit_first.providers[0].rows.size() == 3);
    const auto first_a0 = implicit_first.providers[0].rows[0].action_id;
    const auto first_a1 = implicit_first.providers[0].rows[1].action_id;
    const auto first_b = implicit_first.providers[0].rows[2].action_id;
    CHECK(first_a0 != first_a1);
    CHECK(first_a0 != first_b);
    CHECK(first_a1 != first_b);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_implicit_menu");
        REQUIRE(module != nullptr);
        PyObject* one = PyLong_FromLong(1);
        REQUIRE(one != nullptr);
        REQUIRE(PyObject_SetAttrString(module, "mode", one) == 0);
        Py_DECREF(one);
    }
    MenuCatalogSnapshot implicit_inserted;
    REQUIRE(snapshot_menu_catalog(implicit_inserted) == SAO_OK);
    REQUIRE(implicit_inserted.providers.size() == 1);
    REQUIRE(implicit_inserted.providers[0].rows.size() == 4);
    CHECK(implicit_inserted.providers[0].rows[1].action_id == first_a0);
    CHECK(implicit_inserted.providers[0].rows[2].action_id == first_a1);
    CHECK(implicit_inserted.providers[0].rows[3].action_id == first_b);
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_implicit_menu");
        REQUIRE(module != nullptr);
        PyObject* two = PyLong_FromLong(2);
        REQUIRE(two != nullptr);
        REQUIRE(PyObject_SetAttrString(module, "mode", two) == 0);
        Py_DECREF(two);
    }
    MenuCatalogSnapshot implicit_reordered;
    REQUIRE(snapshot_menu_catalog(implicit_reordered) == SAO_OK);
    REQUIRE(implicit_reordered.providers.size() == 1);
    REQUIRE(implicit_reordered.providers[0].rows.size() == 3);
    CHECK(implicit_reordered.providers[0].rows[0].action_id == first_b);
    CHECK(implicit_reordered.providers[0].rows[1].action_id == first_a0);
    CHECK(implicit_reordered.providers[0].rows[2].action_id == first_a1);
    REQUIRE(sao_plugins_lifecycle_unload(implicit_plugin) == SAO_OK);
    REQUIRE(sao_plugins_registry_remove(registry, implicit_plugin) == SAO_OK);

    TempTree enable_rollback(L"menu_enable_rollback");
    write_text(enable_rollback.root / L"plugin.py", R"PY(
context = None
extension_id = ""
def build_menu():
    return [{"label": "enable rollback", "command": lambda: None}]
def on_load(ctx):
    global context
    context = ctx
def on_enable():
    global extension_id
    extension_id = context.register_menu_category(
        "启用回滚", "R", build_menu, priority=3.0)
    raise RuntimeError("enable menu rollback fixture")
)PY");
    plugin_manifest enable_rollback_manifest =
        make_manifest(enable_rollback, "adapter_menu_enable_rollback");
    plugin_handle_t enable_rollback_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &enable_rollback_manifest,
                                            &enable_rollback_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(enable_rollback_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_enable(enable_rollback_plugin) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(enable_rollback_plugin) == lifecycle_state::loaded_disabled);
    MenuCatalogSnapshot enable_rollback_catalog;
    REQUIRE(snapshot_menu_catalog(enable_rollback_catalog) == SAO_OK);
    CHECK(enable_rollback_catalog.providers.empty());
    CHECK(enable_rollback_catalog.roots.empty());
    std::string enable_rollback_provider;
    {
        GilGuard gil;
        PyObject* module = loaded_module("adapter_menu_enable_rollback");
        REQUIRE(module != nullptr);
        PyObject* extension_id = PyObject_GetAttrString(module, "extension_id");
        REQUIRE(extension_id != nullptr);
        const char* extension_text = PyUnicode_AsUTF8(extension_id);
        REQUIRE(extension_text != nullptr);
        enable_rollback_provider = "adapter_menu_enable_rollback/" + std::string(extension_text);
        Py_DECREF(extension_id);
    }
    CHECK(sao_plugins_entity_provider_invoke(enable_rollback_provider.c_str(), 1, "unused", "{}") !=
          SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(enable_rollback_plugin) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke(enable_rollback_provider.c_str(), 1, "unused", "{}") ==
          SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_registry_remove(registry, enable_rollback_plugin) == SAO_OK);
#else
    TempTree menu_unavailable(L"menu_unavailable");
    write_text(menu_unavailable.root / L"plugin.py", R"PY(
def on_load(ctx):
    ctx.register_menu_category(
        "blocked", "B", lambda: [{"label": "blocked", "command": lambda: None}])
)PY");
    plugin_manifest menu_unavailable_manifest =
        make_manifest(menu_unavailable, "adapter_menu_unavailable");
    plugin_handle_t menu_unavailable_plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(registry, &menu_unavailable_manifest,
                                            &menu_unavailable_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_load(menu_unavailable_plugin) == SAO_ERR_OS_CALL_FAILED);
    char* menu_error = nullptr;
    REQUIRE(sao_plugins_pyhost_loader_adapter_get_last_error(owner, menu_unavailable_plugin,
                                                             &menu_error) == SAO_OK);
    REQUIRE(menu_error != nullptr);
    CHECK(std::string(menu_error).find("dynamic menu provider") != std::string::npos);
    sao_plugins_pyhost_free_string(menu_error);
    REQUIRE(sao_plugins_registry_remove(registry, menu_unavailable_plugin) == SAO_OK);
#endif

    REQUIRE(sao_plugins_pyhost_unregister_loader_adapter(owner) == SAO_OK);
    owner = nullptr;
    REQUIRE(sao_plugins_pyhost_register_loader_adapter(&config, &owner) == SAO_OK);
    REQUIRE(owner != nullptr);
    REQUIRE(sao_plugins_pyhost_unregister_loader_adapter(owner) == SAO_OK);
    REQUIRE(sao_plugins_pyhost_shutdown(direct_host) == SAO_OK);
    REQUIRE(sao_plugins_deps_unregister_provider() == SAO_OK);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    CHECK(dependency_state.retain_calls == dependency_state.release_calls);
    CHECK(capability_probe.create_session_calls == capability_probe.destroy_session_calls);
    CHECK(capability_probe.retain_calls == capability_probe.release_calls);
}

#else

TEST_CASE("production Python loader adapter requires embedded CPython",
          "[plugins][python][adapter][.skip]") {
    SKIP("Python embed not found");
}

#endif
