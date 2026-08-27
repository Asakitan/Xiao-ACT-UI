// py_module_bridge.cpp — sao_sdk 内置模块与 PluginContext 桥接
//
// 老 Python 插件 (star_resonance / hide_seek / midi_piano) 里 on_load(ctx) 拿到
// 一个 ctx 对象。C++ 平台侧把它包成一个 PyObject (PluginContext), 方法名/签名
// 全部对齐旧 Python PluginContext。这样老插件**一个字不用改**就能被本 host 加载。
//
// 已知实装范围:
//   - sao_sdk PyModuleDef + PyInit_sao_sdk
//   - PluginContextType (PyTypeObject) 覆盖 8 组核心方法:
//     * register_ui_panel(panel_id, meta[, render][, on_action])
//     * add_hotkey / register_hotkey(hotkey_id, callback[, default_key][, label])
//     * subscribe(event_type, callback)
//     * publish / emit(event_type, data)
//     * log_info(msg) / log_warn(msg) / log_error(msg) / log(msg)
//     * get_plugin_id() / plugin_id 属性
//     * get_base_dir() / path / assets_path 属性
//     * loader lifecycle / event / engine / timer / notify capabilities
//   - 记录所有注册到 PluginContext state, 供 host / test 内省
//
// 关键设计:
//   list/dict 只用于 Python token 与测试内省；生产调用必须成功转发到
//   loader plugin_context_t 或 SaoSdkContext，否则返回明确 Python 错误。
//
// PluginContext 拥有 self.__dict__ (通过 PyTypeObject tp_dict + tp_getattro), 老插件
// 可以自由塞属性 (ctx.engine.owner 之类) —— 那些属性访问在 sub-attr 层面抛
// AttributeError, 由插件自己 try/except 兜底 (老 hide_seek 就这么写的)。

#include "sao/plugins/python_host/py_module_bridge.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/sdk/sao_sdk.h"

#if defined(SAO_HAS_PYTHON_EMBED)
#define PY_SSIZE_T_CLEAN
#include "sao/plugins/python_host/py_release_abi.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::python_host {

#if defined(SAO_HAS_PYTHON_EMBED)

namespace {

namespace loader = sao::plugins::loader;

static_assert(SAO_PLUGIN_CONTEXT_ENTITY_PROVIDER_ABI_VERSION >= 3u);

struct CallbackGate {
    std::mutex mutex;
    std::condition_variable idle;
    bool accepting = true;
    bool stopping = false;
    size_t in_flight = 0;
};

constexpr size_t kMaximumCallbackNesting = 64;
thread_local std::array<CallbackGate*, kMaximumCallbackNesting> g_active_callback_gates{};
thread_local size_t g_active_callback_depth = 0;

bool callback_active_on_current_thread(const CallbackGate& gate) noexcept {
    return std::find(g_active_callback_gates.begin(),
                     g_active_callback_gates.begin() + g_active_callback_depth,
                     &gate) != g_active_callback_gates.begin() + g_active_callback_depth;
}

bool callback_stop_pending(CallbackGate& gate) {
    std::lock_guard lock(gate.mutex);
    return gate.stopping;
}

bool enter_callback(CallbackGate& gate) {
    if (g_active_callback_depth == kMaximumCallbackNesting)
        return false;
    std::lock_guard lock(gate.mutex);
    if (!gate.accepting)
        return false;
    ++gate.in_flight;
    g_active_callback_gates[g_active_callback_depth++] = &gate;
    return true;
}

void leave_callback(CallbackGate& gate) {
    if (g_active_callback_depth > 0 &&
        g_active_callback_gates[g_active_callback_depth - 1] == &gate) {
        g_active_callback_gates[--g_active_callback_depth] = nullptr;
    }
    {
        std::lock_guard lock(gate.mutex);
        if (gate.in_flight > 0)
            --gate.in_flight;
    }
    gate.idle.notify_all();
}

bool stop_callbacks(CallbackGate& gate) {
    std::unique_lock lock(gate.mutex);
    if (callback_active_on_current_thread(gate)) {
        return false;
    }
    if (gate.stopping) {
        return false;
    }
    gate.stopping = true;
    gate.accepting = false;
    if (gate.in_flight == 0)
        return true;
    lock.unlock();
    PyThreadState* released = PyEval_SaveThread();
    lock.lock();
    gate.idle.wait(lock, [&gate] { return gate.in_flight == 0; });
    lock.unlock();
    PyEval_RestoreThread(released);
    return true;
}

void resume_callbacks(CallbackGate& gate) {
    std::lock_guard lock(gate.mutex);
    gate.stopping = false;
    gate.accepting = true;
}

// ── PluginContext PyObject 状态 ─────────────────────────────────
//
// 每个插件 on_load(ctx) 收到的都是一个新的 PluginContext 实例, 内部持:
//   - opaque_handle: SaoSdkContext* (可空)
//   - loader_context: loader-owned canonical plugin_context_t* (可空)
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
    PyObject_HEAD PyObject* weakreflist;
    void* opaque_handle;                      // SaoSdkContext* (borrowed)
    loader::plugin_context_t* loader_context; // loader-owned canonical ctx
    bool loader_context_lease_held;
    PyObject* plugin_id;                      // str
    PyObject* base_dir;                       // str
    PyObject* assets_path;                    // str
    PyObject* web_path;                       // str
    PyObject* panels;                         // list[dict]
    PyObject* hotkeys;                        // list[dict]
    PyObject* subscriptions;                  // list[dict]
    PyObject* published;                      // list[dict]  (emit / publish 记账)
    PyObject* logs;                           // list[dict]  (log_info/warn/error 记账)
    PyObject* timers;                         // list[dict]  (set_interval/timeout)
    PyObject* notifications;                  // list[dict]  (notify 记账)
    PyObject* settings;                       // dict[str, obj]
    PyObject* menus;                          // list[dict]  (legacy menu categories)
    PyObject* subscription_tokens;            // dict[str, dict]
    PyObject* timer_tokens;                   // dict[str, dict]
    PyObject* notification_tokens;            // dict[str, dict]
    PyObject* engines;                        // dict[str, object]
    PyObject* extras;                         // dict (插件塞的额外属性; ctx.engine.owner 等)
    // 记录 Python 侧引用的 callback (subscribe / hotkey / render / on_action),
    // 让 unload 时统一 DECREF。
    PyObject* callback_refs; // list[obj]
    struct NativePanelBridge* native_panel_bridges = nullptr;
    struct NativeHotkeyBridge* native_hotkey_bridges = nullptr;
    struct NativeEventBridge* native_event_bridges = nullptr;
    struct NativeTimerBridge* native_timer_bridges = nullptr;
    struct NativeNotifyBridge* native_notify_bridges = nullptr;
    struct NativeMenuBridge* native_menu_bridges = nullptr;
    struct NativeActionBridge* native_action_bridge = nullptr;
    uint64_t next_resource_sequence = 1;
    uint64_t next_python_token = 1;
    uint64_t next_enable_checkpoint = 1;
    uint64_t active_enable_checkpoint = 0;
    uint64_t enable_checkpoint_sequence = 0;
    struct NativeActionBridge* enable_checkpoint_action = nullptr;
    bool tearing_down = false;
    bool teardown_deferred = false;
    bool controlled_test_shim = false;
    bool retired = false;
    PluginContextObject* retired_next = nullptr;
};

struct NativePanelBridge {
    CallbackGate gate;
    PyObject* callback = nullptr;
    sao_sdk_ui_panel_t panel = nullptr;
    bool registered = true;
    uint64_t sequence = 0;
    NativePanelBridge* next = nullptr;
};

struct NativeHotkeyBridge {
    CallbackGate gate;
    PyObject* callback = nullptr;
    sao_sdk_hotkey_id_t token = 0;
    bool registered = true;
    uint64_t sequence = 0;
    NativeHotkeyBridge* next = nullptr;
};

struct NativeEventBridge {
    CallbackGate gate;
    PyObject* callback = nullptr;
    sao_sdk_subscription_t token = 0;
    std::string python_token;
    bool registered = true;
    uint64_t sequence = 0;
    NativeEventBridge* next = nullptr;
};

struct NativeTimerBridge {
    CallbackGate gate;
    PluginContextObject* owner = nullptr;
    PyObject* callback = nullptr;
    std::string python_token;
    std::string loader_token;
    sao_sdk_timer_token_t sdk_token = 0;
    bool uses_loader = false;
    bool one_shot = false;
    bool owner_ref_held = false;
    bool registered = true;
    uint64_t sequence = 0;
    NativeTimerBridge* next = nullptr;
};

struct NativeNotifyBridge {
    std::string python_token;
    sao_sdk_notify_token_t sdk_token = 0;
    bool uses_loader = false;
    bool registered = true;
    uint64_t sequence = 0;
    NativeNotifyBridge* next = nullptr;
};

struct NativeMenuRow {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const NativeMenuRow& other) const {
        return label == other.label && icon == other.icon && action_id == other.action_id &&
               payload_json == other.payload_json && can_activate == other.can_activate &&
               keep_menu_open == other.keep_menu_open &&
               close_menu_before == other.close_menu_before;
    }
};

struct NativeMenuBridge {
    CallbackGate gate;
    PyObject* builder = nullptr;
    PyObject* ledger_record = nullptr;
    std::string provider_id;
    std::string qualified_provider_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    uint64_t revision = 0;
    bool enable_scoped = false;
    bool registered = true;
    uint64_t sequence = 0;
    std::vector<NativeMenuRow> rows;
    std::unordered_map<std::string, PyObject*> actions;
    NativeMenuBridge* next = nullptr;
};

struct NativeActionBridge {
    CallbackGate gate;
    PyObject* callback = nullptr;
    std::string provider_id;
    std::string qualified_provider_id;
    bool enable_scoped = false;
    bool registered = true;
    uint64_t sequence = 0;
    NativeActionBridge* previous = nullptr;
};

// forward decl
extern PyTypeObject PluginContextType;
void clear_callback_ledgers(PluginContextObject* self);

// ── 内部小工具 ───────────────────────────────────────────────

// Bridge entry points remain callable during cross-TU global teardown.
std::mutex& g_mod_mu = *new std::mutex();
size_t g_sdk_method_count = 0; // 供 sao_plugins_pyhost_sdk_method_count 查
std::mutex& g_retired_context_mutex = *new std::mutex();
PluginContextObject* g_retired_contexts = nullptr;

