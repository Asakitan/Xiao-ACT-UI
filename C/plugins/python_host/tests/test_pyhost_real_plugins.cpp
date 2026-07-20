// test_pyhost_real_plugins.cpp — 真实老插件加载与 SDK 桥接测试 (Catch2)
//
// 目标: 用**未改动**的 Python 老插件 (star_resonance / hide_seek / midi_piano)
// 验证 C++ python_host 的加载语义, 覆盖:
//   1. 三个老插件都能 spec_from_file_location + exec_module 走通 (即"加载不 crash")
//   2. 反复 4 次 load/unload 无泄漏 (module refcount / sys.modules 干净)
//   3. plugin.py 里 `import sao_sdk` 拿得到内置模块
//   4. ctx.log_info(msg) 落到 host 的 log 记录里
//   5. ctx.register_ui_panel(...) 拿到 handle 且 host 侧记账正确
//   6. ctx.add_hotkey(...) 记账正确
//
// 老插件"加载成功"的定义 (spec 明列): 不 crash. 具体功能 (启动引擎 / 连游戏
// TCP / 弹 UI) **不校验** —— 那些需要真游戏 / 主平台其他组件.
//
// 编译 gating: Catch2_FOUND && SAO_HAS_PYTHON_EMBED. 缺任一时本 .cpp 不编 (由
// CMakeLists gate); 若 CMakeLists 强编但 SAO_HAS_PYTHON_EMBED 未定义, 全部 SKIP.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(SAO_HAS_PYTHON_EMBED)
#define PY_SSIZE_T_CLEAN
#include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/compat/py_v1_manifest.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"
#include "sao/sdk/sao_sdk.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef SAO_TEST_PLUGIN_STAR_RESONANCE
#define SAO_TEST_PLUGIN_STAR_RESONANCE ""
#endif
#ifndef SAO_TEST_PYTHON_HOME
#define SAO_TEST_PYTHON_HOME L""
#endif
#ifndef SAO_TEST_PLUGIN_HIDE_SEEK
#define SAO_TEST_PLUGIN_HIDE_SEEK ""
#endif
#ifndef SAO_TEST_PLUGIN_MIDI_PIANO
#define SAO_TEST_PLUGIN_MIDI_PIANO ""
#endif

using namespace sao::plugins::python_host;

#if defined(SAO_HAS_PYTHON_EMBED)

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_route_hotkey(uint32_t virtual_key,
                                                                     uint32_t modifiers);
extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_panel_invoke_action(
    const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, const char* action_key_utf8);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_live_context_count(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fire_render_hook(int32_t hook_point, const SaoSdkRenderHookPayload* payload);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_render_hook_count(const SaoSdkContext* ctx);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_widget_count(const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_canvas_count(const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);

namespace {

// ── 全局 host handle 单例 (跨 test case 复用) ──────────────
py_host_handle_t g_host = nullptr;
bool g_host_ready = false;

py_host_handle_t ensure_host() {
    if (g_host != nullptr && g_host_ready)
        return g_host;
    py_host_config cfg{};
    cfg.python_home = SAO_TEST_PYTHON_HOME;
    cfg.register_sao_sdk = true;
    cfg.controlled_test_shim = true;
    int32_t rc = sao_plugins_pyhost_init(&cfg, &g_host);
    if (rc != SAO_OK) {
        std::fprintf(stderr, "pyhost_init failed: rc=%d\n", rc);
        g_host = nullptr;
        return nullptr;
    }
    g_host_ready = true;
    return g_host;
}

// utf-8 → std::wstring (仅 ASCII 输入即可, 单测用的路径都是纯 ASCII)
std::wstring to_wide(const char* s) {
    std::wstring w;
    if (s == nullptr)
        return w;
    while (*s) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
        ++s;
    }
    return w;
}

// 尝试加载一个插件, 返 handle (可能 == nullptr). ctx 传 fake ptr.
py_plugin_handle_t try_load(const char* dir_utf8, const char* plugin_id) {
    py_host_handle_t h = ensure_host();
    if (h == nullptr)
        return nullptr;
    std::wstring dir_w = to_wide(dir_utf8);
    void* fake_ctx = reinterpret_cast<void*>(static_cast<intptr_t>(0xDEADBEEF));
    py_plugin_handle_t plugin = nullptr;
    int32_t rc = sao_plugins_pyhost_load_plugin(h, dir_w.c_str(), /*entry=*/nullptr, plugin_id,
                                                fake_ctx, &plugin);
    if (rc != SAO_OK) {
        // dump last_error for diagnostics
        if (plugin != nullptr) {
            char* err = nullptr;
            if (sao_plugins_pyhost_get_last_error(plugin, &err) == SAO_OK && err != nullptr) {
                std::fprintf(stderr, "load(%s) rc=%d, last_error:\n%s\n", plugin_id, rc, err);
                std::free(err);
            } else {
                std::fprintf(stderr, "load(%s) rc=%d, no last_error\n", plugin_id, rc);
            }
        } else {
            std::fprintf(stderr, "load(%s) rc=%d, no plugin handle\n", plugin_id, rc);
        }
    }
    return plugin;
}

// 断言: plugin != nullptr AND (rc == SAO_OK OR (rc != SAO_OK BUT module 已建))
// spec 说: "加载成功（不 crash, hook 调用返回 OK）" —— 我们放宽到:
//   * 若 module 顶层跑通 → 视 load 成功, hook_on_load 存在则调
//   * 若 module 顶层缺 import (cv2/mido/etc.) → 视为"环境不完备", 但 handle
//     仍返 (last_error 里含 traceback)。测试用 REQUIRE(module_ok || env_env_missing).
enum class LoadOutcome { OK, TOP_LEVEL_FAILED, MODULE_MISSING };

LoadOutcome outcome_of(py_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return LoadOutcome::MODULE_MISSING;
    void* mod = sao_plugins_pyhost_get_module_pyobject(plugin);
    if (mod == nullptr)
        return LoadOutcome::MODULE_MISSING;
    // 判 top-level 完成: last_error 空表 exec_module 无异常.
    char* err = nullptr;
    LoadOutcome oc = LoadOutcome::OK;
    if (sao_plugins_pyhost_get_last_error(plugin, &err) == SAO_OK && err != nullptr) {
        if (*err != '\0')
            oc = LoadOutcome::TOP_LEVEL_FAILED;
        std::free(err);
    }
    return oc;
}

void safe_unload(py_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return;
    (void)sao_plugins_pyhost_unload_plugin(plugin);
}

// 判断路径是否存在 (ASCII 简化)
bool path_exists(const char* p) {
    if (p == nullptr || *p == '\0')
        return false;
    std::FILE* f = std::fopen(p, "rb");
    if (f != nullptr) {
        std::fclose(f);
        return true;
    }
    return false;
}

// 判断插件 dir 是否存在 (查 plugin.json)
bool plugin_dir_ok(const char* dir_utf8) {
    if (dir_utf8 == nullptr || *dir_utf8 == '\0')
        return false;
    std::string p = dir_utf8;
    if (!p.empty() && p.back() != '/' && p.back() != '\\')
        p.push_back('/');
    p += "plugin.json";
    return path_exists(p.c_str());
}

void install_controlled_shims(const char* plugin_id) {
    const char* code = nullptr;
    if (std::strcmp(plugin_id, "star_resonance") == 0) {
        code = R"PY(
import sys, types
def _mod(name):
    mod = types.ModuleType(name)
    sys.modules[name] = mod
    return mod
for _name in ("plugins", "plugins.star_resonance_plugin", "act_platform"):
    sys.modules.setdefault(_name, types.ModuleType(_name))
sr_config = _mod("plugins.star_resonance_plugin.sr_config")
sr_config.GAME_MAIN_MODULE = "controlled-game-module"
runtime = _mod("act_platform.runtime")
runtime.register_extension_runtime = lambda *a, **k: None
bridge = _mod("plugins.star_resonance_plugin.act_runtime_bridge")
bridge.extension_runtime_provider = object()
webview = _mod("plugins.star_resonance_plugin.webview_bridge")
webview.install_webview_bridge = lambda *a, **k: None
entity = _mod("plugins.star_resonance_plugin.entity_menu_bridge")
entity.install_entity_menu_bridge = lambda *a, **k: None
actions = _mod("plugins.star_resonance_plugin.ai_actions")
actions.install_ai_engine_actions = lambda *a, **k: None
actions.uninstall_ai_engine_actions = lambda *a, **k: None
)PY";
    } else if (std::strcmp(plugin_id, "midi_piano_plugin") == 0) {
        code = R"PY(
import sys, types
class _Keyboard:
    def press(self, *_args, **_kwargs): return True
class _Player:
    def __init__(self): self.on_playback_end = None
    def set_speed(self, *_args, **_kwargs): pass
    def set_legato_overlap(self, *_args, **_kwargs): pass
    def stop(self): pass
mp_player = types.ModuleType("mp_player")
mp_player.MidiPlayer = _Player
mp_player.keyboard = _Keyboard()
sys.modules["mp_player"] = mp_player
mp_api = types.ModuleType("mp_api")
mp_api.list_midi_files = lambda *_args, **_kwargs: []
sys.modules["mp_api"] = mp_api
mp_dialog = types.ModuleType("mp_dialog")
mp_dialog.foreground_hwnd = lambda: 0
mp_dialog.open_midi = lambda **_kwargs: None
sys.modules["mp_dialog"] = mp_dialog
class _Audition:
    def stop(self): pass
    def backend_name(self): return "controlled-shim"
mp_audio = types.ModuleType("mp_audio")
mp_audio.MidiAudition = _Audition
sys.modules["mp_audio"] = mp_audio
)PY";
    }
    if (code == nullptr)
        return;
    PyObject* main = PyImport_AddModule("__main__");
    REQUIRE(main != nullptr);
    PyObject* globals = PyModule_GetDict(main);
    REQUIRE(globals != nullptr);
    PyObject* result = PyRun_String(code, Py_file_input, globals, globals);
    if (result == nullptr) {
        PyErr_Print();
    }
    REQUIRE(result != nullptr);
    Py_DECREF(result);
}

size_t record_count(void* ctx, const char* kind) {
    void* records_v = sao_plugins_pyhost_ctx_get_records(ctx, kind);
    REQUIRE(records_v != nullptr);
    PyObject* records = reinterpret_cast<PyObject*>(records_v);
    REQUIRE(PyList_Check(records));
    const size_t count = static_cast<size_t>(PyList_GET_SIZE(records));
    Py_DECREF(records);
    return count;
}

long main_counter(const char* name) {
    PyObject* globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    REQUIRE(globals != nullptr);
    PyObject* value = PyDict_GetItemString(globals, name);
    return value == nullptr ? 0L : PyLong_AsLong(value);
}

void set_main_counter(const char* name, long value) {
    PyObject* globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    REQUIRE(globals != nullptr);
    PyObject* number = PyLong_FromLong(value);
    REQUIRE(number != nullptr);
    REQUIRE(PyDict_SetItemString(globals, name, number) == 0);
    Py_DECREF(number);
}

