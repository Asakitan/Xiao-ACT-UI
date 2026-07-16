// py_module_bridge.cpp — Wave 7 真实装 sao_sdk 内置模块
//
// 老 Python 插件 (star_resonance / hide_seek / midi_piano) 里 on_load(ctx) 拿到
// 一个 ctx 对象。C++ 平台侧把它包成一个 PyObject (PluginContext), 方法名/签名
// 全部对齐旧 Python PluginContext。这样老插件**一个字不用改**就能被本 host 加载。
//
// 已知实装范围 (Wave 7):
//   - sao_sdk PyModuleDef + PyInit_sao_sdk
//   - PluginContextType (PyTypeObject) 覆盖 8 组核心方法:
//     * register_ui_panel(panel_id, meta[, render][, on_action])
//     * add_hotkey / register_hotkey(hotkey_id, callback[, default_key][, label])
//     * subscribe(event_type, callback)
//     * publish / emit(event_type, data)
//     * log_info(msg) / log_warn(msg) / log_error(msg) / log(msg)
//     * get_plugin_id() / plugin_id 属性
//     * get_base_dir() / path / assets_path 属性
//     * 若干 lenient no-op 方法 (set_interval / notify / request_redraw / ...)
//   - 记录所有注册到 PluginContext state, 供 host / test 内省
//
// 关键设计:
//   本模块把 PluginContext 实现为一个"记账 shim"—— 老插件调 ctx.register_ui_panel
//   会把参数记进本 PyObject 里的 dict, host 侧可以通过 sao_plugins_pyhost_snapshot_ctx
//   查这些记账数据来验证。真正把面板/热键落地到平台 UI 是另一层的事 (Wave 8+),
//   本 Wave 只保证"加载不 crash + API 表面完整 + 记账正确"。
//
// PluginContext 拥有 self.__dict__ (通过 PyTypeObject tp_dict + tp_getattro), 老插件
// 可以自由塞属性 (ctx.engine.owner 之类) —— 那些属性访问在 sub-attr 层面抛
// AttributeError, 由插件自己 try/except 兜底 (老 hide_seek 就这么写的)。

#include "sao/plugins/python_host/py_module_bridge.h"
#include "sao/sdk/sao_sdk.h"

#if defined(SAO_HAS_PYTHON_EMBED)
#  define PY_SSIZE_T_CLEAN
#  include "sao/plugins/python_host/py_release_abi.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace sao::plugins::python_host {

#if defined(SAO_HAS_PYTHON_EMBED)

namespace {

// ── PluginContext PyObject 状态 ─────────────────────────────────
//
// 每个插件 on_load(ctx) 收到的都是一个新的 PluginContext 实例, 内部持:
//   - opaque_handle: 主平台 C ABI plugin_context_t* (可空)
//   - plugin_id / base_dir / assets_path 字符串
//   - panels / hotkeys / subscriptions / published / logs 记账 list
//   - engine: 一个空 dict (占位, 让 ctx.engine 不 AttributeError)
//   - ui: 一个空 dict
//   - mem: 一个空 dict
//   - owner: None (老 hide_seek 会 getattr 拿 owner, None 也不炸)
//
// PyGetSetDef 暴露只读属性, PyMethodDef 暴露方法, 老插件的
// `ctx.register_ui_panel(...)`, `ctx.log(...)` 全部落到本表。

struct PluginContextObject {
    PyObject_HEAD
    PyObject* weakreflist;
    void* opaque_handle;         // plugin_context_t* (可空, 记账用)
    PyObject* plugin_id;          // str
    PyObject* base_dir;           // str
    PyObject* assets_path;        // str
    PyObject* web_path;           // str
    PyObject* panels;             // list[dict]
    PyObject* hotkeys;            // list[dict]
    PyObject* subscriptions;      // list[dict]
    PyObject* published;          // list[dict]  (emit / publish 记账)
    PyObject* logs;               // list[dict]  (log_info/warn/error 记账)
    PyObject* timers;             // list[dict]  (set_interval/timeout)
    PyObject* notifications;      // list[dict]  (notify 记账)
    PyObject* settings;           // dict[str, obj]
    PyObject* menus;              // list[dict]  (legacy menu categories)
    PyObject* extras;             // dict (插件塞的额外属性; ctx.engine.owner 等)
    // 记录 Python 侧引用的 callback (subscribe / hotkey / render / on_action),
    // 让 unload 时统一 DECREF。
    PyObject* callback_refs;      // list[obj]
    struct NativePanelBridge* native_panel_bridges = nullptr;
    struct NativeHotkeyBridge* native_hotkey_bridges = nullptr;
    struct NativeEventBridge* native_event_bridges = nullptr;
    bool controlled_test_shim = false;
};

struct NativePanelBridge {
    PyObject* callback = nullptr;
    NativePanelBridge* next = nullptr;
};

struct NativeHotkeyBridge {
    PyObject* callback = nullptr;
    NativeHotkeyBridge* next = nullptr;
};

struct NativeEventBridge {
    PyObject* callback = nullptr;
    NativeEventBridge* next = nullptr;
};

// forward decl
extern PyTypeObject PluginContextType;

// ── 内部小工具 ───────────────────────────────────────────────

std::mutex g_mod_mu;
size_t g_sdk_method_count = 0;  // 供 sao_plugins_pyhost_sdk_method_count 查

// 追加一个"记账" dict 到 list 里, 键值对由 (key, PyObject*) 组成, 参数以变长
// 方式给, 结束用 nullptr sentinel。函数吸取 PyObject 引用 (steals)。
PyObject* append_record(PyObject* list, ...) {
    if (list == nullptr) Py_RETURN_NONE;
    PyObject* d = PyDict_New();
    if (d == nullptr) return nullptr;

    va_list ap;
    va_start(ap, list);
    for (;;) {
        const char* key = va_arg(ap, const char*);
        if (key == nullptr) break;
        PyObject* val = va_arg(ap, PyObject*);
        if (val == nullptr) val = Py_NewRef(Py_None);
        if (PyDict_SetItemString(d, key, val) != 0) {
            Py_DECREF(val);
            Py_DECREF(d);
            va_end(ap);
            return nullptr;
        }
        Py_DECREF(val);
    }
    va_end(ap);

    if (PyList_Append(list, d) != 0) {
        Py_DECREF(d);
        return nullptr;
    }
    Py_DECREF(d);
    Py_RETURN_NONE;
}

void keep_callback_ref(PluginContextObject* self, PyObject* cb) {
    if (self == nullptr || cb == nullptr || cb == Py_None) return;
    if (self->callback_refs == nullptr) return;
    (void)PyList_Append(self->callback_refs, cb);
}

SaoSdkContext* native_context(PluginContextObject* self) {
    if (self == nullptr || self->opaque_handle == nullptr) return nullptr;
    auto* ctx = static_cast<SaoSdkContext*>(self->opaque_handle);
    if (ctx->abi_version != SAO_SDK_ABI_VERSION || ctx->ctx_impl == nullptr) {
        return nullptr;
    }
    return ctx;
}

void release_native_bridges(PluginContextObject* self) {
    while (self != nullptr && self->native_panel_bridges != nullptr) {
        NativePanelBridge* bridge = self->native_panel_bridges;
        self->native_panel_bridges = bridge->next;
        Py_XDECREF(bridge->callback);
        delete bridge;
    }
    while (self != nullptr && self->native_hotkey_bridges != nullptr) {
        NativeHotkeyBridge* bridge = self->native_hotkey_bridges;
        self->native_hotkey_bridges = bridge->next;
        Py_XDECREF(bridge->callback);
        delete bridge;
    }
    while (self != nullptr && self->native_event_bridges != nullptr) {
        NativeEventBridge* bridge = self->native_event_bridges;
        self->native_event_bridges = bridge->next;
        Py_XDECREF(bridge->callback);
        delete bridge;
    }
}

void clear_callback_ledgers(PluginContextObject* self) {
    if (self == nullptr) return;
    for (PyObject* records : {self->panels, self->hotkeys, self->subscriptions,
                              self->timers, self->menus, self->callback_refs}) {
        if (records != nullptr && PyList_Check(records)) {
            (void)PyList_SetSlice(records, 0, PyList_GET_SIZE(records), nullptr);
        }
    }
}

void SAO_SDK_CALL native_panel_action(const char* action_key_utf8,
                                      const uint8_t*, size_t,
                                      void* user_data) {
    auto* bridge = static_cast<NativePanelBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallFunction(bridge->callback, "sO",
                                             action_key_utf8 == nullptr ? "" : action_key_utf8,
                                             Py_None);
    Py_XDECREF(result);
    PyErr_Clear();
    PyGILState_Release(gil);
}

void SAO_SDK_CALL native_hotkey_action(sao_sdk_hotkey_id_t, void* user_data) {
    auto* bridge = static_cast<NativeHotkeyBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallNoArgs(bridge->callback);
    Py_XDECREF(result);
    PyErr_Clear();
    PyGILState_Release(gil);
}

void SAO_SDK_CALL native_event_action(const char* topic_utf8,
                                      const uint8_t* payload, size_t payload_len,
                                      void* user_data) {
    auto* bridge = static_cast<NativeEventBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* body = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(payload), static_cast<Py_ssize_t>(payload_len));
    PyObject* result = body == nullptr ? nullptr : PyObject_CallFunction(
        bridge->callback, "sO", topic_utf8 == nullptr ? "" : topic_utf8, body);
    Py_XDECREF(result);
    Py_XDECREF(body);
    PyErr_Clear();
    PyGILState_Release(gil);
}