// 追加一个"记账" dict 到 list 里, 键值对由 (key, PyObject*) 组成, 参数以变长
// 方式给, 结束用 nullptr sentinel。函数吸取 PyObject 引用 (steals)。
PyObject* append_record(PyObject* list, ...) {
    if (list == nullptr)
        Py_RETURN_NONE;
    PyObject* d = PyDict_New();
    if (d == nullptr)
        return nullptr;

    va_list ap;
    va_start(ap, list);
    for (;;) {
        const char* key = va_arg(ap, const char*);
        if (key == nullptr)
            break;
        PyObject* val = va_arg(ap, PyObject*);
        if (val == nullptr)
            val = Py_NewRef(Py_None);
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

bool keep_callback_ref(PluginContextObject* self, PyObject* cb) {
    if (self == nullptr || cb == nullptr || cb == Py_None)
        return true;
    if (self->callback_refs == nullptr)
        return false;
    return PyList_Append(self->callback_refs, cb) == 0;
}

void truncate_list(PyObject* list, Py_ssize_t size) {
    if (list == nullptr || !PyList_Check(list))
        return;
    const Py_ssize_t current = PyList_GET_SIZE(list);
    if (current > size && PyList_SetSlice(list, size, current, nullptr) != 0)
        PyErr_Clear();
}

bool set_dict_item_steal(PyObject* dictionary, const char* key, PyObject* value) {
    if (value == nullptr)
        return false;
    const int status = PyDict_SetItemString(dictionary, key, value);
    Py_DECREF(value);
    return status == 0;
}

bool set_dict_item_steal(PyObject* dictionary, PyObject* key, PyObject* value) {
    if (value == nullptr)
        return false;
    const int status = PyDict_SetItem(dictionary, key, value);
    Py_DECREF(value);
    return status == 0;
}

PyObject* status_error(const char* operation, int32_t status) {
    PyObject* exception = status == loader::SAO_PLUGINS_ERR_UNSUPPORTED ||
                                  status == SAO_SDK_ERR_UNSUPPORTED ||
                                  status == SAO_ERR_NOT_IMPLEMENTED
                              ? PyExc_NotImplementedError
                              : PyExc_RuntimeError;
    PyErr_Format(exception, "%s failed with status %d", operation, status);
    return nullptr;
}

bool require_callable(PyObject* callback, const char* name) {
    if (callback != nullptr && PyCallable_Check(callback) != 0)
        return true;
    PyErr_Format(PyExc_TypeError, "%s must be callable", name);
    return false;
}

bool require_active_context(PluginContextObject* self) {
    if (!self->tearing_down)
        return true;
    PyErr_SetString(PyExc_RuntimeError, "plugin context is tearing down");
    return false;
}

std::string next_token(PluginContextObject* self, const char* prefix) {
    return std::string(prefix) + std::to_string(self->next_python_token++);
}

PyObject* explicit_status(int32_t status, const char* panel_id = nullptr) {
    PyObject* result = PyDict_New();
    if (result == nullptr)
        return nullptr;
    if (!set_dict_item_steal(result, "ok", PyBool_FromLong(status == SAO_OK)) ||
        !set_dict_item_steal(result, "status", PyLong_FromLong(status)) ||
        (panel_id != nullptr &&
         !set_dict_item_steal(result, "panel_id", PyUnicode_FromString(panel_id)))) {
        Py_DECREF(result);
        return nullptr;
    }
    return result;
}

SaoSdkContext* native_context(PluginContextObject* self) {
    if (self == nullptr || self->opaque_handle == nullptr)
        return nullptr;
    auto* ctx = static_cast<SaoSdkContext*>(self->opaque_handle);
    if (ctx->abi_version != SAO_SDK_ABI_VERSION || ctx->ctx_impl == nullptr) {
        return nullptr;
    }
    return ctx;
}

void delete_timer_bridge(NativeTimerBridge* bridge) {
    if (bridge == nullptr)
        return;
    auto* owner = bridge->owner;
    const bool release_owner = bridge->owner_ref_held;
    Py_XDECREF(bridge->callback);
    delete bridge;
    if (release_owner)
        Py_DECREF(reinterpret_cast<PyObject*>(owner));
}

void delete_action_bridge(NativeActionBridge* bridge) {
    if (bridge == nullptr)
        return;
    Py_XDECREF(bridge->callback);
    delete bridge;
}

void delete_action_chain(NativeActionBridge* bridge) {
    while (bridge != nullptr) {
        auto* previous = bridge->previous;
        delete_action_bridge(bridge);
        bridge = previous;
    }
}

void delete_action_prefix(NativeActionBridge* bridge, NativeActionBridge* stop) {
    while (bridge != stop) {
        auto* previous = bridge->previous;
        delete_action_bridge(bridge);
        bridge = previous;
    }
}

int32_t register_action_provider(PluginContextObject* self, NativeActionBridge* bridge) noexcept;

int32_t unregister_provider_ids(PluginContextObject* self,
                                const std::vector<std::string>& qualified_ids) noexcept {
    if (qualified_ids.empty())
        return SAO_OK;
    if (self == nullptr || self->loader_context == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        std::vector<const char*> provider_ids;
        provider_ids.reserve(qualified_ids.size());
        for (const auto& provider_id : qualified_ids)
            provider_ids.push_back(provider_id.c_str());
        return loader::plugin_context_unregister_entity_providers(
            self->loader_context, provider_ids.data(), provider_ids.size());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t release_native_bridges(PluginContextObject* self) {
    if (self == nullptr)
        return SAO_OK;
    if (self->tearing_down) {
        const bool has_native_bridges =
            self->native_panel_bridges != nullptr || self->native_hotkey_bridges != nullptr ||
            self->native_event_bridges != nullptr || self->native_timer_bridges != nullptr ||
            self->native_notify_bridges != nullptr || self->native_menu_bridges != nullptr ||
            self->native_action_bridge != nullptr;
        return has_native_bridges ? loader::SAO_PLUGINS_ERR_BUSY : SAO_OK;
    }

    struct Resource {
        uint64_t sequence;
        enum class Kind { panel, hotkey, event, timer, notify, menu, action } kind;
        void* value;
    };
    std::vector<Resource> resources;
    for (auto* item = self->native_panel_bridges; item != nullptr; item = item->next) {
        resources.push_back({item->sequence, Resource::Kind::panel, item});
    }
    for (auto* item = self->native_hotkey_bridges; item != nullptr; item = item->next) {
        resources.push_back({item->sequence, Resource::Kind::hotkey, item});
    }
    for (auto* item = self->native_event_bridges; item != nullptr; item = item->next) {
        resources.push_back({item->sequence, Resource::Kind::event, item});
    }
    for (auto* item = self->native_timer_bridges; item != nullptr; item = item->next) {
        resources.push_back({item->sequence, Resource::Kind::timer, item});
    }
    for (auto* item = self->native_notify_bridges; item != nullptr; item = item->next) {
        resources.push_back({item->sequence, Resource::Kind::notify, item});
    }
    for (auto* item = self->native_menu_bridges; item != nullptr; item = item->next) {
        resources.push_back({item->sequence, Resource::Kind::menu, item});
    }
    for (auto* item = self->native_action_bridge; item != nullptr; item = item->previous) {
        resources.push_back({item->sequence, Resource::Kind::action, item});
    }
    std::sort(resources.begin(), resources.end(), [](const Resource& left, const Resource& right) {
        return left.sequence > right.sequence;
    });

    for (const auto& resource : resources) {
        CallbackGate* gate = nullptr;
        switch (resource.kind) {
        case Resource::Kind::panel:
            gate = &static_cast<NativePanelBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::hotkey:
            gate = &static_cast<NativeHotkeyBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::event:
            gate = &static_cast<NativeEventBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::timer:
            gate = &static_cast<NativeTimerBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::menu:
            gate = &static_cast<NativeMenuBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::action:
            gate = &static_cast<NativeActionBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::notify:
            break;
        }
        if (gate != nullptr &&
            (callback_active_on_current_thread(*gate) || callback_stop_pending(*gate))) {
            self->teardown_deferred = true;
            return loader::SAO_PLUGINS_ERR_BUSY;
        }
    }
    self->tearing_down = true;
    self->teardown_deferred = false;

    SaoSdkContext* sdk = native_context(self);
    std::vector<CallbackGate*> stopped_gates;
    stopped_gates.reserve(resources.size());
    for (const auto& resource : resources) {
        CallbackGate* gate = nullptr;
        switch (resource.kind) {
        case Resource::Kind::panel:
            gate = &static_cast<NativePanelBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::hotkey:
            gate = &static_cast<NativeHotkeyBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::event:
            gate = &static_cast<NativeEventBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::timer:
            gate = &static_cast<NativeTimerBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::menu:
            gate = &static_cast<NativeMenuBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::action:
            gate = &static_cast<NativeActionBridge*>(resource.value)->gate;
            break;
        case Resource::Kind::notify:
            break;
        }
        if (gate != nullptr) {
            if (!stop_callbacks(*gate)) {
                for (auto* stopped : stopped_gates)
                    resume_callbacks(*stopped);
                self->tearing_down = false;
                self->teardown_deferred = true;
                return loader::SAO_PLUGINS_ERR_BUSY;
            }
            stopped_gates.push_back(gate);
        }
    }

    const auto rollback_teardown = [self, &stopped_gates](int32_t status) {
        for (auto* gate : stopped_gates)
            resume_callbacks(*gate);
        self->tearing_down = false;
        self->teardown_deferred = false;
        return status;
    };
    const auto sdk_unregister_complete = [](int32_t status) {
        return status == SAO_OK || status == SAO_SDK_ERR_NOT_FOUND;
    };
    const auto loader_unregister_complete = [](int32_t status) {
        return status == SAO_OK || status == SAO_ERR_HANDLE_INVALID;
    };

    bool loader_notifications_dismissed = false;
    for (const auto& resource : resources) {
        int32_t status = SAO_OK;
        bool complete = true;
        switch (resource.kind) {
        case Resource::Kind::panel: {
            auto* bridge = static_cast<NativePanelBridge*>(resource.value);
            if (bridge->registered && sdk != nullptr && bridge->panel != nullptr) {
                status = sao_sdk_unregister_ui_panel(sdk, bridge->panel);
                complete = sdk_unregister_complete(status);
            } else if (bridge->registered && bridge->panel != nullptr) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
            if (complete)
                bridge->registered = false;
            break;
        }
        case Resource::Kind::hotkey: {
            auto* bridge = static_cast<NativeHotkeyBridge*>(resource.value);
            if (bridge->registered && sdk != nullptr && bridge->token != 0) {
                status = sao_sdk_unregister_hotkey(sdk, bridge->token);
                complete = sdk_unregister_complete(status);
            } else if (bridge->registered && bridge->token != 0) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
            if (complete)
                bridge->registered = false;
            break;
        }
        case Resource::Kind::event: {
            auto* bridge = static_cast<NativeEventBridge*>(resource.value);
            if (bridge->registered && self->loader_context != nullptr && bridge->token != 0) {
                status = loader::sao_plugins_ctx_unsubscribe(
                    self->loader_context, static_cast<uint32_t>(bridge->token));
                complete = loader_unregister_complete(status);
            } else if (bridge->registered && sdk != nullptr && bridge->token != 0) {
                status = sao_sdk_unsubscribe_event(sdk, bridge->token);
                complete = sdk_unregister_complete(status);
            } else if (bridge->registered && bridge->token != 0) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
            if (complete)
                bridge->registered = false;
            break;
        }
        case Resource::Kind::timer: {
            auto* bridge = static_cast<NativeTimerBridge*>(resource.value);
            if (bridge->registered && bridge->uses_loader && self->loader_context != nullptr &&
                !bridge->loader_token.empty()) {
                status = loader::sao_plugins_ctx_clear_timer(self->loader_context,
                                                             bridge->loader_token.c_str());
                complete = loader_unregister_complete(status);
            } else if (bridge->registered && sdk != nullptr && bridge->sdk_token != 0) {
                status = sao_sdk_timer_unregister(sdk, bridge->sdk_token);
                complete = sdk_unregister_complete(status);
            } else if (bridge->registered &&
                       ((!bridge->loader_token.empty() && bridge->uses_loader) ||
                        bridge->sdk_token != 0)) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
            if (complete)
                bridge->registered = false;
            break;
        }
        case Resource::Kind::notify: {
            auto* bridge = static_cast<NativeNotifyBridge*>(resource.value);
            if (bridge->registered && bridge->uses_loader && self->loader_context != nullptr) {
                if (!loader_notifications_dismissed) {
                    status = loader::sao_plugins_ctx_dismiss_notify(self->loader_context);
                    complete = loader_unregister_complete(status);
                    loader_notifications_dismissed = complete;
                }
            } else if (bridge->registered && sdk != nullptr && bridge->sdk_token != 0) {
                status = sao_sdk_notify_dismiss(sdk, bridge->sdk_token);
                complete = sdk_unregister_complete(status);
            } else if (bridge->registered &&
                       (bridge->uses_loader || bridge->sdk_token != 0)) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
            if (complete)
                bridge->registered = false;
            break;
        }
        case Resource::Kind::menu: {
            auto* bridge = static_cast<NativeMenuBridge*>(resource.value);
#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
            if (bridge->registered && self->loader_context != nullptr &&
                !bridge->qualified_provider_id.empty()) {
                const char* provider_ids[] = {bridge->qualified_provider_id.c_str()};
                status = loader::plugin_context_unregister_entity_providers(
                    self->loader_context, provider_ids, std::size(provider_ids));
                complete = status == SAO_OK;
            } else if (bridge->registered && !bridge->qualified_provider_id.empty()) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
#endif
            if (complete)
                bridge->registered = false;
            break;
        }
        case Resource::Kind::action: {
            auto* bridge = static_cast<NativeActionBridge*>(resource.value);
            if (bridge->registered && self->loader_context != nullptr &&
                !bridge->qualified_provider_id.empty()) {
                const std::vector<std::string> provider_ids{bridge->qualified_provider_id};
                status = unregister_provider_ids(self, provider_ids);
                complete = status == SAO_OK;
            } else if (bridge->registered && !bridge->qualified_provider_id.empty()) {
                status = SAO_ERR_HANDLE_INVALID;
                complete = false;
            }
            if (complete)
                bridge->registered = false;
            break;
        }
        }
        if (!complete)
            return rollback_teardown(status);
    }

    while (self->native_panel_bridges != nullptr) {
        auto* bridge = self->native_panel_bridges;
        self->native_panel_bridges = bridge->next;
        Py_XDECREF(bridge->callback);
        delete bridge;
    }
    while (self->native_hotkey_bridges != nullptr) {
        auto* bridge = self->native_hotkey_bridges;
        self->native_hotkey_bridges = bridge->next;
        Py_XDECREF(bridge->callback);
        delete bridge;
    }
    while (self->native_event_bridges != nullptr) {
        auto* bridge = self->native_event_bridges;
        self->native_event_bridges = bridge->next;
        Py_XDECREF(bridge->callback);
        delete bridge;
    }
    while (self->native_timer_bridges != nullptr) {
        auto* bridge = self->native_timer_bridges;
        self->native_timer_bridges = bridge->next;
        delete_timer_bridge(bridge);
    }
    while (self->native_notify_bridges != nullptr) {
        auto* bridge = self->native_notify_bridges;
        self->native_notify_bridges = bridge->next;
        delete bridge;
    }
    while (self->native_menu_bridges != nullptr) {
        auto* bridge = self->native_menu_bridges;
        self->native_menu_bridges = bridge->next;
        Py_XDECREF(bridge->builder);
        Py_XDECREF(bridge->ledger_record);
        for (const auto& [_, callback] : bridge->actions) {
            Py_XDECREF(callback);
        }
        delete bridge;
    }
    delete_action_chain(self->native_action_bridge);
    self->native_action_bridge = nullptr;
    self->active_enable_checkpoint = 0;
    self->enable_checkpoint_sequence = 0;
    self->enable_checkpoint_action = nullptr;
    if (self->loader_context_lease_held && self->loader_context != nullptr) {
        loader::plugin_context_release_host_lease(self->loader_context);
        self->loader_context_lease_held = false;
    }
    self->loader_context = nullptr;
    self->opaque_handle = nullptr;
    return SAO_OK;
}

void retire_plugin_context(PluginContextObject* self) noexcept {
    if (self == nullptr || self->retired)
        return;
    self->retired = true;
    Py_SET_REFCNT(reinterpret_cast<PyObject*>(self), 1);
    try {
        std::lock_guard lock(g_retired_context_mutex);
        self->retired_next = g_retired_contexts;
        g_retired_contexts = self;
    } catch (...) {
        self->retired_next = nullptr;
    }
}

void drain_retired_contexts() noexcept {
    PluginContextObject* pending = nullptr;
    try {
        std::lock_guard lock(g_retired_context_mutex);
        pending = g_retired_contexts;
        g_retired_contexts = nullptr;
    } catch (...) {
        return;
    }

    PluginContextObject* retry = nullptr;
    while (pending != nullptr) {
        PluginContextObject* context = pending;
        pending = pending->retired_next;
        context->retired_next = nullptr;
        int32_t status = SAO_ERR_OS_CALL_FAILED;
        try {
            status = release_native_bridges(context);
        } catch (...) {
            status = SAO_ERR_OS_CALL_FAILED;
        }
        if (status == SAO_OK) {
            clear_callback_ledgers(context);
            context->retired = false;
            Py_DECREF(reinterpret_cast<PyObject*>(context));
            continue;
        }
        context->retired_next = retry;
        retry = context;
    }
    if (retry == nullptr)
        return;
    try {
        std::lock_guard lock(g_retired_context_mutex);
        while (retry != nullptr) {
            PluginContextObject* context = retry;
            retry = retry->retired_next;
            context->retired_next = g_retired_contexts;
            g_retired_contexts = context;
        }
    } catch (...) {
    }
}

void clear_callback_ledgers(PluginContextObject* self) {
    if (self == nullptr)
        return;
    for (PyObject* records : {self->panels, self->hotkeys, self->subscriptions, self->timers,
                              self->menus, self->callback_refs}) {
        if (records != nullptr && PyList_Check(records)) {
            (void)PyList_SetSlice(records, 0, PyList_GET_SIZE(records), nullptr);
        }
    }
    for (PyObject* tokens : {self->subscription_tokens, self->timer_tokens,
                             self->notification_tokens, self->engines}) {
        if (tokens != nullptr && PyDict_Check(tokens))
            PyDict_Clear(tokens);
    }
}

void SAO_SDK_CALL native_panel_action(const char* action_key_utf8, const uint8_t*, size_t,
                                      void* user_data) {
    auto* bridge = static_cast<NativePanelBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr || !enter_callback(bridge->gate)) {
        return;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallFunction(
        bridge->callback, "sO", action_key_utf8 == nullptr ? "" : action_key_utf8, Py_None);
    Py_XDECREF(result);
    PyErr_Clear();
    leave_callback(bridge->gate);
    drain_retired_contexts();
    PyGILState_Release(gil);
}

void SAO_SDK_CALL native_hotkey_action(sao_sdk_hotkey_id_t, void* user_data) {
    auto* bridge = static_cast<NativeHotkeyBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr || !enter_callback(bridge->gate)) {
        return;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallNoArgs(bridge->callback);
    Py_XDECREF(result);
    PyErr_Clear();
    leave_callback(bridge->gate);
    drain_retired_contexts();
    PyGILState_Release(gil);
}

void SAO_SDK_CALL native_event_action(const char* /*topic_utf8*/, const uint8_t* payload,
                                      size_t payload_len, void* user_data) {
    auto* bridge = static_cast<NativeEventBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr || !enter_callback(bridge->gate)) {
        return;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* json_module = PyImport_ImportModule("json");
    PyObject* loads =
        json_module == nullptr ? nullptr : PyObject_GetAttrString(json_module, "loads");
    Py_XDECREF(json_module);
    PyObject* text = PyUnicode_DecodeUTF8(reinterpret_cast<const char*>(payload),
                                          static_cast<Py_ssize_t>(payload_len), "strict");
    PyObject* body =
        loads == nullptr || text == nullptr ? nullptr : PyObject_CallOneArg(loads, text);
    Py_XDECREF(loads);
    Py_XDECREF(text);
    PyObject* result = body == nullptr ? nullptr : PyObject_CallOneArg(bridge->callback, body);
    Py_XDECREF(result);
    Py_XDECREF(body);
    if (PyErr_Occurred())
        PyErr_WriteUnraisable(bridge->callback);
    leave_callback(bridge->gate);
    drain_retired_contexts();
    PyGILState_Release(gil);
}

void SAO_PLUGINS_CALL native_loader_event_action(const char* topic_utf8,
                                                 const char* event_json_utf8, void* user_data) {
    const auto* payload = event_json_utf8 == nullptr ? "{}" : event_json_utf8;
    native_event_action(topic_utf8, reinterpret_cast<const uint8_t*>(payload), std::strlen(payload),
                        user_data);
}

void remove_list_identity(PyObject* list, PyObject* value) {
    if (list == nullptr || value == nullptr || !PyList_Check(list))
        return;
    for (Py_ssize_t index = 0; index < PyList_GET_SIZE(list); ++index) {
        if (PyList_GET_ITEM(list, index) == value) {
            if (PySequence_DelItem(list, index) != 0)
                PyErr_Clear();
            return;
        }
    }
}

void unlink_timer_bridge(PluginContextObject* owner, NativeTimerBridge* bridge) {
    if (owner == nullptr || bridge == nullptr)
        return;
    NativeTimerBridge* previous = nullptr;
    NativeTimerBridge* current = owner->native_timer_bridges;
    while (current != nullptr && current != bridge) {
        previous = current;
        current = current->next;
    }
    if (current == nullptr)
        return;
    if (previous == nullptr)
        owner->native_timer_bridges = current->next;
    else
        previous->next = current->next;
}

void retire_timer_ledger(NativeTimerBridge* bridge) {
    if (bridge == nullptr || bridge->owner == nullptr)
        return;
    auto* owner = bridge->owner;
    PyObject* record =
        owner->timer_tokens == nullptr
            ? nullptr
            : PyDict_GetItemString(owner->timer_tokens, bridge->python_token.c_str());
    Py_XINCREF(record);
    if (owner->timer_tokens != nullptr &&
        PyDict_DelItemString(owner->timer_tokens, bridge->python_token.c_str()) != 0) {
        PyErr_Clear();
    }
    remove_list_identity(owner->timers, record);
    remove_list_identity(owner->callback_refs, bridge->callback);
    Py_XDECREF(record);
}

void invoke_timer_callback(NativeTimerBridge* bridge) {
    if (bridge == nullptr || bridge->callback == nullptr || !enter_callback(bridge->gate)) {
        return;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    if (bridge->one_shot && bridge->uses_loader && bridge->owner != nullptr &&
        bridge->owner->loader_context != nullptr && !bridge->loader_token.empty()) {
        const int32_t status = loader::sao_plugins_ctx_complete_timer(bridge->owner->loader_context,
                                                                      bridge->loader_token.c_str());
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
            PyErr_Clear();
    }
    PyObject* result = PyObject_CallNoArgs(bridge->callback);
    Py_XDECREF(result);
    if (PyErr_Occurred())
        PyErr_WriteUnraisable(bridge->callback);
    const bool retire_bridge = bridge->one_shot && bridge->owner != nullptr;
    bool teardown_owns_bridge = false;
    if (retire_bridge) {
        retire_timer_ledger(bridge);
        std::lock_guard lock(bridge->gate.mutex);
        bridge->gate.accepting = false;
        teardown_owns_bridge = bridge->owner->tearing_down;
    }
    leave_callback(bridge->gate);
    if (retire_bridge && !teardown_owns_bridge) {
        unlink_timer_bridge(bridge->owner, bridge);
        delete_timer_bridge(bridge);
    }
    drain_retired_contexts();
    PyGILState_Release(gil);
}

void SAO_PLUGINS_CALL native_loader_timer_action(void* user_data) {
    invoke_timer_callback(static_cast<NativeTimerBridge*>(user_data));
}

void SAO_SDK_CALL native_sdk_timer_action(sao_sdk_timer_token_t, void* user_data) {
    invoke_timer_callback(static_cast<NativeTimerBridge*>(user_data));
}

bool parse_hotkey(const char* text_utf8, uint32_t* out_key, uint32_t* out_modifiers) {
    if (out_key == nullptr || out_modifiers == nullptr || text_utf8 == nullptr)
        return false;
    *out_key = 0;
    *out_modifiers = 0;
    std::string token;
    for (const char* p = text_utf8;; ++p) {
        const char c = *p;
        if (c != '+' && c != '\0') {
            token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            continue;
        }
        if (token == "CTRL" || token == "CONTROL")
            *out_modifiers |= 1u << 0;
        else if (token == "ALT")
            *out_modifiers |= 1u << 1;
        else if (token == "SHIFT")
            *out_modifiers |= 1u << 2;
        else if (token == "WIN" || token == "WINDOWS")
            *out_modifiers |= 1u << 3;
        else if (token.size() >= 2 && token[0] == 'F') {
            const int number = std::atoi(token.c_str() + 1);
            if (number >= 1 && number <= 24)
                *out_key = 0x70u + static_cast<uint32_t>(number - 1);
        } else if (token.size() == 1 && std::isalnum(static_cast<unsigned char>(token[0]))) {
            *out_key = static_cast<uint32_t>(token[0]);
        }
        token.clear();
        if (c == '\0')
            break;
    }
    return *out_key != 0;
}

PyObject* json_stringify(PyObject* value) {
    PyObject* json_module = PyImport_ImportModule("json");
    if (json_module == nullptr)
        return nullptr;
    PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
    Py_DECREF(json_module);
    if (dumps == nullptr)
        return nullptr;
    PyObject* result = PyObject_CallOneArg(dumps, value);
    Py_DECREF(dumps);
    if (result != nullptr && PyUnicode_Check(result)) {
        Py_ssize_t length = 0;
        const char* text = PyUnicode_AsUTF8AndSize(result, &length);
        if (text == nullptr || length <= 0 ||
            !sdk_binding::sao_plugins_binding_validate_json_text(
                reinterpret_cast<const uint8_t*>(text), static_cast<size_t>(length))) {
            Py_DECREF(result);
            PyErr_SetString(PyExc_ValueError, "JSON exceeds the shared complexity or UTF-8 budget");
            return nullptr;
        }
    }
    return result;
}

constexpr std::size_t kMaximumMenuRows = 4096;
constexpr std::size_t kMaximumMenuStringBytes = 16U * 1024U;
constexpr std::size_t kMaximumMenuSnapshotBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumRememberedActions = 4096;
constexpr std::size_t kMaximumMenuJsonNestingDepth = 64;
constexpr std::size_t kMaximumMenuJsonNodes = 16384;

class BoundedMenuJsonSax final : public nlohmann::json::json_sax_t {
  public:
    bool null() override {
        return consume_node();
    }
    bool boolean(bool) override {
        return consume_node();
    }
    bool number_integer(number_integer_t) override {
        return consume_node();
    }
    bool number_unsigned(number_unsigned_t value) override {
        return value <= static_cast<number_unsigned_t>((std::numeric_limits<int64_t>::max)()) &&
               consume_node();
    }
    bool number_float(number_float_t, const string_t&) override {
        return consume_node();
    }
    bool string(string_t&) override {
        return consume_node();
    }
    bool binary(binary_t&) override {
        return consume_node();
    }
    bool start_object(std::size_t) override {
        return start_container();
    }
    bool key(string_t&) override {
        return true;
    }
    bool end_object() override {
        return end_container();
    }
    bool start_array(std::size_t) override {
        return start_container();
    }
    bool end_array() override {
        return end_container();
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

  private:
    bool consume_node() noexcept {
        if (nodes_ >= kMaximumMenuJsonNodes)
            return false;
        ++nodes_;
        return true;
    }

    bool start_container() noexcept {
        if (depth_ >= kMaximumMenuJsonNestingDepth || !consume_node())
            return false;
        ++depth_;
        return true;
    }

    bool end_container() noexcept {
        if (depth_ == 0)
            return false;
        --depth_;
        return true;
    }

    std::size_t depth_ = 0;
    std::size_t nodes_ = 0;
};

bool valid_menu_json(std::string_view value) noexcept {
    try {
        BoundedMenuJsonSax sax;
        return sdk_binding::sao_plugins_binding_validate_json_text(
                   reinterpret_cast<const uint8_t*>(value.data()), value.size()) &&
               nlohmann::json::sax_parse(value.begin(), value.end(), &sax);
    } catch (...) {
        return false;
    }
}

std::uint64_t stable_hash(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_suffix(std::string_view value) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(stable_hash(value)));
    return buffer;
}

PyObject* mapping_item(PyObject* mapping, const char* key) {
    PyObject* value = PyMapping_GetItemString(mapping, key);
    if (value == nullptr)
        PyErr_Clear();
    return value;
}

bool unicode_value(PyObject* value, bool required, std::string& out) {
    out.clear();
    if (value == nullptr || value == Py_None)
        return !required;
    if (!PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "menu text fields must be strings");
        return false;
    }
    Py_ssize_t length = 0;
    const char* text = PyUnicode_AsUTF8AndSize(value, &length);
    if (text == nullptr || (required && length == 0) || length < 0 ||
        static_cast<std::size_t>(length) > kMaximumMenuStringBytes) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "menu text field is invalid");
        }
        return false;
    }
    if (std::memchr(text, '\0', static_cast<std::size_t>(length)) != nullptr) {
        PyErr_SetString(PyExc_ValueError, "menu text fields must not contain embedded NUL bytes");
        return false;
    }
    out.assign(text, static_cast<std::size_t>(length));
    return true;
}

bool mapping_string(PyObject* mapping, const char* key, bool required, std::string& out) {
    PyObject* value = mapping_item(mapping, key);
    const bool ok = unicode_value(value, required, out);
    Py_XDECREF(value);
    return ok;
}

bool mapping_flag(PyObject* mapping, const char* key, bool fallback, bool& out) {
    PyObject* value = mapping_item(mapping, key);
    if (value == nullptr) {
        out = fallback;
        return true;
    }
    const int truth = PyObject_IsTrue(value);
    Py_DECREF(value);
    if (truth < 0)
        return false;
    out = truth != 0;
    return true;
}

bool menu_payload(PyObject* mapping, std::string& out) {
    PyObject* encoded = mapping_item(mapping, "payload_json");
    if (encoded != nullptr) {
        const bool ok = unicode_value(encoded, false, out);
        Py_DECREF(encoded);
        if (!ok)
            return false;
        if (!valid_menu_json(out)) {
            PyErr_SetString(PyExc_ValueError,
                            "menu payload_json is invalid or exceeds its complexity budget");
            return false;
        }
        return true;
    }
    PyObject* payload = mapping_item(mapping, "payload");
    if (payload == nullptr) {
        out = "{}";
        return true;
    }
    encoded = json_stringify(payload);
    Py_DECREF(payload);
    if (encoded == nullptr)
        return false;
    const bool ok = unicode_value(encoded, false, out);
    Py_DECREF(encoded);
    if (!ok)
        return false;
    if (!valid_menu_json(out)) {
        PyErr_SetString(PyExc_ValueError,
                        "menu payload_json is invalid or exceeds its complexity budget");
        return false;
    }
    return true;
}

std::string callable_identity(PyObject* command) {
    std::string result;
    for (const char* attribute : {"__module__", "__qualname__"}) {
        PyObject* value = PyObject_GetAttrString(command, attribute);
        if (value == nullptr) {
            PyErr_Clear();
            continue;
        }
        if (PyUnicode_Check(value)) {
            const char* text = PyUnicode_AsUTF8(value);
            if (text != nullptr) {
                if (!result.empty())
                    result.push_back(':');
                result.append(text);
            } else {
                PyErr_Clear();
            }
        }
        Py_DECREF(value);
    }
    if (result.empty()) {
        result = Py_TYPE(command)->tp_name == nullptr ? "callable" : Py_TYPE(command)->tp_name;
    }
    return result;
}

void clear_action_candidates(std::unordered_map<std::string, PyObject*>& actions) {
    for (auto& [_, callback] : actions)
        Py_XDECREF(callback);
    actions.clear();
}

bool build_menu_snapshot(NativeMenuBridge* bridge) {
    PyObject* result = PyObject_CallNoArgs(bridge->builder);
    if (result == nullptr)
        return false;
    PyObject* sequence = PySequence_Fast(result, "menu builder must return a list or sequence");
    Py_DECREF(result);
    if (sequence == nullptr)
        return false;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
    if (count < 0 || static_cast<std::size_t>(count) > kMaximumMenuRows) {
        Py_DECREF(sequence);
        PyErr_SetString(PyExc_ValueError, "menu builder returned too many rows");
        return false;
    }

    std::vector<NativeMenuRow> rows;
    std::unordered_map<std::string, PyObject*> actions;
    std::unordered_map<std::string, std::size_t> identity_occurrences;
    rows.reserve(static_cast<std::size_t>(count));
    std::size_t total_bytes = 0;
    bool ok = true;
    for (Py_ssize_t index = 0; ok && index < count; ++index) {
        PyObject* item = PySequence_Fast_GET_ITEM(sequence, index);
        if (!PyMapping_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "menu rows must be mappings");
            ok = false;
            break;
        }
        NativeMenuRow row;
        ok = mapping_string(item, "label", true, row.label) &&
             mapping_string(item, "icon", false, row.icon) &&
             menu_payload(item, row.payload_json) &&
             mapping_flag(item, "keep_menu_open", false, row.keep_menu_open) &&
             mapping_flag(item, "close_menu_before", false, row.close_menu_before);
        if (!ok)
            break;

        PyObject* command = mapping_item(item, "command");
        if (command != nullptr && command != Py_None && PyCallable_Check(command) == 0) {
            Py_DECREF(command);
            PyErr_SetString(PyExc_TypeError, "menu command must be callable or None");
            ok = false;
            break;
        }
        const bool callable = command != nullptr && command != Py_None;
        bool requested = true;
        ok = mapping_flag(item, "can_activate", true, requested);
        row.can_activate = callable && requested;
        if (!ok) {
            Py_XDECREF(command);
            break;
        }

        std::string explicit_identity;
        PyObject* explicit_value = mapping_item(item, "action_id");
        if (explicit_value == nullptr)
            explicit_value = mapping_item(item, "id");
        if (explicit_value != nullptr) {
            ok = unicode_value(explicit_value, true, explicit_identity);
            Py_DECREF(explicit_value);
        }
        if (!ok) {
            Py_XDECREF(command);
            break;
        }
        std::string identity = explicit_identity;
        if (identity.empty()) {
            const auto append_field = [&identity](std::string_view value) {
                identity.append(std::to_string(value.size()));
                identity.push_back(':');
                identity.append(value);
                identity.push_back('\n');
            };
            if (callable)
                append_field(callable_identity(command));
            else
                append_field({});
            append_field(row.label);
            append_field(row.icon);
            append_field(row.payload_json);
            identity.push_back(row.can_activate ? '1' : '0');
            identity.push_back(row.keep_menu_open ? '1' : '0');
            identity.push_back(row.close_menu_before ? '1' : '0');
            const std::size_t occurrence = identity_occurrences[identity]++;
            identity.append("#").append(std::to_string(occurrence));
        }
        row.action_id = "menu-action-" + hash_suffix(bridge->contribution_id + "\n" + identity);
        if (actions.find(row.action_id) != actions.end()) {
            Py_XDECREF(command);
            PyErr_SetString(PyExc_ValueError, "menu action identities must be unique");
            ok = false;
            break;
        }
        if (callable) {
            actions.emplace(row.action_id, command);
            command = nullptr;
        }
        Py_XDECREF(command);

        total_bytes += row.label.size() + row.icon.size() + row.action_id.size() +
                       row.payload_json.size() + bridge->contribution_id.size() +
                       bridge->name.size() + bridge->icon.size();
        if (total_bytes > kMaximumMenuSnapshotBytes) {
            PyErr_SetString(PyExc_ValueError, "menu builder snapshot exceeds its byte budget");
            ok = false;
            break;
        }
        rows.push_back(std::move(row));
    }
    Py_DECREF(sequence);

    std::size_t new_action_count = 0;
    if (ok) {
        for (const auto& [action_id, _] : actions) {
            if (bridge->actions.find(action_id) == bridge->actions.end()) {
                ++new_action_count;
            }
        }
    }
    if (!ok || new_action_count > kMaximumRememberedActions - bridge->actions.size()) {
        if (ok) {
            PyErr_SetString(PyExc_ValueError, "menu action history exceeds its budget");
        }
        clear_action_candidates(actions);
        return false;
    }

    if (bridge->revision == 0 || bridge->rows != rows) {
        if (bridge->revision == std::numeric_limits<std::uint64_t>::max()) {
            clear_action_candidates(actions);
            PyErr_SetString(PyExc_OverflowError, "menu snapshot revision exhausted");
            return false;
        }
        ++bridge->revision;
    }
    for (auto& [action_id, callback] : actions) {
        const auto existing = bridge->actions.find(action_id);
        if (existing != bridge->actions.end()) {
            Py_XDECREF(existing->second);
            existing->second = callback;
        } else {
            bridge->actions.emplace(action_id, callback);
        }
        callback = nullptr;
    }
    bridge->rows = std::move(rows);
    return true;
}

