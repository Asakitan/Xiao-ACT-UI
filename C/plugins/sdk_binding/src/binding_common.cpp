// binding_common.cpp — Wave 4 首切片
//
// 提供:
//   1. json_node 基础栈 (stub → Wave 5 真解析)
//   2. barrier 屏障 (直接同步调用, 后续 wave 加 SEH)
//   3. plugin_binding_s 定义 (5 语言共享的 runtime binding 状态)
//   4. sao_plugins_sdk_bind_call 中央 dispatcher (5 method 实装)
//   5. test 辅助 API (has_hotkey / event_count / last_log)

#include "sao/plugins/sdk_binding/binding_common.h"

#include "sao/plugins/loader/plugin_context.h"

#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::sdk_binding {

namespace {

using ordered_json = nlohmann::ordered_json;

json_node json_to_node(const ordered_json& value) {
    if (value.is_null()) return json_node{nullptr};
    if (value.is_boolean()) return json_node{value.get<bool>()};
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return json_node{value.get<int64_t>()};
    }
    if (value.is_number_float()) return json_node{value.get<double>()};
    if (value.is_string()) return json_node{value.get<std::string>()};
    if (value.is_array()) {
        std::vector<json_node> output;
        output.reserve(value.size());
        for (const auto& item : value) output.push_back(json_to_node(item));
        return json_node{std::move(output)};
    }
    std::vector<std::pair<std::string, json_node>> output;
    output.reserve(value.size());
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        output.emplace_back(iterator.key(), json_to_node(iterator.value()));
    }
    return json_node{std::move(output)};
}

ordered_json node_to_json(const json_node& node) {
    return std::visit([](const auto& value) -> ordered_json {
        using value_t = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<value_t, std::vector<json_node>>) {
            auto output = ordered_json::array();
            for (const auto& item : value) output.push_back(node_to_json(item));
            return output;
        } else if constexpr (std::is_same_v<
                                 value_t,
                                 std::vector<std::pair<std::string, json_node>>>) {
            auto output = ordered_json::object();
            for (const auto& [key, item] : value) output[key] = node_to_json(item);
            return output;
        } else {
            return ordered_json(value);
        }
    }, node.value);
}

} // namespace

json_node json_node::from_string(std::string_view text) {
    try {
        return json_to_node(ordered_json::parse(text));
    } catch (...) {
        return json_node{};
    }
}

std::string json_node::to_string() const {
    try {
        return node_to_json(*this).dump();
    } catch (...) {
        return "null";
    }
}

// ── plugin_binding_s (5 语言共享的 runtime 状态) ─────────────

struct plugin_binding_s {
    // 语言标记 (仅诊断/日志用)
    enum class language_kind : uint8_t {
        python = 0, lua, angel, emma, csharp
    };
    language_kind lang = language_kind::python;

    language_host_kind host_kind = language_host_kind::python;
    language_host_adapter_vtable host{};
    void* provider_plugin = nullptr;
    std::string last_error;

    // 关联的 sao_plugins_ctx (来自 loader/plugin_context.h)
    void* ctx = nullptr;

    // 关联的语言侧 handle (PyObject*/lua_State*/asIScriptEngine*/...)
    void* lang_state = nullptr;

    // 记录 add_hotkey 注册 (Wave 4 shim, 未来对接真 hotkey manager)
    std::unordered_set<std::string> hotkeys;

    // 记录 publish_event: topic → count (Wave 4 shim, 未来对接真 event bus)
    std::unordered_map<std::string, uint32_t> event_counts;

    // 最近一条 log (Wave 4 shim, 供 test 抓命中)
    std::string last_log;

    // 记录 last plugin_id 请求返回过什么 (Wave 4 shim)
    std::string last_plugin_id_query;

    std::mutex mu;
};