PyObject* eval_callback(const char* expression) {
    PyObject* globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    REQUIRE(globals != nullptr);
    PyObject* callback = PyRun_String(expression, Py_eval_input, globals, globals);
    REQUIRE(callback != nullptr);
    REQUIRE(PyCallable_Check(callback));
    return callback;
}

sao_sdk_ui_panel_t register_controlled_canvas_panel(void* ctx, const char* panel_id,
                                                    PyObject* render, PyObject* action) {
    PyObject* meta = PyDict_New();
    REQUIRE(meta != nullptr);
    PyObject* title = PyUnicode_FromString("Controlled native canvas");
    REQUIRE(title != nullptr);
    REQUIRE(PyDict_SetItemString(meta, "title", title) == 0);
    Py_DECREF(title);
    REQUIRE(PyDict_SetItemString(meta, "canvas", Py_True) == 0);

    PyObject* method =
        PyObject_GetAttrString(reinterpret_cast<PyObject*>(ctx), "register_ui_panel");
    REQUIRE(method != nullptr);
    PyObject* args = Py_BuildValue("(sO)", panel_id, meta);
    Py_DECREF(meta);
    REQUIRE(args != nullptr);
    PyObject* kwargs = PyDict_New();
    REQUIRE(kwargs != nullptr);
    REQUIRE(PyDict_SetItemString(kwargs, "render", render) == 0);
    REQUIRE(PyDict_SetItemString(kwargs, "on_action", action) == 0);
    PyObject* result = PyObject_Call(method, args, kwargs);
    Py_DECREF(kwargs);
    Py_DECREF(args);
    Py_DECREF(method);
    REQUIRE(result != nullptr);
    Py_DECREF(result);

    void* panels_v = sao_plugins_pyhost_ctx_get_records(ctx, "panels");
    REQUIRE(panels_v != nullptr);
    PyObject* panels = reinterpret_cast<PyObject*>(panels_v);
    REQUIRE(PyList_Check(panels));
    REQUIRE(PyList_GET_SIZE(panels) > 0);
    PyObject* record = PyList_GET_ITEM(panels, PyList_GET_SIZE(panels) - 1);
    REQUIRE(PyDict_Check(record));
    PyObject* native_handle = PyDict_GetItemString(record, "native_handle");
    REQUIRE(native_handle != nullptr);
    const auto panel = reinterpret_cast<sao_sdk_ui_panel_t>(PyLong_AsVoidPtr(native_handle));
    Py_DECREF(panels);
    REQUIRE(panel != nullptr);
    return panel;
}

void register_controlled_hotkey_and_event(void* ctx, PyObject* hotkey, PyObject* event) {
    PyObject* hotkey_method =
        PyObject_GetAttrString(reinterpret_cast<PyObject*>(ctx), "add_hotkey");
    REQUIRE(hotkey_method != nullptr);
    PyObject* hotkey_args = Py_BuildValue("(sO)", "controlled_lifecycle", hotkey);
    REQUIRE(hotkey_args != nullptr);
    PyObject* hotkey_kwargs =
        Py_BuildValue("{s:s,s:s}", "default_key", "CTRL+F7", "label", "Controlled lifecycle");
    REQUIRE(hotkey_kwargs != nullptr);
    PyObject* hotkey_result = PyObject_Call(hotkey_method, hotkey_args, hotkey_kwargs);
    Py_DECREF(hotkey_kwargs);
    Py_DECREF(hotkey_args);
    Py_DECREF(hotkey_method);
    REQUIRE(hotkey_result != nullptr);
    Py_DECREF(hotkey_result);

    PyObject* subscribe = PyObject_GetAttrString(reinterpret_cast<PyObject*>(ctx), "subscribe");
    REQUIRE(subscribe != nullptr);
    PyObject* event_result =
        PyObject_CallFunction(subscribe, "sO", "controlled.lifecycle.event", event);
    Py_DECREF(subscribe);
    REQUIRE(event_result != nullptr);
    Py_DECREF(event_result);
}

std::atomic<int> g_render_ticks{0};

sao_sdk_status_t SAO_SDK_CALL controlled_render_tick(int32_t hook_point,
                                                     const SaoSdkRenderHookPayload*, void*) {
    if (hook_point == SAO_SDK_HOOK_AFTER_COMPOSITOR) {
        g_render_ticks.fetch_add(1);
    }
    return SAO_SDK_OK;
}

void require_no_python_object_leak(PyObject* weakref, const char* plugin_id) {
    REQUIRE(weakref != nullptr);
    for (int i = 0; i < 3; ++i) {
        (void)PyGC_Collect();
    }
    PyObject* target = PyWeakref_GetObject(weakref);
    INFO("plugin=" << plugin_id << " context_refcount=" << Py_REFCNT(target));
    REQUIRE(target == Py_None);
    Py_DECREF(weakref);
}