#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
int32_t SAO_PLUGINS_CALL native_menu_snapshot(loader::entity_menu_row* rows, std::uint32_t capacity,
                                              std::uint32_t* out_count, std::uint64_t* out_revision,
                                              void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr ||
        (capacity > 0 && rows == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* bridge = static_cast<NativeMenuBridge*>(user_data);
    if (!enter_callback(bridge->gate))
        return loader::SAO_PLUGINS_ERR_BUSY;
    PyGILState_STATE gil = PyGILState_Ensure();
    if (rows == nullptr && !build_menu_snapshot(bridge)) {
        PyErr_Clear();
        leave_callback(bridge->gate);
        drain_retired_contexts();
        PyGILState_Release(gil);
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_count = static_cast<std::uint32_t>(bridge->rows.size());
    *out_revision = bridge->revision;
    if (capacity < bridge->rows.size()) {
        leave_callback(bridge->gate);
        drain_retired_contexts();
        PyGILState_Release(gil);
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < bridge->rows.size(); ++index) {
        const auto& source = bridge->rows[index];
        rows[index] = {
            sizeof(loader::entity_menu_row),
            bridge->contribution_id.c_str(),
            bridge->name.c_str(),
            bridge->icon.c_str(),
            bridge->priority,
            source.label.c_str(),
            source.icon.c_str(),
            source.action_id.c_str(),
            source.payload_json.c_str(),
            static_cast<std::uint8_t>(source.can_activate),
            static_cast<std::uint8_t>(source.keep_menu_open),
            static_cast<std::uint8_t>(source.close_menu_before),
            {},
        };
    }
    leave_callback(bridge->gate);
    drain_retired_contexts();
    PyGILState_Release(gil);
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL native_menu_action(const char* action_id_utf8, const char*,
                                            void* user_data) {
    if (action_id_utf8 == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* bridge = static_cast<NativeMenuBridge*>(user_data);
    if (!enter_callback(bridge->gate))
        return loader::SAO_PLUGINS_ERR_BUSY;
    PyGILState_STATE gil = PyGILState_Ensure();
    const auto found = bridge->actions.find(action_id_utf8);
    if (found == bridge->actions.end() || found->second == nullptr) {
        leave_callback(bridge->gate);
        drain_retired_contexts();
        PyGILState_Release(gil);
        return SAO_ERR_HANDLE_INVALID;
    }
    PyObject* result = PyObject_CallNoArgs(found->second);
    if (result == nullptr) {
        PyErr_Clear();
        leave_callback(bridge->gate);
        drain_retired_contexts();
        PyGILState_Release(gil);
        return SAO_ERR_OS_CALL_FAILED;
    }
    Py_DECREF(result);
    leave_callback(bridge->gate);
    drain_retired_contexts();
    PyGILState_Release(gil);
    return SAO_OK;
}

PyObject* parse_action_payload(const char* payload_json_utf8) {
    PyObject* json_module = PyImport_ImportModule("json");
    if (json_module == nullptr)
        return nullptr;
    PyObject* loads = PyObject_GetAttrString(json_module, "loads");
    Py_DECREF(json_module);
    if (loads == nullptr)
        return nullptr;
    PyObject* payload = PyObject_CallFunction(loads, "s", payload_json_utf8);
    Py_DECREF(loads);
    return payload;
}

PyObject* serialize_action_result(PyObject* value) {
    PyObject* json_module = PyImport_ImportModule("json");
    if (json_module == nullptr)
        return nullptr;
    PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
    Py_DECREF(json_module);
    if (dumps == nullptr)
        return nullptr;
    PyObject* args = PyTuple_Pack(1, value);
    PyObject* kwargs = PyDict_New();
    if (args == nullptr || kwargs == nullptr ||
        PyDict_SetItemString(kwargs, "allow_nan", Py_False) != 0 ||
        PyDict_SetItemString(kwargs, "ensure_ascii", Py_False) != 0) {
        Py_XDECREF(args);
        Py_XDECREF(kwargs);
        Py_DECREF(dumps);
        return nullptr;
    }
    PyObject* serialized = PyObject_Call(dumps, args, kwargs);
    Py_DECREF(kwargs);
    Py_DECREF(args);
    Py_DECREF(dumps);
    return serialized;
}

int32_t SAO_PLUGINS_CALL native_action_v2(const char* action_id_utf8, const char* payload_json_utf8,
                                          loader::entity_action_result_sink_v2_fn result_sink,
                                          void* result_sink_user_data, void* user_data) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr || result_sink == nullptr ||
        user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* bridge = static_cast<NativeActionBridge*>(user_data);
    if (!enter_callback(bridge->gate))
        return loader::SAO_PLUGINS_ERR_BUSY;
    PyGILState_STATE gil = PyGILState_Ensure();
    const auto finish = [bridge, gil](int32_t status) {
        leave_callback(bridge->gate);
        drain_retired_contexts();
        PyGILState_Release(gil);
        return status;
    };
    try {
        PyObject* action = PyUnicode_FromString(action_id_utf8);
        if (action == nullptr) {
            PyErr_Clear();
            return finish(SAO_ERR_OS_CALL_FAILED);
        }
        PyObject* payload = parse_action_payload(payload_json_utf8);
        if (payload == nullptr) {
            Py_DECREF(action);
            PyErr_Clear();
            return finish(SAO_ERR_INVALID_ARGUMENT);
        }
        PyObject* result = PyObject_CallFunctionObjArgs(bridge->callback, action, payload, nullptr);
        Py_DECREF(payload);
        Py_DECREF(action);
        if (result == nullptr) {
            PyErr_Clear();
            return finish(SAO_ERR_OS_CALL_FAILED);
        }

        loader::entity_action_result_v2 native_result{
            sizeof(loader::entity_action_result_v2),
            loader::kEntityActionAbiVersion2,
            static_cast<std::uint8_t>(result == Py_None ? 0 : 1),
            {},
            nullptr};
        PyObject* serialized = nullptr;
        if (result != Py_None) {
            serialized = serialize_action_result(result);
            if (serialized == nullptr || !PyUnicode_Check(serialized)) {
                Py_XDECREF(serialized);
                Py_DECREF(result);
                PyErr_Clear();
                return finish(SAO_ERR_INVALID_ARGUMENT);
            }
            Py_ssize_t result_size = 0;
            const char* result_json = PyUnicode_AsUTF8AndSize(serialized, &result_size);
            if (result_json == nullptr || result_size < 0 ||
                static_cast<std::size_t>(result_size) >
                    loader::kMaximumEntityActionResultJsonBytes ||
                std::memchr(result_json, '\0', static_cast<std::size_t>(result_size)) != nullptr ||
                !valid_menu_json(
                    std::string_view(result_json, static_cast<std::size_t>(result_size)))) {
                Py_DECREF(serialized);
                Py_DECREF(result);
                PyErr_Clear();
                return finish(SAO_ERR_INVALID_ARGUMENT);
            }
            native_result.result_json_utf8 = result_json;
        }
        const int32_t status = result_sink(&native_result, result_sink_user_data);
        Py_XDECREF(serialized);
        Py_DECREF(result);
        return finish(status);
    } catch (...) {
        PyErr_Clear();
        return finish(SAO_ERR_OS_CALL_FAILED);
    }
}

int32_t register_action_provider(PluginContextObject* self, NativeActionBridge* bridge) noexcept {
    if (self == nullptr || bridge == nullptr || self->loader_context == nullptr ||
        bridge->callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        loader::context_entity_provider_descriptor_v3 provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = bridge->provider_id.c_str();
        provider.action_handler_v2 = native_action_v2;
        provider.action_user_data = bridge;
        provider.flags = loader::kContextEntityProviderV3ActionOnly;
        return loader::sao_plugins_ctx_register_entity_provider_v3(self->loader_context, &provider);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void mark_action_registration(NativeActionBridge* current,
                              NativeActionBridge* registered) noexcept {
    for (auto* bridge = current; bridge != nullptr; bridge = bridge->previous)
        bridge->registered = bridge == registered;
}

void erase_menu_bridges_since(PluginContextObject* self, uint64_t sequence) {
    auto** link = &self->native_menu_bridges;
    while (*link != nullptr) {
        auto* bridge = *link;
        if (bridge->sequence < sequence) {
            link = &bridge->next;
            continue;
        }
        *link = bridge->next;
        remove_list_identity(self->menus, bridge->ledger_record);
        Py_XDECREF(bridge->builder);
        Py_XDECREF(bridge->ledger_record);
        for (const auto& [_, callback] : bridge->actions)
            Py_XDECREF(callback);
        delete bridge;
    }
}

void erase_enable_scoped_menu_bridges(PluginContextObject* self) {
    auto** link = &self->native_menu_bridges;
    while (*link != nullptr) {
        auto* bridge = *link;
        if (!bridge->enable_scoped) {
            link = &bridge->next;
            continue;
        }
        *link = bridge->next;
        remove_list_identity(self->menus, bridge->ledger_record);
        Py_XDECREF(bridge->builder);
        Py_XDECREF(bridge->ledger_record);
        for (const auto& [_, callback] : bridge->actions)
            Py_XDECREF(callback);
        delete bridge;
    }
}

int32_t begin_enable_resource_checkpoint(PluginContextObject* self,
                                         uint64_t* out_checkpoint) noexcept {
    if (self == nullptr || out_checkpoint == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_checkpoint = 0;
    if (self->tearing_down)
        return loader::SAO_PLUGINS_ERR_BUSY;
    if (self->active_enable_checkpoint != 0)
        return loader::SAO_PLUGINS_ERR_BUSY;
    if (self->native_action_bridge != nullptr && !self->native_action_bridge->registered) {
        const int32_t status = register_action_provider(self, self->native_action_bridge);
        if (status != SAO_OK)
            return status;
        mark_action_registration(self->native_action_bridge, self->native_action_bridge);
    }
    if (self->next_enable_checkpoint == 0 ||
        self->next_enable_checkpoint == (std::numeric_limits<uint64_t>::max)()) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    const uint64_t checkpoint = self->next_enable_checkpoint++;
    self->active_enable_checkpoint = checkpoint;
    self->enable_checkpoint_sequence = self->next_resource_sequence;
    self->enable_checkpoint_action = self->native_action_bridge;
    *out_checkpoint = checkpoint;
    return SAO_OK;
}

int32_t commit_enable_resources(PluginContextObject* self, uint64_t checkpoint) noexcept {
    if (self == nullptr || checkpoint == 0 || self->active_enable_checkpoint != checkpoint)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        for (auto* menu = self->native_menu_bridges; menu != nullptr; menu = menu->next) {
            if (menu->sequence >= self->enable_checkpoint_sequence)
                menu->enable_scoped = true;
        }
        if (self->native_action_bridge != self->enable_checkpoint_action) {
            auto* current = self->native_action_bridge;
            if (current == nullptr)
                return SAO_ERR_INVALID_ARGUMENT;
            auto* replaced = current->previous;
            current->previous = self->enable_checkpoint_action;
            current->enable_scoped = true;
            delete_action_prefix(replaced, self->enable_checkpoint_action);
        }
        self->active_enable_checkpoint = 0;
        self->enable_checkpoint_sequence = 0;
        self->enable_checkpoint_action = nullptr;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t rollback_enable_resources(PluginContextObject* self, uint64_t checkpoint) noexcept {
    if (self == nullptr || checkpoint == 0 || self->active_enable_checkpoint != checkpoint)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        NativeActionBridge* const action_base = self->enable_checkpoint_action;
        const bool action_changed = self->native_action_bridge != action_base;
        if (action_changed && action_base != nullptr) {
            const int32_t status = register_action_provider(self, action_base);
            if (status != SAO_OK)
                return status;
            mark_action_registration(self->native_action_bridge, action_base);
        }

        std::vector<std::string> provider_ids;
        for (auto* menu = self->native_menu_bridges; menu != nullptr; menu = menu->next) {
            if (menu->sequence >= self->enable_checkpoint_sequence && menu->registered)
                provider_ids.push_back(menu->qualified_provider_id);
        }
        if (action_changed && action_base == nullptr && self->native_action_bridge != nullptr &&
            self->native_action_bridge->registered) {
            provider_ids.push_back(self->native_action_bridge->qualified_provider_id);
        }
        const int32_t unregister_status = unregister_provider_ids(self, provider_ids);
        if (unregister_status != SAO_OK)
            return unregister_status;

        erase_menu_bridges_since(self, self->enable_checkpoint_sequence);
        while (self->native_action_bridge != action_base) {
            auto* bridge = self->native_action_bridge;
            if (bridge == nullptr)
                return SAO_ERR_INVALID_ARGUMENT;
            self->native_action_bridge = bridge->previous;
            delete_action_bridge(bridge);
        }
        if (action_base != nullptr)
            action_base->registered = true;
        self->active_enable_checkpoint = 0;
        self->enable_checkpoint_sequence = 0;
        self->enable_checkpoint_action = nullptr;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t remove_enable_resources(PluginContextObject* self) noexcept {
    if (self == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (self->active_enable_checkpoint != 0)
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        NativeActionBridge* action_base = self->native_action_bridge;
        while (action_base != nullptr && action_base->enable_scoped)
            action_base = action_base->previous;
        bool action_changed = self->native_action_bridge != action_base;
        if (action_changed && action_base != nullptr) {
            auto* current = self->native_action_bridge;
            Py_SETREF(current->callback, Py_NewRef(action_base->callback));
            current->enable_scoped = false;
            current->sequence = action_base->sequence;
            auto* persistent_previous = action_base->previous;
            delete_action_prefix(current->previous, action_base);
            delete_action_bridge(action_base);
            current->previous = persistent_previous;
            action_base = current;
            action_changed = false;
        }

        std::vector<std::string> provider_ids;
        for (auto* menu = self->native_menu_bridges; menu != nullptr; menu = menu->next) {
            if (menu->enable_scoped && menu->registered)
                provider_ids.push_back(menu->qualified_provider_id);
        }
        if (action_changed && self->native_action_bridge != nullptr &&
            self->native_action_bridge->registered) {
            provider_ids.push_back(self->native_action_bridge->qualified_provider_id);
        }
        const int32_t unregister_status = unregister_provider_ids(self, provider_ids);
        if (unregister_status != SAO_OK)
            return unregister_status;

        erase_enable_scoped_menu_bridges(self);
        while (self->native_action_bridge != action_base) {
            auto* bridge = self->native_action_bridge;
            self->native_action_bridge = bridge->previous;
            delete_action_bridge(bridge);
        }
        if (action_base != nullptr)
            action_base->registered = true;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}
#endif

// ── PluginContext 生命周期 ──────────────────────────────────

PyObject* PluginContext_new(PyTypeObject* type, PyObject* /*args*/, PyObject* /*kwds*/) {
    PluginContextObject* self = reinterpret_cast<PluginContextObject*>(type->tp_alloc(type, 0));
    if (self == nullptr)
        return nullptr;
    self->weakreflist = nullptr;
    self->opaque_handle = nullptr;
    self->loader_context = nullptr;
    self->loader_context_lease_held = false;
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
    self->subscription_tokens = nullptr;
    self->timer_tokens = nullptr;
    self->notification_tokens = nullptr;
    self->engines = nullptr;
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
    self->subscription_tokens = PyDict_New();
    self->timer_tokens = PyDict_New();
    self->notification_tokens = PyDict_New();
    self->engines = PyDict_New();
    self->extras = PyDict_New();
    self->callback_refs = PyList_New(0);
    self->native_panel_bridges = nullptr;
    self->native_hotkey_bridges = nullptr;
    self->native_event_bridges = nullptr;
    self->native_timer_bridges = nullptr;
    self->native_notify_bridges = nullptr;
    self->native_menu_bridges = nullptr;
    self->native_action_bridge = nullptr;
    self->next_resource_sequence = 1;
    self->next_python_token = 1;
    self->next_enable_checkpoint = 1;
    self->active_enable_checkpoint = 0;
    self->enable_checkpoint_sequence = 0;
    self->enable_checkpoint_action = nullptr;
    self->tearing_down = false;
    self->teardown_deferred = false;
    self->retired = false;
    self->retired_next = nullptr;
    if (self->plugin_id == nullptr || self->base_dir == nullptr || self->assets_path == nullptr ||
        self->web_path == nullptr || self->panels == nullptr || self->hotkeys == nullptr ||
        self->subscriptions == nullptr || self->published == nullptr || self->logs == nullptr ||
        self->timers == nullptr || self->notifications == nullptr || self->settings == nullptr ||
        self->menus == nullptr || self->subscription_tokens == nullptr ||
        self->timer_tokens == nullptr || self->notification_tokens == nullptr ||
        self->engines == nullptr || self->extras == nullptr || self->callback_refs == nullptr) {
        Py_DECREF(self);
        return nullptr;
    }
    return reinterpret_cast<PyObject*>(self);
}

int PluginContext_init(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"plugin_id", "base_dir", "opaque_handle", "controlled_test_shim",
                                   nullptr};
    const char* pid = "";
    const char* dir_ = "";
    Py_ssize_t handle_int = 0;
    int controlled_test_shim = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssnp", const_cast<char**>(kwlist), &pid, &dir_,
                                     &handle_int, &controlled_test_shim)) {
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
    Py_VISIT(self->subscription_tokens);
    Py_VISIT(self->timer_tokens);
    Py_VISIT(self->notification_tokens);
    Py_VISIT(self->engines);
    Py_VISIT(self->extras);
    Py_VISIT(self->callback_refs);
    for (auto* bridge = self->native_panel_bridges; bridge != nullptr; bridge = bridge->next)
        Py_VISIT(bridge->callback);
    for (auto* bridge = self->native_hotkey_bridges; bridge != nullptr; bridge = bridge->next)
        Py_VISIT(bridge->callback);
    for (auto* bridge = self->native_event_bridges; bridge != nullptr; bridge = bridge->next)
        Py_VISIT(bridge->callback);
    for (auto* bridge = self->native_timer_bridges; bridge != nullptr; bridge = bridge->next) {
        if (bridge->owner_ref_held)
            Py_VISIT(reinterpret_cast<PyObject*>(bridge->owner));
        Py_VISIT(bridge->callback);
    }
    for (auto* bridge = self->native_menu_bridges; bridge != nullptr; bridge = bridge->next) {
        Py_VISIT(bridge->builder);
        Py_VISIT(bridge->ledger_record);
        for (const auto& [_, callback] : bridge->actions)
            Py_VISIT(callback);
    }
    for (auto* bridge = self->native_action_bridge; bridge != nullptr; bridge = bridge->previous)
        Py_VISIT(bridge->callback);
    return 0;
}

int PluginContext_clear(PluginContextObject* self) {
    try {
        if (release_native_bridges(self) != SAO_OK)
            return 0;
    } catch (...) {
        return 0;
    }
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
    Py_CLEAR(self->subscription_tokens);
    Py_CLEAR(self->timer_tokens);
    Py_CLEAR(self->notification_tokens);
    Py_CLEAR(self->engines);
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
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = release_native_bridges(self);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) {
        retire_plugin_context(self);
        return;
    }
    clear_callback_ledgers(self);
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
PyObject* PluginContext_get_should_stop_attr(PluginContextObject* self, void*) {
    if (self->loader_context == nullptr) {
        if (self->controlled_test_shim)
            Py_RETURN_FALSE;
        return status_error("should_stop", loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    return PyBool_FromLong(loader::sao_plugins_ctx_should_stop(self->loader_context));
}
// EngineAccess compatibility methods are exposed directly by PluginContext.
PyObject* PluginContext_get_engine_attr(PluginContextObject* self, void*) {
    return Py_NewRef(reinterpret_cast<PyObject*>(self));
}
PyObject* PluginContext_get_ui_attr(PluginContextObject* self, void*) {
    PyObject* v = PyDict_GetItemString(self->extras, "ui");
    if (v != nullptr)
        return Py_NewRef(v);
    PyObject* d = PyDict_New();
    if (d == nullptr)
        return nullptr;
    if (PyDict_SetItemString(self->extras, "ui", d) != 0) {
        Py_DECREF(d);
        return nullptr;
    }
    return d;
}
PyObject* PluginContext_get_mem_attr(PluginContextObject* self, void*) {
    PyObject* v = PyDict_GetItemString(self->extras, "mem");
    if (v != nullptr)
        return Py_NewRef(v);
    PyObject* d = PyDict_New();
    if (d == nullptr)
        return nullptr;
    if (PyDict_SetItemString(self->extras, "mem", d) != 0) {
        Py_DECREF(d);
        return nullptr;
    }
    return d;
}
PyObject* PluginContext_get_owner_attr(PluginContextObject* self, void*) {
    // Phase 1 (P0 compat shim): owner facade — 无条件返回一个真实 SimpleNamespace,
    // 让老 Python 插件 (star_resonance 等) 的 owner._x=42 / getattr(owner, '_x', None)
    // 属性存取直接生效, 不再触发 `if owner is None: return` 短路。
    // 之前只在 controlled_test_shim=True 时生成; 现在默认路径也生成。
    PyObject* owner = PyDict_GetItemString(self->extras, "engine_owner");
    if (owner != nullptr)
        return Py_NewRef(owner);
    PyObject* types = PyImport_ImportModule("types");
    if (types == nullptr)
        return nullptr;
    PyObject* namespace_type = PyObject_GetAttrString(types, "SimpleNamespace");
    Py_DECREF(types);
    if (namespace_type == nullptr)
        return nullptr;
    owner = PyObject_CallNoArgs(namespace_type);
    Py_DECREF(namespace_type);
    if (owner == nullptr)
        return nullptr;
    if (PyDict_SetItemString(self->extras, "engine_owner", owner) != 0) {
        Py_DECREF(owner);
        return nullptr;
    }
    return owner;
}


PyGetSetDef PluginContext_getset[] = {
    {"plugin_id", reinterpret_cast<getter>(PluginContext_get_plugin_id_attr), nullptr, nullptr,
     nullptr},
    {"base_dir", reinterpret_cast<getter>(PluginContext_get_base_dir_attr), nullptr, nullptr,
     nullptr},
    {"path", reinterpret_cast<getter>(PluginContext_get_path_attr), nullptr, nullptr, nullptr},
    {"assets_path", reinterpret_cast<getter>(PluginContext_get_assets_path_attr), nullptr, nullptr,
     nullptr},
    {"web_path", reinterpret_cast<getter>(PluginContext_get_web_path_attr), nullptr, nullptr,
     nullptr},
    {"should_stop", reinterpret_cast<getter>(PluginContext_get_should_stop_attr), nullptr, nullptr,
     nullptr},
    {"engine", reinterpret_cast<getter>(PluginContext_get_engine_attr), nullptr, nullptr, nullptr},
    {"ui", reinterpret_cast<getter>(PluginContext_get_ui_attr), nullptr, nullptr, nullptr},
    {"mem", reinterpret_cast<getter>(PluginContext_get_mem_attr), nullptr, nullptr, nullptr},
    {"owner", reinterpret_cast<getter>(PluginContext_get_owner_attr), nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

// ── Method 实现 ─────────────────────────────────────────────

// register_ui_panel(panel_id, meta[, render][, on_action]) — 记账
PyObject* PluginContext_register_ui_panel(PluginContextObject* self, PyObject* args,
                                          PyObject* kwds) {
    if (!require_active_context(self))
        return nullptr;
    static const char* kwlist[] = {"panel_id", "meta", "render", "on_action", nullptr};
    const char* panel_id = nullptr;
    PyObject* meta = nullptr;
    PyObject* render = Py_None;
    PyObject* on_action = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|OO", const_cast<char**>(kwlist), &panel_id,
                                     &meta, &render, &on_action)) {
        return nullptr;
    }
    if ((render != Py_None && !require_callable(render, "render")) ||
        (on_action != Py_None && !require_callable(on_action, "on_action"))) {
        return nullptr;
    }

    const Py_ssize_t panel_ledger_size = PyList_GET_SIZE(self->panels);
    const Py_ssize_t callback_ledger_size = PyList_GET_SIZE(self->callback_refs);
    PyObject* result = PyUnicode_FromString(panel_id);
    PyObject* record = PyDict_New();
    if (result == nullptr || record == nullptr ||
        !set_dict_item_steal(record, "panel_id", PyUnicode_FromString(panel_id)) ||
        PyDict_SetItemString(record, "meta", meta) != 0 ||
        PyDict_SetItemString(record, "render", render) != 0 ||
        PyDict_SetItemString(record, "on_action", on_action) != 0 ||
        !set_dict_item_steal(record, "native_handle", PyLong_FromVoidPtr(nullptr)) ||
        PyList_Append(self->panels, record) != 0 || !keep_callback_ref(self, render) ||
        !keep_callback_ref(self, on_action)) {
        truncate_list(self->callback_refs, callback_ledger_size);
        truncate_list(self->panels, panel_ledger_size);
        Py_XDECREF(record);
        Py_XDECREF(result);
        return nullptr;
    }

    SaoSdkContext* ctx = native_context(self);
    sao_sdk_ui_panel_t native_panel = nullptr;
    std::unique_ptr<NativePanelBridge> bridge;
    if (ctx != nullptr) {
        PyObject* json = json_stringify(meta);
        if (json == nullptr) {
            truncate_list(self->callback_refs, callback_ledger_size);
            truncate_list(self->panels, panel_ledger_size);
            Py_DECREF(record);
            Py_DECREF(result);
            return nullptr;
        }
        const char* json_utf8 = PyUnicode_AsUTF8(json);
        if (json_utf8 == nullptr) {
            Py_DECREF(json);
            truncate_list(self->callback_refs, callback_ledger_size);
            truncate_list(self->panels, panel_ledger_size);
            Py_DECREF(record);
            Py_DECREF(result);
            return nullptr;
        }
        bridge.reset(new (std::nothrow) NativePanelBridge{});
        if (bridge == nullptr) {
            Py_DECREF(json);
            truncate_list(self->callback_refs, callback_ledger_size);
            truncate_list(self->panels, panel_ledger_size);
            Py_DECREF(record);
            Py_DECREF(result);
            return PyErr_NoMemory();
        }
        if (on_action != Py_None)
            bridge->callback = Py_NewRef(on_action);
        const sao_sdk_status_t rc = sao_sdk_ui_register_panel(
            ctx, panel_id, panel_id, reinterpret_cast<const uint8_t*>(json_utf8),
            std::strlen(json_utf8), bridge->callback == nullptr ? nullptr : native_panel_action,
            bridge.get(), &native_panel);
        Py_DECREF(json);
        if (rc != SAO_SDK_OK) {
            Py_XDECREF(bridge->callback);
            bridge->callback = nullptr;
            truncate_list(self->callback_refs, callback_ledger_size);
            truncate_list(self->panels, panel_ledger_size);
            Py_DECREF(record);
            Py_DECREF(result);
            PyErr_Format(PyExc_RuntimeError, "native panel registration failed: %d", rc);
            return nullptr;
        }
        bridge->panel = native_panel;
        bridge->sequence = self->next_resource_sequence++;
        bridge->next = self->native_panel_bridges;
        self->native_panel_bridges = bridge.release();
    }

    PyObject* native_handle = PyLong_FromVoidPtr(native_panel);
    if (native_handle == nullptr ||
        PyDict_SetItemString(record, "native_handle", native_handle) != 0) {
        Py_XDECREF(native_handle);
        PyObject* error_type = nullptr;
        PyObject* error_value = nullptr;
        PyObject* error_traceback = nullptr;
        PyErr_Fetch(&error_type, &error_value, &error_traceback);
        if (self->native_panel_bridges != nullptr &&
            self->native_panel_bridges->panel == native_panel) {
            auto* rollback = self->native_panel_bridges;
            self->native_panel_bridges = rollback->next;
            if (ctx != nullptr && native_panel != nullptr)
                (void)sao_sdk_unregister_ui_panel(ctx, native_panel);
            Py_XDECREF(rollback->callback);
            delete rollback;
        }
        truncate_list(self->callback_refs, callback_ledger_size);
        truncate_list(self->panels, panel_ledger_size);
        Py_DECREF(record);
        Py_DECREF(result);
        PyErr_Restore(error_type, error_value, error_traceback);
        return nullptr;
    }
    Py_DECREF(native_handle);
    Py_DECREF(record);
    return result;
}

// add_hotkey / register_hotkey(hotkey_id, callback[, default_key][, label])
PyObject* PluginContext_add_hotkey(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    if (!require_active_context(self))
        return nullptr;
    static const char* kwlist[] = {"hotkey_id", "callback", "default_key", "label", nullptr};
    const char* hotkey_id = nullptr;
    PyObject* callback = nullptr;
    const char* default_key = "";
    const char* label = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|ss", const_cast<char**>(kwlist), &hotkey_id,
                                     &callback, &default_key, &label)) {
        return nullptr;
    }
    if (!require_callable(callback, "callback"))
        return nullptr;

    SaoSdkContext* ctx = native_context(self);
    sao_sdk_hotkey_id_t native_hotkey = 0;
    uint32_t virtual_key = 0;
    uint32_t modifiers = 0;
    if (ctx != nullptr) {
        if (!parse_hotkey(default_key, &virtual_key, &modifiers)) {
            PyErr_SetString(PyExc_ValueError, "default_key must be a supported key chord");
            return nullptr;
        }
    }

    const Py_ssize_t hotkey_ledger_size = PyList_GET_SIZE(self->hotkeys);
    const Py_ssize_t callback_ledger_size = PyList_GET_SIZE(self->callback_refs);
    PyObject* record = PyDict_New();
    if (record == nullptr ||
        !set_dict_item_steal(record, "hotkey_id", PyUnicode_FromString(hotkey_id)) ||
        !set_dict_item_steal(record, "default_key", PyUnicode_FromString(default_key)) ||
        !set_dict_item_steal(record, "label", PyUnicode_FromString(label)) ||
        PyDict_SetItemString(record, "callback", callback) != 0 ||
        !set_dict_item_steal(record, "native_handle", PyLong_FromUnsignedLongLong(0)) ||
        PyList_Append(self->hotkeys, record) != 0 || !keep_callback_ref(self, callback)) {
        truncate_list(self->callback_refs, callback_ledger_size);
        truncate_list(self->hotkeys, hotkey_ledger_size);
        Py_XDECREF(record);
        return nullptr;
    }

    std::unique_ptr<NativeHotkeyBridge> bridge;
    if (ctx != nullptr) {
        bridge.reset(new (std::nothrow) NativeHotkeyBridge{});
        if (bridge == nullptr) {
            truncate_list(self->callback_refs, callback_ledger_size);
            truncate_list(self->hotkeys, hotkey_ledger_size);
            Py_DECREF(record);
            return PyErr_NoMemory();
        }
        bridge->callback = Py_NewRef(callback);
        SaoSdkHotkeySpec spec{};
        spec.binding_id_utf8 = hotkey_id;
        spec.virtual_key = virtual_key;
        spec.modifiers = modifiers;
        spec.enforce_ctrl_prefix = (modifiers & (1u << 0)) != 0;
        const sao_sdk_status_t rc =
            sao_sdk_register_hotkey(ctx, &spec, native_hotkey_action, bridge.get(), &native_hotkey);
        if (rc != SAO_SDK_OK) {
            Py_DECREF(bridge->callback);
            bridge->callback = nullptr;
            truncate_list(self->callback_refs, callback_ledger_size);
            truncate_list(self->hotkeys, hotkey_ledger_size);
            Py_DECREF(record);
            PyErr_Format(PyExc_RuntimeError, "native hotkey registration failed: %d", rc);
            return nullptr;
        }
        bridge->token = native_hotkey;
        bridge->sequence = self->next_resource_sequence++;
        bridge->next = self->native_hotkey_bridges;
        self->native_hotkey_bridges = bridge.release();
    }

    PyObject* native_handle = PyLong_FromUnsignedLongLong(native_hotkey);
    if (native_handle == nullptr ||
        PyDict_SetItemString(record, "native_handle", native_handle) != 0) {
        Py_XDECREF(native_handle);
        PyObject* error_type = nullptr;
        PyObject* error_value = nullptr;
        PyObject* error_traceback = nullptr;
        PyErr_Fetch(&error_type, &error_value, &error_traceback);
        if (self->native_hotkey_bridges != nullptr &&
            self->native_hotkey_bridges->token == native_hotkey) {
            auto* rollback = self->native_hotkey_bridges;
            self->native_hotkey_bridges = rollback->next;
            if (ctx != nullptr && native_hotkey != 0)
                (void)sao_sdk_unregister_hotkey(ctx, native_hotkey);
            Py_XDECREF(rollback->callback);
            delete rollback;
        }
        truncate_list(self->callback_refs, callback_ledger_size);
        truncate_list(self->hotkeys, hotkey_ledger_size);
        Py_DECREF(record);
        PyErr_Restore(error_type, error_value, error_traceback);
        return nullptr;
    }
    Py_DECREF(native_handle);
    Py_DECREF(record);
    Py_RETURN_NONE;
}

// subscribe(event_type, callback)
PyObject* PluginContext_subscribe(PluginContextObject* self, PyObject* args) {
    const char* event_type = nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &event_type, &callback))
        return nullptr;
    if (!require_callable(callback, "callback"))
        return nullptr;
    if (self->tearing_down) {
        PyErr_SetString(PyExc_RuntimeError, "plugin context is tearing down");
        return nullptr;
    }
    auto* bridge = new (std::nothrow) NativeEventBridge{};
    if (bridge == nullptr)
        return PyErr_NoMemory();
    bridge->callback = Py_NewRef(callback);
    bridge->python_token = next_token(self, "event_");

    SaoSdkContext* ctx = native_context(self);
    int32_t status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    if (self->loader_context != nullptr) {
        uint32_t token = 0;
        status = loader::sao_plugins_ctx_subscribe(self->loader_context, event_type,
                                                   native_loader_event_action, bridge, &token);
        bridge->token = token;
    } else if (ctx != nullptr && ctx->event != nullptr) {
        status =
            sao_sdk_subscribe_event(ctx, event_type, native_event_action, bridge, &bridge->token);
    } else if (self->controlled_test_shim) {
        status = SAO_OK;
    }
    if (status != SAO_OK) {
        Py_DECREF(bridge->callback);
        delete bridge;
        return status_error("event subscription", status);
    }

    PyObject* rec = PyDict_New();
    if (rec == nullptr ||
        !set_dict_item_steal(rec, "event_type", PyUnicode_FromString(event_type)) ||
        PyDict_SetItemString(rec, "callback", callback) != 0 ||
        !set_dict_item_steal(rec, "native_handle", PyLong_FromUnsignedLongLong(bridge->token)) ||
        PyDict_SetItemString(self->subscription_tokens, bridge->python_token.c_str(), rec) != 0 ||
        PyList_Append(self->subscriptions, rec) != 0) {
        PyObject* error_type = nullptr;
        PyObject* error_value = nullptr;
        PyObject* error_traceback = nullptr;
        PyErr_Fetch(&error_type, &error_value, &error_traceback);
        if (PyDict_DelItemString(self->subscription_tokens, bridge->python_token.c_str()) != 0) {
            PyErr_Clear();
        }
        if (self->loader_context != nullptr && bridge->token != 0) {
            (void)loader::sao_plugins_ctx_unsubscribe(self->loader_context,
                                                      static_cast<uint32_t>(bridge->token));
        } else if (ctx != nullptr && bridge->token != 0) {
            (void)sao_sdk_unsubscribe_event(ctx, bridge->token);
        }
        Py_XDECREF(rec);
        Py_DECREF(bridge->callback);
        delete bridge;
        PyErr_Restore(error_type, error_value, error_traceback);
        return nullptr;
    }
    Py_DECREF(rec);
    bridge->sequence = self->next_resource_sequence++;
    bridge->next = self->native_event_bridges;
    self->native_event_bridges = bridge;
    keep_callback_ref(self, callback);
    return PyUnicode_FromString(bridge->python_token.c_str());
}

// publish(event_type[, data]) / emit alias
PyObject* PluginContext_publish(PluginContextObject* self, PyObject* args) {
    const char* event_type = nullptr;
    PyObject* data = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &event_type, &data))
        return nullptr;
    SaoSdkContext* ctx = native_context(self);
    if (self->loader_context != nullptr || ctx != nullptr) {
        PyObject* json = json_stringify(data);
        if (json == nullptr)
            return nullptr;
        const char* json_utf8 = PyUnicode_AsUTF8(json);
        if (json_utf8 == nullptr) {
            Py_DECREF(json);
            return nullptr;
        }
        const int32_t rc =
            self->loader_context != nullptr
                ? loader::sao_plugins_ctx_emit(self->loader_context, event_type, json_utf8)
                : sao_sdk_publish_event(ctx, event_type,
                                        reinterpret_cast<const uint8_t*>(json_utf8),
                                        std::strlen(json_utf8));
        Py_DECREF(json);
        if (rc != SAO_OK)
            return status_error("event publish", rc);
    } else if (!self->controlled_test_shim) {
        return status_error("event publish", loader::SAO_PLUGINS_ERR_UNSUPPORTED);
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
    if (!PyArg_ParseTuple(args, "s", &msg))
        return nullptr;
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "level", PyUnicode_FromString(level));
    PyDict_SetItemString(rec, "message", PyUnicode_FromString(msg));
    PyList_Append(self->logs, rec);
    Py_DECREF(rec);
    if (self->loader_context != nullptr) {
        loader::sao_plugins_ctx_log(self->loader_context, msg);
    }
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
    if (!PyArg_ParseTuple(args, "s|s", &msg, &level))
        return nullptr;
    PyObject* rec = PyDict_New();
    PyDict_SetItemString(rec, "level", PyUnicode_FromString(level));
    PyDict_SetItemString(rec, "message", PyUnicode_FromString(msg));
    PyList_Append(self->logs, rec);
    Py_DECREF(rec);
    if (self->loader_context != nullptr) {
        loader::sao_plugins_ctx_log(self->loader_context, msg);
    }
    Py_RETURN_NONE;
}

// get_plugin_id() / get_base_dir()
PyObject* PluginContext_get_plugin_id(PluginContextObject* self, PyObject* /*args*/) {
    return Py_NewRef(self->plugin_id);
}
PyObject* PluginContext_get_base_dir(PluginContextObject* self, PyObject* /*args*/) {
    return Py_NewRef(self->base_dir);
}

PyObject* PluginContext_time(PluginContextObject*, PyObject* /*args*/) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return PyFloat_FromDouble(std::chrono::duration<double>(now).count());
}

// ── 旧 PluginContext capability adapters ──────────────────

PyObject* register_timer(PluginContextObject* self, PyObject* args, bool one_shot) {
    PyObject* callback = nullptr;
    double seconds = 0.0;
    if (!PyArg_ParseTuple(args, "Od", &callback, &seconds))
        return nullptr;
    if (!require_callable(callback, "callback"))
        return nullptr;
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        PyErr_SetString(PyExc_ValueError, "seconds must be finite and positive");
        return nullptr;
    }
    if (self->tearing_down) {
        PyErr_SetString(PyExc_RuntimeError, "plugin context is tearing down");
        return nullptr;
    }

    auto* bridge = new (std::nothrow) NativeTimerBridge{};
    if (bridge == nullptr)
        return PyErr_NoMemory();
    bridge->owner = self;
    bridge->callback = Py_NewRef(callback);
    bridge->python_token = next_token(self, "timer_");
    bridge->one_shot = one_shot;

    int32_t status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    if (self->loader_context != nullptr) {
        char* native_token = nullptr;
        status = one_shot ? loader::sao_plugins_ctx_set_timeout(self->loader_context,
                                                                native_loader_timer_action, seconds,
                                                                bridge, &native_token)
                          : loader::sao_plugins_ctx_set_interval(self->loader_context,
                                                                 native_loader_timer_action,
                                                                 seconds, bridge, &native_token);
        if (native_token != nullptr) {
            bridge->loader_token = native_token;
            loader::sao_plugins_ctx_free_string(native_token);
        }
        bridge->uses_loader = status == SAO_OK;
    } else if (SaoSdkContext* sdk = native_context(self); sdk != nullptr) {
        if (one_shot) {
            status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        } else if (seconds > static_cast<double>(std::numeric_limits<uint32_t>::max()) / 1000.0) {
            status = SAO_ERR_INVALID_ARGUMENT;
        } else {
            const uint32_t interval_ms =
                static_cast<uint32_t>(std::max(1.0, std::ceil(seconds * 1000.0)));
            status = sao_sdk_timer_register(sdk, interval_ms, native_sdk_timer_action, bridge,
                                            &bridge->sdk_token);
        }
    } else if (self->controlled_test_shim) {
        status = SAO_OK;
    }
    if (status == loader::SAO_PLUGINS_ERR_UNSUPPORTED && self->controlled_test_shim) {
        bridge->uses_loader = false;
        status = SAO_OK;
    }
    if (status != SAO_OK) {
        delete_timer_bridge(bridge);
        return status_error(one_shot ? "set_timeout" : "set_interval", status);
    }

    PyObject* rec = PyDict_New();
    if (rec == nullptr ||
        !set_dict_item_steal(rec, "kind",
                             PyUnicode_FromString(one_shot ? "timeout" : "interval")) ||
        !set_dict_item_steal(rec, "seconds", PyFloat_FromDouble(seconds)) ||
        PyDict_SetItemString(rec, "callback", callback) != 0 ||
        !set_dict_item_steal(rec, "native_token",
                             PyUnicode_FromString(bridge->loader_token.c_str())) ||
        PyDict_SetItemString(self->timer_tokens, bridge->python_token.c_str(), rec) != 0 ||
        PyList_Append(self->timers, rec) != 0) {
        PyObject* error_type = nullptr;
        PyObject* error_value = nullptr;
        PyObject* error_traceback = nullptr;
        PyErr_Fetch(&error_type, &error_value, &error_traceback);
        if (PyDict_DelItemString(self->timer_tokens, bridge->python_token.c_str()) != 0) {
            PyErr_Clear();
        }
        if (bridge->uses_loader && self->loader_context != nullptr &&
            !bridge->loader_token.empty()) {
            (void)loader::sao_plugins_ctx_clear_timer(self->loader_context,
                                                      bridge->loader_token.c_str());
        } else if (SaoSdkContext* sdk = native_context(self);
                   sdk != nullptr && bridge->sdk_token != 0) {
            (void)sao_sdk_timer_unregister(sdk, bridge->sdk_token);
        }
        stop_callbacks(bridge->gate);
        Py_XDECREF(rec);
        delete_timer_bridge(bridge);
        PyErr_Restore(error_type, error_value, error_traceback);
        return nullptr;
    }
    Py_DECREF(rec);
    bridge->sequence = self->next_resource_sequence++;
    Py_INCREF(reinterpret_cast<PyObject*>(self));
    bridge->owner_ref_held = true;
    bridge->next = self->native_timer_bridges;
    self->native_timer_bridges = bridge;
    keep_callback_ref(self, callback);
    return PyUnicode_FromString(bridge->python_token.c_str());
}

PyObject* PluginContext_set_interval(PluginContextObject* self, PyObject* args) {
    return register_timer(self, args, false);
}

PyObject* PluginContext_set_timeout(PluginContextObject* self, PyObject* args) {
    return register_timer(self, args, true);
}

PyObject* PluginContext_clear_timer(PluginContextObject* self, PyObject* args) {
    if (!require_active_context(self))
        return nullptr;
    const char* token = nullptr;
    if (!PyArg_ParseTuple(args, "s", &token))
        return nullptr;
    NativeTimerBridge* previous = nullptr;
    NativeTimerBridge* bridge = self->native_timer_bridges;
    while (bridge != nullptr && bridge->python_token != token) {
        previous = bridge;
        bridge = bridge->next;
    }
    if (bridge == nullptr || PyDict_GetItemString(self->timer_tokens, token) == nullptr) {
        Py_RETURN_FALSE;
    }

    if (!stop_callbacks(bridge->gate)) {
        return status_error("clear_timer", loader::SAO_PLUGINS_ERR_BUSY);
    }
    int32_t status = SAO_OK;
    if (bridge->uses_loader && self->loader_context != nullptr && !bridge->loader_token.empty()) {
        status =
            loader::sao_plugins_ctx_clear_timer(self->loader_context, bridge->loader_token.c_str());
    } else if (SaoSdkContext* sdk = native_context(self);
               sdk != nullptr && bridge->sdk_token != 0) {
        status = sao_sdk_timer_unregister(sdk, bridge->sdk_token);
    }
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID &&
        status != SAO_SDK_ERR_HANDLE_INVALID) {
        resume_callbacks(bridge->gate);
        return status_error("clear_timer", status);
    }
    if (previous == nullptr) {
        self->native_timer_bridges = bridge->next;
    } else {
        previous->next = bridge->next;
    }
    retire_timer_ledger(bridge);
    delete_timer_bridge(bridge);
    Py_RETURN_TRUE;
}
PyObject* PluginContext_notify(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    if (!require_active_context(self))
        return nullptr;
    static const char* kwlist[] = {"title", "message", "duration_s", "kind", nullptr};
    const char* title = "";
    const char* message = "";
    double duration = 60.0;
    const char* kind = "plugin";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssds", const_cast<char**>(kwlist), &title,
                                     &message, &duration, &kind)) {
        return nullptr;
    }
    if (message[0] == '\0' || !std::isfinite(duration) || duration <= 0.0) {
        PyErr_SetString(PyExc_ValueError, "message must be non-empty and duration_s positive");
        return nullptr;
    }
    auto* bridge = new (std::nothrow) NativeNotifyBridge{};
    if (bridge == nullptr)
        return PyErr_NoMemory();
    bridge->python_token = next_token(self, "notify_");

    int32_t status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    if (self->loader_context != nullptr) {
        status =
            loader::sao_plugins_ctx_notify(self->loader_context, title, message, duration, kind);
        bridge->uses_loader = status == SAO_OK;
    } else if (SaoSdkContext* sdk = native_context(self); sdk != nullptr) {
        if (duration > static_cast<double>(std::numeric_limits<uint32_t>::max()) / 1000.0) {
            status = SAO_ERR_INVALID_ARGUMENT;
        } else {
            std::string text(title);
            if (!text.empty())
                text.append(": ");
            text.append(message);
            SaoSdkNotifySpec spec{};
            spec.text_utf8 = text.c_str();
            spec.duration_ms = static_cast<uint32_t>(std::ceil(duration * 1000.0));
            spec.argb_color = 0xffffffffu;
            status = sao_sdk_notify_show(sdk, &spec, &bridge->sdk_token);
        }
    } else if (self->controlled_test_shim) {
        status = SAO_OK;
    }
    if (status == loader::SAO_PLUGINS_ERR_UNSUPPORTED && self->controlled_test_shim) {
        bridge->uses_loader = false;
        status = SAO_OK;
    }
    if (status != SAO_OK) {
        delete bridge;
        return status_error("notify", status);
    }

    PyObject* rec = PyDict_New();
    if (rec == nullptr || !set_dict_item_steal(rec, "title", PyUnicode_FromString(title)) ||
        !set_dict_item_steal(rec, "message", PyUnicode_FromString(message)) ||
        !set_dict_item_steal(rec, "duration_s", PyFloat_FromDouble(duration)) ||
        !set_dict_item_steal(rec, "kind", PyUnicode_FromString(kind)) ||
        !set_dict_item_steal(rec, "native_token", PyLong_FromUnsignedLongLong(bridge->sdk_token)) ||
        PyDict_SetItemString(self->notification_tokens, bridge->python_token.c_str(), rec) != 0 ||
        PyList_Append(self->notifications, rec) != 0) {
        PyObject* error_type = nullptr;
        PyObject* error_value = nullptr;
        PyObject* error_traceback = nullptr;
        PyErr_Fetch(&error_type, &error_value, &error_traceback);
        if (PyDict_DelItemString(self->notification_tokens, bridge->python_token.c_str()) != 0) {
            PyErr_Clear();
        }
        if (bridge->uses_loader && self->loader_context != nullptr) {
            (void)loader::sao_plugins_ctx_dismiss_notify(self->loader_context);
        } else if (SaoSdkContext* sdk = native_context(self);
                   sdk != nullptr && bridge->sdk_token != 0) {
            (void)sao_sdk_notify_dismiss(sdk, bridge->sdk_token);
        }
        Py_XDECREF(rec);
        delete bridge;
        PyErr_Restore(error_type, error_value, error_traceback);
        return nullptr;
    }
    Py_DECREF(rec);
    bridge->sequence = self->next_resource_sequence++;
    bridge->next = self->native_notify_bridges;
    self->native_notify_bridges = bridge;
    Py_RETURN_TRUE;
}