bool parse_hotkey(const char* text_utf8, uint32_t* out_key, uint32_t* out_modifiers) {
    if (out_key == nullptr || out_modifiers == nullptr || text_utf8 == nullptr) return false;
    *out_key = 0;
    *out_modifiers = 0;
    std::string token;
    for (const char* p = text_utf8;; ++p) {
        const char c = *p;
        if (c != '+' && c != '\0') {
            token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            continue;
        }
        if (token == "CTRL" || token == "CONTROL") *out_modifiers |= 1u << 0;
        else if (token == "ALT") *out_modifiers |= 1u << 1;
        else if (token == "SHIFT") *out_modifiers |= 1u << 2;
        else if (token == "WIN" || token == "WINDOWS") *out_modifiers |= 1u << 3;
        else if (token.size() >= 2 && token[0] == 'F') {
            const int number = std::atoi(token.c_str() + 1);
            if (number >= 1 && number <= 24) *out_key = 0x70u + static_cast<uint32_t>(number - 1);
        } else if (token.size() == 1 && std::isalnum(static_cast<unsigned char>(token[0]))) {
            *out_key = static_cast<uint32_t>(token[0]);
        }
        token.clear();
        if (c == '\0') break;
    }
    return *out_key != 0;
}

PyObject* json_stringify(PyObject* value) {
    PyObject* json_module = PyImport_ImportModule("json");
    if (json_module == nullptr) return nullptr;
    PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
    Py_DECREF(json_module);
    if (dumps == nullptr) return nullptr;
    PyObject* result = PyObject_CallOneArg(dumps, value);
    Py_DECREF(dumps);
    return result;
}

// ── PluginContext 生命周期 ──────────────────────────────────

PyObject* PluginContext_new(PyTypeObject* type, PyObject* /*args*/, PyObject* /*kwds*/) {
    PluginContextObject* self = reinterpret_cast<PluginContextObject*>(
        type->tp_alloc(type, 0));
    if (self == nullptr) return nullptr;
    self->weakreflist = nullptr;
    self->opaque_handle = nullptr;
    self->plugin_id = nullptr;
    self->base_dir = nullptr;
    self->assets_path = nullptr;
    self->web_path = nullptr;
    self->panels = nullptr;
    self->hotkeys = nullptr;
    self->subscriptions = nullptr;
    self->published = nullptr;
    self->logs = nullptr;
    self->timers = nullptr;
    self->notifications = nullptr;
    self->settings = nullptr;
    self->menus = nullptr;
    self->extras = nullptr;
    self->callback_refs = nullptr;
    self->plugin_id = PyUnicode_FromString("");
    self->base_dir = PyUnicode_FromString("");
    self->assets_path = PyUnicode_FromString("");
    self->web_path = PyUnicode_FromString("");
    self->panels = PyList_New(0);
    self->hotkeys = PyList_New(0);
    self->subscriptions = PyList_New(0);
    self->published = PyList_New(0);
    self->logs = PyList_New(0);
    self->timers = PyList_New(0);
    self->notifications = PyList_New(0);
    self->settings = PyDict_New();
    self->menus = PyList_New(0);
    self->extras = PyDict_New();
    self->callback_refs = PyList_New(0);
    self->native_panel_bridges = nullptr;
    self->native_hotkey_bridges = nullptr;
    self->native_event_bridges = nullptr;
    if (self->plugin_id == nullptr || self->base_dir == nullptr ||
        self->assets_path == nullptr || self->web_path == nullptr ||
        self->panels == nullptr || self->hotkeys == nullptr ||
        self->subscriptions == nullptr || self->published == nullptr ||
        self->logs == nullptr || self->timers == nullptr ||
        self->notifications == nullptr || self->settings == nullptr ||
        self->menus == nullptr ||
        self->extras == nullptr || self->callback_refs == nullptr) {
        Py_DECREF(self);
        return nullptr;
    }
    return reinterpret_cast<PyObject*>(self);
}

int PluginContext_init(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {
        "plugin_id", "base_dir", "opaque_handle", "controlled_test_shim", nullptr};
    const char* pid = "";
    const char* dir_ = "";
    Py_ssize_t handle_int = 0;
    int controlled_test_shim = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssnp",
                                      const_cast<char**>(kwlist),
                                      &pid, &dir_, &handle_int, &controlled_test_shim)) {
        return -1;
    }
    Py_XDECREF(self->plugin_id);
    self->plugin_id = PyUnicode_FromString(pid);
    Py_XDECREF(self->base_dir);
    self->base_dir = PyUnicode_FromString(dir_);
    // assets_path / web_path 默认拼在 base_dir 下
    std::string dir_s = dir_;
    if (!dir_s.empty() && dir_s.back() != '/' && dir_s.back() != '\\') {
        dir_s.push_back('/');
    }
    Py_XDECREF(self->assets_path);
    self->assets_path = PyUnicode_FromString((dir_s + "assets").c_str());
    Py_XDECREF(self->web_path);
    self->web_path = PyUnicode_FromString((dir_s + "web").c_str());
    self->opaque_handle = reinterpret_cast<void*>(static_cast<intptr_t>(handle_int));
    self->controlled_test_shim = controlled_test_shim != 0;
    return 0;
}

int PluginContext_traverse(PluginContextObject* self, visitproc visit, void* arg) {
    Py_VISIT(self->plugin_id);
    Py_VISIT(self->base_dir);
    Py_VISIT(self->assets_path);
    Py_VISIT(self->web_path);
    Py_VISIT(self->panels);
    Py_VISIT(self->hotkeys);
    Py_VISIT(self->subscriptions);
    Py_VISIT(self->published);
    Py_VISIT(self->logs);
    Py_VISIT(self->timers);
    Py_VISIT(self->notifications);
    Py_VISIT(self->settings);
    Py_VISIT(self->menus);
    Py_VISIT(self->extras);
    Py_VISIT(self->callback_refs);
    return 0;
}

