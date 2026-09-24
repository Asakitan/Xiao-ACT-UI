// Full-graph routing precedes execution; execution failures never change engines.
#include "sao/plugins/pymini/pymini_host.h"

#include "pymini_interp.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_context_lifetime_internal.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/sdk_binding/binding_common.h"

#if defined(SAO_PLUGINS_ENABLE_PYTHON)
#include "sao/plugins/python_host/py_error.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_module_bridge.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::pymini {

using loader::host_adapter_vtable;
using loader::plugin_context_t;
using loader::plugin_handle_t;
using loader::plugin_manifest;

#if defined(SAO_PLUGINS_ENABLE_PYTHON)
struct helper_zombie_node {
    python_host::py_plugin_handle_t plugin = nullptr;
    std::unique_ptr<helper_zombie_node> next;
};
#endif

// adapter owner — concrete def of the opaque public handle
struct pymini_adapter_owner_s {
    // per-plugin record
    enum class route_e { pymini, cpython };
    struct rec {
        route_e route = route_e::pymini;
        std::string plugin_id;
        bool ready = false;
        int32_t load_status = SAO_ERR_NOT_INITIALIZED;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        python_host::py_plugin_handle_t pyh = nullptr;
#endif
    };
    bool active = false;
    bool adapter_registered = false;
    std::wstring python_home;
    std::vector<std::wstring> extra_dirs;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    python_host::py_host_handle_t host = nullptr;
    bool host_init_attempted = false;
    int32_t host_init_status = SAO_ERR_NOT_INITIALIZED;
    std::string host_init_error;
    DWORD host_thread = 0;
    size_t helper_states = 0;
    uint64_t next_helper_generation = 1;
    std::unique_ptr<helper_zombie_node> helper_zombies;
#endif
    std::map<plugin_handle_t, rec> plugins;
    std::unordered_map<plugin_handle_t, std::string> last_errors;
    std::string last_error;
};

// ── utf8 ↔ wide helpers (independent of pymini_host's anon-namespace) ──
namespace {

std::wstring widen_u8(const std::string& u8) {
    if (u8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(),
                                      static_cast<int>(u8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()),
                        w.data(), n);
    return w;
}
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
std::string narrow_w(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}
#endif

// error-string helper owned by free via sao_plugins_pymini_free_string
char* dup_err(const std::string& s) {
    if (s.empty())
        return nullptr;
    char* b = static_cast<char*>(std::malloc(s.size() + 1));
    if (!b)
        return nullptr;
    std::memcpy(b, s.data(), s.size() + 1);
    return b;
}

// per-plugin record + route enum (aliases onto the owner's nested rec)
using rec = pymini_adapter_owner_s::rec;
using route_t = pymini_adapter_owner_s::route_e;

std::recursive_mutex g_mu;
pymini_adapter_owner_s* g_owner = nullptr;

pymini_adapter_owner_s* active_owner(void* ud) {
    return static_cast<pymini_adapter_owner_s*>(ud);
}

void set_err(pymini_adapter_owner_s* o, plugin_handle_t plugin, const std::string& e) {
    if (e.empty())
        o->last_errors.erase(plugin);
    else
        o->last_errors[plugin] = e;
}

// ── pyhost delegate wrappers (only compiled with SAO_PLUGINS_ENABLE_PYTHON) ──
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
struct gil_scope {
    void* s = nullptr;
    explicit gil_scope(bool take) {
        if (take)
            s = python_host::sao_plugins_pyhost_gil_scope_enter();
    }
    ~gil_scope() {
        if (s)
            python_host::sao_plugins_pyhost_gil_scope_leave(s);
    }
    bool held() const {
        return s != nullptr;
    }
    gil_scope(const gil_scope&) = delete;
    gil_scope& operator=(const gil_scope&) = delete;
};

struct pyhost_string_deleter {
    void operator()(char* value) const noexcept {
        python_host::sao_plugins_pyhost_free_string(value);
    }
};
using pyhost_string_ptr = std::unique_ptr<char, pyhost_string_deleter>;

int32_t cpython_thread_status(pymini_adapter_owner_s* o, std::string* err) {
    if (o->host_thread != 0 && o->host_thread != GetCurrentThreadId()) {
        *err = "CPython cold-init host requires its initialization thread; host bridge has no "
               "detach API";
        return loader::SAO_PLUGINS_ERR_BUSY;
    }
    return SAO_OK;
}

int32_t ensure_cpython_host(pymini_adapter_owner_s* o, std::string* err) {
    if (o->host_init_attempted) {
        *err = o->host_init_error;
        return o->host_init_status == SAO_OK ? cpython_thread_status(o, err) : o->host_init_status;
    }
    o->host_init_error = "CPython host initialization failed before plugin execution";
    o->host_init_status = SAO_ERR_OS_CALL_FAILED;
    o->host_init_attempted = true;
    if (o->python_home.empty()) {
        o->host_init_status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        o->host_init_error = "CPython required but python_home is unconfigured";
    } else {
        python_host::py_host_config hc{};
        hc.python_home = o->python_home.c_str();
        std::vector<const wchar_t*> extra_ptrs;
        extra_ptrs.reserve(o->extra_dirs.size());
        for (const auto& dir : o->extra_dirs)
            extra_ptrs.push_back(dir.c_str());
        hc.extra_module_dirs = extra_ptrs.data();
        hc.extra_module_dirs_count = static_cast<uint32_t>(extra_ptrs.size());
        // A cold init retains the GIL; the existing bridge only releases ensured scopes.
        gil_scope initial_gil(true);
        o->host_init_status = python_host::sao_plugins_pyhost_init(&hc, &o->host);
        if (o->host_init_status == SAO_OK && !o->host)
            o->host_init_status = SAO_ERR_OS_CALL_FAILED;
        if (o->host_init_status == SAO_OK) {
            if (!initial_gil.held())
                o->host_thread = GetCurrentThreadId();
            o->host_init_error.clear();
        } else {
            o->host_init_error += ": python_home=" + narrow_w(o->python_home) +
                                  ", status=" + std::to_string(o->host_init_status);
            char* detail = nullptr;
            (void)python_host::sao_plugins_pyhost_take_error(&detail);
            pyhost_string_ptr owned_detail(detail);
            if (owned_detail) {
                o->host_init_error += ": ";
                o->host_init_error += owned_detail.get();
            }
        }
    }
    *err = o->host_init_error;
    return o->host_init_status;
}

void pyh_error(rec& r, std::string* err) {
    char* text = nullptr;
    if (r.pyh)
        (void)python_host::sao_plugins_pyhost_get_last_error(r.pyh, &text);
    pyhost_string_ptr owned_text(text);
    if (owned_text && err != nullptr) {
        *err = owned_text.get();
    }
}

int32_t pyh_load(pymini_adapter_owner_s* o, plugin_context_t* lctx, const std::wstring& dir,
                 const std::string& entry, const std::string& id, bool module_identity, rec* r,
                 std::string* err) {
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        return SAO_ERR_OS_CALL_FAILED;
    }
    python_host::py_plugin_handle_t pyh = nullptr;
    const int32_t st = python_host::sao_plugins_pyhost_load_plugin(
        o->host, dir.c_str(), entry.c_str(), id.c_str(), nullptr, &pyh);
    r->pyh = pyh;
    if (st != SAO_OK || !pyh) {
        // pyhost keeps a published handle on load failure so last_error stays
        // introspectable — harvest it, then release the handle so
        // pyhost_shutdown does not see a leaked active_plugin.
        if (err) {
            char* pe = nullptr;
            if (pyh)
                (void)python_host::sao_plugins_pyhost_get_last_error(pyh, &pe);
            pyhost_string_ptr owned_error(pe);
            *err = owned_error ? owned_error.get() : "pyhost load failed";
        }
        if (pyh && python_host::sao_plugins_pyhost_unload_plugin(pyh) == SAO_OK)
            r->pyh = nullptr;
        return st == SAO_OK ? SAO_ERR_OS_CALL_FAILED : st;
    }
    // bind the canonical loader ctx (the pyhost ctx PyObject ↔ plugin_context_t)
    const int32_t bs =
        module_identity
            ? python_host::sao_plugins_pyhost_ctx_bind_loader_context_module(
                  python_host::sao_plugins_pyhost_get_ctx_pyobject(pyh), lctx, id.c_str())
            : python_host::sao_plugins_pyhost_ctx_bind_loader_context(
                  python_host::sao_plugins_pyhost_get_ctx_pyobject(pyh), lctx);
    if (bs != SAO_OK) {
        if (python_host::sao_plugins_pyhost_unload_plugin(pyh) == SAO_OK)
            r->pyh = nullptr;
        if (err)
            *err = "pyhost ctx_bind failed";
        return bs;
    }
    r->pyh = pyh;
    return SAO_OK;
}
int32_t pyh_hook(rec& r, const char* which, std::string* err) {
    if (!r.pyh)
        return SAO_ERR_HANDLE_INVALID;
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        return SAO_ERR_OS_CALL_FAILED;
    }
    int32_t st = SAO_ERR_INVALID_ARGUMENT;
    if (std::strcmp(which, "on_load") == 0)
        st = python_host::sao_plugins_pyhost_call_on_load(r.pyh);
    else if (std::strcmp(which, "on_enable") == 0)
        st = python_host::sao_plugins_pyhost_call_on_enable(r.pyh);
    else if (std::strcmp(which, "on_disable") == 0)
        st = python_host::sao_plugins_pyhost_call_on_disable(r.pyh);
    if (st != SAO_OK)
        pyh_error(r, err);
    return st;
}
int32_t pyh_on_unload(rec& r, bool* allow, std::string* err) {
    if (!r.pyh) {
        *allow = true;
        return SAO_OK;
    }
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        *allow = false;
        return SAO_ERR_OS_CALL_FAILED;
    }
    bool a = true;
    const int32_t st = python_host::sao_plugins_pyhost_call_on_unload(r.pyh, &a);
    *allow = a;
    if (st != SAO_OK)
        pyh_error(r, err);
    return st;
}
int32_t pyh_unload(rec& r, std::string* err) {
    if (!r.pyh)
        return SAO_OK;
    gil_scope gil(true);
    if (!gil.held()) {
        if (err)
            *err = "cannot acquire CPython GIL";
        return SAO_ERR_OS_CALL_FAILED;
    }
    const int32_t st = python_host::sao_plugins_pyhost_unload_plugin(r.pyh);
    if (st == SAO_OK)
        r.pyh = nullptr;
    else
        pyh_error(r, err);
    return st;
}