PyObject* PluginContext_dismiss_notify(PluginContextObject* self, PyObject* /*args*/) {
    if (!require_active_context(self))
        return nullptr;
    bool found = false;
    int32_t status = SAO_OK;
    if (self->loader_context != nullptr) {
        status = loader::sao_plugins_ctx_dismiss_notify(self->loader_context);
        found = self->native_notify_bridges != nullptr;
    }
    SaoSdkContext* sdk = native_context(self);
    while (self->native_notify_bridges != nullptr) {
        auto* bridge = self->native_notify_bridges;
        self->native_notify_bridges = bridge->next;
        found = true;
        if (!bridge->uses_loader && sdk != nullptr && bridge->sdk_token != 0) {
            const int32_t dismiss_status = sao_sdk_notify_dismiss(sdk, bridge->sdk_token);
            if (status == SAO_OK && dismiss_status != SAO_OK &&
                dismiss_status != SAO_SDK_ERR_HANDLE_INVALID) {
                status = dismiss_status;
            }
        }
        delete bridge;
    }
    PyDict_Clear(self->notification_tokens);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID &&
        status != SAO_SDK_ERR_HANDLE_INVALID) {
        return status_error("dismiss_notify", status);
    }
    return PyBool_FromLong(found);
}
PyObject* PluginContext_request_redraw_native(PluginContextObject* self, PyObject* args) {
    const char* panel_id = nullptr;
    if (!PyArg_ParseTuple(args, "|z", &panel_id))
        return nullptr;
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
    if (!PyArg_ParseTuple(args, "s|O", &key, &default_val))
        return nullptr;
    SaoSdkContext* ctx = native_context(self);
    if (ctx != nullptr) {
        if (PyBool_Check(default_val)) {
            bool value = false;
            const sao_sdk_status_t rc = sao_sdk_config_get_bool(ctx, key, &value);
            if (rc == SAO_SDK_OK)
                return PyBool_FromLong(value ? 1 : 0);
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        } else if (PyLong_Check(default_val)) {
            int64_t value = 0;
            const sao_sdk_status_t rc = sao_sdk_config_get_int(ctx, key, &value);
            if (rc == SAO_SDK_OK)
                return PyLong_FromLongLong(value);
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        } else if (PyFloat_Check(default_val)) {
            double value = 0.0;
            const sao_sdk_status_t rc = sao_sdk_config_get_double(ctx, key, &value);
            if (rc == SAO_SDK_OK)
                return PyFloat_FromDouble(value);
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
                if (rc == SAO_SDK_OK)
                    return PyUnicode_FromString(value.data());
            }
            if (rc != SAO_SDK_ERR_NOT_FOUND) {
                PyErr_Format(PyExc_RuntimeError, "native config get failed: %d", rc);
                return nullptr;
            }
        }
    }
    PyObject* v = PyDict_GetItemString(self->settings, key);
    if (v != nullptr)
        return Py_NewRef(v);
    return Py_NewRef(default_val);
}
PyObject* PluginContext_set_setting(PluginContextObject* self, PyObject* args) {
    const char* key = nullptr;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &key, &value))
        return nullptr;
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
        if (PyErr_Occurred())
            return nullptr;
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
    if (!PyArg_ParseTuple(args, "O", &defaults))
        return nullptr;
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
std::string normalize_engine_name(const char* name) {
    std::string result;
    if (name == nullptr)
        return result;
    for (const unsigned char value : std::string(name)) {
        if (std::isspace(value) != 0)
            continue;
        result.push_back(value == '-' ? '_' : static_cast<char>(std::tolower(value)));
    }
    return result;
}