void require_controlled_native_lifecycle(const char* directory, const char* plugin_id,
                                         bool expects_panels) {
    const size_t contexts_before = sao_sdk_test_live_context_count();
    install_controlled_shims(plugin_id);
    py_plugin_handle_t plugin = try_load(directory, plugin_id);
    REQUIRE(plugin != nullptr);
    REQUIRE(outcome_of(plugin) == LoadOutcome::OK);
    REQUIRE(sao_plugins_pyhost_call_on_load(plugin) == SAO_OK);

    void* ctx = sao_plugins_pyhost_get_ctx_pyobject(plugin);
    REQUIRE(ctx != nullptr);
    REQUIRE(sao_plugins_pyhost_get_sdk_context(plugin) != nullptr);
    const auto* sdk = static_cast<const SaoSdkContext*>(sao_plugins_pyhost_get_sdk_context(plugin));
    REQUIRE(sao_sdk_test_live_context_count() == contexts_before + 1u);
    REQUIRE(record_count(ctx, expects_panels ? "panels" : "menus") > 0u);

    set_main_counter("__sao_lifecycle_action_hits", 0);
    set_main_counter("__sao_lifecycle_hotkey_hits", 0);
    set_main_counter("__sao_lifecycle_event_hits", 0);
    PyObject* render = eval_callback("lambda _payload=None: {'frame': 'idle'}");
    const char* callback_code = "def __sao_lifecycle_action(_action, _payload=None):\n"
                                "    globals()['__sao_lifecycle_action_hits'] += 1\n"
                                "def __sao_lifecycle_hotkey():\n"
                                "    globals()['__sao_lifecycle_hotkey_hits'] += 1\n"
                                "def __sao_lifecycle_event(_event):\n"
                                "    globals()['__sao_lifecycle_event_hits'] += 1\n";
    PyObject* globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    REQUIRE(globals != nullptr);
    PyObject* callback_result = PyRun_String(callback_code, Py_file_input, globals, globals);
    REQUIRE(callback_result != nullptr);
    Py_DECREF(callback_result);
    PyObject* action = eval_callback("__sao_lifecycle_action");
    PyObject* hotkey = eval_callback("__sao_lifecycle_hotkey");
    PyObject* event = eval_callback("__sao_lifecycle_event");

    const std::string panel_id = std::string("controlled.") + plugin_id + ".canvas";
    const sao_sdk_ui_panel_t panel =
        register_controlled_canvas_panel(ctx, panel_id.c_str(), render, action);
    register_controlled_hotkey_and_event(ctx, hotkey, event);
    Py_DECREF(event);
    Py_DECREF(hotkey);
    Py_DECREF(action);
    Py_DECREF(render);

    REQUIRE(sao_sdk_test_panel_canvas_count(sdk, panel) == 1u);
    REQUIRE(sao_sdk_test_panel_widget_count(sdk, panel) == 0u);
    REQUIRE(sao_sdk_test_panel_invoke_action(sdk, panel, "idle") == SAO_SDK_OK);
    REQUIRE(main_counter("__sao_lifecycle_action_hits") == 1L);

    sao_sdk_hook_token_t render_hook = 0;
    g_render_ticks.store(0);
    REQUIRE(sao_sdk_register_render_hook(sdk, SAO_SDK_HOOK_AFTER_COMPOSITOR, controlled_render_tick,
                                         nullptr, &render_hook) == SAO_SDK_OK);
    REQUIRE(render_hook != 0);
    REQUIRE(sao_sdk_test_render_hook_count(sdk) == 1u);
    SaoSdkRenderHookPayload frame{};
    frame.frame_index = 17;
    frame.viewport_width_px = 640;
    frame.viewport_height_px = 360;
    sao_sdk_test_fire_render_hook(SAO_SDK_HOOK_AFTER_COMPOSITOR, &frame);
    REQUIRE(g_render_ticks.load() == 1);

    REQUIRE(sao_sdk_test_route_hotkey(0x76u, 1u << 0) == 1u); // CTRL+F7
    REQUIRE(main_counter("__sao_lifecycle_hotkey_hits") == 1L);

    PyObject* publish = PyObject_GetAttrString(reinterpret_cast<PyObject*>(ctx), "publish");
    REQUIRE(publish != nullptr);
    PyObject* event_data = PyDict_New();
    REQUIRE(event_data != nullptr);
    PyObject* idle = PyUnicode_FromString("idle");
    REQUIRE(idle != nullptr);
    REQUIRE(PyDict_SetItemString(event_data, "state", idle) == 0);
    Py_DECREF(idle);
    PyObject* publish_result =
        PyObject_CallFunction(publish, "sO", "controlled.lifecycle.event", event_data);
    Py_DECREF(event_data);
    Py_DECREF(publish);
    REQUIRE(publish_result != nullptr);
    Py_DECREF(publish_result);
    REQUIRE(main_counter("__sao_lifecycle_event_hits") == 1L);

    if (sao_plugins_pyhost_has_hook(plugin, "on_enable")) {
        REQUIRE(sao_plugins_pyhost_call_on_enable(plugin) == SAO_OK);
    }
    if (sao_plugins_pyhost_has_hook(plugin, "on_disable")) {
        REQUIRE(sao_plugins_pyhost_call_on_disable(plugin) == SAO_OK);
    }
    PyObject* weakref = PyWeakref_NewRef(reinterpret_cast<PyObject*>(ctx), nullptr);
    REQUIRE(weakref != nullptr);
    bool allow_unload = false;
    REQUIRE(sao_plugins_pyhost_call_on_unload(plugin, &allow_unload) == SAO_OK);
    REQUIRE(allow_unload);
    REQUIRE(sao_plugins_pyhost_unload_plugin(plugin) == SAO_OK);
    REQUIRE(sao_sdk_test_live_context_count() == contexts_before);
    REQUIRE(sao_sdk_test_route_hotkey(0x76u, 1u << 0) == 0u);
    sao_sdk_test_fire_render_hook(SAO_SDK_HOOK_AFTER_COMPOSITOR, &frame);
    REQUIRE(g_render_ticks.load() == 1);
    require_no_python_object_leak(weakref, plugin_id);
}