int32_t helper_owner_status(pymini_adapter_owner_s* owner, std::string* error) {
    std::lock_guard lock(g_mu);
    if (owner == nullptr || owner != g_owner || !owner->active || owner->host == nullptr) {
        if (error != nullptr)
            *error = "CPython helper owner is unavailable";
        return SAO_ERR_HANDLE_INVALID;
    }
    return cpython_thread_status(owner, error);
}

bool quarantine_helper_plugin(pymini_adapter_owner_s* owner, rec& runtime,
                              std::unique_ptr<helper_zombie_node>& slot) noexcept {
    if (runtime.pyh == nullptr)
        return true;
    try {
        std::lock_guard lock(g_mu);
        if (owner == nullptr || owner != g_owner || slot == nullptr)
            return false;
        slot->plugin = runtime.pyh;
        slot->next = std::move(owner->helper_zombies);
        owner->helper_zombies = std::move(slot);
        runtime.pyh = nullptr;
        return true;
    } catch (...) {
        return false;
    }
}

bool cleanup_unpublished_helper(pymini_adapter_owner_s* owner, rec& runtime,
                                std::unique_ptr<helper_zombie_node>& slot) noexcept {
    if (runtime.pyh == nullptr)
        return true;
    try {
        std::string ignored;
        if (helper_owner_status(owner, &ignored) == SAO_OK &&
            pyh_unload(runtime, &ignored) == SAO_OK) {
            return true;
        }
    } catch (...) {
    }
    return quarantine_helper_plugin(owner, runtime, slot);
}

namespace script = sao::plugins::script_ctx;
using ordered_json = nlohmann::ordered_json;

constexpr size_t kCrossLanguageMaxDepth = sdk_binding::kMaximumBindingJsonDepth;
constexpr size_t kCrossLanguageMaxNodes = sdk_binding::kMaximumBindingJsonNodes;
const char k_cpython_helper_resource_key = 0;

bool consume_cross_language_string(std::string_view value, size_t& string_bytes, std::string& error,
                                   const char* subject) {
    constexpr size_t maximum_string = sdk_binding::kMaximumBindingJsonStringBytes;
    constexpr size_t maximum_total = sdk_binding::kMaximumBindingJsonTotalStringBytes;
    if (value.find('\0') != std::string_view::npos || value.size() > maximum_string ||
        value.size() > maximum_total || string_bytes > maximum_total - value.size()) {
        error = std::string(subject) + " exceeds the cross-language string budget";
        return false;
    }
    string_bytes += value.size();
    return true;
}

enum class cpython_module_stage { loading, ready, failed };

struct cpython_module_record {
    rec runtime;
    std::string module_id;
    std::unique_ptr<helper_zombie_node> quarantine_slot;
    std::condition_variable ready;
    DWORD loading_thread = 0;
    cpython_module_stage stage = cpython_module_stage::loading;
    int32_t status = SAO_ERR_NOT_INITIALIZED;
    std::string error;
};