PyObject* PluginContext_get_engine_by_name(PluginContextObject* self, PyObject* args) {
    const char* name = nullptr;
    PyObject* fallback = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &name, &fallback))
        return nullptr;
    const std::string key = normalize_engine_name(name);
    if (key.empty())
        return Py_NewRef(fallback);
    PyObject* engine = PyDict_GetItemString(self->engines, key.c_str());
    if (engine != nullptr)
        return Py_NewRef(engine);
    if (self->loader_context != nullptr) {
        void* native_engine = loader::sao_plugins_ctx_get_engine(self->loader_context, key.c_str());
        if (native_engine != nullptr) {
            PyObject* candidate = nullptr;
            PyObject* item_key = nullptr;
            PyObject* item_value = nullptr;
            Py_ssize_t position = 0;
            while (PyDict_Next(self->engines, &position, &item_key, &item_value)) {
                if (item_value == native_engine) {
                    candidate = item_value;
                    break;
                }
            }
            if (candidate != nullptr)
                return Py_NewRef(candidate);
        }
    }
    return Py_NewRef(fallback);
}

PyObject* PluginContext_get_engine_method(PluginContextObject* self, PyObject* args) {
    const char* engine_name = nullptr;
    const char* method_name = nullptr;
    PyObject* fallback = Py_None;
    if (!PyArg_ParseTuple(args, "ss|O", &engine_name, &method_name, &fallback)) {
        return nullptr;
    }
    PyObject* engine_args = Py_BuildValue("(sO)", engine_name, Py_None);
    if (engine_args == nullptr)
        return nullptr;
    PyObject* engine = PluginContext_get_engine_by_name(self, engine_args);
    Py_DECREF(engine_args);
    if (engine == nullptr)
        return nullptr;
    if (engine == Py_None) {
        Py_DECREF(engine);
        return Py_NewRef(fallback);
    }
    PyObject* method = PyObject_GetAttrString(engine, method_name);
    Py_DECREF(engine);
    if (method == nullptr) {
        PyErr_Clear();
        return Py_NewRef(fallback);
    }
    if (PyCallable_Check(method) == 0) {
        Py_DECREF(method);
        return Py_NewRef(fallback);
    }
    return method;
}
PyObject* PluginContext_load_local(PluginContextObject* self, PyObject* args) {
    // 老插件用 ctx.load_local("engine_module.py") 载入插件目录里的模块。
    // 简化实装: 拼路径 → 用 importlib.util.spec_from_file_location 装载。
    const char* rel = nullptr;
    if (!PyArg_ParseTuple(args, "s", &rel))
        return nullptr;
    if (std::strcmp(rel, "bootstrap.py") == 0) {
        PyObject* shim = PyModule_New("sao_controlled_bootstrap");
        if (shim == nullptr)
            return nullptr;
        PyObject* globals = PyModule_GetDict(shim);
        const char* code = "def ensure_requirements(plugin_dir, ctx=None, install=False):\n"
                           "    if ctx is not None:\n"
                           "        return ctx.ensure_requirements(install=False)\n"
                           "    return {'added': [], 'deps': {}}\n"
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
    if (base == nullptr)
        Py_RETURN_NONE;
    const char* base_c = PyUnicode_AsUTF8(base);
    if (base_c == nullptr)
        return nullptr;

    std::string full = base_c;
    if (!full.empty() && full.back() != '/' && full.back() != '\\')
        full.push_back('/');
    full += rel;

    // module 名: sao_local_<plugin_id>_<escaped_rel>
    std::string name = "sao_local_";
    const char* pid = PyUnicode_AsUTF8(self->plugin_id);
    if (pid != nullptr)
        name += pid;
    name += "_";
    for (const char* p = rel; *p; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            name.push_back(c);
        } else {
            name.push_back('_');
        }
    }

    PyObject* importlib_util = PyImport_ImportModule("importlib.util");
    if (importlib_util == nullptr)
        return nullptr;
    PyObject* spec_fn = PyObject_GetAttrString(importlib_util, "spec_from_file_location");
    if (spec_fn == nullptr) {
        Py_DECREF(importlib_util);
        return nullptr;
    }
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
    if (module_from_spec_fn == nullptr) {
        Py_DECREF(spec);
        return nullptr;
    }
    PyObject* mod = PyObject_CallOneArg(module_from_spec_fn, spec);
    Py_DECREF(module_from_spec_fn);
    if (mod == nullptr) {
        Py_DECREF(spec);
        return nullptr;
    }
    PyObject* loader = PyObject_GetAttrString(spec, "loader");
    Py_DECREF(spec);
    if (loader == nullptr) {
        Py_DECREF(mod);
        return nullptr;
    }
    PyObject* exec_fn = PyObject_GetAttrString(loader, "exec_module");
    Py_DECREF(loader);
    if (exec_fn == nullptr) {
        Py_DECREF(mod);
        return nullptr;
    }
    PyObject* res = PyObject_CallOneArg(exec_fn, mod);
    Py_DECREF(exec_fn);
    if (res == nullptr) {
        Py_DECREF(mod);
        return nullptr;
    }
    Py_DECREF(res);
    return mod;
}
// register_hotkey 老名兼容 add_hotkey
PyObject* PluginContext_register_hotkey_alias(PluginContextObject* self, PyObject* args,
                                              PyObject* kwds) {
    return PluginContext_add_hotkey(self, args, kwds);
}
// emit alias for publish
PyObject* PluginContext_emit(PluginContextObject* self, PyObject* args) {
    return PluginContext_publish(self, args);
}
// open_window(panel_id[, width, height])
PyObject* PluginContext_open_window(PluginContextObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"panel_id", "width", "height", nullptr};
    const char* panel_id = "";
    unsigned int width = 0;
    unsigned int height = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|sII", const_cast<char**>(kwlist), &panel_id,
                                     &width, &height)) {
        return nullptr;
    }
    const int32_t status =
        self->loader_context == nullptr
            ? loader::SAO_PLUGINS_ERR_UNSUPPORTED
            : loader::sao_plugins_ctx_open_window(self->loader_context, panel_id, width, height);
    return explicit_status(status, panel_id);
}
// register_engine(name, obj)
PyObject* PluginContext_register_engine(PluginContextObject* self, PyObject* args) {
    if (!require_active_context(self))
        return nullptr;
    const char* name = nullptr;
    PyObject* obj = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &name, &obj))
        return nullptr;
    const std::string key = normalize_engine_name(name);
    if (key.empty()) {
        PyErr_SetString(PyExc_ValueError, "engine name must be non-empty");
        return nullptr;
    }
    if (self->loader_context == nullptr && !self->controlled_test_shim) {
        return status_error("register_engine", loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    if (PyDict_GetItemString(self->engines, key.c_str()) != nullptr) {
        PyErr_Format(PyExc_ValueError, "engine already registered: %s", key.c_str());
        return nullptr;
    }
    if (PyDict_SetItemString(self->engines, key.c_str(), obj) != 0) {
        return nullptr;
    }
    if (self->loader_context != nullptr) {
        const int32_t status =
            loader::sao_plugins_ctx_register_engine(self->loader_context, key.c_str(), obj);
        if (status != SAO_OK) {
            if (PyDict_DelItemString(self->engines, key.c_str()) != 0) {
                PyErr_Clear();
            }
            return status_error("register_engine", status);
        }
    }
    Py_RETURN_NONE;
}

PyObject* PluginContext_engine_set_owner_attr(PluginContextObject* self, PyObject* args) {
    const char* name = nullptr;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &name, &value))
        return nullptr;
    PyObject* owner = PluginContext_get_owner_attr(self, nullptr);
    if (owner == nullptr)
        return nullptr;
    if (owner == Py_None) {
        Py_DECREF(owner);
        return status_error("engine owner projection", loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    const int status = PyObject_SetAttrString(owner, name, value);
    Py_DECREF(owner);
    if (status != 0)
        return nullptr;
    Py_RETURN_NONE;
}

PyObject* PluginContext_engine_owner_attr(PluginContextObject* self, PyObject* args) {
    const char* name = nullptr;
    PyObject* fallback = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &name, &fallback))
        return nullptr;
    PyObject* owner = PluginContext_get_owner_attr(self, nullptr);
    if (owner == nullptr)
        return nullptr;
    if (owner == Py_None) {
        Py_DECREF(owner);
        return Py_NewRef(fallback);
    }
    PyObject* result = PyObject_GetAttrString(owner, name);
    Py_DECREF(owner);
    if (result == nullptr) {
        PyErr_Clear();
        return Py_NewRef(fallback);
    }
    return result;
}