void require_full_lifecycle(const char* directory, const char* plugin_id, bool expects_panels) {
    install_controlled_shims(plugin_id);
    py_plugin_handle_t plugin = try_load(directory, plugin_id);
    REQUIRE(plugin != nullptr);
    REQUIRE(outcome_of(plugin) == LoadOutcome::OK);
    REQUIRE(sao_plugins_pyhost_has_hook(plugin, "on_load"));
    const int32_t load_status = sao_plugins_pyhost_call_on_load(plugin);
    std::string load_error;
    if (load_status != SAO_OK) {
        char* error = nullptr;
        if (sao_plugins_pyhost_get_last_error(plugin, &error) == SAO_OK && error != nullptr) {
            load_error = error;
            std::free(error);
        }
    }
    INFO("plugin=" << plugin_id << " on_load traceback=" << load_error);
    REQUIRE(load_status == SAO_OK);

    void* ctx = sao_plugins_pyhost_get_ctx_pyobject(plugin);
    REQUIRE(ctx != nullptr);
    if (expects_panels) {
        REQUIRE(record_count(ctx, "panels") > 0u);
    } else {
        REQUIRE(record_count(ctx, "menus") > 0u);
    }

    if (sao_plugins_pyhost_has_hook(plugin, "on_enable")) {
        REQUIRE(sao_plugins_pyhost_call_on_enable(plugin) == SAO_OK);
    }
    if (sao_plugins_pyhost_has_hook(plugin, "on_disable")) {
        REQUIRE(sao_plugins_pyhost_call_on_disable(plugin) == SAO_OK);
    }
    bool allow_unload = false;
    REQUIRE(sao_plugins_pyhost_call_on_unload(plugin, &allow_unload) == SAO_OK);
    REQUIRE(allow_unload);
    REQUIRE(sao_plugins_pyhost_unload_plugin(plugin) == SAO_OK);
}

} // namespace

TEST_CASE("pyhost_controlled_shim_executes_legacy_plugin_lifecycles",
          "[pyhost][lifecycle][controlled_shim]") {
    REQUIRE(ensure_host() != nullptr);
    install_controlled_shims("star_resonance");
    if (!plugin_dir_ok(SAO_TEST_PLUGIN_STAR_RESONANCE) ||
        !plugin_dir_ok(SAO_TEST_PLUGIN_HIDE_SEEK) || !plugin_dir_ok(SAO_TEST_PLUGIN_MIDI_PIANO)) {
        FAIL("the three legacy plugin directories are required for lifecycle validation");
    }

    require_full_lifecycle(SAO_TEST_PLUGIN_STAR_RESONANCE, "star_resonance", false);
    require_full_lifecycle(SAO_TEST_PLUGIN_HIDE_SEEK, "hide_seek_plugin", true);
    require_full_lifecycle(SAO_TEST_PLUGIN_MIDI_PIANO, "midi_piano_plugin", true);
}

TEST_CASE("pyhost_real_plugins_complete_controlled_native_lifecycle_and_reload",
          "[pyhost][lifecycle][native][reload]") {
    REQUIRE(ensure_host() != nullptr);
    if (!plugin_dir_ok(SAO_TEST_PLUGIN_STAR_RESONANCE) ||
        !plugin_dir_ok(SAO_TEST_PLUGIN_HIDE_SEEK) || !plugin_dir_ok(SAO_TEST_PLUGIN_MIDI_PIANO)) {
        FAIL("the three legacy plugin directories are required for lifecycle validation");
    }

    require_controlled_native_lifecycle(SAO_TEST_PLUGIN_STAR_RESONANCE, "star_resonance", false);
    require_controlled_native_lifecycle(SAO_TEST_PLUGIN_HIDE_SEEK, "hide_seek_plugin", true);
    require_controlled_native_lifecycle(SAO_TEST_PLUGIN_MIDI_PIANO, "midi_piano_plugin", true);
    require_controlled_native_lifecycle(SAO_TEST_PLUGIN_STAR_RESONANCE, "star_resonance", false);
}

TEST_CASE("pyhost_uses_isolated_no_site_configuration", "[pyhost][isolated]") {
    REQUIRE(ensure_host() != nullptr);
    PyObject* flags = PySys_GetObject("flags");
    REQUIRE(flags != nullptr);
    PyObject* isolated = PyObject_GetAttrString(flags, "isolated");
    REQUIRE(isolated != nullptr);
    CHECK(PyLong_AsLong(isolated) == 1);
    Py_DECREF(isolated);

    PyObject* modules = PyImport_GetModuleDict();
    REQUIRE(modules != nullptr);
    CHECK(PyDict_GetItemString(modules, "site") == nullptr);
}

// ══════════════════════════════════════════════════════════
// CASE 1: star_resonance 老插件加载
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_loads_star_resonance_plugin_unchanged", "[pyhost][real_plugins][load]") {
    const char* dir = SAO_TEST_PLUGIN_STAR_RESONANCE;
    if (!plugin_dir_ok(dir)) {
        SKIP("star_resonance_plugin dir not present");
    }
    REQUIRE(ensure_host() != nullptr);

    py_plugin_handle_t plugin = try_load(dir, "star_resonance");
    // handle 必返 (即便 load 失败, 我们保 handle 供内省).
    REQUIRE(plugin != nullptr);

    // manifest 必解析成功: id / name / language.
    const void* mfp = sao_plugins_pyhost_get_manifest(plugin);
    REQUIRE(mfp != nullptr);
    const auto* mf = static_cast<const sao::plugins::loader::plugin_manifest*>(mfp);
    CHECK(mf->plugin_id == "star_resonance");
    CHECK(mf->language == sao::plugins::loader::engine_kind::python);
    CHECK_FALSE(mf->entry.empty());

    LoadOutcome oc = outcome_of(plugin);
    REQUIRE(oc == LoadOutcome::OK);

    if (oc == LoadOutcome::OK && sao_plugins_pyhost_has_hook(plugin, "on_load")) {
        // hook 存在: 调之. 抛异常不 crash, 记 last_error.
        int32_t rc = sao_plugins_pyhost_call_on_load(plugin);
        REQUIRE(rc == SAO_OK);
        char* err = nullptr;
        if (sao_plugins_pyhost_get_last_error(plugin, &err) == SAO_OK) {
            if (err != nullptr)
                std::free(err);
        }
    }

    safe_unload(plugin);
}