int PluginContext_clear(PluginContextObject* self) {
    release_native_bridges(self);
    clear_callback_ledgers(self);
    Py_CLEAR(self->plugin_id);
    Py_CLEAR(self->base_dir);
    Py_CLEAR(self->assets_path);
    Py_CLEAR(self->web_path);
    Py_CLEAR(self->panels);
    Py_CLEAR(self->hotkeys);
    Py_CLEAR(self->subscriptions);
    Py_CLEAR(self->published);
    Py_CLEAR(self->logs);
    Py_CLEAR(self->timers);
    Py_CLEAR(self->notifications);
    Py_CLEAR(self->settings);
    Py_CLEAR(self->menus);
    Py_CLEAR(self->extras);
    Py_CLEAR(self->callback_refs);
    return 0;
}

void PluginContext_dealloc(PluginContextObject* self) {
    if (PyObject_GC_IsTracked(reinterpret_cast<PyObject*>(self))) {
        PyObject_GC_UnTrack(self);
    }
    if (self->weakreflist != nullptr) {
        PyObject_ClearWeakRefs(reinterpret_cast<PyObject*>(self));
    }
    (void)PluginContext_clear(self);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ── PyGetSetDef 属性 ────────────────────────────────────────

PyObject* PluginContext_get_plugin_id_attr(PluginContextObject* self, void*) {
    return Py_NewRef(self->plugin_id);
}
PyObject* PluginContext_get_base_dir_attr(PluginContextObject* self, void*) {
    return Py_NewRef(self->base_dir);
}
PyObject* PluginContext_get_path_attr(PluginContextObject* self, void*) {
    return Py_NewRef(self->base_dir);
}
PyObject* PluginContext_get_assets_path_attr(PluginContextObject* self, void*) {
    return Py_NewRef(self->assets_path);
}
PyObject* PluginContext_get_web_path_attr(PluginContextObject* self, void*) {
    return Py_NewRef(self->web_path);
}
PyObject* PluginContext_get_should_stop_attr(PluginContextObject* /*self*/, void*) {
    Py_RETURN_FALSE;
}
// ctx.engine / ctx.ui / ctx.mem / ctx.owner —— 老插件访问用, 返 dict 占位
PyObject* PluginContext_get_engine_attr(PluginContextObject* self, void*) {
    PyObject* v = PyDict_GetItemString(self->extras, "engine");
    if (v != nullptr) return Py_NewRef(v);
    PyObject* types = PyImport_ImportModule("types");
    if (types == nullptr) return nullptr;
    PyObject* namespace_type = PyObject_GetAttrString(types, "SimpleNamespace");
    Py_DECREF(types);
    if (namespace_type == nullptr) return nullptr;
    PyObject* engine = PyObject_CallNoArgs(namespace_type);
    Py_DECREF(namespace_type);
    if (engine == nullptr) return nullptr;
    if (PyObject_SetAttrString(engine, "owner", Py_None) != 0 ||
        PyDict_SetItemString(self->extras, "engine", engine) != 0) {
        Py_DECREF(engine);
        return nullptr;
    }
    return engine;
}
PyObject* PluginContext_get_ui_attr(PluginContextObject* self, void*) {
    PyObject* v = PyDict_GetItemString(self->extras, "ui");
    if (v != nullptr) return Py_NewRef(v);
    PyObject* d = PyDict_New();
    if (d == nullptr) return nullptr;
    if (PyDict_SetItemString(self->extras, "ui", d) != 0) {
        Py_DECREF(d);
        return nullptr;
    }
    return d;
}
PyObject* PluginContext_get_mem_attr(PluginContextObject* self, void*) {
    PyObject* v = PyDict_GetItemString(self->extras, "mem");
    if (v != nullptr) return Py_NewRef(v);
    PyObject* d = PyDict_New();
    if (d == nullptr) return nullptr;
    if (PyDict_SetItemString(self->extras, "mem", d) != 0) {
        Py_DECREF(d);
        return nullptr;
    }
    return d;
}
PyObject* PluginContext_get_owner_attr(PluginContextObject* /*self*/, void*) {
    // 老 hide_seek 用 getattr(ctx.owner, "_register_persistent_alert_kind", None)
    // 拿; 我们返 None 让 getattr 兜底走 default 分支.
    Py_RETURN_NONE;
}

PyGetSetDef PluginContext_getset[] = {
    {"plugin_id", reinterpret_cast<getter>(PluginContext_get_plugin_id_attr), nullptr, nullptr, nullptr},
    {"base_dir", reinterpret_cast<getter>(PluginContext_get_base_dir_attr), nullptr, nullptr, nullptr},
    {"path", reinterpret_cast<getter>(PluginContext_get_path_attr), nullptr, nullptr, nullptr},
    {"assets_path", reinterpret_cast<getter>(PluginContext_get_assets_path_attr), nullptr, nullptr, nullptr},
    {"web_path", reinterpret_cast<getter>(PluginContext_get_web_path_attr), nullptr, nullptr, nullptr},
    {"should_stop", reinterpret_cast<getter>(PluginContext_get_should_stop_attr), nullptr, nullptr, nullptr},
    {"engine", reinterpret_cast<getter>(PluginContext_get_engine_attr), nullptr, nullptr, nullptr},
    {"ui", reinterpret_cast<getter>(PluginContext_get_ui_attr), nullptr, nullptr, nullptr},
    {"mem", reinterpret_cast<getter>(PluginContext_get_mem_attr), nullptr, nullptr, nullptr},
    {"owner", reinterpret_cast<getter>(PluginContext_get_owner_attr), nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

// ── Method 实现 ─────────────────────────────────────────────

// register_ui_panel(panel_id, meta[, render][, on_action]) — 记账
PyObject* PluginContext_register_ui_panel(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"panel_id", "meta", "render", "on_action", nullptr};
    const char* panel_id = nullptr;
    PyObject* meta = nullptr;
    PyObject* render = Py_None;
    PyObject* on_action = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|OO",
                                      const_cast<char**>(kwlist),
                                      &panel_id, &meta, &render, &on_action)) {
        return nullptr;
    }
    keep_callback_ref(self, render);
    keep_callback_ref(self, on_action);

    SaoSdkContext* ctx = native_context(self);
    sao_sdk_ui_panel_t native_panel = nullptr;
    if (ctx != nullptr) {
        PyObject* json = json_stringify(meta);
        if (json == nullptr) return nullptr;
        const char* json_utf8 = PyUnicode_AsUTF8(json);
        if (json_utf8 == nullptr) {
            Py_DECREF(json);
            return nullptr;
        }
        NativePanelBridge* bridge = nullptr;
        if (on_action != Py_None && PyCallable_Check(on_action)) {
            bridge = new (std::nothrow) NativePanelBridge{};
            if (bridge == nullptr) {
                Py_DECREF(json);
                return PyErr_NoMemory();
            }
            bridge->callback = Py_NewRef(on_action);
        }
        const sao_sdk_status_t rc = sao_sdk_ui_register_panel(
            ctx, panel_id, panel_id, reinterpret_cast<const uint8_t*>(json_utf8),
            std::strlen(json_utf8), bridge == nullptr ? nullptr : native_panel_action,
            bridge, &native_panel);
        Py_DECREF(json);
        if (rc != SAO_SDK_OK) {
            if (bridge != nullptr) {
                Py_DECREF(bridge->callback);
                delete bridge;
            }
            PyErr_Format(PyExc_RuntimeError, "native panel registration failed: %d", rc);
            return nullptr;
        }
        if (bridge != nullptr) {
            bridge->next = self->native_panel_bridges;
            self->native_panel_bridges = bridge;
        }
    }

    PyObject* rec = PyDict_New();
    if (rec == nullptr) return nullptr;
    PyDict_SetItemString(rec, "panel_id", PyUnicode_FromString(panel_id));
    PyDict_SetItemString(rec, "meta", Py_NewRef(meta));
    PyDict_SetItemString(rec, "render", Py_NewRef(render));
    PyDict_SetItemString(rec, "on_action", Py_NewRef(on_action));
    PyDict_SetItemString(rec, "native_handle",
                         PyLong_FromVoidPtr(native_panel));
    PyList_Append(self->panels, rec);
    Py_DECREF(rec);
    // 返回一个 handle (str) 让插件可选保存
    return PyUnicode_FromString(panel_id);
}