PyObject* PluginContext_register_menu_surface(PluginContextObject* self, PyObject* args,
                                              PyObject* kwds) {
    if (!require_active_context(self))
        return nullptr;
    static const char* kwlist[] = {"surface_id", "descriptor", "priority", nullptr};
    const char* surface_id = nullptr;
    PyObject* descriptor = nullptr;
    double priority = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|d", const_cast<char**>(kwlist), &surface_id,
                                     &descriptor, &priority)) {
        return nullptr;
    }
    if (surface_id[0] == '\0' || !PyMapping_Check(descriptor) || !std::isfinite(priority)) {
        PyErr_SetString(PyExc_ValueError,
                        "menu surface id, descriptor, and priority must be valid");
        return nullptr;
    }
    const int32_t status =
        self->loader_context == nullptr
            ? loader::SAO_PLUGINS_ERR_UNSUPPORTED
            : loader::sao_plugins_ctx_register_menu_surface(self->loader_context, surface_id, "{}",
                                                            static_cast<float>(priority));
    if (status != SAO_OK)
        return status_error("menu surface registration", status);
    return PyUnicode_FromString(surface_id);
}

PyObject* PluginContext_register_action_handler(PluginContextObject* self, PyObject* args) {
    if (!require_active_context(self))
        return nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args, "O", &callback))
        return nullptr;
    if (!require_callable(callback, "action handler"))
        return nullptr;
    if (self->loader_context == nullptr) {
        return status_error("action handler registration", loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    try {
        const char* plugin_id = PyUnicode_AsUTF8(self->plugin_id);
        if (plugin_id == nullptr)
            return nullptr;
        auto candidate = std::make_unique<NativeActionBridge>();
        candidate->provider_id = "opaque-actions";
        candidate->qualified_provider_id = std::string(plugin_id) + "/" + candidate->provider_id;
        auto* replaced = self->native_action_bridge;
        if (self->active_enable_checkpoint != 0) {
            candidate->previous = replaced;
        } else if (replaced != nullptr && replaced->enable_scoped) {
            auto* persistent_base = replaced;
            while (persistent_base != nullptr && persistent_base->enable_scoped)
                persistent_base = persistent_base->previous;
            candidate->previous = persistent_base;
            candidate->enable_scoped = true;
        }
        candidate->callback = Py_NewRef(callback);
        const int32_t status = register_action_provider(self, candidate.get());
        if (status != SAO_OK) {
            Py_CLEAR(candidate->callback);
            return status_error("action handler registration", status);
        }
        if (replaced != nullptr)
            replaced->registered = false;
        candidate->sequence = self->next_resource_sequence++;
        self->native_action_bridge = candidate.release();
        if (self->active_enable_checkpoint == 0)
            delete_action_prefix(replaced, self->native_action_bridge->previous);
        return Py_NewRef(self->plugin_id);
    } catch (const std::bad_alloc&) {
        return PyErr_NoMemory();
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "action handler registration failed");
        return nullptr;
    }
}