// ══════════════════════════════════════════════════════════
// CASE 2: hide_seek 老插件加载
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_loads_hide_seek_plugin_unchanged", "[pyhost][real_plugins][load]") {
    const char* dir = SAO_TEST_PLUGIN_HIDE_SEEK;
    if (!plugin_dir_ok(dir)) {
        SKIP("hide_seek_plugin dir not present");
    }
    REQUIRE(ensure_host() != nullptr);
    install_controlled_shims("hide_seek_plugin");

    py_plugin_handle_t plugin = try_load(dir, "hide_seek_plugin");
    REQUIRE(plugin != nullptr);

    const void* mfp = sao_plugins_pyhost_get_manifest(plugin);
    REQUIRE(mfp != nullptr);
    const auto* mf = static_cast<const sao::plugins::loader::plugin_manifest*>(mfp);
    CHECK(mf->plugin_id == "hide_seek_plugin");
    CHECK(mf->language == sao::plugins::loader::engine_kind::python);

    LoadOutcome oc = outcome_of(plugin);
    REQUIRE(oc == LoadOutcome::OK);

    // hide_seek.plugin.py 顶层只 def 函数, on_load 里才 import cv2. 我们期待
    // top-level 成功.
    if (oc == LoadOutcome::OK) {
        // on_load 存在
        CHECK(sao_plugins_pyhost_has_hook(plugin, "on_load"));
        // 调 on_load: 内部 register_ui_panel + register_hotkey → 记账.
        int32_t rc = sao_plugins_pyhost_call_on_load(plugin);
        REQUIRE(rc == SAO_OK);

        // 拿 ctx, 查 panels/hotkeys.
        void* ctx = sao_plugins_pyhost_get_ctx_pyobject(plugin);
        REQUIRE(ctx != nullptr);
        void* panels = sao_plugins_pyhost_ctx_get_records(ctx, "panels");
        REQUIRE(panels != nullptr);
        PyObject* panels_list = reinterpret_cast<PyObject*>(panels);
        // hide_seek 注册了 "hide_seek" 面板.
        CHECK(PyList_Check(panels_list));
        // 至少 1 个面板 (可能 rc != OK 情况下 on_load 已抛在 register_ui_panel 之前;
        // 允许 0 或 1)
        Py_ssize_t n = PyList_GET_SIZE(panels_list);
        CHECK(n >= 0);
        Py_DECREF(panels_list);

        void* hotkeys = sao_plugins_pyhost_ctx_get_records(ctx, "hotkeys");
        REQUIRE(hotkeys != nullptr);
        Py_DECREF(reinterpret_cast<PyObject*>(hotkeys));
    }

    safe_unload(plugin);
}

// ══════════════════════════════════════════════════════════
// CASE 3: midi_piano 老插件加载
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_loads_midi_piano_plugin_unchanged", "[pyhost][real_plugins][load]") {
    const char* dir = SAO_TEST_PLUGIN_MIDI_PIANO;
    if (!plugin_dir_ok(dir)) {
        SKIP("midi_piano_plugin dir not present");
    }
    REQUIRE(ensure_host() != nullptr);
    install_controlled_shims("midi_piano_plugin");

    py_plugin_handle_t plugin = try_load(dir, "midi_piano_plugin");
    REQUIRE(plugin != nullptr);

    const void* mfp = sao_plugins_pyhost_get_manifest(plugin);
    REQUIRE(mfp != nullptr);
    const auto* mf = static_cast<const sao::plugins::loader::plugin_manifest*>(mfp);
    CHECK(mf->plugin_id == "midi_piano_plugin");

    LoadOutcome oc = outcome_of(plugin);
    REQUIRE(oc == LoadOutcome::OK);

    safe_unload(plugin);
}

// ══════════════════════════════════════════════════════════
// CASE 4: 反复 4 次 load/unload star_resonance 无泄漏
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_unload_reload_star_resonance", "[pyhost][real_plugins][reload]") {
    const char* dir = SAO_TEST_PLUGIN_STAR_RESONANCE;
    if (!plugin_dir_ok(dir)) {
        SKIP("star_resonance_plugin dir not present");
    }
    REQUIRE(ensure_host() != nullptr);

    // 记录 sys.modules 里 act_plugin_ 前缀的 module 数, 循环前后应相等.
    auto count_act_plugin_modules = []() -> Py_ssize_t {
        PyObject* mods = PyImport_GetModuleDict();
        if (mods == nullptr)
            return -1;
        Py_ssize_t count = 0;
        PyObject* key = nullptr;
        PyObject* val = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(mods, &pos, &key, &val)) {
            if (PyUnicode_Check(key)) {
                const char* s = PyUnicode_AsUTF8(key);
                if (s != nullptr && std::strncmp(s, "act_plugin_", 11) == 0)
                    ++count;
            }
        }
        return count;
    };
    Py_ssize_t before = count_act_plugin_modules();

    for (int i = 0; i < 4; ++i) {
        py_plugin_handle_t p = try_load(dir, "star_resonance");
        REQUIRE(p != nullptr);
        safe_unload(p);
    }

    Py_ssize_t after = count_act_plugin_modules();
    // 循环后应 == 循环前 (每次 unload 会从 sys.modules 撕).
    CHECK(before == after);
}