// add_hotkey / register_hotkey(hotkey_id, callback[, default_key][, label])
PyObject* PluginContext_add_hotkey(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"hotkey_id", "callback", "default_key", "label", nullptr};
    const char* hotkey_id = nullptr;
    PyObject* callback = nullptr;
    const char* default_key = "";
    const char* label = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|ss",
                                      const_cast<char**>(kwlist),
                                      &hotkey_id, &callback, &default_key, &label)) {
        return nullptr;
    }
    keep_callback_ref(self, callback);

    SaoSdkContext* ctx = native_context(self);
    sao_sdk_hotkey_id_t native_hotkey = 0;
    if (ctx != nullptr) {
        uint32_t virtual_key = 0;
        uint32_t modifiers = 0;
        if (!parse_hotkey(default_key, &virtual_key, &modifiers)) {
            PyErr_SetString(PyExc_ValueError, "default_key must be a supported key chord");
            return nullptr;
        }
        auto* bridge = new (std::nothrow) NativeHotkeyBridge{};
        if (bridge == nullptr) return PyErr_NoMemory();
        bridge->callback = Py_NewRef(callback);
        SaoSdkHotkeySpec spec{};
        spec.binding_id_utf8 = hotkey_id;
        spec.virtual_key = virtual_key;
        spec.modifiers = modifiers;
        spec.enforce_ctrl_prefix = (modifiers & (1u << 0)) != 0;
        const sao_sdk_status_t rc = sao_sdk_register_hotkey(
            ctx, &spec, native_hotkey_action, bridge, &native_hotkey);
        if (rc != SAO_SDK_OK) {
            Py_DECREF(bridge->callback);
            delete bridge;
            PyErr_Format(PyExc_RuntimeError, "native hotkey registration failed: %d", rc);
            return nullptr;
        }
        bridge->next = self->native_hotkey_bridges;
        self->native_hotkey_bridges = bridge;
    }
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "hotkey_id", PyUnicode_FromString(hotkey_id));
    PyDict_SetItemString(rec, "default_key", PyUnicode_FromString(default_key));
    PyDict_SetItemString(rec, "label", PyUnicode_FromString(label));
    PyDict_SetItemString(rec, "callback", Py_NewRef(callback));
    PyDict_SetItemString(rec, "native_handle", PyLong_FromUnsignedLongLong(native_hotkey));
    PyList_Append(self->hotkeys, rec);
    Py_DECREF(rec);
    Py_RETURN_NONE;
}

// subscribe(event_type, callback)
PyObject* PluginContext_subscribe(PluginContextObject* self, PyObject* args) {
    const char* event_type = nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &event_type, &callback)) return nullptr;
    keep_callback_ref(self, callback);
    SaoSdkContext* ctx = native_context(self);
    sao_sdk_subscription_t native_subscription = 0;
    if (ctx != nullptr) {
        auto* bridge = new (std::nothrow) NativeEventBridge{};
        if (bridge == nullptr) return PyErr_NoMemory();
        bridge->callback = Py_NewRef(callback);
        const sao_sdk_status_t rc = sao_sdk_subscribe_event(
            ctx, event_type, native_event_action, bridge, &native_subscription);
        if (rc != SAO_SDK_OK) {
            Py_DECREF(bridge->callback);
            delete bridge;
            PyErr_Format(PyExc_RuntimeError, "native event subscription failed: %d", rc);
            return nullptr;
        }
        bridge->next = self->native_event_bridges;
        self->native_event_bridges = bridge;
    }
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "event_type", PyUnicode_FromString(event_type));
    PyDict_SetItemString(rec, "callback", Py_NewRef(callback));
    PyDict_SetItemString(rec, "native_handle",
                         PyLong_FromUnsignedLongLong(native_subscription));
    PyList_Append(self->subscriptions, rec);
    Py_DECREF(rec);
    // token = index
    return PyLong_FromSsize_t(PyList_GET_SIZE(self->subscriptions) - 1);
}

// publish(event_type[, data]) / emit alias
PyObject* PluginContext_publish(PluginContextObject* self, PyObject* args) {
    const char* event_type = nullptr;
    PyObject* data = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &event_type, &data)) return nullptr;
    SaoSdkContext* ctx = native_context(self);
    if (ctx != nullptr) {
        PyObject* json = json_stringify(data);
        if (json == nullptr) return nullptr;
        const char* json_utf8 = PyUnicode_AsUTF8(json);
        if (json_utf8 == nullptr) {
            Py_DECREF(json);
            return nullptr;
        }
        const sao_sdk_status_t rc = sao_sdk_publish_event(
            ctx, event_type, reinterpret_cast<const uint8_t*>(json_utf8),
            std::strlen(json_utf8));
        Py_DECREF(json);
        if (rc != SAO_SDK_OK) {
            PyErr_Format(PyExc_RuntimeError, "native event publish failed: %d", rc);
            return nullptr;
        }
    }
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "event_type", PyUnicode_FromString(event_type));
    PyDict_SetItemString(rec, "data", Py_NewRef(data));
    PyList_Append(self->published, rec);
    Py_DECREF(rec);
    Py_RETURN_NONE;
}

// log_info / log_warn / log_error / log(msg[, level])
PyObject* PluginContext_log_impl(PluginContextObject* self, PyObject* args, const char* level) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "level", PyUnicode_FromString(level));
    PyDict_SetItemString(rec, "message", PyUnicode_FromString(msg));
    PyList_Append(self->logs, rec);
    Py_DECREF(rec);
    Py_RETURN_NONE;
}
PyObject* PluginContext_log_info(PluginContextObject* self, PyObject* args) {
    return PluginContext_log_impl(self, args, "info");
}
PyObject* PluginContext_log_warn(PluginContextObject* self, PyObject* args) {
    return PluginContext_log_impl(self, args, "warn");
}
PyObject* PluginContext_log_error(PluginContextObject* self, PyObject* args) {
    return PluginContext_log_impl(self, args, "error");
}
// log(msg[, level]) — 老插件常用
PyObject* PluginContext_log(PluginContextObject* self, PyObject* args) {
    const char* msg = nullptr;
    const char* level = "info";
    if (!PyArg_ParseTuple(args, "s|s", &msg, &level)) return nullptr;
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "level", PyUnicode_FromString(level));
    PyDict_SetItemString(rec, "message", PyUnicode_FromString(msg));
    PyList_Append(self->logs, rec);
    Py_DECREF(rec);
    Py_RETURN_NONE;
}

// get_plugin_id() / get_base_dir()
PyObject* PluginContext_get_plugin_id(PluginContextObject* self, PyObject* /*args*/) {
    return Py_NewRef(self->plugin_id);
}
PyObject* PluginContext_get_base_dir(PluginContextObject* self, PyObject* /*args*/) {
    return Py_NewRef(self->base_dir);
}

// ── 让老插件不 crash 的 lenient no-op 方法族 ──────────────────