PyObject* PluginContext_register_menu_category(PluginContextObject* self, PyObject* args,
                                               PyObject* kwds) {
    if (!require_active_context(self))
        return nullptr;
    static const char* kwlist[] = {"name", "icon", "builder", "priority", nullptr};
    PyObject* name_object = nullptr;
    PyObject* icon_object = nullptr;
    PyObject* builder = nullptr;
    double priority = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO|d", const_cast<char**>(kwlist), &name_object,
                                     &icon_object, &builder, &priority)) {
        return nullptr;
    }
    std::string name;
    std::string icon;
    if (!unicode_value(name_object, true, name) || !unicode_value(icon_object, false, icon) ||
        !require_callable(builder, "builder") || !std::isfinite(priority)) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "menu priority must be finite");
        }
        return nullptr;
    }
    if (self->tearing_down) {
        PyErr_SetString(PyExc_RuntimeError, "plugin context is tearing down");
        return nullptr;
    }
    if (self->loader_context == nullptr && !self->controlled_test_shim) {
        return status_error("dynamic menu provider registration",
                            loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
#if !defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
    if (!self->controlled_test_shim) {
        return status_error("dynamic menu provider registration",
                            loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
#endif
    const std::string identity = "menu-" + hash_suffix(name);
#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
    const char* plugin_id = nullptr;
    std::unique_ptr<NativeMenuBridge> bridge;
    if (self->loader_context != nullptr) {
        plugin_id = PyUnicode_AsUTF8(self->plugin_id);
        if (plugin_id == nullptr)
            return nullptr;
        bridge.reset(new (std::nothrow) NativeMenuBridge{});
        if (bridge == nullptr)
            return PyErr_NoMemory();
        bridge->provider_id = identity;
        bridge->qualified_provider_id = std::string(plugin_id) + "/" + identity;
        bridge->contribution_id = identity;
        bridge->root_id = "plugin:" + hash_suffix(std::string(plugin_id) + "\n" + name);
        bridge->name = name;
        bridge->icon = icon;
        bridge->priority = priority;
    }
#endif

    PyObject* result =
        PyUnicode_FromStringAndSize(identity.data(), static_cast<Py_ssize_t>(identity.size()));
    PyObject* record = PyDict_New();
    if (result == nullptr || record == nullptr ||
        PyDict_SetItemString(record, "name", name_object) != 0 ||
        PyDict_SetItemString(record, "icon", icon_object) != 0 ||
        PyDict_SetItemString(record, "builder", builder) != 0 ||
        !set_dict_item_steal(record, "priority", PyFloat_FromDouble(priority)) ||
        !set_dict_item_steal(record, "extension_id", PyUnicode_FromString(identity.c_str())) ||
        PyList_Append(self->menus, record) != 0) {
        Py_XDECREF(record);
        Py_XDECREF(result);
        return nullptr;
    }
#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
    if (bridge != nullptr)
        bridge->ledger_record = Py_NewRef(record);
#endif
    Py_DECREF(record);
#if defined(SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER)
    const Py_ssize_t record_index = PyList_GET_SIZE(self->menus) - 1;

    if (bridge != nullptr) {
        bridge->builder = Py_NewRef(builder);
        loader::entity_root_contribution_descriptor root{};
        root.struct_size = sizeof(root);
        root.contribution_id_utf8 = bridge->contribution_id.c_str();
        root.root_id_utf8 = bridge->root_id.c_str();
        root.name_utf8 = bridge->name.c_str();
        root.icon_utf8 = bridge->icon.c_str();
        root.priority = bridge->priority;

        loader::context_entity_provider_descriptor provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = bridge->provider_id.c_str();
        provider.snapshot = native_menu_snapshot;
        provider.action_handler = native_menu_action;
        provider.user_data = bridge.get();
        provider.root_contribution = &root;
        const int32_t status =
            loader::sao_plugins_ctx_register_entity_provider(self->loader_context, &provider);
        if (status != SAO_OK) {
            if (PySequence_DelItem(self->menus, record_index) != 0) {
                PyErr_Clear();
            }
            Py_DECREF(bridge->builder);
            bridge->builder = nullptr;
            Py_CLEAR(bridge->ledger_record);
            Py_DECREF(result);
            return status_error("dynamic menu provider registration", status);
        }
        bridge->sequence = self->next_resource_sequence++;
        bridge->next = self->native_menu_bridges;
        self->native_menu_bridges = bridge.release();
    }
#endif
    return result;
}
PyObject* PluginContext_unsubscribe(PluginContextObject* self, PyObject* args) {
    if (!require_active_context(self))
        return nullptr;
    const char* token = nullptr;
    if (!PyArg_ParseTuple(args, "s", &token))
        return nullptr;
    NativeEventBridge* previous = nullptr;
    NativeEventBridge* bridge = self->native_event_bridges;
    while (bridge != nullptr && bridge->python_token != token) {
        previous = bridge;
        bridge = bridge->next;
    }
    if (bridge == nullptr || PyDict_GetItemString(self->subscription_tokens, token) == nullptr) {
        Py_RETURN_FALSE;
    }

    if (!stop_callbacks(bridge->gate)) {
        return status_error("event unsubscribe", loader::SAO_PLUGINS_ERR_BUSY);
    }
    int32_t status = SAO_OK;
    SaoSdkContext* sdk = native_context(self);
    if (self->loader_context != nullptr && bridge->token != 0) {
        status = loader::sao_plugins_ctx_unsubscribe(self->loader_context,
                                                     static_cast<uint32_t>(bridge->token));
    } else if (sdk != nullptr && bridge->token != 0) {
        status = sao_sdk_unsubscribe_event(sdk, bridge->token);
    }
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID &&
        status != SAO_SDK_ERR_HANDLE_INVALID) {
        resume_callbacks(bridge->gate);
        return status_error("event unsubscribe", status);
    }
    if (previous == nullptr) {
        self->native_event_bridges = bridge->next;
    } else {
        previous->next = bridge->next;
    }
    PyObject* record = PyDict_GetItemString(self->subscription_tokens, token);
    Py_XINCREF(record);
    if (PyDict_DelItemString(self->subscription_tokens, token) != 0) {
        PyErr_Clear();
    }
    remove_list_identity(self->subscriptions, record);
    remove_list_identity(self->callback_refs, bridge->callback);
    Py_XDECREF(record);
    Py_DECREF(bridge->callback);
    delete bridge;
    Py_RETURN_TRUE;
}
PyObject* PluginContext_toast(PluginContextObject* self, PyObject* args) {
    const char* message = nullptr;
    if (!PyArg_ParseTuple(args, "s", &message))
        return nullptr;
    if (message[0] == '\0') {
        PyErr_SetString(PyExc_ValueError, "message must be non-empty");
        return nullptr;
    }
    if (self->loader_context != nullptr) {
        const int32_t status = loader::sao_plugins_ctx_toast(self->loader_context, message);
        if (status != SAO_OK) {
            if (status == loader::SAO_PLUGINS_ERR_UNSUPPORTED && self->controlled_test_shim) {
                Py_RETURN_TRUE;
            }
            return status_error("toast", status);
        }
        auto* bridge = new (std::nothrow) NativeNotifyBridge{};
        if (bridge == nullptr) {
            (void)loader::sao_plugins_ctx_dismiss_notify(self->loader_context);
            return PyErr_NoMemory();
        }
        bridge->python_token = next_token(self, "notify_");
        bridge->uses_loader = true;
        bridge->sequence = self->next_resource_sequence++;
        bridge->next = self->native_notify_bridges;
        self->native_notify_bridges = bridge;
        Py_RETURN_TRUE;
    }
    SaoSdkContext* sdk = native_context(self);
    if (sdk == nullptr) {
        if (self->controlled_test_shim)
            Py_RETURN_TRUE;
        return status_error("toast", loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    SaoSdkNotifySpec spec{};
    spec.text_utf8 = message;
    spec.duration_ms = 3000;
    spec.argb_color = 0xffffffffu;
    auto* bridge = new (std::nothrow) NativeNotifyBridge{};
    if (bridge == nullptr)
        return PyErr_NoMemory();
    const int32_t status = sao_sdk_notify_show(sdk, &spec, &bridge->sdk_token);
    if (status != SAO_OK) {
        delete bridge;
        return status_error("toast", status);
    }
    bridge->python_token = next_token(self, "notify_");
    bridge->sequence = self->next_resource_sequence++;
    bridge->next = self->native_notify_bridges;
    self->native_notify_bridges = bridge;
    Py_RETURN_TRUE;
}
PyObject* PluginContext_ensure_requirements(PluginContextObject* self, PyObject* args,
                                            PyObject* kwds) {
    static const char* kwlist[] = {"install", nullptr};
    int ignored_install = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p", const_cast<char**>(kwlist),
                                     &ignored_install)) {
        return nullptr;
    }
    (void)ignored_install;
    if (self == nullptr || self->base_dir == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "plugin base directory is unavailable");
        return nullptr;
    }
    Py_ssize_t length = 0;
    const wchar_t* plugin_dir = PyUnicode_AsWideCharString(self->base_dir, &length);
    if (plugin_dir == nullptr)
        return nullptr;
    char* report_json = nullptr;
    const int32_t status = sao_plugins_pyhost_report_requirements(plugin_dir, &report_json);
    PyMem_Free(const_cast<wchar_t*>(plugin_dir));
    if (status != SAO_OK || report_json == nullptr) {
        sao_plugins_pyhost_free_string(report_json);
        PyErr_Format(PyExc_RuntimeError, "requirements report failed: %d", status);
        return nullptr;
    }
    PyObject* json_module = PyImport_ImportModule("json");
    PyObject* result = nullptr;
    if (json_module != nullptr) {
        PyObject* loads = PyObject_GetAttrString(json_module, "loads");
        Py_DECREF(json_module);
        if (loads != nullptr) {
            result = PyObject_CallFunction(loads, "s", report_json);
            Py_DECREF(loads);
        }
    }
    sao_plugins_pyhost_free_string(report_json);
    return result;
}

// ── PyMethodDef 表 ──────────────────────────────────────────

PyMethodDef PluginContext_methods[] = {
    // 核心 8 SDK 函数 (spec 明列):
    {"register_ui_panel", reinterpret_cast<PyCFunction>(PluginContext_register_ui_panel),
     METH_VARARGS | METH_KEYWORDS, "Register a UI panel."},
    {"add_hotkey", reinterpret_cast<PyCFunction>(PluginContext_add_hotkey),
     METH_VARARGS | METH_KEYWORDS, "Register a hotkey."},
    {"subscribe", reinterpret_cast<PyCFunction>(PluginContext_subscribe), METH_VARARGS,
     "Subscribe to an event."},
    {"publish", reinterpret_cast<PyCFunction>(PluginContext_publish), METH_VARARGS,
     "Publish an event."},
    {"log_info", reinterpret_cast<PyCFunction>(PluginContext_log_info), METH_VARARGS, "Log info."},
    {"log_warn", reinterpret_cast<PyCFunction>(PluginContext_log_warn), METH_VARARGS, "Log warn."},
    {"log_error", reinterpret_cast<PyCFunction>(PluginContext_log_error), METH_VARARGS,
     "Log error."},
    {"get_plugin_id", reinterpret_cast<PyCFunction>(PluginContext_get_plugin_id), METH_NOARGS,
     "Get plugin id."},
    {"get_base_dir", reinterpret_cast<PyCFunction>(PluginContext_get_base_dir), METH_NOARGS,
     "Get base dir."},
    {"time", reinterpret_cast<PyCFunction>(PluginContext_time), METH_NOARGS,
     "Get the current epoch time."},

    // 老 Python 平台 PluginContext 兼容方法 (lenient):
    {"log", reinterpret_cast<PyCFunction>(PluginContext_log), METH_VARARGS,
     "Log a message (old-style)."},
    {"register_hotkey", reinterpret_cast<PyCFunction>(PluginContext_register_hotkey_alias),
     METH_VARARGS | METH_KEYWORDS, "Register a hotkey (alias)."},
    {"emit", reinterpret_cast<PyCFunction>(PluginContext_emit), METH_VARARGS,
     "Emit an event (alias for publish)."},
    {"unsubscribe", reinterpret_cast<PyCFunction>(PluginContext_unsubscribe), METH_VARARGS,
     "Unsubscribe."},
    {"set_interval", reinterpret_cast<PyCFunction>(PluginContext_set_interval), METH_VARARGS,
     "Set an interval timer."},
    {"set_timeout", reinterpret_cast<PyCFunction>(PluginContext_set_timeout), METH_VARARGS,
     "Set a one-shot timer."},
    {"clear_timer", reinterpret_cast<PyCFunction>(PluginContext_clear_timer), METH_VARARGS,
     "Clear a timer."},
    {"notify", reinterpret_cast<PyCFunction>(PluginContext_notify), METH_VARARGS | METH_KEYWORDS,
     "Show a notification."},
    {"dismiss_notify", reinterpret_cast<PyCFunction>(PluginContext_dismiss_notify), METH_VARARGS,
     "Dismiss the current notification."},
    {"request_redraw", reinterpret_cast<PyCFunction>(PluginContext_request_redraw_native),
     METH_VARARGS, "Request UI redraw."},
    {"get_setting", reinterpret_cast<PyCFunction>(PluginContext_get_setting), METH_VARARGS,
     "Get a setting."},
    {"set_setting", reinterpret_cast<PyCFunction>(PluginContext_set_setting), METH_VARARGS,
     "Set a setting."},
    {"set_defaults", reinterpret_cast<PyCFunction>(PluginContext_set_defaults), METH_VARARGS,
     "Merge default settings."},
    {"get_engine", reinterpret_cast<PyCFunction>(PluginContext_get_engine_by_name), METH_VARARGS,
     "Get an engine by name."},
    {"get", reinterpret_cast<PyCFunction>(PluginContext_get_engine_by_name), METH_VARARGS,
     "Get an engine by name (EngineAccess alias)."},
    {"get_engine_method", reinterpret_cast<PyCFunction>(PluginContext_get_engine_method),
     METH_VARARGS, "Get an engine method."},
    {"load_local", reinterpret_cast<PyCFunction>(PluginContext_load_local), METH_VARARGS,
     "Load a bundled module from the plugin directory."},
    {"open_window", reinterpret_cast<PyCFunction>(PluginContext_open_window),
     METH_VARARGS | METH_KEYWORDS, "Open a plugin panel window."},
    {"register_engine", reinterpret_cast<PyCFunction>(PluginContext_register_engine), METH_VARARGS,
     "Register a plugin-scoped engine."},
    {"register", reinterpret_cast<PyCFunction>(PluginContext_register_engine), METH_VARARGS,
     "Register a plugin-scoped engine (EngineAccess alias)."},
    {"set_owner_attr", reinterpret_cast<PyCFunction>(PluginContext_engine_set_owner_attr),
     METH_VARARGS, "Set an attribute on the projected engine owner."},
    {"owner_attr", reinterpret_cast<PyCFunction>(PluginContext_engine_owner_attr), METH_VARARGS,
     "Read an attribute from the projected engine owner."},
    {"register_menu_category", reinterpret_cast<PyCFunction>(PluginContext_register_menu_category),
     METH_VARARGS | METH_KEYWORDS, "Register a legacy plugin menu category."},
    {"register_menu_surface", reinterpret_cast<PyCFunction>(PluginContext_register_menu_surface),
     METH_VARARGS | METH_KEYWORDS, "Register a typed plugin menu surface."},
    {"register_action_handler",
     reinterpret_cast<PyCFunction>(PluginContext_register_action_handler), METH_VARARGS,
     "Register an opaque plugin action handler."},
    {"toast", reinterpret_cast<PyCFunction>(PluginContext_toast), METH_VARARGS, "Show a toast."},
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
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssn", const_cast<char**>(kwlist), &pid, &dir_,
                                     &handle_int)) {
        return nullptr;
    }
    PyObject* pid_o = PyUnicode_FromString(pid);
    PyObject* dir_o = PyUnicode_FromString(dir_);
    PyObject* hnd_o = PyLong_FromSsize_t(handle_int);
    PyObject* args_t = PyTuple_Pack(3, pid_o, dir_o, hnd_o);
    Py_DECREF(pid_o);
    Py_DECREF(dir_o);
    Py_DECREF(hnd_o);
    if (args_t == nullptr)
        return nullptr;
    PyObject* obj = PyObject_CallObject(reinterpret_cast<PyObject*>(&PluginContextType), args_t);
    Py_DECREF(args_t);
    return obj;
}
// 模块级 log_info/warn/error (给不用 ctx 也想打日志的场景)
PyObject* sao_sdk_log_info(PyObject* /*self*/, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
        return nullptr;
    std::fprintf(stdout, "[sao_sdk][info] %s\n", msg);
    Py_RETURN_NONE;
}
PyObject* sao_sdk_log_warn(PyObject* /*self*/, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
        return nullptr;
    std::fprintf(stderr, "[sao_sdk][warn] %s\n", msg);
    Py_RETURN_NONE;
}
PyObject* sao_sdk_log_error(PyObject* /*self*/, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
        return nullptr;
    std::fprintf(stderr, "[sao_sdk][error] %s\n", msg);
    Py_RETURN_NONE;
}
// version helper
PyObject* sao_sdk_version(PyObject* /*self*/, PyObject* /*args*/) {
    return PyUnicode_FromString("sao_sdk 1.0 (native)");
}