struct canonical_path_less {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

struct cpython_helper_state final : loader::context_runtime_resource {
    std::weak_ptr<loader::context_runtime_state> lifetime;
    pymini_adapter_owner_s* owner = nullptr;
    std::mutex mutex;
    std::map<std::wstring, std::shared_ptr<cpython_module_record>, canonical_path_less> modules;
    uint64_t generation = 0;
    uint64_t next_module_identity = 1;
    bool counted = false;
    bool retired = false;

    void retire() noexcept override {
        decltype(modules) pending;
        {
            std::lock_guard lock(mutex);
            if (retired)
                return;
            retired = true;
            pending.swap(modules);
        }

        for (auto& [path, module] : pending) {
            (void)path;
            if (module == nullptr)
                continue;
            if (module->stage == cpython_module_stage::loading) {
                module->status = SAO_ERR_HANDLE_INVALID;
                module->stage = cpython_module_stage::failed;
                module->ready.notify_all();
            }
            (void)cleanup_unpublished_helper(owner, module->runtime, module->quarantine_slot);
        }

        try {
            std::lock_guard owner_lock(g_mu);
            if (owner != nullptr && owner == g_owner) {
                if (counted && owner->helper_states != 0)
                    --owner->helper_states;
            }
        } catch (...) {
        }
        counted = false;
        owner = nullptr;
    }

    ~cpython_helper_state() override {
        retire();
    }
};

struct cpython_helper_invocation {
    std::shared_ptr<cpython_helper_state> state;
    loader::context_runtime_lease lease;

    explicit cpython_helper_invocation(const std::weak_ptr<cpython_helper_state>& weak)
        : state(weak.lock()), lease(state ? state->lifetime.lock() : nullptr) {}
    explicit operator bool() const noexcept {
        return static_cast<bool>(lease);
    }
};

int32_t acquire_helper_plugin(cpython_helper_invocation& invocation, const std::wstring& key,
                              python_host::py_plugin_handle_t* out_plugin, std::string* out_error) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    std::string owner_error;
    const int32_t owner_status = helper_owner_status(invocation.state->owner, &owner_error);
    if (owner_status != SAO_OK) {
        if (out_error != nullptr)
            *out_error = std::move(owner_error);
        return owner_status;
    }
    std::lock_guard lock(invocation.state->mutex);
    const auto found = invocation.state->modules.find(key);
    if (invocation.state->retired || found == invocation.state->modules.end() ||
        found->second == nullptr || found->second->stage != cpython_module_stage::ready ||
        found->second->runtime.pyh == nullptr) {
        if (out_error != nullptr)
            *out_error = "CPython helper module is unavailable";
        return SAO_ERR_HANDLE_INVALID;
    }
    *out_plugin = found->second->runtime.pyh;
    return SAO_OK;
}

bool script_value_to_json(const script::script_value_ptr& value, ordered_json& output, size_t depth,
                          size_t& nodes, size_t& string_bytes, std::string& error) {
    if (++nodes > kCrossLanguageMaxNodes) {
        error = "helper arguments exceed the cross-language complexity budget";
        return false;
    }
    if (!value || value->k == script::script_value::kind::null) {
        output = nullptr;
        return true;
    }
    switch (value->k) {
    case script::script_value::kind::boolean:
        output = value->boolean;
        return true;
    case script::script_value::kind::integer:
        output = value->integer;
        return true;
    case script::script_value::kind::number:
        if (!std::isfinite(value->number)) {
            error = "helper arguments cannot contain non-finite numbers";
            return false;
        }
        output = value->number;
        return true;
    case script::script_value::kind::string:
        if (!consume_cross_language_string(value->text, string_bytes, error, "helper arguments"))
            return false;
        output = value->text;
        return true;
    case script::script_value::kind::bytes:
        error = "raw bytes cannot cross the CPython helper JSON boundary";
        return false;
    case script::script_value::kind::list:
        if (depth >= kCrossLanguageMaxDepth) {
            error = "helper arguments exceed the cross-language depth budget";
            return false;
        }
        output = ordered_json::array();
        for (const auto& item : value->items) {
            ordered_json converted;
            if (!script_value_to_json(item, converted, depth + 1, nodes, string_bytes, error))
                return false;
            output.push_back(std::move(converted));
        }
        return true;
    case script::script_value::kind::map:
        if (depth >= kCrossLanguageMaxDepth) {
            error = "helper arguments exceed the cross-language depth budget";
            return false;
        }
        output = ordered_json::object();
        {
            std::unordered_set<std::string> keys;
            keys.reserve(value->object.size());
            for (const auto& [key, item] : value->object) {
                if (!consume_cross_language_string(key, string_bytes, error, "helper arguments") ||
                    !keys.emplace(key).second) {
                    if (error.empty())
                        error = "helper arguments contain duplicate map keys";
                    return false;
                }
                ordered_json converted;
                if (!script_value_to_json(item, converted, depth + 1, nodes, string_bytes, error))
                    return false;
                output[key] = std::move(converted);
            }
        }
        return true;
    case script::script_value::kind::function:
        error = "callbacks cannot cross the CPython helper JSON boundary";
        return false;
    }
    error = "unsupported helper argument";
    return false;
}

bool json_to_script_value(const ordered_json& value, script::script_value_ptr& output, size_t depth,
                          size_t& nodes, size_t& string_bytes, std::string& error) {
    if (++nodes > kCrossLanguageMaxNodes) {
        error = "helper result exceeds the cross-language complexity budget";
        return false;
    }
    if (value.is_null()) {
        output = script::script_value::null_value();
    } else if (value.is_boolean()) {
        output = script::script_value::make_boolean(value.get<bool>());
    } else if (value.is_number_integer()) {
        output = script::script_value::make_integer(value.get<int64_t>());
    } else if (value.is_number_unsigned()) {
        const uint64_t number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            error = "helper result integer exceeds int64";
            return false;
        }
        output = script::script_value::make_integer(static_cast<int64_t>(number));
    } else if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            error = "helper result contains a non-finite number";
            return false;
        }
        output = script::script_value::make_number(number);
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (!consume_cross_language_string(text, string_bytes, error, "helper result"))
            return false;
        output = script::script_value::make_string(text);
    } else if (value.is_array()) {
        if (depth >= kCrossLanguageMaxDepth) {
            error = "helper result exceeds the cross-language depth budget";
            return false;
        }
        std::vector<script::script_value_ptr> items;
        items.reserve(value.size());
        for (const auto& item : value) {
            script::script_value_ptr converted;
            if (!json_to_script_value(item, converted, depth + 1, nodes, string_bytes, error))
                return false;
            items.push_back(std::move(converted));
        }
        output = script::script_value::make_list(std::move(items));
    } else if (value.is_object()) {
        if (depth >= kCrossLanguageMaxDepth) {
            error = "helper result exceeds the cross-language depth budget";
            return false;
        }
        std::vector<std::pair<std::string, script::script_value_ptr>> items;
        items.reserve(value.size());
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            if (!consume_cross_language_string(iterator.key(), string_bytes, error,
                                               "helper result"))
                return false;
            script::script_value_ptr converted;
            if (!json_to_script_value(iterator.value(), converted, depth + 1, nodes, string_bytes,
                                      error))
                return false;
            items.emplace_back(iterator.key(), std::move(converted));
        }
        output = script::script_value::make_map(std::move(items));
    } else {
        error = "unsupported helper result";
        return false;
    }
    return true;
}