namespace {

struct host_record {
    language_host_adapter_vtable adapter{};
    bool present = false;
};

struct callback_record {
    void* user_data = nullptr;
    release_callback_fn release = nullptr;
};

std::mutex g_hosts_mutex;
std::array<host_record, static_cast<size_t>(language_host_kind::count_)> g_hosts;
std::mutex g_callbacks_mutex;
std::vector<callback_record> g_callbacks;

constexpr bool valid_language(language_host_kind language) noexcept {
    return static_cast<size_t>(language) <
           static_cast<size_t>(language_host_kind::count_);
}

int32_t unsupported() noexcept {
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

bool snapshot_host(language_host_kind language,
                   language_host_adapter_vtable& adapter) {
    if (!valid_language(language)) return false;
    std::lock_guard lock(g_hosts_mutex);
    const auto& record = g_hosts[static_cast<size_t>(language)];
    if (!record.present) return false;
    adapter = record.adapter;
    return true;
}

bool host_available(const language_host_adapter_vtable& adapter) noexcept {
    if (adapter.available == nullptr) return true;
    try {
        return adapter.available(adapter.user_data);
    } catch (...) {
        return false;
    }
}

char* duplicate_error(const char* message) noexcept {
    if (message == nullptr || message[0] == '\0') return nullptr;
    const size_t length = std::strlen(message);
    auto copy = std::unique_ptr<char[]>(new (std::nothrow) char[length + 1]);
    if (!copy) return nullptr;
    std::memcpy(copy.get(), message, length + 1);
    return copy.release();
}

int32_t copy_to_caller(std::string_view value,
                       char* output,
                       size_t capacity,
                       size_t* required) noexcept {
    if (required != nullptr) *required = value.size() + 1;
    if (output == nullptr || capacity < value.size() + 1) {
        if (output != nullptr && capacity > 0) output[0] = '\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = '\0';
    return SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_register_language_host(
    const language_host_adapter_vtable* adapter) {
    if (adapter == nullptr ||
        adapter->struct_size < sizeof(language_host_adapter_vtable) ||
        adapter->abi_version != SAO_LANGUAGE_HOST_PROVIDER_ABI_VERSION ||
        !valid_language(adapter->language) ||
        adapter->load_plugin == nullptr || adapter->unload_plugin == nullptr ||
        adapter->invoke == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (adapter->language == language_host_kind::python &&
        (adapter->flags & SAO_LANGUAGE_HOST_PROVIDER_ISOLATED_PYTHON_ABI) == 0) {
        return unsupported();
    }
    std::lock_guard lock(g_hosts_mutex);
    g_hosts[static_cast<size_t>(adapter->language)] = {*adapter, true};
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_unregister_language_host(language_host_kind language) {
    if (!valid_language(language)) return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_hosts_mutex);
    g_hosts[static_cast<size_t>(language)] = {};
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_language_host_available(language_host_kind language) {
    language_host_adapter_vtable adapter{};
    return snapshot_host(language, adapter) && host_available(adapter);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_barrier(barrier_fn fn,
                            void* user_data,
                            char** out_error_utf8) {
    if (out_error_utf8 != nullptr) *out_error_utf8 = nullptr;
    if (fn == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        return fn(user_data);
    } catch (const std::exception& error) {
        if (out_error_utf8 != nullptr) {
            *out_error_utf8 = duplicate_error(error.what());
        }
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        if (out_error_utf8 != nullptr) {
            *out_error_utf8 = duplicate_error(
                "language host crossed exception barrier");
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_free_error(char* error) {
    delete[] error;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_track_callback(void* user_data,
                                   release_callback_fn release_fn) {
    if (user_data == nullptr || release_fn == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(g_callbacks_mutex);
        g_callbacks.push_back({user_data, release_fn});
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_release_all_callbacks(void) {
    std::vector<callback_record> callbacks;
    {
        std::lock_guard lock(g_callbacks_mutex);
        callbacks.swap(g_callbacks);
    }
    for (auto iterator = callbacks.rbegin(); iterator != callbacks.rend();
         ++iterator) {
        try {
            iterator->release(iterator->user_data);
        } catch (...) {
        }
    }
}

// ── method name 表 (对齐 sdk_method_id 枚举) ─────────────────

namespace {
struct method_name_map {
    sdk_method_id id;
    const char* name;
};

static constexpr method_name_map k_names[] = {
    {sdk_method_id::prop_plugin_id, "plugin_id"},
    {sdk_method_id::prop_path, "path"},
    {sdk_method_id::prop_web_path, "web_path"},
    {sdk_method_id::prop_assets_path, "assets_path"},
    {sdk_method_id::prop_should_stop, "should_stop"},
    {sdk_method_id::method_log, "log"},
    {sdk_method_id::method_subscribe, "subscribe"},
    {sdk_method_id::method_subscribe_once, "subscribe_once"},
    {sdk_method_id::method_unsubscribe, "unsubscribe"},
    {sdk_method_id::method_on_damage, "on_damage"},
    {sdk_method_id::method_on_heal, "on_heal"},
    {sdk_method_id::method_on_skill, "on_skill"},
    {sdk_method_id::method_on_boss, "on_boss"},
    {sdk_method_id::method_on_snapshot, "on_snapshot"},
    {sdk_method_id::method_on_encounter_finalized,
     "on_encounter_finalized"},
    {sdk_method_id::method_emit, "emit"},
    {sdk_method_id::method_get_snapshot, "get_snapshot"},
    {sdk_method_id::method_snapshot_value, "snapshot_value"},
    {sdk_method_id::method_recent_events, "recent_events"},
    {sdk_method_id::method_get_setting, "get_setting"},
    {sdk_method_id::method_setting, "setting"},
    {sdk_method_id::method_set_setting, "set_setting"},
    {sdk_method_id::method_set_defaults, "set_defaults"},
    {sdk_method_id::method_register_parser_adapter,
     "register_parser_adapter"},
    {sdk_method_id::method_register_exporter, "register_exporter"},
    {sdk_method_id::method_register_formatter, "register_formatter"},
    {sdk_method_id::method_register_trigger_type,
     "register_trigger_type"},
    {sdk_method_id::method_register_report_view,
     "register_report_view"},
    {sdk_method_id::method_register_timer, "register_timer"},
    {sdk_method_id::method_register_ui_panel, "register_ui_panel"},
    {sdk_method_id::method_register_render_hook, "register_render_hook"},
    {sdk_method_id::method_set_overlay, "set_overlay"},
    {sdk_method_id::method_clear_overlay, "clear_overlay"},
    {sdk_method_id::method_register_hotkey, "register_hotkey"},
    {sdk_method_id::method_register_engine, "register_engine"},
    {sdk_method_id::method_register_data_source, "register_data_source"},
    {sdk_method_id::method_register_menu_category,
     "register_menu_category"},
    {sdk_method_id::method_register_menu_surface,
     "register_menu_surface"},
    {sdk_method_id::method_register_action_handler,
     "register_action_handler"},
    {sdk_method_id::method_request_redraw, "request_redraw"},
    {sdk_method_id::method_set_interval, "set_interval"},
    {sdk_method_id::method_set_timeout, "set_timeout"},
    {sdk_method_id::method_clear_timer, "clear_timer"},
    {sdk_method_id::method_run_on_ui, "run_on_ui"},
    {sdk_method_id::method_notify, "notify"},
    {sdk_method_id::method_dismiss_notify, "dismiss_notify"},
    {sdk_method_id::method_toast, "toast"},
    {sdk_method_id::method_open_file, "open_file"},
    {sdk_method_id::method_open_window, "open_window"},
    {sdk_method_id::method_create_compositor_layer,
     "create_compositor_layer"},
    {sdk_method_id::method_upload_compositor_frame,
     "upload_compositor_frame"},
    {sdk_method_id::method_set_compositor_layer_mmf_source,
     "set_compositor_layer_mmf_source"},
    {sdk_method_id::method_set_compositor_layer_shared_texture_source,
     "set_compositor_layer_shared_texture_source"},
    {sdk_method_id::method_set_compositor_layer_position,
     "set_compositor_layer_position"},
    {sdk_method_id::method_set_compositor_layer_visible,
     "set_compositor_layer_visible"},
    {sdk_method_id::method_set_compositor_layer_input,
     "set_compositor_layer_input"},
    {sdk_method_id::method_destroy_compositor_layer,
     "destroy_compositor_layer"},
    {sdk_method_id::method_compositor_gpu_interop_available,
     "compositor_gpu_interop_available"},
    {sdk_method_id::method_compositor_layer_shared_texture_active,
     "compositor_layer_shared_texture_active"},
    {sdk_method_id::method_compositor_display_refresh_hz,
     "compositor_display_refresh_hz"},
    {sdk_method_id::method_get_engine, "get_engine"},
    {sdk_method_id::method_require_engine, "require_engine"},
    {sdk_method_id::method_call_engine, "call_engine"},
    {sdk_method_id::method_call_runtime, "call_runtime"},
    {sdk_method_id::method_ensure_requirements, "ensure_requirements"},
    {sdk_method_id::method_load_local, "load_local"},
};
static_assert(std::size(k_names) ==
              static_cast<size_t>(sdk_method_id::method_count_));

// 简单字符串范围复制 (args_ptr 可能没 \0)
std::string bytes_to_string(const char* ptr, size_t size) {
    if (ptr == nullptr || size == 0) return {};
    return std::string(ptr, size);
}
} // namespace

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_method_name(sdk_method_id method) {
    for (const auto& e : k_names) {
        if (e.id == method) return e.name;
    }
    return "";
}

extern "C" SAO_PLUGINS_API sdk_method_id SAO_PLUGINS_CALL
sao_plugins_binding_method_from_name(const char* name) {
    if (name == nullptr || *name == '\0') return sdk_method_id::method_count_;
    for (const auto& e : k_names) {
        if (std::strcmp(e.name, name) == 0) return e.id;
    }
    return sdk_method_id::method_count_;
}

namespace {

struct load_call {
    language_host_adapter_vtable adapter{};
    void* context = nullptr;
    void* runtime = nullptr;
    void** output = nullptr;
};

int32_t call_load(void* opaque) {
    auto* call = static_cast<load_call*>(opaque);
    return call->adapter.load_plugin(call->context, call->runtime, call->output,
                                     call->adapter.user_data);
}

struct unload_call {
    language_host_adapter_vtable adapter{};
    void* plugin = nullptr;
};

int32_t call_unload(void* opaque) {
    auto* call = static_cast<unload_call*>(opaque);
    return call->adapter.unload_plugin(call->plugin, call->adapter.user_data);
}

struct invoke_call {
    plugin_binding_s* binding = nullptr;
    const char* method = nullptr;
    const uint8_t* arguments = nullptr;
    size_t arguments_size = 0;
    uint8_t* output = nullptr;
    size_t output_capacity = 0;
    size_t* required = nullptr;
    char* error = nullptr;
    size_t error_capacity = 0;
};

int32_t call_invoke(void* opaque) {
    auto* call = static_cast<invoke_call*>(opaque);
    return call->binding->host.invoke(
        call->binding->provider_plugin, call->method, call->arguments,
        call->arguments_size, call->output, call->output_capacity,
        call->required, call->error, call->error_capacity,
        call->binding->host.user_data);
}

struct dispatch_call {
    language_host_adapter_vtable adapter{};
    language_binding_operation operation{};
    language_binding_request* request = nullptr;
};

int32_t call_dispatch(void* opaque) {
    auto* call = static_cast<dispatch_call*>(opaque);
    return call->adapter.dispatch(call->operation, call->request,
                                  call->adapter.user_data);
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_load(language_host_kind language,
                                void* plugin_ctx,
                                void* runtime,
                                plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (!valid_language(language) || plugin_ctx == nullptr || runtime == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    language_host_adapter_vtable adapter{};
    if (!snapshot_host(language, adapter) || !host_available(adapter)) {
        return unsupported();
    }

    void* provider_plugin = nullptr;
    load_call call{adapter, plugin_ctx, runtime, &provider_plugin};
    char* error = nullptr;
    const int32_t status = sao_plugins_binding_barrier(&call_load, &call, &error);
    sao_plugins_binding_free_error(error);
    if (status != SAO_OK) return status;
    if (provider_plugin == nullptr) return SAO_ERR_NOT_INITIALIZED;

    auto binding = std::unique_ptr<plugin_binding_s>(
        new (std::nothrow) plugin_binding_s{});
    if (!binding) {
        unload_call rollback{adapter, provider_plugin};
        (void)sao_plugins_binding_barrier(&call_unload, &rollback, nullptr);
        return SAO_ERR_OS_CALL_FAILED;
    }
    binding->ctx = plugin_ctx;
    binding->lang_state = runtime;
    binding->host_kind = language;
    binding->host = adapter;
    binding->provider_plugin = provider_plugin;
    binding->lang = static_cast<plugin_binding_s::language_kind>(language);
    *out_plugin = binding.release();
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_unload(plugin_binding_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    unload_call call{plugin->host, plugin->provider_plugin};
    char* error = nullptr;
    const int32_t status = sao_plugins_binding_barrier(&call_unload, &call, &error);
    if (status != SAO_OK) {
        std::lock_guard lock(plugin->mu);
        plugin->last_error = error != nullptr ? error : "language host unload failed";
        sao_plugins_binding_free_error(error);
        return status;
    }
    sao_plugins_binding_free_error(error);
    delete plugin;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_invoke(plugin_binding_handle_t plugin,
                                  const char* method_name_utf8,
                                  const uint8_t* args_json_utf8,
                                  size_t args_size,
                                  uint8_t* out_result_json_utf8,
                                  size_t out_capacity,
                                  size_t* out_required) {
    if (out_required != nullptr) *out_required = 0;
    if (plugin == nullptr || method_name_utf8 == nullptr ||
        method_name_utf8[0] == '\0' || out_required == nullptr ||
        (args_size > 0 && args_json_utf8 == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::array<char, 512> provider_error{};
    invoke_call call{plugin,
                     method_name_utf8,
                     args_json_utf8,
                     args_size,
                     out_result_json_utf8,
                     out_capacity,
                     out_required,
                     provider_error.data(),
                     provider_error.size()};
    char* barrier_error = nullptr;
    const int32_t status = sao_plugins_binding_barrier(
        &call_invoke, &call, &barrier_error);
    {
        std::lock_guard lock(plugin->mu);
        if (provider_error[0] != '\0') plugin->last_error = provider_error.data();
        else if (barrier_error != nullptr) plugin->last_error = barrier_error;
        else if (status == SAO_OK || status == SAO_ERR_BUFFER_TOO_SMALL) {
            plugin->last_error.clear();
        } else {
            plugin->last_error = "language host invoke failed";
        }
    }
    sao_plugins_binding_free_error(barrier_error);
    return status;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_last_error(plugin_binding_handle_t plugin,
                                      char* out_error_utf8,
                                      size_t out_capacity,
                                      size_t* out_required) {
    if (plugin == nullptr || out_required == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(plugin->mu);
    return copy_to_caller(plugin->last_error, out_error_utf8, out_capacity,
                          out_required);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_dispatch_provider(language_host_kind language,
                                      language_binding_operation operation,
                                      language_binding_request* request) {
    if (request == nullptr || !valid_language(language)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    language_host_adapter_vtable adapter{};
    if (!snapshot_host(language, adapter) || !host_available(adapter) ||
        adapter.dispatch == nullptr) {
        return unsupported();
    }
    dispatch_call call{adapter, operation, request};
    return sao_plugins_binding_barrier(&call_dispatch, &call, nullptr);
}

// ── 中央 dispatcher (Wave 4 实装 5 method) ──────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_bind_call(plugin_binding_handle_t plugin,
                          sdk_method_id method_id,
                          const char* args_ptr,
                          size_t args_size,
                          char* ret_ptr,
                          size_t ret_size) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(plugin->mu);

    switch (method_id) {
    case sdk_method_id::method_log: {
        std::string msg = bytes_to_string(args_ptr, args_size);
        loader::sao_plugins_ctx_log(
            static_cast<loader::plugin_context_t*>(plugin->ctx), msg.c_str());
        plugin->last_log = std::move(msg);
        return SAO_OK;
    }

    case sdk_method_id::prop_plugin_id: {
        const char* plugin_id = loader::sao_plugins_ctx_plugin_id(
            static_cast<loader::plugin_context_t*>(plugin->ctx));
        if (plugin_id == nullptr) return SAO_ERR_NOT_INITIALIZED;
        plugin->last_plugin_id_query = plugin_id;
        if (ret_ptr != nullptr && ret_size > 0) {
            const std::string& s = plugin->last_plugin_id_query;
            if (s.size() + 1 > ret_size) {
                ret_ptr[0] = '\0';
                return SAO_ERR_BUFFER_TOO_SMALL;
            }
            std::memcpy(ret_ptr, s.c_str(), s.size() + 1);
        }
        return SAO_OK;
    }

    case sdk_method_id::method_register_hotkey: {
        std::string key = bytes_to_string(args_ptr, args_size);
        if (key.empty()) return SAO_ERR_INVALID_ARGUMENT;
        const int32_t status = loader::sao_plugins_ctx_register_hotkey(
            static_cast<loader::plugin_context_t*>(plugin->ctx), key.c_str(),
            "", key.c_str(), nullptr, nullptr);
        if (status == SAO_OK) plugin->hotkeys.insert(std::move(key));
        return status;
    }

    case sdk_method_id::method_emit: {
        std::string topic = bytes_to_string(args_ptr, args_size);
        if (topic.empty()) return SAO_ERR_INVALID_ARGUMENT;
        const int32_t status = loader::sao_plugins_ctx_emit(
            static_cast<loader::plugin_context_t*>(plugin->ctx), topic.c_str(),
            "null");
        if (status == SAO_OK) plugin->event_counts[topic] += 1;
        return status;
    }

    default:
        return unsupported();
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_bind_call_ex(plugin_binding_handle_t plugin,
                             sdk_method_id method_id,
                             const char* args_ptr,
                             size_t args_size,
                             char* ret_ptr,
                             size_t ret_size,
                             size_t* out_required) {
    if (out_required == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_required = 0;
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (method_id == sdk_method_id::prop_plugin_id) {
        const char* plugin_id = loader::sao_plugins_ctx_plugin_id(
            static_cast<loader::plugin_context_t*>(plugin->ctx));
        if (plugin_id == nullptr) return SAO_ERR_NOT_INITIALIZED;
        return copy_to_caller(plugin_id, ret_ptr, ret_size, out_required);
    }
    return sao_plugins_sdk_bind_call(plugin, method_id, args_ptr, args_size,
                                     ret_ptr, ret_size);
}

// ── test 辅助 ────────────────────────────────────────────────

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_test_has_hotkey(plugin_binding_handle_t plugin,
                                    const char* hotkey_id) {
    if (plugin == nullptr || hotkey_id == nullptr) return false;
    std::lock_guard<std::mutex> lk(plugin->mu);
    return plugin->hotkeys.count(hotkey_id) > 0;
}

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_binding_test_event_count(plugin_binding_handle_t plugin,
                                     const char* topic) {
    if (plugin == nullptr || topic == nullptr) return 0;
    std::lock_guard<std::mutex> lk(plugin->mu);
    auto it = plugin->event_counts.find(topic);
    return it == plugin->event_counts.end() ? 0u : it->second;
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_test_last_log(plugin_binding_handle_t plugin) {
    if (plugin == nullptr) return "";
    // 注意: 返回内部 string 引用, 调用方不能修改。Wave 4 shim。
    return plugin->last_log.c_str();
}

// ── activate / deactivate 通用工厂 (各语言复用) ──────────────

namespace detail {

// int lang: 0=python, 1=lua, 2=angel, 3=emma, 4=csharp
plugin_binding_handle_t make_binding(void* ctx,
                                     void* lang_state,
                                     int lang) {
    plugin_binding_handle_t binding = nullptr;
    const auto language = static_cast<language_host_kind>(lang);
    return sao_plugins_binding_plugin_load(language, ctx, lang_state,
                                           &binding) == SAO_OK
               ? binding
               : nullptr;
}

void free_binding(plugin_binding_handle_t plugin) {
    (void)sao_plugins_binding_plugin_unload(plugin);
}

} // namespace detail

} // namespace sao::plugins::sdk_binding