PyObject* PluginContext_set_interval(PluginContextObject* self, PyObject* args) {
    PyObject* callback = nullptr;
    double seconds = 0.0;
    if (!PyArg_ParseTuple(args, "Od", &callback, &seconds)) return nullptr;
    keep_callback_ref(self, callback);
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "kind", PyUnicode_FromString("interval"));
    PyDict_SetItemString(rec, "seconds", PyFloat_FromDouble(seconds));
    PyDict_SetItemString(rec, "callback", Py_NewRef(callback));
    PyList_Append(self->timers, rec);
    Py_DECREF(rec);
    return PyUnicode_FromFormat("timer_%zd", PyList_GET_SIZE(self->timers) - 1);
}
PyObject* PluginContext_set_timeout(PluginContextObject* self, PyObject* args) {
    PyObject* callback = nullptr;
    double seconds = 0.0;
    if (!PyArg_ParseTuple(args, "Od", &callback, &seconds)) return nullptr;
    keep_callback_ref(self, callback);
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "kind", PyUnicode_FromString("timeout"));
    PyDict_SetItemString(rec, "seconds", PyFloat_FromDouble(seconds));
    PyDict_SetItemString(rec, "callback", Py_NewRef(callback));
    PyList_Append(self->timers, rec);
    Py_DECREF(rec);
    return PyUnicode_FromFormat("timer_%zd", PyList_GET_SIZE(self->timers) - 1);
}
PyObject* PluginContext_clear_timer(PluginContextObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}
PyObject* PluginContext_notify(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"title", "message", "duration_s", "kind", nullptr};
    const char* title = "";
    const char* message = "";
    double duration = 0.0;
    const char* kind = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssds",
                                      const_cast<char**>(kwlist),
                                      &title, &message, &duration, &kind)) {
        return nullptr;
    }
    SaoSdkContext* ctx = native_context(self);
    if (ctx != nullptr) {
        std::string text(title);
        if (!text.empty() && message[0] != '\0') text.append("\n");
        text.append(message);
        const uint32_t duration_ms = duration <= 0.0
            ? 0u
            : static_cast<uint32_t>(std::min(duration * 1000.0, 4294967295.0));
        const sao_sdk_status_t rc = sao_sdk_banner_show(ctx, text.c_str(), duration_ms, 0);
        if (rc != SAO_SDK_OK) {
            PyErr_Format(PyExc_RuntimeError, "native banner show failed: %d", rc);
            return nullptr;
        }
    }
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "title", PyUnicode_FromString(title));
    PyDict_SetItemString(rec, "message", PyUnicode_FromString(message));
    PyDict_SetItemString(rec, "duration_s", PyFloat_FromDouble(duration));
    PyDict_SetItemString(rec, "kind", PyUnicode_FromString(kind));
    PyList_Append(self->notifications, rec);
    Py_DECREF(rec);
    Py_RETURN_NONE;
}
PyObject* PluginContext_dismiss_notify(PluginContextObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}
PyObject* PluginContext_request_redraw_native(PluginContextObject* self, PyObject* args) {
    const char* panel_id = nullptr;
    if (!PyArg_ParseTuple(args, "|z", &panel_id)) return nullptr;
    SaoSdkContext* ctx = native_context(self);
    if (ctx != nullptr) {
        const sao_sdk_status_t rc = sao_sdk_ui_request_redraw(ctx, panel_id);
        if (rc != SAO_SDK_OK && rc != SAO_SDK_ERR_NOT_FOUND) {
            PyErr_Format(PyExc_RuntimeError, "native redraw failed: %d", rc);
            return nullptr;
        }
    }
    Py_RETURN_NONE;
}
PyObject* PluginContext_get_setting(PluginContextObject* self, PyObject* args) {
    const char* key = nullptr;
    PyObject* default_val = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &key, &default_val)) return nullptr;
    SaoSdkContext* ctx = native_context(self);
    if (ctx != nullptr) {
        if (PyBool_Check(default_val)) {
            bool value = false;
            const sao_sdk_status_t rc = sao_sdk_config_get_bool(ctx, key, &value);
            if (rc == SAO_SDK_OK) return PyBool_FromLong(value ? 1 : 0);
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        } else if (PyLong_Check(default_val)) {
            int64_t value = 0;
            const sao_sdk_status_t rc = sao_sdk_config_get_int(ctx, key, &value);
            if (rc == SAO_SDK_OK) return PyLong_FromLongLong(value);
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        } else if (PyFloat_Check(default_val)) {
            double value = 0.0;
            const sao_sdk_status_t rc = sao_sdk_config_get_double(ctx, key, &value);
            if (rc == SAO_SDK_OK) return PyFloat_FromDouble(value);
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        } else if (PyUnicode_Check(default_val)) {
            size_t required = 0;
            sao_sdk_status_t rc = sao_sdk_config_get_string(ctx, key, nullptr, 0, &required);
            if (rc == SAO_SDK_ERR_BUFFER_TOO_SMALL && required > 0) {
                std::vector<char> value(required);
                rc = sao_sdk_config_get_string(ctx, key, value.data(), value.size(), &required);
                if (rc == SAO_SDK_OK) return PyUnicode_FromString(value.data());
            }
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        }
    }
    PyObject* v = PyDict_GetItemString(self->settings, key);
    if (v != nullptr) return Py_NewRef(v);
    return Py_NewRef(default_val);
}
PyObject* PluginContext_set_setting(PluginContextObject* self, PyObject* args) {
    const char* key = nullptr;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &key, &value)) return nullptr;
    SaoSdkContext* ctx = native_context(self);
    if (ctx != nullptr) {
        sao_sdk_status_t rc = SAO_SDK_ERR_INVALID_ARGUMENT;
        if (PyBool_Check(value)) {
            rc = sao_sdk_config_set_bool(ctx, key, value == Py_True);
        } else if (PyLong_Check(value)) {
            rc = sao_sdk_config_set_int(ctx, key, PyLong_AsLongLong(value));
        } else if (PyFloat_Check(value)) {
            rc = sao_sdk_config_set_double(ctx, key, PyFloat_AsDouble(value));
        } else if (PyUnicode_Check(value)) {
            rc = sao_sdk_config_set_string(ctx, key, PyUnicode_AsUTF8(value));
        }
        if (PyErr_Occurred()) return nullptr;
        if (rc != SAO_SDK_OK) {
            PyErr_Format(PyExc_RuntimeError, "native config set failed: %d", rc);
            return nullptr;
        }
    }
    PyDict_SetItemString(self->settings, key, value);
    Py_RETURN_NONE;
}
PyObject* PluginContext_set_defaults(PluginContextObject* self, PyObject* args) {
    PyObject* defaults = nullptr;
    if (!PyArg_ParseTuple(args, "O", &defaults)) return nullptr;
    if (!PyDict_Check(defaults)) {
        PyErr_SetString(PyExc_TypeError, "defaults must be dict");
        return nullptr;
    }
    // 老语义: 只填 not-yet-set 的 key
    PyObject* key = nullptr;
    PyObject* val = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(defaults, &pos, &key, &val)) {
        if (PyDict_GetItem(self->settings, key) == nullptr) {
            PyDict_SetItem(self->settings, key, val);
        }
    }
    Py_RETURN_NONE;
}
PyObject* PluginContext_get_engine_method(PluginContextObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}
PyObject* PluginContext_get_engine_by_name(PluginContextObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}
PyObject* PluginContext_load_local(PluginContextObject* self, PyObject* args) {
    // 老插件用 ctx.load_local("engine_module.py") 载入插件目录里的模块。
    // 简化实装: 拼路径 → 用 importlib.util.spec_from_file_location 装载。
    const char* rel = nullptr;
    if (!PyArg_ParseTuple(args, "s", &rel)) return nullptr;
    if (self->controlled_test_shim && std::strcmp(rel, "bootstrap.py") == 0) {
        PyObject* shim = PyModule_New("sao_controlled_bootstrap");
        if (shim == nullptr) return nullptr;
        PyObject* globals = PyModule_GetDict(shim);
        const char* code =
            "def ensure_requirements(*args, **kwargs):\n"
            "    return {'added': [], 'deps': {'controlled': 'shim'}}\n"
            "def restore_paths(*args, **kwargs):\n"
            "    return None\n";
        PyObject* result = PyRun_String(code, Py_file_input, globals, globals);
        if (result == nullptr) {
            Py_DECREF(shim);
            return nullptr;
        }
        Py_DECREF(result);
        return shim;
    }
    PyObject* base = self->base_dir;
    if (base == nullptr) Py_RETURN_NONE;
    const char* base_c = PyUnicode_AsUTF8(base);
    if (base_c == nullptr) return nullptr;

    std::string full = base_c;
    if (!full.empty() && full.back() != '/' && full.back() != '\\') full.push_back('/');
    full += rel;

    // module 名: sao_local_<plugin_id>_<escaped_rel>
    std::string name = "sao_local_";
    const char* pid = PyUnicode_AsUTF8(self->plugin_id);
    if (pid != nullptr) name += pid;
    name += "_";
    for (const char* p = rel; *p; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            name.push_back(c);
        } else {
            name.push_back('_');
        }
    }

    PyObject* importlib_util = PyImport_ImportModule("importlib.util");
    if (importlib_util == nullptr) return nullptr;
    PyObject* spec_fn = PyObject_GetAttrString(importlib_util, "spec_from_file_location");
    if (spec_fn == nullptr) { Py_DECREF(importlib_util); return nullptr; }
    PyObject* spec = PyObject_CallFunction(spec_fn, "ss", name.c_str(), full.c_str());
    Py_DECREF(spec_fn);
    if (spec == nullptr || spec == Py_None) {
        Py_XDECREF(spec);
        Py_DECREF(importlib_util);
        PyErr_Format(PyExc_ImportError, "cannot make spec for %s", rel);
        return nullptr;
    }
    PyObject* module_from_spec_fn = PyObject_GetAttrString(importlib_util, "module_from_spec");
    Py_DECREF(importlib_util);
    if (module_from_spec_fn == nullptr) { Py_DECREF(spec); return nullptr; }
    PyObject* mod = PyObject_CallOneArg(module_from_spec_fn, spec);
    Py_DECREF(module_from_spec_fn);
    if (mod == nullptr) { Py_DECREF(spec); return nullptr; }
    PyObject* loader = PyObject_GetAttrString(spec, "loader");
    Py_DECREF(spec);
    if (loader == nullptr) { Py_DECREF(mod); return nullptr; }
    PyObject* exec_fn = PyObject_GetAttrString(loader, "exec_module");
    Py_DECREF(loader);
    if (exec_fn == nullptr) { Py_DECREF(mod); return nullptr; }
    PyObject* res = PyObject_CallOneArg(exec_fn, mod);
    Py_DECREF(exec_fn);
    if (res == nullptr) { Py_DECREF(mod); return nullptr; }
    Py_DECREF(res);
    return mod;
}
// register_hotkey 老名兼容 add_hotkey
PyObject* PluginContext_register_hotkey_alias(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    return PluginContext_add_hotkey(self, args, kwds);
}
// emit alias for publish
PyObject* PluginContext_emit(PluginContextObject* self, PyObject* args) {
    return PluginContext_publish(self, args);
}
// open_window(panel_id[, width, height])
PyObject* PluginContext_open_window(PluginContextObject* /*self*/, PyObject* /*args*/, PyObject* /*kwds*/) {
    Py_RETURN_NONE;
}
// register_engine(name, obj)
PyObject* PluginContext_register_engine(PluginContextObject* self, PyObject* args) {
    const char* name = nullptr;
    PyObject* obj = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &name, &obj)) return nullptr;
    PyObject* engines = PyDict_GetItemString(self->extras, "engine");
    if (engines == nullptr) {
        PyObject* engine_obj = PluginContext_get_engine_attr(self, nullptr);
        Py_XDECREF(engine_obj);
        engines = PyDict_GetItemString(self->extras, "engine");
    }
    if (engines != nullptr && PyObject_SetAttrString(engines, name, obj) != 0) {
        return nullptr;
    }
    Py_RETURN_NONE;
}
PyObject* PluginContext_register_menu_category(PluginContextObject* self,
                                                PyObject* args,
                                                PyObject* kwds) {
    static const char* kwlist[] = {"name", "icon", "builder", "priority", nullptr};
    const char* name = nullptr;
    const char* icon = "";
    PyObject* builder = Py_None;
    int priority = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ssO|i",
                                     const_cast<char**>(kwlist), &name, &icon,
                                     &builder, &priority)) {
        return nullptr;
    }
    keep_callback_ref(self, builder);
    PyObject* rec = PyDict_New();
    if (rec == nullptr) return nullptr;
    PyDict_SetItemString(rec, "name", PyUnicode_FromString(name));
    PyDict_SetItemString(rec, "icon", PyUnicode_FromString(icon));
    PyDict_SetItemString(rec, "builder", Py_NewRef(builder));
    PyDict_SetItemString(rec, "priority", PyLong_FromLong(priority));
    PyList_Append(self->menus, rec);
    Py_DECREF(rec);
    Py_RETURN_NONE;
}
// register_engine (some old code path)
PyObject* PluginContext_unsubscribe(PluginContextObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}
PyObject* PluginContext_toast(PluginContextObject* /*self*/, PyObject* /*args*/) {
    Py_RETURN_NONE;
}
PyObject* PluginContext_ensure_requirements(PluginContextObject* /*self*/, PyObject* /*args*/, PyObject* /*kwds*/) {
    Py_RETURN_NONE;
}