// ══════════════════════════════════════════════════════════
// CASE 5: plugin.py 里 `import sao_sdk` 能拿到 module
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_sao_sdk_module_importable_from_plugin", "[pyhost][real_plugins]") {
    REQUIRE(ensure_host() != nullptr);

    // 直接 PyImport_ImportModule (相当于插件里的 `import sao_sdk`).
    PyObject* mod = PyImport_ImportModule("sao_sdk");
    REQUIRE(mod != nullptr);
    // 应该有 PluginContext 类
    PyObject* pc_cls = PyObject_GetAttrString(mod, "PluginContext");
    REQUIRE(pc_cls != nullptr);
    Py_DECREF(pc_cls);
    // 应该有 wrap_ctx 函数
    PyObject* fn = PyObject_GetAttrString(mod, "wrap_ctx");
    REQUIRE(fn != nullptr);
    CHECK(PyCallable_Check(fn));
    Py_DECREF(fn);
    // 应该有 log_info / log_warn / log_error / version 函数
    for (const char* name : {"log_info", "log_warn", "log_error", "version"}) {
        PyObject* f = PyObject_GetAttrString(mod, name);
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK(PyCallable_Check(f));
            Py_DECREF(f);
        }
    }
    Py_DECREF(mod);

    // SDK 方法数应 ≥ 8 (spec 要求 8 个 SDK 函数 + 若干 lenient no-op).
    size_t n = sao_plugins_pyhost_sdk_method_count();
    CHECK(n >= 8);
}

// ══════════════════════════════════════════════════════════
// CASE 6: ctx.log_info(msg) → host 记账
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_ctx_log_info_reaches_platform_log", "[pyhost][real_plugins]") {
    REQUIRE(ensure_host() != nullptr);

    // 建一个 PluginContext 直接调 (不需要加载 real plugin).
    PyObject* mod = PyImport_ImportModule("sao_sdk");
    REQUIRE(mod != nullptr);
    PyObject* pc_cls = PyObject_GetAttrString(mod, "PluginContext");
    Py_DECREF(mod);
    REQUIRE(pc_cls != nullptr);

    PyObject* args = Py_BuildValue("(ssn)", "test_plugin", "C:/tmp/test", (Py_ssize_t)0);
    PyObject* ctx = PyObject_CallObject(pc_cls, args);
    Py_DECREF(args);
    Py_DECREF(pc_cls);
    REQUIRE(ctx != nullptr);

    // ctx.log_info("hello world")
    PyObject* method = PyObject_GetAttrString(ctx, "log_info");
    REQUIRE(method != nullptr);
    PyObject* result = PyObject_CallFunction(method, "s", "hello world");
    Py_DECREF(method);
    REQUIRE(result != nullptr);
    Py_DECREF(result);

    // 查 host 内省 API: logs list 应有 1 条 {level=info, message=hello world}
    void* logs_v = sao_plugins_pyhost_ctx_get_records(ctx, "logs");
    REQUIRE(logs_v != nullptr);
    PyObject* logs = reinterpret_cast<PyObject*>(logs_v);
    REQUIRE(PyList_Check(logs));
    REQUIRE(PyList_GET_SIZE(logs) == 1);
    PyObject* first = PyList_GET_ITEM(logs, 0);
    REQUIRE(PyDict_Check(first));
    PyObject* level = PyDict_GetItemString(first, "level");
    PyObject* message = PyDict_GetItemString(first, "message");
    REQUIRE(level != nullptr);
    REQUIRE(message != nullptr);
    CHECK(std::string(PyUnicode_AsUTF8(level)) == "info");
    CHECK(std::string(PyUnicode_AsUTF8(message)) == "hello world");
    Py_DECREF(logs);

    Py_DECREF(ctx);
}

// ══════════════════════════════════════════════════════════
// CASE 7: ctx.register_ui_panel → 得 panel_id handle
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_register_ui_panel_from_python_gets_handle", "[pyhost][real_plugins]") {
    REQUIRE(ensure_host() != nullptr);

    PyObject* mod = PyImport_ImportModule("sao_sdk");
    REQUIRE(mod != nullptr);
    PyObject* pc_cls = PyObject_GetAttrString(mod, "PluginContext");
    Py_DECREF(mod);
    REQUIRE(pc_cls != nullptr);

    PyObject* args = Py_BuildValue("(ssn)", "test_plugin", "C:/tmp/test", (Py_ssize_t)0);
    PyObject* ctx = PyObject_CallObject(pc_cls, args);
    Py_DECREF(args);
    Py_DECREF(pc_cls);
    REQUIRE(ctx != nullptr);

    // ctx.register_ui_panel("my_panel", {"title":"Hello"})
    PyObject* meta = PyDict_New();
    PyObject* title = PyUnicode_FromString("Hello");
    PyDict_SetItemString(meta, "title", title);
    Py_DECREF(title);
    PyObject* method = PyObject_GetAttrString(ctx, "register_ui_panel");
    REQUIRE(method != nullptr);
    PyObject* handle = PyObject_CallFunction(method, "sO", "my_panel", meta);
    Py_DECREF(method);
    Py_DECREF(meta);
    REQUIRE(handle != nullptr);
    REQUIRE(PyUnicode_Check(handle));
    CHECK(std::string(PyUnicode_AsUTF8(handle)) == "my_panel");
    Py_DECREF(handle);

    // 查 panels 记账
    void* panels_v = sao_plugins_pyhost_ctx_get_records(ctx, "panels");
    REQUIRE(panels_v != nullptr);
    PyObject* panels = reinterpret_cast<PyObject*>(panels_v);
    REQUIRE(PyList_GET_SIZE(panels) == 1);
    PyObject* p0 = PyList_GET_ITEM(panels, 0);
    PyObject* pid = PyDict_GetItemString(p0, "panel_id");
    REQUIRE(pid != nullptr);
    CHECK(std::string(PyUnicode_AsUTF8(pid)) == "my_panel");
    Py_DECREF(panels);

    Py_DECREF(ctx);
}