PyMethodDef sao_sdk_module_methods[] = {
    {"wrap_ctx", reinterpret_cast<PyCFunction>(sao_sdk_wrap_ctx), METH_VARARGS | METH_KEYWORDS,
     "Create a PluginContext instance."},
    {"log_info", sao_sdk_log_info, METH_VARARGS, "Module-level info log."},
    {"log_warn", sao_sdk_log_warn, METH_VARARGS, "Module-level warn log."},
    {"log_error", sao_sdk_log_error, METH_VARARGS, "Module-level error log."},
    {"version", sao_sdk_version, METH_NOARGS, "Get sao_sdk version."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef sao_sdk_module_def = {PyModuleDef_HEAD_INIT,
                                  "sao_sdk",                                         // m_name
                                  "SAO plugin host SDK — the platform side of ctx.", // m_doc
                                  -1,                     // m_size (no module state)
                                  sao_sdk_module_methods, // m_methods
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr};

PyObject* PyInit_sao_sdk_impl(void) {
    if (PyType_Ready(&PluginContextType) < 0)
        return nullptr;
    PyObject* m = PyModule_Create(&sao_sdk_module_def);
    if (m == nullptr)
        return nullptr;
    Py_INCREF(&PluginContextType);
    if (PyModule_AddObject(m, "PluginContext", reinterpret_cast<PyObject*>(&PluginContextType)) <
        0) {
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
    for (auto* p = PluginContext_methods; p->ml_name != nullptr; ++p)
        ++n;
    for (auto* p = sao_sdk_module_methods; p->ml_name != nullptr; ++p)
        ++n;
    return n;
}

} // namespace

// ── 公开 API ────────────────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_native_module(void) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    if (g_sdk_method_count == 0)
        g_sdk_method_count = compute_sdk_method_count();
    // Py_Initialize 前调 → PyImport_AppendInittab; Py_Initialize 后调 →
    // 通过 sys.modules['sao_sdk'] 手动注入.
    if (Py_IsInitialized() == 0) {
        int rc = PyImport_AppendInittab("sao_sdk", PyInit_sao_sdk_impl);
        if (rc != 0)
            return SAO_ERR_OS_CALL_FAILED;
        return SAO_OK;
    } else {
        // 后置注入: 检查是否已在 sys.modules
        PyObject* mods = PySys_GetObject("modules");
        if (mods != nullptr) {
            if (PyDict_GetItemString(mods, "sao_sdk") != nullptr)
                return SAO_OK;
        }
        PyObject* m = PyInit_sao_sdk_impl();
        if (m == nullptr) {
            PyErr_Clear();
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (mods != nullptr)
            PyDict_SetItemString(mods, "sao_sdk", m);
        Py_DECREF(m);
        return SAO_OK;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_register_shim_module(void) {
    // 老代码可能 `from act_platform.plugins import PluginContext`。
    // 我们在 sys.modules 里放个 shim 指到 sao_sdk 里的同名类。
    if (Py_IsInitialized() == 0)
        return SAO_ERR_NOT_INITIALIZED;
    PyObject* sao_sdk = PyImport_ImportModule("sao_sdk");
    if (sao_sdk == nullptr) {
        PyErr_Clear();
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* pc_cls = PyObject_GetAttrString(sao_sdk, "PluginContext");
    Py_DECREF(sao_sdk);
    if (pc_cls == nullptr) {
        PyErr_Clear();
        return SAO_ERR_OS_CALL_FAILED;
    }

    // 建 act_platform 包 module + act_platform.plugins submodule
    PyObject* mods = PySys_GetObject("modules");
    if (mods == nullptr) {
        Py_DECREF(pc_cls);
        return SAO_ERR_OS_CALL_FAILED;
    }

    // Skip if already registered (idempotent)
    if (PyDict_GetItemString(mods, "act_platform.plugins") != nullptr) {
        Py_DECREF(pc_cls);
        return SAO_OK;
    }

    // 建 act_platform (若无) —— PyModule_New 建纯 name-only module.
    PyObject* act_platform_mod = PyDict_GetItemString(mods, "act_platform");
    if (act_platform_mod == nullptr) {
        act_platform_mod = PyModule_New("act_platform");
        if (act_platform_mod == nullptr) {
            Py_DECREF(pc_cls);
            return SAO_ERR_OS_CALL_FAILED;
        }
        PyDict_SetItemString(mods, "act_platform", act_platform_mod);
        // PyDict_SetItem 取新引用, PyModule_New 建时 refcount=1 → 现在 2, 平衡回 1
        Py_DECREF(act_platform_mod);
        act_platform_mod = PyDict_GetItemString(mods, "act_platform"); // borrowed
    }
    Py_INCREF(act_platform_mod); // 为下面的 DECREF 对齐
    PyObject* plugins_mod = PyModule_New("act_platform.plugins");
    if (plugins_mod == nullptr) {
        Py_DECREF(act_platform_mod);
        Py_DECREF(pc_cls);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject_SetAttrString(plugins_mod, "PluginContext", pc_cls);
    PyObject_SetAttrString(act_platform_mod, "plugins", plugins_mod);
    PyDict_SetItemString(mods, "act_platform.plugins", plugins_mod);

    // Phase 2 (P0 compat shim): 追加 act_platform.runtime 子 module。
    // star_resonance 的 plugin.py:160 会做:
    //   from act_platform.runtime import ensure_act_event_bus, ensure_act_plugin_manager
    // 这里注入两个函数, 返回挂在 owner 上的共享对象 (若无则建 no-op stub).
    if (PyDict_GetItemString(mods, "act_platform.runtime") == nullptr) {
        PyObject* runtime_mod = PyModule_New("act_platform.runtime");
        if (runtime_mod != nullptr) {
            PyObject* globals = PyModule_GetDict(runtime_mod);
            const char* runtime_src =
                "import types\n"
                "def ensure_act_event_bus(owner):\n"
                "    bus = getattr(owner, '_event_bus', None)\n"
                "    if bus is None:\n"
                "        bus = types.SimpleNamespace()\n"
                "        bus.subscribe = lambda *a, **kw: None\n"
                "        bus.unsubscribe = lambda *a, **kw: None\n"
                "        bus.publish = lambda *a, **kw: None\n"
                "        bus.emit = lambda *a, **kw: None\n"
                "        try:\n"
                "            setattr(owner, '_event_bus', bus)\n"
                "        except Exception:\n"
                "            pass\n"
                "    return bus\n"
                "\n"
                "def ensure_act_plugin_manager(owner, load=False):\n"
                "    pm = getattr(owner, '_plugin_manager', None)\n"
                "    if pm is None:\n"
                "        pm = types.SimpleNamespace()\n"
                "        pm.get_plugin = lambda name: None\n"
                "        pm.list_plugins = lambda: []\n"
                "        pm.register = lambda *a, **kw: None\n"
                "        try:\n"
                "            setattr(owner, '_plugin_manager', pm)\n"
                "        except Exception:\n"
                "            pass\n"
                "    return pm\n";
            PyObject* res = PyRun_String(runtime_src, Py_file_input, globals, globals);
            if (res != nullptr) {
                Py_DECREF(res);
                PyObject_SetAttrString(act_platform_mod, "runtime", runtime_mod);
                PyDict_SetItemString(mods, "act_platform.runtime", runtime_mod);
            } else {
                PyErr_Clear();
            }
            Py_DECREF(runtime_mod);
        }
    }

    Py_DECREF(act_platform_mod);
    Py_DECREF(plugins_mod);
    Py_DECREF(pc_cls);
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_wrap_ctx(void* ctx_ptr, void** out_pyobject) {
    if (out_pyobject == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_pyobject = nullptr;
    if (Py_IsInitialized() == 0)
        return SAO_ERR_NOT_INITIALIZED;
    // 找 PluginContext 类
    PyObject* sao_sdk = PyImport_ImportModule("sao_sdk");
    if (sao_sdk == nullptr) {
        PyErr_Clear();
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* pc_cls = PyObject_GetAttrString(sao_sdk, "PluginContext");
    Py_DECREF(sao_sdk);
    if (pc_cls == nullptr) {
        PyErr_Clear();
        return SAO_ERR_OS_CALL_FAILED;
    }

    Py_ssize_t hnd = static_cast<Py_ssize_t>(reinterpret_cast<intptr_t>(ctx_ptr));
    PyObject* args = Py_BuildValue("(ssn)", "", "", hnd);
    if (args == nullptr) {
        Py_DECREF(pc_cls);
        return SAO_ERR_OS_CALL_FAILED;
    }
    PyObject* obj = PyObject_CallObject(pc_cls, args);
    Py_DECREF(args);
    Py_DECREF(pc_cls);
    if (obj == nullptr) {
        PyErr_Clear();
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_pyobject = obj;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_free_wrapped_ctx(void* pyobject) {
    if (pyobject == nullptr)
        return;
    if (Py_IsInitialized() == 0)
        return;
    PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
    Py_DECREF(obj);
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_teardown_native(void* pyobject) {
    try {
        if (pyobject == nullptr || Py_IsInitialized() == 0)
            return;
        PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(obj, &PluginContextType))
            return;
        auto* context = reinterpret_cast<PluginContextObject*>(obj);
        if (release_native_bridges(context) == SAO_OK)
            clear_callback_ledgers(context);
    } catch (...) {
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_try_teardown_native(void* pyobject) {
    try {
        if (pyobject == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        if (Py_IsInitialized() == 0)
            return SAO_ERR_NOT_INITIALIZED;
        PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(obj, &PluginContextType))
            return SAO_ERR_INVALID_ARGUMENT;
        auto* context = reinterpret_cast<PluginContextObject*>(obj);
        const int32_t status = release_native_bridges(context);
        if (status == SAO_OK)
            clear_callback_ledgers(context);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_bind_loader_context(void* pyobject, void* loader_context) {
    auto* canonical = static_cast<loader::plugin_context_t*>(loader_context);
    bool lease_held = false;
    try {
        if (pyobject == nullptr || canonical == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        if (Py_IsInitialized() == 0)
            return SAO_ERR_NOT_INITIALIZED;
        PyObject* object = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(object, &PluginContextType))
            return SAO_ERR_INVALID_ARGUMENT;
        auto* context = reinterpret_cast<PluginContextObject*>(object);
        if (context->tearing_down)
            return loader::SAO_PLUGINS_ERR_BUSY;
        if (context->loader_context != nullptr || context->loader_context_lease_held)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;

        const int32_t retain_status = loader::plugin_context_retain_host_lease(canonical);
        if (retain_status != SAO_OK)
            return retain_status;
        lease_held = true;

        const char* loader_plugin_id = loader::sao_plugins_ctx_plugin_id(canonical);
        const char* python_plugin_id = PyUnicode_AsUTF8(context->plugin_id);
        if (python_plugin_id == nullptr) {
            PyErr_Clear();
            loader::plugin_context_release_host_lease(canonical);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (loader_plugin_id == nullptr || std::strcmp(loader_plugin_id, python_plugin_id) != 0) {
            loader::plugin_context_release_host_lease(canonical);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        context->loader_context = canonical;
        context->loader_context_lease_held = true;
        return SAO_OK;
    } catch (...) {
        if (lease_held)
            loader::plugin_context_release_host_lease(canonical);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t pyhost_ctx_begin_enable_resources(void* pyobject, uint64_t* out_checkpoint) noexcept {
    try {
        if (pyobject == nullptr || out_checkpoint == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        if (Py_IsInitialized() == 0)
            return SAO_ERR_NOT_INITIALIZED;
        auto* object = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(object, &PluginContextType))
            return SAO_ERR_INVALID_ARGUMENT;
        return begin_enable_resource_checkpoint(reinterpret_cast<PluginContextObject*>(object),
                                                out_checkpoint);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t pyhost_ctx_commit_enable_resources(void* pyobject, uint64_t checkpoint) noexcept {
    try {
        if (pyobject == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        if (Py_IsInitialized() == 0)
            return SAO_ERR_NOT_INITIALIZED;
        auto* object = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(object, &PluginContextType))
            return SAO_ERR_INVALID_ARGUMENT;
        return commit_enable_resources(reinterpret_cast<PluginContextObject*>(object), checkpoint);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t pyhost_ctx_rollback_enable_resources(void* pyobject, uint64_t checkpoint) noexcept {
    try {
        if (pyobject == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        if (Py_IsInitialized() == 0)
            return SAO_ERR_NOT_INITIALIZED;
        auto* object = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(object, &PluginContextType))
            return SAO_ERR_INVALID_ARGUMENT;
        return rollback_enable_resources(reinterpret_cast<PluginContextObject*>(object),
                                         checkpoint);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t pyhost_ctx_remove_enable_resources(void* pyobject) noexcept {
    try {
        if (pyobject == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        if (Py_IsInitialized() == 0)
            return SAO_ERR_NOT_INITIALIZED;
        auto* object = reinterpret_cast<PyObject*>(pyobject);
        if (!PyObject_TypeCheck(object, &PluginContextType))
            return SAO_ERR_INVALID_ARGUMENT;
        return remove_enable_resources(reinterpret_cast<PluginContextObject*>(object));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL sao_plugins_pyhost_sdk_method_count(void) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    if (g_sdk_method_count == 0)
        g_sdk_method_count = compute_sdk_method_count();
    return g_sdk_method_count;
}

// 供 py_host.cpp、单测和宿主内省 PluginContext state.
// 返回 void* 以避免 header include Python.h; 调用方 cast 为 PyObject* 用完 DECREF.
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_get_records(void* pyobject, const char* record_kind) {
    if (pyobject == nullptr || record_kind == nullptr)
        return nullptr;
    if (Py_IsInitialized() == 0)
        return nullptr;
    PyObject* obj = reinterpret_cast<PyObject*>(pyobject);
    if (!PyObject_TypeCheck(obj, &PluginContextType))
        return nullptr;
    PluginContextObject* pc = reinterpret_cast<PluginContextObject*>(obj);
    if (std::strcmp(record_kind, "panels") == 0)
        return Py_NewRef(pc->panels);
    if (std::strcmp(record_kind, "hotkeys") == 0)
        return Py_NewRef(pc->hotkeys);
    if (std::strcmp(record_kind, "subscriptions") == 0)
        return Py_NewRef(pc->subscriptions);
    if (std::strcmp(record_kind, "published") == 0)
        return Py_NewRef(pc->published);
    if (std::strcmp(record_kind, "logs") == 0)
        return Py_NewRef(pc->logs);
    if (std::strcmp(record_kind, "timers") == 0)
        return Py_NewRef(pc->timers);
    if (std::strcmp(record_kind, "timer_tokens") == 0)
        return Py_NewRef(pc->timer_tokens);
    if (std::strcmp(record_kind, "callback_refs") == 0)
        return Py_NewRef(pc->callback_refs);
    if (std::strcmp(record_kind, "notifications") == 0)
        return Py_NewRef(pc->notifications);
    if (std::strcmp(record_kind, "settings") == 0)
        return Py_NewRef(pc->settings);
    if (std::strcmp(record_kind, "menus") == 0)
        return Py_NewRef(pc->menus);
    return nullptr;
}

#else // !SAO_HAS_PYTHON_EMBED

// stub 实装 (无 embed 时).

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_register_native_module(void) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_register_shim_module(void) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_wrap_ctx(void* /*ctx_ptr*/, void** out_pyobject) {
    if (out_pyobject != nullptr)
        *out_pyobject = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_free_wrapped_ctx(void* /*pyobject*/) {}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_teardown_native(void* /*pyobject*/) {}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_try_teardown_native(void* /*pyobject*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_bind_loader_context(void* /*pyobject*/, void* /*loader_context*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL sao_plugins_pyhost_sdk_method_count(void) {
    return 0;
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_get_records(void* /*pyobject*/, const char* /*record_kind*/) {
    return nullptr;
}

#endif // SAO_HAS_PYTHON_EMBED

} // namespace sao::plugins::python_host