// ── PyMethodDef 表 ──────────────────────────────────────────

PyMethodDef PluginContext_methods[] = {
    // 核心 8 SDK 函数 (spec 明列):
    {"register_ui_panel", reinterpret_cast<PyCFunction>(PluginContext_register_ui_panel),
     METH_VARARGS | METH_KEYWORDS, "Register a UI panel."},
    {"add_hotkey", reinterpret_cast<PyCFunction>(PluginContext_add_hotkey),
     METH_VARARGS | METH_KEYWORDS, "Register a hotkey."},
    {"subscribe", reinterpret_cast<PyCFunction>(PluginContext_subscribe),
     METH_VARARGS, "Subscribe to an event."},
    {"publish", reinterpret_cast<PyCFunction>(PluginContext_publish),
     METH_VARARGS, "Publish an event."},
    {"log_info", reinterpret_cast<PyCFunction>(PluginContext_log_info),
     METH_VARARGS, "Log info."},
    {"log_warn", reinterpret_cast<PyCFunction>(PluginContext_log_warn),
     METH_VARARGS, "Log warn."},
    {"log_error", reinterpret_cast<PyCFunction>(PluginContext_log_error),
     METH_VARARGS, "Log error."},
    {"get_plugin_id", reinterpret_cast<PyCFunction>(PluginContext_get_plugin_id),
     METH_NOARGS, "Get plugin id."},
    {"get_base_dir", reinterpret_cast<PyCFunction>(PluginContext_get_base_dir),
     METH_NOARGS, "Get base dir."},

    // 老 Python 平台 PluginContext 兼容方法 (lenient):
    {"log", reinterpret_cast<PyCFunction>(PluginContext_log),
     METH_VARARGS, "Log a message (old-style)."},
    {"register_hotkey", reinterpret_cast<PyCFunction>(PluginContext_register_hotkey_alias),
     METH_VARARGS | METH_KEYWORDS, "Register a hotkey (alias)."},
    {"emit", reinterpret_cast<PyCFunction>(PluginContext_emit),
     METH_VARARGS, "Emit an event (alias for publish)."},
    {"unsubscribe", reinterpret_cast<PyCFunction>(PluginContext_unsubscribe),
     METH_VARARGS, "Unsubscribe."},
    {"set_interval", reinterpret_cast<PyCFunction>(PluginContext_set_interval),
     METH_VARARGS, "Set an interval timer."},
    {"set_timeout", reinterpret_cast<PyCFunction>(PluginContext_set_timeout),
     METH_VARARGS, "Set a one-shot timer."},
    {"clear_timer", reinterpret_cast<PyCFunction>(PluginContext_clear_timer),
     METH_VARARGS, "Clear a timer."},
    {"notify", reinterpret_cast<PyCFunction>(PluginContext_notify),
     METH_VARARGS | METH_KEYWORDS, "Show a notification."},
    {"dismiss_notify", reinterpret_cast<PyCFunction>(PluginContext_dismiss_notify),
     METH_VARARGS, "Dismiss the current notification."},
    {"request_redraw", reinterpret_cast<PyCFunction>(PluginContext_request_redraw_native),
     METH_VARARGS, "Request UI redraw."},
    {"get_setting", reinterpret_cast<PyCFunction>(PluginContext_get_setting),
     METH_VARARGS, "Get a setting."},
    {"set_setting", reinterpret_cast<PyCFunction>(PluginContext_set_setting),
     METH_VARARGS, "Set a setting."},
    {"set_defaults", reinterpret_cast<PyCFunction>(PluginContext_set_defaults),
     METH_VARARGS, "Merge default settings."},
    {"get_engine", reinterpret_cast<PyCFunction>(PluginContext_get_engine_by_name),
     METH_VARARGS, "Get an engine by name (returns None in host stub)."},
    {"get_engine_method", reinterpret_cast<PyCFunction>(PluginContext_get_engine_method),
     METH_VARARGS, "Get an engine method (returns None in host stub)."},
    {"load_local", reinterpret_cast<PyCFunction>(PluginContext_load_local),
     METH_VARARGS, "Load a bundled module from the plugin directory."},
    {"open_window", reinterpret_cast<PyCFunction>(PluginContext_open_window),
     METH_VARARGS | METH_KEYWORDS, "Open a plugin panel window."},
    {"register_engine", reinterpret_cast<PyCFunction>(PluginContext_register_engine),
     METH_VARARGS, "Register a plugin-scoped engine."},
    {"register_menu_category", reinterpret_cast<PyCFunction>(PluginContext_register_menu_category),
     METH_VARARGS | METH_KEYWORDS, "Register a legacy plugin menu category."},
    {"toast", reinterpret_cast<PyCFunction>(PluginContext_toast),
     METH_VARARGS, "Show a toast."},
    {"ensure_requirements", reinterpret_cast<PyCFunction>(PluginContext_ensure_requirements),
     METH_VARARGS | METH_KEYWORDS, "Ensure external requirements."},
    {nullptr, nullptr, 0, nullptr},
};