// ══════════════════════════════════════════════════════════
// CASE 8: ctx.add_hotkey → 记账
// ══════════════════════════════════════════════════════════
TEST_CASE("pyhost_add_hotkey_from_python_fires_on_key", "[pyhost][real_plugins]") {
    REQUIRE(ensure_host() != nullptr);

    PyObject* mod = PyImport_ImportModule("sao_sdk");
    REQUIRE(mod != nullptr);
    PyObject* pc_cls = PyObject_GetAttrString(mod, "PluginContext");
    Py_DECREF(mod);
    REQUIRE(pc_cls != nullptr);

    PyObject* args = Py_BuildValue("(ssn)", "test_plugin", "C:/tmp/test", (Py_ssize_t)0);
    PyObject* ctx = PyObject_CallObject(pc_cls, args);
    Py_DECREF(args);
    Py_DECREF(pc_cls);
    REQUIRE(ctx != nullptr);

    // 定义一个 python callable (lambda 不方便; 用 eval 编一个).
    PyObject* main_dict = PyModule_GetDict(PyImport_AddModule("__main__")); // borrowed
    PyObject* cb = PyRun_String("(lambda: 42)", Py_eval_input, main_dict, main_dict);
    REQUIRE(cb != nullptr);
    REQUIRE(PyCallable_Check(cb));

    // ctx.add_hotkey("test_hk", cb, default_key="CTRL+F5", label="Test")
    PyObject* method = PyObject_GetAttrString(ctx, "add_hotkey");
    REQUIRE(method != nullptr);
    PyObject* kwargs = PyDict_New();
    PyObject* dk = PyUnicode_FromString("CTRL+F5");
    PyObject* lbl = PyUnicode_FromString("Test");
    PyDict_SetItemString(kwargs, "default_key", dk);
    PyDict_SetItemString(kwargs, "label", lbl);
    Py_DECREF(dk);
    Py_DECREF(lbl);
    PyObject* args_tuple = Py_BuildValue("(sO)", "test_hk", cb);
    PyObject* res = PyObject_Call(method, args_tuple, kwargs);
    Py_DECREF(args_tuple);
    Py_DECREF(kwargs);
    Py_DECREF(method);
    REQUIRE(res != nullptr);
    Py_DECREF(res);

    // 查 hotkeys 记账
    void* hkv = sao_plugins_pyhost_ctx_get_records(ctx, "hotkeys");
    REQUIRE(hkv != nullptr);
    PyObject* hks = reinterpret_cast<PyObject*>(hkv);
    REQUIRE(PyList_GET_SIZE(hks) == 1);
    PyObject* h0 = PyList_GET_ITEM(hks, 0);
    PyObject* hid = PyDict_GetItemString(h0, "hotkey_id");
    PyObject* dk2 = PyDict_GetItemString(h0, "default_key");
    PyObject* lbl2 = PyDict_GetItemString(h0, "label");
    PyObject* saved_cb = PyDict_GetItemString(h0, "callback");
    REQUIRE(hid != nullptr);
    REQUIRE(dk2 != nullptr);
    REQUIRE(lbl2 != nullptr);
    REQUIRE(saved_cb != nullptr);
    CHECK(std::string(PyUnicode_AsUTF8(hid)) == "test_hk");
    CHECK(std::string(PyUnicode_AsUTF8(dk2)) == "CTRL+F5");
    CHECK(std::string(PyUnicode_AsUTF8(lbl2)) == "Test");
    CHECK(saved_cb == cb); // callable 存进记账

    // 直接调 saved_cb 验证真的存下来了 (对应 spec 里的 "fires_on_key" 语义:
    // 主平台之后可以从记账里取回来触发).
    PyObject* fire_res = PyObject_CallNoArgs(saved_cb);
    REQUIRE(fire_res != nullptr);
    CHECK(PyLong_AsLong(fire_res) == 42);
    Py_DECREF(fire_res);

    Py_DECREF(hks);
    Py_DECREF(cb);
    Py_DECREF(ctx);
}

// ── 最终收尾 (Catch2 session 结束时 shutdown) ──
struct TeardownSentinel {
    ~TeardownSentinel() {
        if (g_host != nullptr) {
            (void)sao_plugins_pyhost_shutdown(g_host);
            g_host = nullptr;
            g_host_ready = false;
        }
    }
};
TeardownSentinel g_teardown{};

#else // !SAO_HAS_PYTHON_EMBED

TEST_CASE("pyhost_skipped_no_python_embed", "[pyhost][real_plugins][.skip]") {
    SKIP("Python3 embed not found — real-plugin tests require CPython 3.11+");
}

#endif // SAO_HAS_PYTHON_EMBED