std::string pyhost_text(char* value) {
    if (value == nullptr)
        return {};
    pyhost_string_ptr owned(value);
    return std::string(owned.get());
}

int32_t invoke_cpython_helper(const std::weak_ptr<cpython_helper_state>& weak,
                              const std::wstring& key, const std::string& member,
                              const std::vector<script::script_value_ptr>& arguments,
                              script::script_value_ptr* output, std::string* out_error) noexcept
    try {
    if (output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    if (out_error)
        out_error->clear();
    if (member.empty() || member.size() > 4096 || member.find('\0') != std::string::npos) {
        if (out_error)
            *out_error = "invalid CPython helper member name";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    cpython_helper_invocation invocation(weak);
    if (!invocation) {
        if (out_error)
            *out_error = "CPython helper is retired";
        return SAO_ERR_HANDLE_INVALID;
    }
    python_host::py_plugin_handle_t plugin = nullptr;
    const int32_t admission = acquire_helper_plugin(invocation, key, &plugin, out_error);
    if (admission != SAO_OK)
        return admission;

    ordered_json json_arguments = ordered_json::array();
    size_t nodes = 1;
    size_t string_bytes = 0;
    std::string error;
    std::string request;
    try {
        for (const auto& argument : arguments) {
            ordered_json converted;
            if (!script_value_to_json(argument, converted, 0, nodes, string_bytes, error)) {
                if (out_error)
                    *out_error = std::move(error);
                return SAO_ERR_INVALID_ARGUMENT;
            }
            json_arguments.push_back(std::move(converted));
        }
        request = json_arguments.dump();
    } catch (...) {
        if (out_error)
            *out_error = "helper arguments could not be serialized";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (request.empty() || request.size() > sdk_binding::kMaximumBindingJsonBytes ||
        !sdk_binding::sao_plugins_binding_validate_json_text(
            reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
        if (out_error)
            *out_error = "helper arguments exceed the cross-language JSON budget";
        return SAO_ERR_INVALID_ARGUMENT;
    }

    char* result_text = nullptr;
    char* error_text = nullptr;
    const int32_t status = python_host::sao_plugins_pyhost_module_call_json(
        plugin, member.c_str(), request.c_str(), &result_text, &error_text);
    pyhost_string_ptr owned_result(result_text);
    const std::string retained_error = pyhost_text(error_text);
    if (status != SAO_OK) {
        if (out_error)
            *out_error = retained_error.empty() ? "CPython helper call failed" : retained_error;
        return status;
    }
    size_t result_size = 0;
    if (owned_result != nullptr &&
        (!sdk_binding::sao_plugins_binding_bounded_json_c_string(owned_result.get(), result_size) ||
         !sdk_binding::sao_plugins_binding_validate_json_text(
             reinterpret_cast<const uint8_t*>(owned_result.get()), result_size))) {
        if (out_error)
            *out_error = "CPython helper returned out-of-contract JSON";
        return SAO_ERR_OS_CALL_FAILED;
    }
    const char* result_begin = owned_result == nullptr ? "null" : owned_result.get();
    const size_t result_length = owned_result == nullptr ? 4U : result_size;
    ordered_json result =
        ordered_json::parse(result_begin, result_begin + result_length, nullptr, false);
    if (result.is_discarded()) {
        if (out_error)
            *out_error = "CPython helper returned invalid JSON";
        return SAO_ERR_OS_CALL_FAILED;
    }
    nodes = 0;
    string_bytes = 0;
    if (!json_to_script_value(result, *output, 0, nodes, string_bytes, error)) {
        if (out_error)
            *out_error = std::move(error);
        return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    return SAO_OK;
} catch (...) {
    if (output != nullptr)
        output->reset();
    if (out_error != nullptr) {
        try {
            *out_error = "CPython helper call failed at the native boundary";
        } catch (...) {
        }
    }
    return SAO_ERR_OS_CALL_FAILED;
}

struct cpython_script_module final : script::script_module {
    std::string id;
    std::wstring key;
    std::weak_ptr<cpython_helper_state> state;

    const std::string& module_id() const noexcept override {
        return id;
    }

    std::vector<std::string> member_names() const override try {
        cpython_helper_invocation invocation(state);
        if (!invocation)
            return {};
        python_host::py_plugin_handle_t plugin = nullptr;
        if (acquire_helper_plugin(invocation, key, &plugin, nullptr) != SAO_OK)
            return {};
        char* json_text = nullptr;
        char* error_text = nullptr;
        const int32_t status =
            python_host::sao_plugins_pyhost_module_members_json(plugin, &json_text, &error_text);
        pyhost_string_ptr owned_json(json_text);
        pyhost_string_ptr owned_error(error_text);
        (void)owned_error;
        if (status != SAO_OK)
            return {};
        size_t json_size = 0;
        if (owned_json != nullptr &&
            (!sdk_binding::sao_plugins_binding_bounded_json_c_string(owned_json.get(), json_size) ||
             !sdk_binding::sao_plugins_binding_validate_json_text(
                 reinterpret_cast<const uint8_t*>(owned_json.get()), json_size))) {
            return {};
        }
        const char* begin = owned_json == nullptr ? "[]" : owned_json.get();
        const size_t length = owned_json == nullptr ? 2U : json_size;
        ordered_json names = ordered_json::parse(begin, begin + length, nullptr, false);
        if (!names.is_array())
            return {};
        std::vector<std::string> output;
        output.reserve(names.size());
        for (const auto& name : names)
            if (name.is_string())
                output.push_back(name.get<std::string>());
        return output;
    } catch (...) {
        return {};
    }

    int32_t get(const std::string& name, script::script_value_ptr* output,
                std::string* out_error) override try {
        if (output == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *output = nullptr;
        if (out_error)
            out_error->clear();
        if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos) {
            if (out_error)
                *out_error = "invalid CPython helper member name";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        cpython_helper_invocation invocation(state);
        if (!invocation) {
            if (out_error)
                *out_error = "CPython helper is retired";
            return SAO_ERR_HANDLE_INVALID;
        }
        python_host::py_plugin_handle_t plugin = nullptr;
        const int32_t admission = acquire_helper_plugin(invocation, key, &plugin, out_error);
        if (admission != SAO_OK)
            return admission;
        bool callable = false;
        char* json_text = nullptr;
        char* error_text = nullptr;
        const int32_t status = python_host::sao_plugins_pyhost_module_get_json(
            plugin, name.c_str(), &callable, &json_text, &error_text);
        pyhost_string_ptr owned_json(json_text);
        const std::string retained_error = pyhost_text(error_text);
        if (status != SAO_OK) {
            if (out_error)
                *out_error = retained_error;
            return status;
        }
        if (callable) {
            const auto weak = state;
            const auto module_key = key;
            *output = script::script_value::make_function(
                [weak, module_key, name](const auto& arguments, auto* result, auto* error) {
                    return invoke_cpython_helper(weak, module_key, name, arguments, result, error);
                });
            return SAO_OK;
        }
        size_t json_size = 0;
        if (owned_json != nullptr &&
            (!sdk_binding::sao_plugins_binding_bounded_json_c_string(owned_json.get(), json_size) ||
             !sdk_binding::sao_plugins_binding_validate_json_text(
                 reinterpret_cast<const uint8_t*>(owned_json.get()), json_size))) {
            if (out_error)
                *out_error = "CPython helper member returned out-of-contract JSON";
            return SAO_ERR_OS_CALL_FAILED;
        }
        const char* begin = owned_json == nullptr ? "null" : owned_json.get();
        const size_t length = owned_json == nullptr ? 4U : json_size;
        ordered_json value = ordered_json::parse(begin, begin + length, nullptr, false);
        if (value.is_discarded()) {
            if (out_error)
                *out_error = "CPython helper member returned invalid JSON";
            return SAO_ERR_OS_CALL_FAILED;
        }
        size_t nodes = 0;
        size_t string_bytes = 0;
        std::string error;
        if (!json_to_script_value(value, *output, 0, nodes, string_bytes, error)) {
            if (out_error)
                *out_error = std::move(error);
            return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        return SAO_OK;
    } catch (...) {
        if (output != nullptr)
            output->reset();
        if (out_error != nullptr) {
            try {
                *out_error = "CPython helper member access failed at the native boundary";
            } catch (...) {
            }
        }
        return SAO_ERR_OS_CALL_FAILED;
    }

    int32_t call(const std::string& name, const std::vector<script::script_value_ptr>& arguments,
                 script::script_value_ptr* output, std::string* out_error) override {
        return invoke_cpython_helper(state, key, name, arguments, output, out_error);
    }
};

uint64_t helper_path_hash(const std::wstring& value) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t character : value) {
        const uint32_t code = static_cast<uint32_t>(character);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<uint8_t>((code >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void append_hex64(std::string& output, uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        output.push_back(digits[(value >> shift) & 0xfU]);
}

void append_hex_bytes(std::string& output, std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
}

std::string helper_runtime_id(std::string_view owner_id, const std::wstring& path,
                              uint64_t generation, uint64_t identity) {
    std::string result = "sao_cp_helper_";
    result.reserve(result.size() + owner_id.size() * 2U + 55U);
    append_hex_bytes(result, owner_id);
    result += "_g";
    append_hex64(result, generation);
    result += "_p";
    append_hex64(result, helper_path_hash(path));
    result += "_i";
    append_hex64(result, identity);
    return result;
}

int32_t cpython_helper_probe(loader::plugin_context_t*, const wchar_t*, std::string& note,
                             void*) noexcept {
    try {
        std::lock_guard lock(g_mu);
        if (g_owner == nullptr || !g_owner->active || g_owner->python_home.empty()) {
            note = "CPython helper runtime is unconfigured";
            return SAO_ERR_NOT_IMPLEMENTED;
        }
        if (!g_owner->host_init_attempted &&
            !python_host::sao_plugins_pyhost_available(g_owner->python_home.c_str())) {
            note = "configured CPython helper runtime is unavailable";
            return SAO_ERR_NOT_IMPLEMENTED;
        }
        if (g_owner->host_init_attempted && g_owner->host_init_status != SAO_OK) {
            note = g_owner->host_init_error.empty() ? "CPython helper initialization failed"
                                                    : g_owner->host_init_error;
            return g_owner->host_init_status == loader::SAO_PLUGINS_ERR_UNSUPPORTED
                       ? SAO_ERR_NOT_IMPLEMENTED
                       : g_owner->host_init_status;
        }
        if (g_owner->host_init_attempted && g_owner->host != nullptr) {
            const int32_t thread_status = cpython_thread_status(g_owner, &note);
            if (thread_status != SAO_OK)
                return thread_status;
        }
        note.clear();
        return SAO_OK;
    } catch (...) {
        note = "CPython helper preflight failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cpython_helper_load(loader::plugin_context_t* context, const wchar_t* absolute_path,
                            const std::string& logical_name,
                            std::shared_ptr<script::script_module>* out_module,
                            std::string* out_error, void*) noexcept {
    if (out_module != nullptr)
        out_module->reset();
    if (out_error != nullptr)
        out_error->clear();
    if (context == nullptr || absolute_path == nullptr || out_module == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        loader::context_runtime_lease invocation(context);
        if (!invocation)
            return SAO_ERR_HANDLE_INVALID;

        pymini_adapter_owner_s* owner = nullptr;
        std::string host_error;
        int32_t status = SAO_OK;
        auto state = std::static_pointer_cast<cpython_helper_state>(
            invocation.resource(&k_cpython_helper_resource_key));
        if (!state) {
            auto candidate = std::make_shared<cpython_helper_state>();
            candidate->lifetime = invocation.state();
            {
                std::lock_guard lock(g_mu);
                owner = g_owner;
                if (owner == nullptr || !owner->active)
                    return SAO_ERR_NOT_INITIALIZED;
                status = ensure_cpython_host(owner, &host_error);
                if (status != SAO_OK) {
                    if (out_error)
                        *out_error = host_error.empty() ? "CPython helper initialization failed"
                                                        : std::move(host_error);
                    return status;
                }
                if (owner->next_helper_generation == 0)
                    return SAO_ERR_OS_CALL_FAILED;
                if (owner->helper_states == (std::numeric_limits<size_t>::max)())
                    return SAO_ERR_OS_CALL_FAILED;
                candidate->owner = owner;
                candidate->generation = owner->next_helper_generation++;
                candidate->counted = true;
                ++owner->helper_states;
            }
            state = std::static_pointer_cast<cpython_helper_state>(
                invocation.resource(&k_cpython_helper_resource_key, candidate));
            candidate.reset();
        }
        if (!state)
            return SAO_ERR_OS_CALL_FAILED;
        owner = state->owner;
        status = helper_owner_status(owner, &host_error);
        if (status != SAO_OK) {
            if (out_error != nullptr)
                *out_error = host_error.empty() ? "CPython helper owner is unavailable"
                                                : std::move(host_error);
            return status;
        }

        const wchar_t* root_text = loader::sao_plugins_ctx_path(context);
        if (root_text == nullptr || *root_text == L'\0')
            return SAO_ERR_HANDLE_INVALID;
        const char* owner_id_text = loader::sao_plugins_ctx_plugin_id(context);
        if (owner_id_text == nullptr || *owner_id_text == '\0')
            return SAO_ERR_HANDLE_INVALID;
        const std::string_view owner_id(owner_id_text);
        constexpr size_t kMaximumHelperIdentityBytes = 64U * 1024U;
        if (owner_id.size() > kMaximumHelperIdentityBytes || logical_name.empty() ||
            logical_name.size() > kMaximumHelperIdentityBytes ||
            logical_name.find('\0') != std::string::npos) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::error_code path_error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(root_text, path_error);
        if (path_error)
            return SAO_ERR_OS_CALL_FAILED;
        const std::filesystem::path full =
            std::filesystem::weakly_canonical(absolute_path, path_error);
        if (path_error)
            return SAO_ERR_OS_CALL_FAILED;
        const std::filesystem::path relative = full.lexically_relative(root);
        if (relative.empty() || relative == L"." || relative.has_root_path() ||
            *relative.begin() == L"..") {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const std::wstring key = full.native();
        const std::u8string encoded_entry = relative.generic_u8string();
        const std::string entry(reinterpret_cast<const char*>(encoded_entry.data()),
                                encoded_entry.size());

        std::shared_ptr<cpython_module_record> record;
        bool load_owner = false;
        {
            std::unique_lock state_lock(state->mutex);
            if (state->retired)
                return SAO_ERR_HANDLE_INVALID;
            const auto found = state->modules.find(key);
            if (found != state->modules.end()) {
                record = found->second;
                if (record == nullptr)
                    return SAO_ERR_OS_CALL_FAILED;
                if (record->stage == cpython_module_stage::loading) {
                    if (record->loading_thread == GetCurrentThreadId()) {
                        if (out_error != nullptr)
                            *out_error = "recursive CPython helper load for the same module";
                        return loader::SAO_PLUGINS_ERR_BUSY;
                    }
                    record->ready.wait(state_lock, [&] {
                        return state->retired || record->stage != cpython_module_stage::loading;
                    });
                }
                if (state->retired)
                    return SAO_ERR_HANDLE_INVALID;
                if (record->stage != cpython_module_stage::ready) {
                    if (out_error != nullptr)
                        *out_error = record->error;
                    return record->status;
                }
            } else {
                if (state->next_module_identity == 0)
                    return SAO_ERR_OS_CALL_FAILED;
                const uint64_t module_identity = state->next_module_identity++;
                record = std::make_shared<cpython_module_record>();
                record->quarantine_slot = std::make_unique<helper_zombie_node>();
                record->loading_thread = GetCurrentThreadId();
                record->module_id = logical_name;
                record->runtime.route = route_t::cpython;
                record->runtime.plugin_id =
                    helper_runtime_id(owner_id, key, state->generation, module_identity);
                const auto [_, inserted] = state->modules.emplace(key, record);
                if (!inserted)
                    return SAO_ERR_OS_CALL_FAILED;
                load_owner = true;
            }
        }

        if (load_owner) {
            std::string load_error;
            try {
                status = pyh_load(owner, context, root.wstring(), entry, record->runtime.plugin_id,
                                  true, &record->runtime, &load_error);
            } catch (const std::exception& error) {
                status = SAO_ERR_OS_CALL_FAILED;
                try {
                    load_error = error.what();
                } catch (...) {
                }
            } catch (...) {
                status = SAO_ERR_OS_CALL_FAILED;
            }
            const bool cleanup_complete = status == SAO_OK || record->runtime.pyh == nullptr;
            {
                std::lock_guard state_lock(state->mutex);
                record->status = status;
                record->error = std::move(load_error);
                record->loading_thread = 0;
                record->stage =
                    status == SAO_OK ? cpython_module_stage::ready : cpython_module_stage::failed;
                if (status != SAO_OK && cleanup_complete) {
                    const auto found = state->modules.find(key);
                    if (found != state->modules.end() && found->second == record)
                        state->modules.erase(found);
                }
            }
            record->ready.notify_all();
            if (status != SAO_OK) {
                if (out_error != nullptr) {
                    *out_error =
                        record->error.empty() ? "CPython helper load failed" : record->error;
                }
                return status;
            }
        }

        auto module = std::make_shared<cpython_script_module>();
        module->id = record->module_id;
        module->key = key;
        module->state = state;
        *out_module = std::move(module);
        return SAO_OK;
    } catch (const std::exception& error) {
        if (out_error)
            *out_error = error.what();
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        if (out_error)
            *out_error = "CPython helper load failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

const char* const k_cpython_helper_extensions[] = {"py", nullptr};
script::script_engine_ops g_cpython_helper_ops{
    "cpython", 100, k_cpython_helper_extensions, &cpython_helper_probe, &cpython_helper_load,
    nullptr,
};
#endif

// ── route decision ───────────────────────────────────────────────────
int32_t classify_plugin(const plugin_manifest* m, const pymini_adapter_owner_s* o, route_t* route,
                        std::string* reason) {
    *route = route_t::pymini;
    reason->clear();
    if (!m || m->language != loader::engine_kind::python || !m->native_entry.empty() ||
        m->source_path.empty() || m->entry.empty() ||
        m->source_path.find('\0') != std::string::npos ||
        m->entry.find('\0') != std::string::npos) {
        *reason = "expected a Python manifest with source_path and entry";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto root = widen_u8(m->source_path);
    const auto entry = widen_u8(m->entry);
    if (root.empty() || entry.empty()) {
        *reason = "invalid UTF-8 in plugin path";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!m->py_runtime.empty() && m->py_runtime != "auto" && m->py_runtime != "pymini" &&
        m->py_runtime != "cpython") {
        *reason = "invalid py_runtime: " + m->py_runtime;
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        if (m->py_runtime == "cpython") {
            const std::filesystem::path relative(entry);
            if (relative.has_root_path() || entry.find(L':') != std::wstring::npos) {
                *reason = "CPython entry must be a relative path";
                return SAO_ERR_INVALID_ARGUMENT;
            }
            for (const auto& part : relative)
                if (part == L"..") {
                    *reason = "CPython entry escapes plugin root";
                    return SAO_ERR_INVALID_ARGUMENT;
                }
            const auto path = std::filesystem::path(root) / relative;
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                *reason = m->entry +
                          ": CPython entry read failed, win32=" + std::to_string(GetLastError());
                return SAO_ERR_OS_CALL_FAILED;
            }
            CloseHandle(file);
            *route = route_t::cpython;
            *reason = "py_runtime:cpython requested";
            return SAO_OK;
        }
        const bool native = pymini_preflight_plugin(root, entry, o->extra_dirs, reason);
        if (native) {
            reason->clear();
            return SAO_OK;
        }
        if (m->py_runtime == "pymini") {
            *reason = "py_runtime:pymini does not support " + *reason;
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        *route = route_t::cpython;
        return SAO_OK;
    } catch (const py_error& e) {
        *reason = (e.file.empty() ? m->entry : e.file) + ":" + std::to_string(e.pos.line) + ": " +
                  e.kind + ": " + e.message;
        return e.code != SAO_OK && e.code != loader::SAO_PLUGINS_ERR_UNSUPPORTED
                   ? e.code
                   : SAO_ERR_OS_CALL_FAILED;
    } catch (const std::exception& e) {
        *reason = e.what();
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        *reason = "plugin preflight failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── vtable functions ─────────────────────────────────────────────────
int32_t SAO_PLUGINS_CALL adapter_load(plugin_handle_t plugin, const plugin_manifest* manifest,
                                      void* host_user_data) try {
    pymini_adapter_owner_s* o = active_owner(host_user_data);
    std::lock_guard<std::recursive_mutex> g(g_mu);
    if (!o || o != g_owner || !o->active)
        return SAO_ERR_NOT_INITIALIZED;
    if (!plugin || !manifest)
        return SAO_ERR_INVALID_ARGUMENT;
    if (o->plugins.count(plugin))
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    try {
        set_err(o, plugin, {});
        route_t route;
        std::string reason;
        int32_t status = classify_plugin(manifest, o, &route, &reason);
        if (status != SAO_OK) {
            set_err(o, plugin, reason);
            return status;
        }
        if (route == route_t::cpython) {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            std::string host_error;
            status = ensure_cpython_host(o, &host_error);
            if (status != SAO_OK) {
                set_err(o, plugin, reason + "; " + host_error);
                return status;
            }
#else
            set_err(o, plugin, reason + "; CPython delegate not built");
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
        }
        plugin_context_t* lctx = nullptr;
        if (loader::sao_plugins_lifecycle_get_context(plugin, &lctx) != SAO_OK || !lctx) {
            set_err(o, plugin, "loader plugin context unavailable");
            return SAO_ERR_NOT_INITIALIZED;
        }
        sao::plugins::script_ctx::ctx_surface_advisory_check(loader::engine_kind::python, lctx,
                                                             manifest);
        rec candidate;
        candidate.route = route;
        candidate.plugin_id = manifest->plugin_id;
        rec& slot = o->plugins.emplace(plugin, std::move(candidate)).first->second;
        const std::wstring dir = widen_u8(manifest->source_path);
        std::string err;
        if (route == route_t::pymini) {
            const int rc =
                pymini_host_load_plugin(lctx, manifest->plugin_id.c_str(), dir.c_str(),
                                        widen_u8(manifest->entry).c_str(), o->extra_dirs, &err);
            status = rc == 0 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
        } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            status =
                pyh_load(o, lctx, dir, manifest->entry, manifest->plugin_id, false, &slot, &err);
#endif
        }
        if (status != SAO_OK) {
            bool retained =
                route == route_t::pymini && pymini_host_is_loaded(manifest->plugin_id.c_str());
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            retained = retained || slot.pyh != nullptr;
#endif
            if (!retained)
                o->plugins.erase(plugin);
            else
                slot.load_status = status;
            set_err(o, plugin, err.empty() ? "plugin load failed" : err);
            // Resident failures enter on_load so lifecycle rollback owns their cleanup.
            return retained ? SAO_OK : status;
        }
        slot.ready = true;
        set_err(o, plugin, {});
        return SAO_OK;
    } catch (const std::exception& e) {
        set_err(o, plugin, e.what());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        set_err(o, plugin, "plugin load failed before completion");
        return SAO_ERR_OS_CALL_FAILED;
    }
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t call_named(plugin_handle_t plugin, void* ud, const char* hook,
                   bool* allow_unload = nullptr) try {
    pymini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::recursive_mutex> g(g_mu);
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
    int32_t status = SAO_OK;
    try {
        if (r.route == route_t::pymini) {
            int rc = 0;
            if (allow_unload) {
                rc = pymini_host_call_hook_allow_unload(r.plugin_id.c_str(), hook, allow_unload,
                                                        &err);
            } else {
                rc = pymini_host_call_hook(r.plugin_id.c_str(), hook, nullptr, &err);
            }
            status = rc == 0 || rc == 1 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
        } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            status = cpython_thread_status(o, &err);
            if (status == SAO_OK)
                status =
                    allow_unload ? pyh_on_unload(r, allow_unload, &err) : pyh_hook(r, hook, &err);
#else
            status = loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
        }
    } catch (const std::exception& e) {
        status = SAO_ERR_OS_CALL_FAILED;
        err = e.what();
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
        err = "plugin hook failed";
    }
    if (status != SAO_OK && err.empty())
        err = std::string(hook) + " failed: status=" + std::to_string(status);
    if (status != SAO_OK || !allow_unload)
        set_err(o, plugin, status == SAO_OK ? std::string{} : err);
    return status;
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t SAO_PLUGINS_CALL adapter_on_load(plugin_handle_t plugin, void* ud) {
    return call_named(plugin, ud, "on_load");
}
int32_t SAO_PLUGINS_CALL adapter_on_enable(plugin_handle_t plugin, void* ud) {
    return call_named(plugin, ud, "on_enable");
}
int32_t SAO_PLUGINS_CALL adapter_on_disable(plugin_handle_t plugin, void* ud) {
    return call_named(plugin, ud, "on_disable");
}
int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t plugin, bool* allow, void* ud) {
    if (!allow)
        return SAO_ERR_INVALID_ARGUMENT;
    *allow = false;
    return call_named(plugin, ud, "on_unload", allow);
}

int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t plugin, void* ud) try {
    pymini_adapter_owner_s* o = active_owner(ud);
    if (!o)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard<std::recursive_mutex> g(g_mu);
    if (o != g_owner)
        return SAO_ERR_HANDLE_INVALID;
    auto it = o->plugins.find(plugin);
    if (it == o->plugins.end())
        return o->last_errors.count(plugin) ? SAO_OK : SAO_ERR_HANDLE_INVALID;
    rec& r = it->second;
    std::string err;
    int32_t status = SAO_OK;
    try {
        if (r.route == route_t::pymini) {
            if (pymini_host_is_loaded(r.plugin_id.c_str()))
                status = pymini_host_unload_plugin(r.plugin_id.c_str(), &err);
        } else {
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
            status = cpython_thread_status(o, &err);
            if (status == SAO_OK)
                status = pyh_unload(r, &err);
#endif
        }
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
        err = "plugin unload failed";
    }
    if (status == SAO_OK) {
        o->plugins.erase(it);
    } else {
        set_err(o, plugin, err.empty() ? "plugin unload failed" : err);
    }
    return status;
} catch (...) {
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t SAO_PLUGINS_CALL adapter_last_error(void* user_data, loader::plugin_handle_t plugin,
                                            char** out_utf8) {
    return sao_plugins_pymini_adapter_get_last_error(static_cast<pymini_adapter_owner_t>(user_data),
                                                     plugin, out_utf8);
}

host_adapter_vtable make_vtable(pymini_adapter_owner_s* o) {
    host_adapter_vtable t{};
    t.load_plugin = adapter_load;
    t.call_on_load = adapter_on_load;
    t.call_on_enable = adapter_on_enable;
    t.call_on_disable = adapter_on_disable;
    t.call_on_unload = adapter_on_unload;
    t.unload_plugin = adapter_unload;
    t.host_user_data = o;
    t.get_last_error = adapter_last_error;
    t.free_error_string = sao_plugins_pymini_free_string;
    return t;
}

} // namespace

// ═══ public C-ABI exports ═══════════════════════════════════════════
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pymini_register_loader_adapter(
    const pymini_adapter_config* cfg, pymini_adapter_owner_t* out_owner) {
    if (!out_owner)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard<std::recursive_mutex> g(g_mu);
        if (g_owner)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        auto o = std::make_unique<pymini_adapter_owner_s>();
        if (cfg && cfg->python_home)
            o->python_home = cfg->python_home;
        if (cfg && cfg->extra_module_dirs)
            for (uint32_t k = 0; k < cfg->extra_module_dirs_count; ++k)
                if (cfg->extra_module_dirs[k])
                    o->extra_dirs.emplace_back(cfg->extra_module_dirs[k]);

        const auto table = make_vtable(o.get());
        const int32_t st = loader::sao_plugins_lifecycle_register_host_adapter(
            loader::engine_kind::python, &table);
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
sao_plugins_pymini_unregister_loader_adapter(pymini_adapter_owner_t owner) {
    if (!owner)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::recursive_mutex> g(g_mu);
        if (owner != g_owner)
            return SAO_ERR_HANDLE_INVALID;
        if (!owner->plugins.empty())
            return loader::SAO_PLUGINS_ERR_BUSY;
        int32_t st = SAO_OK;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
        if (owner->helper_states != 0)
            return loader::SAO_PLUGINS_ERR_BUSY;
        if (owner->host) {
            st = cpython_thread_status(owner, &owner->last_error);
            if (st != SAO_OK)
                return st;
        }
        if (owner->helper_zombies != nullptr) {
            gil_scope scope(true);
            if (!scope.held()) {
                owner->last_error = "cannot acquire CPython GIL for helper cleanup";
                return SAO_ERR_OS_CALL_FAILED;
            }
            while (owner->helper_zombies != nullptr) {
                st = python_host::sao_plugins_pyhost_unload_plugin(owner->helper_zombies->plugin);
                if (st != SAO_OK) {
                    owner->last_error =
                        "CPython helper cleanup failed: status=" + std::to_string(st);
                    return st;
                }
                auto next = std::move(owner->helper_zombies->next);
                owner->helper_zombies = std::move(next);
            }
        }
        if (owner->host) {
            gil_scope scope(true);
            if (!scope.held()) {
                owner->last_error = "cannot acquire CPython GIL for shutdown";
                return SAO_ERR_OS_CALL_FAILED;
            }
            st = python_host::sao_plugins_pyhost_shutdown(owner->host);
            if (st != SAO_OK) {
                owner->last_error = "CPython shutdown failed: status=" + std::to_string(st);
                return st;
            }
            owner->host = nullptr;
        }
#endif
        if (owner->adapter_registered) {
            st = loader::sao_plugins_lifecycle_unregister_host_adapter(loader::engine_kind::python);
            if (st != SAO_OK) {
                return st;
            }
            owner->adapter_registered = false;
        }
        owner->active = false;
        g_owner = nullptr;
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pymini_adapter_plugin_count(pymini_adapter_owner_t owner) {
    try {
        std::lock_guard<std::recursive_mutex> g(g_mu);
        return owner && owner == g_owner ? owner->plugins.size() : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pymini_adapter_requires_python(
    pymini_adapter_owner_t owner, const plugin_manifest* manifest, bool* out_required,
    char** out_reason) {
    if (out_required)
        *out_required = false;
    if (out_reason)
        *out_reason = nullptr;
    if (!owner || !manifest || !out_required || !out_reason)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::recursive_mutex> g(g_mu);
        if (owner != g_owner || !owner->active)
            return SAO_ERR_HANDLE_INVALID;
        route_t route;
        std::string reason;
        const int32_t status = classify_plugin(manifest, owner, &route, &reason);
        *out_reason = dup_err(reason);
        if (!reason.empty() && !*out_reason)
            return SAO_ERR_OS_CALL_FAILED;
        *out_required = status == SAO_OK && route == route_t::cpython;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pymini_adapter_get_last_error(
    pymini_adapter_owner_t owner, void* loader_plugin_handle, char** out_utf8) {
    if (out_utf8)
        *out_utf8 = nullptr;
    if (!owner || !out_utf8)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::recursive_mutex> g(g_mu);
        if (owner != g_owner)
            return SAO_ERR_HANDLE_INVALID;
        const std::string* error = &owner->last_error;
        if (loader_plugin_handle) {
            const auto found =
                owner->last_errors.find(static_cast<plugin_handle_t>(loader_plugin_handle));
            if (found == owner->last_errors.end())
                return SAO_ERR_HANDLE_INVALID;
            error = &found->second;
        }
        if (error->empty())
            return SAO_ERR_HANDLE_INVALID;
        *out_utf8 = dup_err(*error);
        return *out_utf8 ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_pymini_free_string(char* value) {
    std::free(value);
}

// CPython follows pymini only after an unsupported preflight.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_register_script_engine(void) {
    const int32_t native_status = pymini_register_script_engine();
    if (native_status != SAO_OK)
        return native_status;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    const int32_t full_status = script::runtime_bridge_register(&g_cpython_helper_ops);
    if (full_status != SAO_OK) {
        (void)pymini_unregister_script_engine();
        return full_status;
    }
#endif
    return SAO_OK;
}
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pymini_unregister_script_engine(void) {
    int32_t status = SAO_OK;
#if defined(SAO_PLUGINS_ENABLE_PYTHON)
    status = script::runtime_bridge_unregister(&g_cpython_helper_ops);
    if (status != SAO_OK)
        return status;
#endif
    return pymini_unregister_script_engine();
}

} // namespace sao::plugins::pymini