// PluginContext PyTypeObject
PyTypeObject PluginContextType = []() {
    PyTypeObject t{PyVarObject_HEAD_INIT(nullptr, 0)};
    t.tp_name = "sao_sdk.PluginContext";
    t.tp_basicsize = sizeof(PluginContextObject);
    t.tp_itemsize = 0;
    t.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC;
    t.tp_doc = "SAO plugin context — exposes the host SDK to a Python plugin.";
    t.tp_new = PluginContext_new;
    t.tp_init = reinterpret_cast<initproc>(PluginContext_init);
    t.tp_dealloc = reinterpret_cast<destructor>(PluginContext_dealloc);
    t.tp_methods = PluginContext_methods;
    t.tp_getset = PluginContext_getset;
    t.tp_traverse = reinterpret_cast<traverseproc>(PluginContext_traverse);
    t.tp_clear = reinterpret_cast<inquiry>(PluginContext_clear);
    t.tp_weaklistoffset = offsetof(PluginContextObject, weakreflist);
    return t;
}();

// ── sao_sdk 模块层面 helper ────────────────────────────

// wrap_ctx(plugin_id, base_dir, opaque_handle=0) → PluginContext
PyObject* sao_sdk_wrap_ctx(PyObject* /*self*/, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"plugin_id", "base_dir", "opaque_handle", nullptr};
    const char* pid = "";
    const char* dir_ = "";
    Py_ssize_t handle_int = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssn",
                                      const_cast<char**>(kwlist),
                                      &pid, &dir_, &handle_int)) {
        return nullptr;
    }
    PyObject* pid_o = PyUnicode_FromString(pid);
    PyObject* dir_o = PyUnicode_FromString(dir_);
    PyObject* hnd_o = PyLong_FromSsize_t(handle_int);
    PyObject* args_t = PyTuple_Pack(3, pid_o, dir_o, hnd_o);
    Py_DECREF(pid_o);
    Py_DECREF(dir_o);
    Py_DECREF(hnd_o);
    if (args_t == nullptr) return nullptr;
    PyObject* obj = PyObject_CallObject(reinterpret_cast<PyObject*>(&PluginContextType), args_t);
    Py_DECREF(args_t);
    return obj;
}
// 模块级 log_info/warn/error (给不用 ctx 也想打日志的场景)
PyObject* sao_sdk_log_info(PyObject* /*self*/, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
    std::fprintf(stdout, "[sao_sdk][info] %s\n", msg);
    Py_RETURN_NONE;
}
PyObject* sao_sdk_log_warn(PyObject* /*self*/, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
    std::fprintf(stderr, "[sao_sdk][warn] %s\n", msg);
    Py_RETURN_NONE;
}
PyObject* sao_sdk_log_error(PyObject* /*self*/, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
    std::fprintf(stderr, "[sao_sdk][error] %s\n", msg);
    Py_RETURN_NONE;
}
// version helper
PyObject* sao_sdk_version(PyObject* /*self*/, PyObject* /*args*/) {
    return PyUnicode_FromString("sao_sdk 1.0 (wave7)");
}

PyMethodDef sao_sdk_module_methods[] = {
    {"wrap_ctx", reinterpret_cast<PyCFunction>(sao_sdk_wrap_ctx),
     METH_VARARGS | METH_KEYWORDS, "Create a PluginContext instance."},
    {"log_info", sao_sdk_log_info, METH_VARARGS, "Module-level info log."},
    {"log_warn", sao_sdk_log_warn, METH_VARARGS, "Module-level warn log."},
    {"log_error", sao_sdk_log_error, METH_VARARGS, "Module-level error log."},
    {"version", sao_sdk_version, METH_NOARGS, "Get sao_sdk version."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef sao_sdk_module_def = {
    PyModuleDef_HEAD_INIT,
    "sao_sdk",             // m_name
    "SAO plugin host SDK — the platform side of ctx.",  // m_doc
    -1,                     // m_size (no module state)
    sao_sdk_module_methods, // m_methods
    nullptr, nullptr, nullptr, nullptr
};

PyObject* PyInit_sao_sdk_impl(void) {
    if (PyType_Ready(&PluginContextType) < 0) return nullptr;
    PyObject* m = PyModule_Create(&sao_sdk_module_def);
    if (m == nullptr) return nullptr;
    Py_INCREF(&PluginContextType);
    if (PyModule_AddObject(m, "PluginContext",
                            reinterpret_cast<PyObject*>(&PluginContextType)) < 0) {
        Py_DECREF(&PluginContextType);
        Py_DECREF(m);
        return nullptr;
    }
    return m;
}

// 统计模块级方法 (spec 要求"暴露 8 SDK 函数给 Python")
// PluginContext_methods 里的 8 个核心方法 + module_methods 里的 5 个 helper。
size_t compute_sdk_method_count() {
    size_t n = 0;
    for (auto* p = PluginContext_methods; p->ml_name != nullptr; ++p) ++n;
    for (auto* p = sao_sdk_module_methods; p->ml_name != nullptr; ++p) ++n;
    return n;
}

} // namespace

// ── 公开 API ────────────────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_native_module(void) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    if (g_sdk_method_count == 0) g_sdk_method_count = compute_sdk_method_count();
    // Py_Initialize 前调 → PyImport_AppendInittab; Py_Initialize 后调 →
    // 通过 sys.modules['sao_sdk'] 手动注入.
    if (Py_IsInitialized() == 0) {
        int rc = PyImport_AppendInittab("sao_sdk", PyInit_sao_sdk_impl);
        if (rc != 0) return SAO_ERR_OS_CALL_FAILED;
        return SAO_OK;
    } else {
        // 后置注入: 检查是否已在 sys.modules
        PyObject* mods = PySys_GetObject("modules");
        if (mods != nullptr) {
            if (PyDict_GetItemString(mods, "sao_sdk") != nullptr) return SAO_OK;
        }
        PyObject* m = PyInit_sao_sdk_impl();
        if (m == nullptr) {
            PyErr_Clear();
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (mods != nullptr) PyDict_SetItemString(mods, "sao_sdk", m);
        Py_DECREF(m);
        return SAO_OK;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_shim_module(void) {
    // 老代码可能 `from act_platform.plugins import PluginContext`。
    // 我们在 sys.modules 里放个 shim 指到 sao_sdk 里的同名类。
    if (Py_IsInitialized() == 0) return SAO_ERR_NOT_INITIALIZED;
    PyObject* sao_sdk = PyImport_ImportModule("sao_sdk");
    if (sao_sdk == nullptr) { PyErr_Clear(); return SAO_ERR_OS_CALL_FAILED; }
    PyObject* pc_cls = PyObject_GetAttrString(sao_sdk, "PluginContext");
    Py_DECREF(sao_sdk);
    if (pc_cls == nullptr) { PyErr_Clear(); return SAO_ERR_OS_CALL_FAILED; }

    // 建 act_platform 包 module + act_platform.plugins submodule
    PyObject* mods = PySys_GetObject("modules");
    if (mods == nullptr) { Py_DECREF(pc_cls); return SAO_ERR_OS_CALL_FAILED; }

    // Skip if already registered (idempotent)
    if (PyDict_GetItemString(mods, "act_platform.plugins") != nullptr) {
        Py_DECREF(pc_cls);
        return SAO_OK;
    }

    // 建 act_platform (若无) —— PyModule_New 建纯 name-only module.
    PyObject* act_platform_mod = PyDict_GetItemString(mods, "act_platform");
    if (act_platform_mod == nullptr) {
        act_platform_mod = PyModule_New("act_platform");
        if (act_platform_mod == nullptr) { Py_DECREF(pc_cls); return SAO_ERR_OS_CALL_FAILED; }
        PyDict_SetItemString(mods, "act_platform", act_platform_mod);
        // PyDict_SetItem 取新引用, PyModule_New 建时 refcount=1 → 现在 2, 平衡回 1
        Py_DECREF(act_platform_mod);
        act_platform_mod = PyDict_GetItemString(mods, "act_platform");  // borrowed
    }
    Py_INCREF(act_platform_mod);  // 为下面的 DECREF 对齐
    PyObject* plugins_mod = PyModule_New("act_platform.plugins");
    if (plugins_mod == nullptr) {
        Py_DECREF(act_platform_mod);
        Py_DECREF(pc_cls);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject_SetAttrString(plugins_mod, "PluginContext", pc_cls);
    PyObject_SetAttrString(act_platform_mod, "plugins", plugins_mod);
    PyDict_SetItemString(mods, "act_platform.plugins", plugins_mod);
    Py_DECREF(act_platform_mod);
    Py_DECREF(plugins_mod);
    Py_DECREF(pc_cls);
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_wrap_ctx(void* ctx_ptr, void** out_pyobject) {
    if (out_pyobject == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_pyobject = nullptr;
    if (Py_IsInitialized() == 0) return SAO_ERR_NOT_INITIALIZED;
    // 找 PluginContext 类
    PyObject* sao_sdk = PyImport_ImportModule("sao_sdk");
    if (sao_sdk == nullptr) { PyErr_Clear(); return SAO_ERR_OS_CALL_FAILED; }
    PyObject* pc_cls = PyObject_GetAttrString(sao_sdk, "PluginContext");
    Py_DECREF(sao_sdk);
    if (pc_cls == nullptr) { PyErr_Clear(); return SAO_ERR_OS_CALL_FAILED; }

    Py_ssize_t hnd = static_cast<Py_ssize_t>(reinterpret_cast<intptr_t>(ctx_ptr));
    PyObject* args = Py_BuildValue("(ssn)", "", "", hnd);
    if (args == nullptr) { Py_DECREF(pc_cls); return SAO_ERR_OS_CALL_FAILED; }
    PyObject* obj = PyObject_CallObject(pc_cls, args);
    Py_DECREF(args);
    Py_DECREF(pc_cls);
    if (obj == nullptr) { PyErr_Clear(); return SAO_ERR_OS_CALL_FAILED; }
    *out_pyobject = obj;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_free_wrapped_ctx(void* pyobject) {
    if (pyobject == nullptr) return;
    if (Py_IsInitialized() == 0) return;
    PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
    Py_DECREF(obj);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_teardown_native(void* pyobject) {
    if (pyobject == nullptr || Py_IsInitialized() == 0) return;
    PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
    if (!PyObject_TypeCheck(obj, &PluginContextType)) return;
    auto* context = reinterpret_cast<PluginContextObject*>(obj);
    (void)PluginContext_clear(context);
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sdk_method_count(void) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    if (g_sdk_method_count == 0) g_sdk_method_count = compute_sdk_method_count();
    return g_sdk_method_count;
}

// 供 py_host.cpp 内部访问 PluginContext state (Wave 7 内省 API).
// 返回 void* 以避免 header include Python.h; 调用方 cast 为 PyObject* 用完 DECREF.
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_get_records(void* pyobject, const char* record_kind) {
    if (pyobject == nullptr || record_kind == nullptr) return nullptr;
    if (Py_IsInitialized() == 0) return nullptr;
    PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
    if (!PyObject_TypeCheck(obj, &PluginContextType)) return nullptr;
    PluginContextObject* pc = reinterpret_cast<PluginContextObject*>(obj);
    if (std::strcmp(record_kind, "panels") == 0) return Py_NewRef(pc->panels);
    if (std::strcmp(record_kind, "hotkeys") == 0) return Py_NewRef(pc->hotkeys);
    if (std::strcmp(record_kind, "subscriptions") == 0) return Py_NewRef(pc->subscriptions);
    if (std::strcmp(record_kind, "published") == 0) return Py_NewRef(pc->published);
    if (std::strcmp(record_kind, "logs") == 0) return Py_NewRef(pc->logs);
    if (std::strcmp(record_kind, "timers") == 0) return Py_NewRef(pc->timers);
    if (std::strcmp(record_kind, "notifications") == 0) return Py_NewRef(pc->notifications);
    if (std::strcmp(record_kind, "settings") == 0) return Py_NewRef(pc->settings);
    if (std::strcmp(record_kind, "menus") == 0) return Py_NewRef(pc->menus);
    return nullptr;
}

#else // !SAO_HAS_PYTHON_EMBED

// stub 实装 (无 embed 时).

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_native_module(void) { return SAO_ERR_NOT_IMPLEMENTED; }

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_shim_module(void) { return SAO_ERR_NOT_IMPLEMENTED; }

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_wrap_ctx(void* /*ctx_ptr*/, void** out_pyobject) {
    if (out_pyobject != nullptr) *out_pyobject = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_free_wrapped_ctx(void* /*pyobject*/) {}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_teardown_native(void* /*pyobject*/) {}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sdk_method_count(void) { return 0; }

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_get_records(void* /*pyobject*/, const char* /*record_kind*/) {
    return nullptr;
}

#endif // SAO_HAS_PYTHON_EMBED

} // namespace sao::plugins::python_host
