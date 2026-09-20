// emma_call.cpp — Emma 脚本加载 + 生命周期 hook 分派 实装
//
// 对齐 Python emma_runtime.py EmmaRuntime.load_script:
// 加入 host-level in-memory 便利函数 (test / repl 用):
//   sao_plugins_emma_execute_source(interp, source, source_len, out_error)
//   sao_plugins_emma_call_by_name(interp, name, args, argc, out_result, out_error)

#include "sao/plugins/emma_host/emma_call.h"
#include "emma_json_internal.h"
#include "emma_source_io.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_lexer.h"
#include "sao/plugins/emma_host/emma_parser.h"
#include "sao/plugins/emma_host/emma_stdlib.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/script_ui.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_emma.h"
#include "sao/plugins/sdk_binding/binding_engine.h"
#include "sao/sdk/sao_sdk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sao::plugins::emma_host {

// 外部符号 (定义在 emma_interpreter.cpp)
std::string emma_value_to_string(const emma_value& v);

struct emma_plugin_runtime;

constexpr std::string_view kActionProviderLocalId = "action-handler";

struct emma_menu_row {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = false;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const emma_menu_row&) const = default;
};

struct emma_menu_bridge {
    emma_plugin_runtime* runtime = nullptr;
    bool enable_scoped = false;
    std::atomic_bool closing{false};
    std::string provider_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::uint64_t revision = 0;
    std::shared_ptr<callable> builder;
    std::vector<emma_menu_row> rows;
    std::unordered_map<std::string, std::shared_ptr<callable>> actions;
};

struct emma_action_bridge {
    emma_plugin_runtime* runtime = nullptr;
    bool enable_scoped = false;
    std::atomic_bool closing{false};
    std::string provider_id{kActionProviderLocalId};
    std::uint64_t registration_revision = 1;
    std::shared_ptr<callable> handler;
    std::shared_ptr<callable> disable_restore_handler;
};

struct emma_action_checkpoint {
    bool present = false;
    bool enable_scoped = false;
    std::uint64_t registration_revision = 0;
    std::shared_ptr<callable> handler;
    std::shared_ptr<callable> disable_restore_handler;
};

struct emma_plugin_runtime {
    struct callback_record;

    ~emma_plugin_runtime();

    enum class resource_kind {
        event_subscription,
        hotkey,
        timer,
        notification,
        overlay,
        render_hook,
        data_source,
        compositor_input,
    };

    struct owned_resource {
        uint64_t id = 0;
        resource_kind kind = resource_kind::event_subscription;
        uint32_t numeric_token = 0;
        std::string key;
        std::shared_ptr<callback_record> callback;
        std::shared_ptr<void> attachment;
    };

    std::string plugin_id;
    std::filesystem::path plugin_root;
    std::filesystem::path entry_path;
    ast_pool pool;
    std::unique_ptr<interpreter> interp;
    emma_value context_value = nullptr;
    sao::plugins::loader::plugin_context_t* context = nullptr;
    sao::plugins::sdk_binding::plugin_binding_handle_t binding = nullptr;
    bool owns_context_lease = false;
    // 反射引擎面 (binding_engine.h): 本 host 自有的 SaoSdkContext —— loader
    // plugin_context_t 与 SaoSdkContext 是两种身份, 不能互转; 与
    // angel/csharp/csmini/python host 同模式 (sao_sdk_context_create +
    // bind_platform_services), 生命周期随 runtime。创建失败不阻断加载,
    // sdk_init_status 记录原因, engine.* 调用照常报错 (fail closed)。
    SaoSdkContext* sdk_context = nullptr;
    int32_t sdk_init_status = SAO_OK;
    // engine.on(channel, cb): channel → callback_record 路由表; 每次 engine
    // dispatch 的 request.engine_callback 以 runtime 作 heap 锚定经本表分发。
    std::mutex engine_callbacks_mutex;
    std::unordered_map<std::string, std::shared_ptr<callback_record>> engine_callbacks;
    // 已下表且停止接收的 engine channel 记录 (replace/unregister 时
    // quiesce 遇到同线程在飞执行就进这里, 防 last-ref 悬空在飞回调)。
    std::vector<std::shared_ptr<callback_record>> engine_callbacks_retired;
    // engine 面 dispatch 串行化 + 结果 scratch (一次性分配按协议上限;
    // write_result 溢出返回发生在 invoke 实跑之后, 不能安全做两遍探测)。
    std::mutex engine_dispatch_mutex;
    std::vector<char> engine_result_buffer;
    std::atomic_bool callbacks_accepting{true};
    std::mutex resources_mutex;
    std::vector<owned_resource> resources;
    std::vector<std::unique_ptr<emma_menu_bridge>> menus;
    std::unique_ptr<emma_action_bridge> action;
    std::unordered_map<std::string, std::shared_ptr<callable>> extension_handlers;
    uint64_t next_resource_id = 1;
    emma_error last_error;
    std::mutex invocation_mutex;
};

struct emma_plugin_runtime::callback_record {
    enum class one_shot_state {
        none,
        registering,
        fired_pending,
        armed,
        claimed,
    } one_shot_phase = one_shot_state::none;

    emma_plugin_runtime* runtime = nullptr;
    std::shared_ptr<callable> function;
    std::mutex mutex;
    std::condition_variable idle;
    bool accepting = true;
    size_t in_flight = 0;
    bool one_shot = false;
    std::string timer_token;
    std::vector<emma_value> pending_arguments;
};

// 数据源 bundle: register_data_source 的 start/stop 回调共用一个 user_data，
// 生命周期由 owned_resource(kind::data_source) 的 attachment 持有。
struct emma_data_source_bundle {
    emma_plugin_runtime* runtime = nullptr;
    std::string source_id;
    std::shared_ptr<emma_plugin_runtime::callback_record> start;
    std::shared_ptr<emma_plugin_runtime::callback_record> stop;
};

// compositor input bundle: set_compositor_layer_input 的四只回调共享一个
// user_data, 生命周期由 owned_resource(kind::compositor_input) 的 attachment
// 持有。
struct emma_compositor_input_bundle {
    emma_plugin_runtime* runtime = nullptr;
    std::string layer_name;
    std::shared_ptr<emma_plugin_runtime::callback_record> cursor_pos;
    std::shared_ptr<emma_plugin_runtime::callback_record> mouse_button;
    std::shared_ptr<emma_plugin_runtime::callback_record> cursor_leave;
    std::shared_ptr<emma_plugin_runtime::callback_record> scroll;
};

// 句柄记录作为 tombstone 保留到进程退出，重复/过期调用因此不会解引用
// 已释放内存；真正占资源的 runtime 在 unload 成功时立即销毁。
struct emma_plugin_s {
    enum class state {
        open,
        closing,
        tearing_down,
        destroyed,
    };

    std::unique_ptr<emma_plugin_runtime> runtime;
    state lifecycle = state::open;
    size_t active_operations = 0;
};

namespace {

using json = nlohmann::json;
using loader_context_t = sao::plugins::loader::plugin_context_t;

constexpr size_t kMaximumCallbackNesting = 64;
thread_local std::array<emma_plugin_runtime::callback_record*, kMaximumCallbackNesting>
    g_active_callbacks{};
thread_local size_t g_active_callback_depth = 0;
thread_local emma_plugin_runtime* g_active_runtime = nullptr;

struct emma_binding_plugin {
    void* context = nullptr;
    void* runtime = nullptr;
};

std::mutex g_emma_provider_mutex;
bool g_default_emma_provider_registered = false;
std::atomic_size_t g_default_emma_provider_plugins{0};

bool SAO_PLUGINS_CALL default_provider_available(void*) {
    return true;
}

int32_t SAO_PLUGINS_CALL default_provider_load(void* context, void* runtime, void** out_plugin,
                                               void*) {
    if (context == nullptr || runtime == nullptr || out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    auto plugin = std::unique_ptr<emma_binding_plugin>(new (std::nothrow) emma_binding_plugin{});
    if (!plugin)
        return SAO_ERR_OS_CALL_FAILED;
    plugin->context = context;
    plugin->runtime = runtime;
    *out_plugin = plugin.release();
    g_default_emma_provider_plugins.fetch_add(1, std::memory_order_relaxed);
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL default_provider_unload(void* plugin, void*) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    delete static_cast<emma_binding_plugin*>(plugin);
    g_default_emma_provider_plugins.fetch_sub(1, std::memory_order_relaxed);
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL default_provider_invoke(void*, const char*, const uint8_t*, size_t,
                                                 uint8_t*, size_t, size_t*, char*, size_t, void*) {
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

int32_t SAO_PLUGINS_CALL
default_provider_dispatch(sao::plugins::sdk_binding::language_binding_operation,
                          sao::plugins::sdk_binding::language_binding_request*, void*) {
    return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

sao::plugins::sdk_binding::language_host_adapter_vtable default_emma_provider() {
    sao::plugins::sdk_binding::language_host_adapter_vtable provider{};
    provider.language = sao::plugins::sdk_binding::language_host_kind::emma;
    provider.available = default_provider_available;
    provider.load_plugin = default_provider_load;
    provider.unload_plugin = default_provider_unload;
    provider.invoke = default_provider_invoke;
    provider.dispatch = default_provider_dispatch;
    return provider;
}

int32_t activate_emma_binding(emma_plugin_runtime& runtime) {
    std::lock_guard lock(g_emma_provider_mutex);
    if (!sao::plugins::sdk_binding::sao_plugins_binding_language_host_available(
            sao::plugins::sdk_binding::language_host_kind::emma)) {
        const auto provider = default_emma_provider();
        const int32_t status =
            sao::plugins::sdk_binding::sao_plugins_binding_register_language_host(&provider);
        if (status != SAO_OK)
            return status;
        g_default_emma_provider_registered = true;
    }
    return sao::plugins::sdk_binding::sao_plugins_binding_emma_activate(
        reinterpret_cast<sao::plugins::sdk_binding::plugin_context_ptr>(runtime.context),
        reinterpret_cast<sao::plugins::sdk_binding::emma_interpreter_ptr>(runtime.interp.get()),
        &runtime.binding);
}

void unregister_default_emma_provider_if_idle() noexcept {
    try {
        std::lock_guard lock(g_emma_provider_mutex);
        if (g_default_emma_provider_registered &&
            g_default_emma_provider_plugins.load(std::memory_order_acquire) == 0) {
            if (sao::plugins::sdk_binding::sao_plugins_binding_unregister_language_host(
                    sao::plugins::sdk_binding::language_host_kind::emma) == SAO_OK) {
                g_default_emma_provider_registered = false;
            }
        }
    } catch (...) {
    }
}

int32_t deactivate_emma_binding(emma_plugin_runtime& runtime) noexcept {
    if (runtime.binding == nullptr)
        return SAO_OK;
    const int32_t status =
        sao::plugins::sdk_binding::sao_plugins_binding_emma_deactivate(runtime.binding);
    if (status != SAO_OK)
        return status;
    runtime.binding = nullptr;
    unregister_default_emma_provider_if_idle();
    return SAO_OK;
}

void drain_pending_one_shots(emma_plugin_runtime& runtime) noexcept;

class runtime_invocation_guard final {
  public:
    explicit runtime_invocation_guard(emma_plugin_runtime* runtime) noexcept : runtime_(runtime) {
        if (runtime_ != nullptr && g_active_runtime == nullptr) {
            g_active_runtime = runtime_;
            acquired_ = true;
        }
    }

    ~runtime_invocation_guard() {
        if (acquired_)
            g_active_runtime = nullptr;
    }

    runtime_invocation_guard(const runtime_invocation_guard&) = delete;
    runtime_invocation_guard& operator=(const runtime_invocation_guard&) = delete;

    bool acquired() const noexcept {
        return acquired_;
    }

  private:
    emma_plugin_runtime* runtime_ = nullptr;
    bool acquired_ = false;
};

bool callback_active_on_current_thread(
    const emma_plugin_runtime::callback_record& callback) noexcept {
    return std::find(g_active_callbacks.begin(),
                     g_active_callbacks.begin() + g_active_callback_depth,
                     &callback) != g_active_callbacks.begin() + g_active_callback_depth;
}

class callback_lease {
  public:
    callback_lease() = default;
    ~callback_lease() {
        reset();
    }

    callback_lease(const callback_lease&) = delete;
    callback_lease& operator=(const callback_lease&) = delete;

    bool acquire(emma_plugin_runtime::callback_record* callback,
                 std::vector<emma_value>& pending_arguments) {
        if (callback == nullptr || g_active_callback_depth == kMaximumCallbackNesting) {
            return false;
        }
        std::lock_guard lock(callback->mutex);
        if (!callback->accepting || callback->runtime == nullptr || callback->function == nullptr ||
            !callback->runtime->callbacks_accepting.load(std::memory_order_acquire)) {
            return false;
        }
        if (callback->one_shot) {
            if (callback->one_shot_phase ==
                emma_plugin_runtime::callback_record::one_shot_state::registering) {
                callback->one_shot_phase =
                    emma_plugin_runtime::callback_record::one_shot_state::fired_pending;
                callback->pending_arguments = std::move(pending_arguments);
                return false;
            }
            if (callback->one_shot_phase !=
                emma_plugin_runtime::callback_record::one_shot_state::armed) {
                return false;
            }
            callback->one_shot_phase =
                emma_plugin_runtime::callback_record::one_shot_state::claimed;
            callback->accepting = false;
        }
        ++callback->in_flight;
        callback_ = callback;
        g_active_callbacks[g_active_callback_depth++] = callback;
        return true;
    }

    bool claim() {
        if (callback_ == nullptr)
            return false;
        std::lock_guard lock(callback_->mutex);
        if (!callback_->one_shot)
            return true;
        return callback_->one_shot_phase ==
               emma_plugin_runtime::callback_record::one_shot_state::claimed;
    }

    void defer(std::vector<emma_value> arguments) {
        if (callback_ == nullptr)
            return;
        std::lock_guard lock(callback_->mutex);
        if (callback_->one_shot &&
            callback_->one_shot_phase ==
                emma_plugin_runtime::callback_record::one_shot_state::claimed) {
            callback_->one_shot_phase =
                emma_plugin_runtime::callback_record::one_shot_state::fired_pending;
            callback_->accepting = false;
            callback_->pending_arguments = std::move(arguments);
        }
    }

    emma_plugin_runtime::callback_record& value() const noexcept {
        return *callback_;
    }

  private:
    void reset() noexcept {
        if (callback_ == nullptr)
            return;
        if (g_active_callback_depth > 0 &&
            g_active_callbacks[g_active_callback_depth - 1] == callback_) {
            g_active_callbacks[--g_active_callback_depth] = nullptr;
        }
        try {
            {
                std::lock_guard lock(callback_->mutex);
                if (callback_->in_flight > 0)
                    --callback_->in_flight;
            }
            callback_->idle.notify_all();
        } catch (...) {
        }
        callback_ = nullptr;
    }

    emma_plugin_runtime::callback_record* callback_ = nullptr;
};

bool teardown_status_is_complete(int32_t status) noexcept;

int32_t quiesce_callback(const std::shared_ptr<emma_plugin_runtime::callback_record>& callback) {
    if (callback == nullptr)
        return SAO_OK;
    std::unique_lock lock(callback->mutex);
    if (callback_active_on_current_thread(*callback)) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    callback->accepting = false;
    callback->idle.wait(lock, [&callback] { return callback->in_flight == 0; });
    return SAO_OK;
}

void resume_callback(const std::shared_ptr<emma_plugin_runtime::callback_record>& callback) {
    if (callback == nullptr)
        return;
    std::lock_guard lock(callback->mutex);
    if (callback->runtime != nullptr &&
        callback->one_shot_phase != emma_plugin_runtime::callback_record::one_shot_state::claimed &&
        callback->runtime->callbacks_accepting.load(std::memory_order_acquire)) {
        callback->accepting = true;
    }
}

void retire_callback(const std::shared_ptr<emma_plugin_runtime::callback_record>& callback) {
    if (callback == nullptr)
        return;
    std::lock_guard lock(callback->mutex);
    callback->accepting = false;
    callback->runtime = nullptr;
    callback->function.reset();
    callback->pending_arguments.clear();
}

struct direct_plugin_registry {
    std::mutex mutex;
    std::unordered_map<emma_plugin_handle_t, std::unique_ptr<emma_plugin_s>> plugins;
};

direct_plugin_registry& direct_plugins() {
    static direct_plugin_registry registry;
    return registry;
}

class direct_operation_lease {
  public:
    direct_operation_lease() = default;
    ~direct_operation_lease() {
        reset();
    }

    direct_operation_lease(const direct_operation_lease&) = delete;
    direct_operation_lease& operator=(const direct_operation_lease&) = delete;

    emma_plugin_runtime& runtime() const noexcept {
        return *runtime_;
    }

    void assign(emma_plugin_s* plugin, emma_plugin_runtime* runtime) noexcept {
        plugin_ = plugin;
        runtime_ = runtime;
    }

    void reset() noexcept {
        if (plugin_ == nullptr)
            return;
        try {
            auto& registry = direct_plugins();
            std::lock_guard lock(registry.mutex);
            if (plugin_->active_operations > 0) {
                --plugin_->active_operations;
            }
        } catch (...) {
        }
        plugin_ = nullptr;
        runtime_ = nullptr;
    }

  private:
    emma_plugin_s* plugin_ = nullptr;
    emma_plugin_runtime* runtime_ = nullptr;
};

int32_t acquire_direct_plugin(emma_plugin_handle_t plugin, direct_operation_lease& lease) {
    auto& registry = direct_plugins();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.plugins.find(plugin);
    if (found == registry.plugins.end() || found->second == nullptr ||
        found->second->runtime == nullptr ||
        found->second->lifecycle == emma_plugin_s::state::destroyed) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second->lifecycle != emma_plugin_s::state::open) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second->active_operations;
    lease.assign(found->second.get(), found->second->runtime.get());
    return SAO_OK;
}

template <typename Callback>
int32_t with_direct_plugin(emma_plugin_handle_t plugin, Callback&& callback) {
    direct_operation_lease lease;
    const int32_t status = acquire_direct_plugin(plugin, lease);
    if (status != SAO_OK)
        return status;
    auto& runtime = lease.runtime();
    int32_t result = SAO_ERR_OS_CALL_FAILED;
    {
        runtime_invocation_guard guard(&runtime);
        if (!guard.acquired())
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        std::lock_guard invocation_lock(runtime.invocation_mutex);
        result = callback(runtime);
    }
    drain_pending_one_shots(runtime);
    return result;
}

std::shared_ptr<callable>
make_host_callable(const char* name,
                   std::function<emma_value(std::vector<emma_value>)> implementation) {
    auto result = std::make_shared<callable>();
    result->name = name;
    result->host_impl = std::move(implementation);
    return result;
}

std::string require_string(const std::vector<emma_value>& arguments, size_t index,
                           const char* method) {
    if (index >= arguments.size() || !std::holds_alternative<std::string>(arguments[index])) {
        emma_error error;
        error.kind = error_kind::runtime_error;
        error.status = SAO_ERR_INVALID_ARGUMENT;
        error.message = std::string(method) + ": expected string argument";
        throw emma_exception(std::move(error));
    }
    return std::get<std::string>(arguments[index]);
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

int64_t require_integer(const std::vector<emma_value>& arguments, size_t index,
                        const char* method) {
    if (index >= arguments.size() || !std::holds_alternative<int64_t>(arguments[index])) {
        emma_error error;
        error.kind = error_kind::runtime_error;
        error.status = SAO_ERR_INVALID_ARGUMENT;
        error.message = std::string(method) + ": expected integer argument";
        throw emma_exception(std::move(error));
    }
    return std::get<int64_t>(arguments[index]);
}

double require_number(const std::vector<emma_value>& arguments, size_t index, const char* method) {
    if (index < arguments.size()) {
        if (const auto* integer = std::get_if<int64_t>(&arguments[index])) {
            return static_cast<double>(*integer);
        }
        if (const auto* number = std::get_if<double>(&arguments[index])) {
            return *number;
        }
    }
    emma_error error;
    error.kind = error_kind::runtime_error;
    error.status = SAO_ERR_INVALID_ARGUMENT;
    error.message = std::string(method) + ": expected numeric argument";
    throw emma_exception(std::move(error));
}

std::shared_ptr<callable> require_callable(const std::vector<emma_value>& arguments, size_t index,
                                           const char* method) {
    if (index < arguments.size()) {
        if (const auto* function = std::get_if<std::shared_ptr<callable>>(&arguments[index]);
            function != nullptr && *function != nullptr) {
            return *function;
        }
    }
    emma_error error;
    error.kind = error_kind::runtime_error;
    error.status = SAO_ERR_INVALID_ARGUMENT;
    error.message = std::string(method) + ": expected callback argument";
    throw emma_exception(std::move(error));
}

[[noreturn]] void throw_context_status(const char* method, int32_t status) {
    emma_error error;
    error.kind = error_kind::runtime_error;
    error.status = status;
    error.message = std::string(method) + " failed with status " + std::to_string(status);
    throw emma_exception(std::move(error));
}

std::string require_metadata_only_panel(const std::vector<emma_value>& arguments) {
    if (arguments.size() < 2 || arguments.size() > 4)
        throw_context_status("ctx.register_ui_panel", SAO_ERR_INVALID_ARGUMENT);
    const std::string panel_id = require_string(arguments, 0, "ctx.register_ui_panel");
    if ((arguments.size() > 2 && !std::holds_alternative<std::nullptr_t>(arguments[2])) ||
        (arguments.size() > 3 && !std::holds_alternative<std::nullptr_t>(arguments[3]))) {
        throw_context_status("ctx.register_ui_panel callbacks",
                             sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    }
    return panel_id;
}

constexpr std::size_t kMaximumJsonInputBytes = detail::kMaximumEmmaJsonInputBytes;

bool valid_bounded_json(std::string_view input, std::size_t byte_limit) noexcept {
    if (input.size() > byte_limit)
        return false;
    try {
        std::string error;
        return detail::validate_json(input, error);
    } catch (...) {
        return false;
    }
}

bool parse_bounded_json(std::string_view input, std::size_t byte_limit, json& output) noexcept {
    if (input.size() > byte_limit)
        return false;
    try {
        std::string error;
        return detail::parse_json(input.data(), input.size(), output, error);
    } catch (...) {
        return false;
    }
}

bool parse_bounded_json_c_string(const char* input, std::size_t byte_limit,
                                 json& output) noexcept {
    if (input == nullptr)
        return false;
    std::size_t length = 0;
    while (length <= byte_limit && input[length] != '\0')
        ++length;
    if (length > byte_limit)
        return false;
    return parse_bounded_json(std::string_view(input, length), byte_limit, output);
}

bool value_to_json(const emma_value& value, json& output, size_t depth = 0) {
    if (depth != 0)
        return false;
    std::string error;
    return detail::emma_value_to_json(value, output, error);
}

bool json_to_value(const json& input, emma_value& output, size_t depth = 0) {
    if (depth != 0)
        return false;
    std::string error;
    return detail::json_to_emma_value(input, output, error);
}

std::string serialize_value_or_throw(const emma_value& value) {
    std::string serialized;
    std::string message;
    if (!detail::serialize_emma_value(value, serialized, message)) {
        emma_error error;
        error.kind = error_kind::runtime_error;
        error.status = SAO_ERR_INVALID_ARGUMENT;
        error.message = "JSON conversion: " + message;
        throw emma_exception(std::move(error));
    }
    return serialized;
}

constexpr std::size_t kMaximumMenuRows = 4096;
constexpr std::size_t kMaximumMenuStringBytes = 16U * 1024U;
constexpr std::size_t kMaximumMenuSnapshotBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumRememberedActions = 4096;

std::uint64_t stable_hash(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_suffix(std::uint64_t hash) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::string hash_suffix(std::string_view value) {
    return hash_suffix(stable_hash(value));
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
}

void hash_integer(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<unsigned char>((value >> shift) & 0xffU);
        hash_bytes(hash, &byte, 1);
    }
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, value.size());
    hash_bytes(hash, value.data(), value.size());
}

void hash_ast_node(const ast_pool& pool, node_id id, std::uint64_t& hash,
                   std::size_t depth = 0) noexcept {
    if (depth > 1024 || id >= pool.kinds.size() || id >= pool.strings.size() ||
        id >= pool.ints.size() || id >= pool.floats.size() || id >= pool.children.size()) {
        hash_integer(hash, std::numeric_limits<std::uint64_t>::max());
        return;
    }
    hash_integer(hash, static_cast<std::uint64_t>(pool.kinds[id]));
    hash_string(hash, pool.strings[id]);
    hash_integer(hash, static_cast<std::uint64_t>(pool.ints[id]));
    std::uint64_t floating_bits = 0;
    static_assert(sizeof(floating_bits) == sizeof(pool.floats[id]));
    std::memcpy(&floating_bits, &pool.floats[id], sizeof(floating_bits));
    hash_integer(hash, floating_bits);
    hash_integer(hash, pool.children[id].size());
    for (const node_id child : pool.children[id])
        hash_ast_node(pool, child, hash, depth + 1);
}

std::string callable_identity(const emma_plugin_runtime& runtime, const callable& function) {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, function.name);
    hash_integer(hash, function.params.size());
    for (const auto& parameter : function.params)
        hash_string(hash, parameter);
    hash_integer(hash, function.body.size());
    for (const node_id body : function.body) {
        hash_integer(hash, body);
        hash_ast_node(runtime.pool, body, hash);
    }
    return "emma-callable-" + hash_suffix(hash);
}

bool valid_menu_string(std::string_view value, bool required) noexcept {
    return (!required || !value.empty()) && value.size() <= kMaximumMenuStringBytes &&
           value.find('\0') == std::string_view::npos && detail::valid_utf8(value);
}

void remember_menu_error(emma_plugin_runtime& runtime, int32_t status, std::string message) {
    runtime.last_error = {};
    runtime.last_error.kind = error_kind::runtime_error;
    runtime.last_error.status = status;
    runtime.last_error.message = std::move(message);
}

bool menu_string(const emma_dict& dictionary, const char* key, bool required, std::string& output,
                 std::string& error) {
    output.clear();
    const auto found = dictionary.items.find(key);
    if (found == dictionary.items.end() || std::holds_alternative<std::nullptr_t>(found->second)) {
        if (required)
            error = "menu text field is required";
        return !required;
    }
    const auto* value = std::get_if<std::string>(&found->second);
    if (value == nullptr) {
        error = "menu text fields must be strings";
        return false;
    }
    if (!valid_menu_string(*value, required)) {
        error = "menu text field is invalid";
        return false;
    }
    output = *value;
    return true;
}

bool menu_flag(const emma_dict& dictionary, const char* key, bool fallback, bool& output,
               std::string& error) {
    const auto found = dictionary.items.find(key);
    if (found == dictionary.items.end() || std::holds_alternative<std::nullptr_t>(found->second)) {
        output = fallback;
        return true;
    }
    const auto* value = std::get_if<bool>(&found->second);
    if (value == nullptr) {
        error = "menu flag fields must be booleans";
        return false;
    }
    output = *value;
    return true;
}

bool menu_payload(const emma_dict& dictionary, std::string& output, std::string& error) {
    const auto encoded = dictionary.items.find("payload_json");
    if (encoded != dictionary.items.end() &&
        !std::holds_alternative<std::nullptr_t>(encoded->second)) {
        const auto* value = std::get_if<std::string>(&encoded->second);
        if (value == nullptr || !valid_menu_string(*value, false)) {
            error = "menu payload_json must be a valid text field";
            return false;
        }
        if (!valid_bounded_json(*value, kMaximumMenuStringBytes)) {
            error = "menu payload_json must be valid JSON";
            return false;
        }
        output = *value;
        return true;
    }

    const auto payload = dictionary.items.find("payload");
    if (payload == dictionary.items.end() ||
        std::holds_alternative<std::nullptr_t>(payload->second)) {
        output = "{}";
        return true;
    }
    std::string conversion_error;
    if (!detail::serialize_emma_value(payload->second, output, conversion_error)) {
        error = "menu payload cannot be converted to JSON";
        return false;
    }
    if (!valid_menu_string(output, false)) {
        error = "menu payload exceeds its field budget";
        return false;
    }
    return true;
}

void append_identity_field(std::string& identity, std::string_view value) {
    identity.append(std::to_string(value.size()));
    identity.push_back(':');
    identity.append(value);
    identity.push_back('\n');
}

bool add_snapshot_bytes(std::size_t& total, std::size_t amount) noexcept {
    if (total > kMaximumMenuSnapshotBytes || amount > kMaximumMenuSnapshotBytes - total)
        return false;
    total += amount;
    return true;
}

bool build_menu_snapshot(emma_menu_bridge& bridge) {
    auto& runtime = *bridge.runtime;
    std::string call_message;
    emma_error call_error;
    emma_value result =
        runtime.interp->call_function(bridge.builder, {}, call_message, &call_error);
    if (call_error.kind != error_kind::none || !call_message.empty()) {
        if (call_error.kind == error_kind::none) {
            call_error.kind = error_kind::runtime_error;
            call_error.status = SAO_ERR_OS_CALL_FAILED;
            call_error.message = std::move(call_message);
        }
        runtime.last_error = std::move(call_error);
        return false;
    }
    const auto* list = std::get_if<std::shared_ptr<emma_list>>(&result);
    if (list == nullptr || !*list) {
        remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT, "menu builder must return a list");
        return false;
    }
    if ((*list)->items.size() > kMaximumMenuRows) {
        remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                            "menu builder returned too many rows");
        return false;
    }

    std::vector<emma_menu_row> rows;
    std::unordered_map<std::string, std::shared_ptr<callable>> actions;
    std::unordered_map<std::string, std::size_t> identity_occurrences;
    std::unordered_set<std::string> action_ids;
    rows.reserve((*list)->items.size());
    actions.reserve((*list)->items.size());
    identity_occurrences.reserve((*list)->items.size());
    action_ids.reserve((*list)->items.size());
    std::size_t total_bytes = 0;

    for (const auto& item : (*list)->items) {
        const auto* dictionary = std::get_if<std::shared_ptr<emma_dict>>(&item);
        if (dictionary == nullptr || !*dictionary) {
            remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                                "menu rows must be dictionaries");
            return false;
        }
        emma_menu_row row;
        std::string conversion_error;
        if (!menu_string(**dictionary, "label", true, row.label, conversion_error) ||
            !menu_string(**dictionary, "icon", false, row.icon, conversion_error) ||
            !menu_payload(**dictionary, row.payload_json, conversion_error) ||
            !menu_flag(**dictionary, "keep_menu_open", false, row.keep_menu_open,
                       conversion_error) ||
            !menu_flag(**dictionary, "close_menu_before", false, row.close_menu_before,
                       conversion_error)) {
            remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT, std::move(conversion_error));
            return false;
        }

        std::shared_ptr<callable> command;
        const auto command_value = (*dictionary)->items.find("command");
        if (command_value != (*dictionary)->items.end() &&
            !std::holds_alternative<std::nullptr_t>(command_value->second)) {
            const auto* function = std::get_if<std::shared_ptr<callable>>(&command_value->second);
            if (function == nullptr || !*function) {
                remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                                    "menu command must be callable");
                return false;
            }
            command = *function;
        }
        bool requested_can_activate = command != nullptr;
        if (!menu_flag(**dictionary, "can_activate", requested_can_activate, requested_can_activate,
                       conversion_error)) {
            remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT, std::move(conversion_error));
            return false;
        }
        row.can_activate = command != nullptr && requested_can_activate;

        std::string explicit_identity;
        const auto explicit_action = (*dictionary)->items.find("action_id");
        const auto explicit_id = (*dictionary)->items.find("id");
        if (explicit_action != (*dictionary)->items.end() &&
            !std::holds_alternative<std::nullptr_t>(explicit_action->second)) {
            if (!menu_string(**dictionary, "action_id", true, explicit_identity,
                             conversion_error)) {
                remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT, std::move(conversion_error));
                return false;
            }
        } else if (explicit_id != (*dictionary)->items.end() &&
                   !std::holds_alternative<std::nullptr_t>(explicit_id->second) &&
                   !menu_string(**dictionary, "id", true, explicit_identity, conversion_error)) {
            remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT, std::move(conversion_error));
            return false;
        }

        std::string identity = explicit_identity;
        if (identity.empty()) {
            append_identity_field(identity, command == nullptr
                                                ? std::string_view{}
                                                : callable_identity(runtime, *command));
            append_identity_field(identity, row.label);
            append_identity_field(identity, row.icon);
            append_identity_field(identity, row.payload_json);
            identity.push_back(row.can_activate ? '1' : '0');
            identity.push_back(row.keep_menu_open ? '1' : '0');
            identity.push_back(row.close_menu_before ? '1' : '0');
            const std::size_t occurrence = identity_occurrences[identity]++;
            identity.push_back('#');
            identity.append(std::to_string(occurrence));
        }
        row.action_id = "menu-action-" + hash_suffix(bridge.contribution_id + "\n" + identity);
        if (!action_ids.emplace(row.action_id).second) {
            remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                                "menu action identities must be unique");
            return false;
        }
        if (command != nullptr)
            actions.emplace(row.action_id, std::move(command));

        const std::size_t row_bytes = row.label.size() + row.icon.size() + row.action_id.size() +
                                      row.payload_json.size() + bridge.contribution_id.size() +
                                      bridge.name.size() + bridge.icon.size();
        if (!add_snapshot_bytes(total_bytes, row_bytes)) {
            remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                                "menu builder snapshot exceeds its byte budget");
            return false;
        }
        rows.push_back(std::move(row));
    }

    std::size_t new_action_count = 0;
    for (const auto& [action_id, _] : actions) {
        if (!bridge.actions.contains(action_id))
            ++new_action_count;
    }
    if (bridge.actions.size() > kMaximumRememberedActions ||
        new_action_count > kMaximumRememberedActions - bridge.actions.size()) {
        remember_menu_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                            "menu action history exceeds its budget");
        return false;
    }

    const bool rows_changed = bridge.revision == 0 || bridge.rows != rows;
    if (rows_changed && bridge.revision == std::numeric_limits<std::uint64_t>::max()) {
        remember_menu_error(runtime, SAO_ERR_OS_CALL_FAILED, "menu snapshot revision exhausted");
        return false;
    }
    auto next_actions = bridge.actions;
    for (auto& [action_id, command] : actions)
        next_actions.insert_or_assign(action_id, std::move(command));
    bridge.actions = std::move(next_actions);
    bridge.rows = std::move(rows);
    if (rows_changed)
        ++bridge.revision;
    runtime.last_error = {};
    return true;
}

int32_t SAO_PLUGINS_CALL native_menu_snapshot_v2(
    void* rows, std::uint32_t capacity, std::uint32_t row_stride_bytes,
    std::uint32_t* out_count, std::uint64_t* out_revision,
    sao::plugins::loader::entity_snapshot_content_token_t* out_content_token,
    std::uint32_t* out_row_stride_bytes, void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || out_content_token == nullptr ||
        out_row_stride_bytes == nullptr || user_data == nullptr ||
        (capacity > 0 && rows == nullptr) || (rows == nullptr && row_stride_bytes != 0)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& bridge = *static_cast<emma_menu_bridge*>(user_data);
    auto* runtime = bridge.runtime;
    if (runtime == nullptr || bridge.closing.load(std::memory_order_acquire))
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    try {
        runtime_invocation_guard guard(runtime);
        if (!guard.acquired())
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        std::unique_lock invocation_lock(runtime->invocation_mutex, std::try_to_lock);
        if (!invocation_lock.owns_lock() || bridge.closing.load(std::memory_order_acquire))
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        if (rows == nullptr && !build_menu_snapshot(bridge))
            return SAO_ERR_OS_CALL_FAILED;
        *out_count = static_cast<std::uint32_t>(bridge.rows.size());
        *out_revision = bridge.revision;
        *out_content_token =
            bridge.revision == sao::plugins::loader::kInvalidEntitySnapshotContentToken
                ? 1
                : bridge.revision;
        *out_row_stride_bytes =
            bridge.rows.empty()
                ? 0
                : static_cast<std::uint32_t>(
                      sizeof(sao::plugins::loader::entity_menu_row_v2));
        if (capacity < bridge.rows.size())
            return SAO_ERR_BUFFER_TOO_SMALL;
        if (!bridge.rows.empty() &&
            row_stride_bytes < sizeof(sao::plugins::loader::entity_menu_row_v2)) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ABI_MISMATCH;
        }
        if (!bridge.rows.empty() &&
            row_stride_bytes % alignof(sao::plugins::loader::entity_menu_row_v2) != 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        for (std::size_t index = 0; index < bridge.rows.size(); ++index) {
            const auto& source = bridge.rows[index];
            const sao::plugins::loader::entity_menu_row_v2 row{
                sizeof(sao::plugins::loader::entity_menu_row_v2),
                bridge.contribution_id.c_str(),
                bridge.name.c_str(),
                bridge.icon.c_str(),
                bridge.priority,
                source.label.c_str(),
                source.icon.c_str(),
                source.action_id.c_str(),
                source.payload_json.c_str(),
                static_cast<std::uint8_t>(source.can_activate),
                static_cast<std::uint8_t>(source.keep_menu_open),
                static_cast<std::uint8_t>(source.close_menu_before),
                {},
            };
            std::memcpy(static_cast<std::byte*>(rows) + index * row_stride_bytes, &row,
                        sizeof(row));
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void remember_action_error(emma_plugin_runtime& runtime, int32_t status, std::string message) {
    runtime.last_error = {};
    runtime.last_error.kind = error_kind::runtime_error;
    runtime.last_error.status = status;
    runtime.last_error.message = std::move(message);
}

int32_t submit_action_result(sao::plugins::loader::entity_action_result_sink_v2_fn sink,
                             void* sink_user_data, bool handled,
                             const char* result_json_utf8) noexcept {
    if (sink == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const sao::plugins::loader::entity_action_result_v2 result{
        sizeof(sao::plugins::loader::entity_action_result_v2),
        sao::plugins::loader::kEntityActionAbiVersion2,
        static_cast<std::uint8_t>(handled),
        {},
        result_json_utf8,
    };
    return sink(&result, sink_user_data);
}

int32_t submit_emma_action_value(
    emma_plugin_runtime& runtime, const emma_value& value, bool null_is_decline,
    sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
    void* result_sink_user_data) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        const int32_t status = submit_action_result(result_sink, result_sink_user_data,
                                                    !null_is_decline, nullptr);
        if (status == SAO_OK) {
            runtime.last_error = {};
        } else {
            remember_action_error(runtime, status,
                                  null_is_decline ? "action result sink rejected decline"
                                                  : "action result sink rejected result");
        }
        return status;
    }

    std::string serialized;
    std::string conversion_error;
    if (!detail::serialize_emma_value(value, serialized, conversion_error)) {
        remember_action_error(runtime, SAO_ERR_INVALID_ARGUMENT,
                              "action result cannot be converted to JSON: " + conversion_error);
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status =
        submit_action_result(result_sink, result_sink_user_data, true, serialized.c_str());
    if (status == SAO_OK) {
        runtime.last_error = {};
    } else {
        remember_action_error(runtime, status, "action result sink rejected result");
    }
    return status;
}

int32_t SAO_PLUGINS_CALL native_menu_action_v2(
    const char* action_id_utf8, const char* payload_json_utf8,
    sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
    void* result_sink_user_data, void* user_data) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr || result_sink == nullptr ||
        user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& bridge = *static_cast<emma_menu_bridge*>(user_data);
    auto* runtime = bridge.runtime;
    if (runtime == nullptr || bridge.closing.load(std::memory_order_acquire))
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    try {
        runtime_invocation_guard guard(runtime);
        if (!guard.acquired())
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        std::unique_lock invocation_lock(runtime->invocation_mutex, std::try_to_lock);
        if (!invocation_lock.owns_lock() || bridge.closing.load(std::memory_order_acquire))
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        const auto found = bridge.actions.find(action_id_utf8);
        if (found == bridge.actions.end() || found->second == nullptr) {
            const emma_value declined = nullptr;
            return submit_emma_action_value(*runtime, declined, true, result_sink,
                                            result_sink_user_data);
        }

        std::string message;
        emma_error error;
        emma_value value = runtime->interp->call_function(found->second, {}, message, &error);
        if (error.kind != error_kind::none || !message.empty()) {
            if (error.kind == error_kind::none) {
                error.kind = error_kind::runtime_error;
                error.status = SAO_ERR_OS_CALL_FAILED;
                error.message = std::move(message);
            }
            const int32_t status = sao_plugins_emma_error_status(&error);
            runtime->last_error = std::move(error);
            return status;
        }
        return submit_emma_action_value(*runtime, value, false, result_sink,
                                        result_sink_user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL
native_action_handler(const char* action_id_utf8, const char* payload_json_utf8,
                      sao::plugins::loader::entity_action_result_sink_v2_fn result_sink,
                      void* result_sink_user_data, void* user_data) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr || result_sink == nullptr ||
        user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& bridge = *static_cast<emma_action_bridge*>(user_data);
    auto* runtime = bridge.runtime;
    if (runtime == nullptr || bridge.closing.load(std::memory_order_acquire))
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    try {
        runtime_invocation_guard guard(runtime);
        if (!guard.acquired())
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        std::unique_lock invocation_lock(runtime->invocation_mutex, std::try_to_lock);
        if (!invocation_lock.owns_lock() || bridge.closing.load(std::memory_order_acquire))
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        if (bridge.handler == nullptr)
            return SAO_ERR_HANDLE_INVALID;

        emma_value payload = nullptr;
        std::string conversion_error;
        if (!detail::parse_json_c_string_to_emma_value(payload_json_utf8, payload,
                                                       conversion_error)) {
            remember_action_error(*runtime, SAO_ERR_INVALID_ARGUMENT,
                                  "action payload cannot be converted from JSON: " +
                                      conversion_error);
            return SAO_ERR_INVALID_ARGUMENT;
        }

        std::string call_message;
        emma_error call_error;
        emma_value value = runtime->interp->call_function(
            bridge.handler, {std::string(action_id_utf8), std::move(payload)}, call_message,
            &call_error);
        if (call_error.kind != error_kind::none || !call_message.empty()) {
            if (call_error.kind == error_kind::none) {
                call_error.kind = error_kind::runtime_error;
                call_error.status = SAO_ERR_OS_CALL_FAILED;
                call_error.message = std::move(call_message);
            }
            const int32_t status = sao_plugins_emma_error_status(&call_error);
            runtime->last_error = std::move(call_error);
            return status;
        }

        return submit_emma_action_value(*runtime, value, true, result_sink,
                                        result_sink_user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_action_provider(emma_plugin_runtime& runtime,
                                 emma_action_bridge& action) noexcept {
    if (runtime.context == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        sao::plugins::loader::context_entity_provider_descriptor_v3 provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = action.provider_id.c_str();
        provider.action_handler_v2 = native_action_handler;
        provider.action_user_data = &action;
        provider.flags = sao::plugins::loader::kContextEntityProviderV3ActionOnly;
        return sao::plugins::loader::sao_plugins_ctx_register_entity_provider_v3(runtime.context,
                                                                                 &provider);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_menu_provider(emma_plugin_runtime& runtime, emma_menu_bridge& menu) noexcept {
    if (runtime.context == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        sao::plugins::loader::entity_root_contribution_descriptor root{};
        root.struct_size = sizeof(root);
        root.contribution_id_utf8 = menu.contribution_id.c_str();
        root.root_id_utf8 = menu.root_id.c_str();
        root.name_utf8 = menu.name.c_str();
        root.icon_utf8 = menu.icon.c_str();
        root.priority = menu.priority;

        sao::plugins::loader::context_entity_provider_descriptor_v3 provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = menu.provider_id.c_str();
        provider.snapshot = native_menu_snapshot_v2;
        provider.action_handler = nullptr;
        provider.user_data = &menu;
        provider.root_contribution = &root;
        provider.action_handler_v2 = native_menu_action_v2;
        provider.action_user_data = &menu;
        return sao::plugins::loader::sao_plugins_ctx_register_entity_provider_v3(
            runtime.context, &provider);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_menu_providers(emma_plugin_runtime& runtime,
                                  const std::vector<emma_menu_bridge*>& menus) noexcept {
    if (runtime.context == nullptr || menus.empty())
        return SAO_OK;
    try {
        std::vector<std::string> qualified_ids;
        std::vector<const char*> provider_ids;
        qualified_ids.reserve(menus.size());
        provider_ids.reserve(menus.size());
        for (auto* menu : menus) {
            if (menu == nullptr)
                return SAO_ERR_INVALID_ARGUMENT;
            menu->closing.store(true, std::memory_order_release);
            qualified_ids.push_back(runtime.plugin_id + "/" + menu->provider_id);
        }
        for (const auto& provider_id : qualified_ids)
            provider_ids.push_back(provider_id.c_str());
        const int32_t status = sao::plugins::loader::plugin_context_unregister_entity_providers(
            runtime.context, provider_ids.data(), provider_ids.size());
        if (status != SAO_OK) {
            for (auto* menu : menus)
                menu->closing.store(false, std::memory_order_release);
        }
        return status;
    } catch (...) {
        for (auto* menu : menus) {
            if (menu != nullptr)
                menu->closing.store(false, std::memory_order_release);
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_context_providers(emma_plugin_runtime& runtime,
                                     const std::vector<emma_menu_bridge*>& menus,
                                     emma_action_bridge* action) noexcept {
    if (runtime.context == nullptr || (menus.empty() && action == nullptr))
        return SAO_OK;
    try {
        std::vector<std::string> qualified_ids;
        std::vector<const char*> provider_ids;
        qualified_ids.reserve(menus.size() + (action == nullptr ? 0U : 1U));
        provider_ids.reserve(qualified_ids.capacity());
        for (auto* menu : menus) {
            if (menu == nullptr)
                return SAO_ERR_INVALID_ARGUMENT;
            menu->closing.store(true, std::memory_order_release);
            qualified_ids.push_back(runtime.plugin_id + "/" + menu->provider_id);
        }
        if (action != nullptr) {
            action->closing.store(true, std::memory_order_release);
            qualified_ids.push_back(runtime.plugin_id + "/" + action->provider_id);
        }
        for (const auto& provider_id : qualified_ids)
            provider_ids.push_back(provider_id.c_str());
        const int32_t status = sao::plugins::loader::plugin_context_unregister_entity_providers(
            runtime.context, provider_ids.data(), provider_ids.size());
        if (status != SAO_OK) {
            for (auto* menu : menus)
                menu->closing.store(false, std::memory_order_release);
            if (action != nullptr)
                action->closing.store(false, std::memory_order_release);
        }
        return status;
    } catch (...) {
        for (auto* menu : menus) {
            if (menu != nullptr)
                menu->closing.store(false, std::memory_order_release);
        }
        if (action != nullptr)
            action->closing.store(false, std::memory_order_release);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

std::vector<emma_menu_bridge*> menu_range(emma_plugin_runtime& runtime, std::size_t checkpoint) {
    std::vector<emma_menu_bridge*> result;
    if (checkpoint >= runtime.menus.size())
        return result;
    result.reserve(runtime.menus.size() - checkpoint);
    for (std::size_t index = checkpoint; index < runtime.menus.size(); ++index)
        result.push_back(runtime.menus[index].get());
    return result;
}

int32_t register_menu_providers(emma_plugin_runtime& runtime) noexcept {
    try {
        std::vector<emma_menu_bridge*> registered;
        registered.reserve(runtime.menus.size());
        for (const auto& menu : runtime.menus) {
            const int32_t status = register_menu_provider(runtime, *menu);
            if (status != SAO_OK) {
                const int32_t rollback_status = unregister_menu_providers(runtime, registered);
                return rollback_status == SAO_OK ? status : rollback_status;
            }
            registered.push_back(menu.get());
        }
        for (const auto& menu : runtime.menus)
            menu->closing.store(false, std::memory_order_release);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_context_providers(emma_plugin_runtime& runtime) noexcept {
    bool action_registered = false;
    if (runtime.action != nullptr) {
        const int32_t status = register_action_provider(runtime, *runtime.action);
        if (status != SAO_OK)
            return status;
        action_registered = true;
    }
    const int32_t menu_status = register_menu_providers(runtime);
    if (menu_status != SAO_OK && action_registered) {
        const int32_t rollback_status =
            unregister_context_providers(runtime, {}, runtime.action.get());
        return rollback_status == SAO_OK ? menu_status : rollback_status;
    }
    if (menu_status == SAO_OK && runtime.action != nullptr)
        runtime.action->closing.store(false, std::memory_order_release);
    return menu_status;
}

int32_t quiesce_context_providers(emma_plugin_runtime& runtime) noexcept {
    try {
        return unregister_context_providers(runtime, menu_range(runtime, 0), runtime.action.get());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t rollback_menus(emma_plugin_runtime& runtime, std::size_t checkpoint) noexcept {
    if (checkpoint > runtime.menus.size())
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        const int32_t status = unregister_menu_providers(runtime, menu_range(runtime, checkpoint));
        if (status != SAO_OK)
            return status;
        for (std::size_t index = checkpoint; index < runtime.menus.size(); ++index)
            runtime.menus[index]->runtime = nullptr;
        runtime.menus.erase(runtime.menus.begin() + static_cast<std::ptrdiff_t>(checkpoint),
                            runtime.menus.end());
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t commit_enable_menus(emma_plugin_runtime& runtime, std::size_t checkpoint) noexcept {
    if (checkpoint > runtime.menus.size())
        return SAO_ERR_INVALID_ARGUMENT;
    for (std::size_t index = checkpoint; index < runtime.menus.size(); ++index)
        runtime.menus[index]->enable_scoped = true;
    return SAO_OK;
}

int32_t remove_enable_menus(emma_plugin_runtime& runtime) noexcept {
    try {
        std::vector<emma_menu_bridge*> selected;
        for (const auto& menu : runtime.menus) {
            if (menu->enable_scoped)
                selected.push_back(menu.get());
        }
        const int32_t status = unregister_menu_providers(runtime, selected);
        if (status != SAO_OK)
            return status;
        std::erase_if(runtime.menus, [](const auto& menu) {
            if (!menu->enable_scoped)
                return false;
            menu->runtime = nullptr;
            return true;
        });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

emma_action_checkpoint action_checkpoint(const emma_plugin_runtime& runtime) {
    emma_action_checkpoint checkpoint;
    if (runtime.action == nullptr)
        return checkpoint;
    checkpoint.present = true;
    checkpoint.enable_scoped = runtime.action->enable_scoped;
    checkpoint.registration_revision = runtime.action->registration_revision;
    checkpoint.handler = runtime.action->handler;
    checkpoint.disable_restore_handler = runtime.action->disable_restore_handler;
    return checkpoint;
}

void reset_action_bridge(emma_plugin_runtime& runtime) noexcept {
    if (runtime.action == nullptr)
        return;
    runtime.action->closing.store(true, std::memory_order_release);
    runtime.action->runtime = nullptr;
    runtime.action->handler.reset();
    runtime.action->disable_restore_handler.reset();
    runtime.action.reset();
}

int32_t rollback_action(emma_plugin_runtime& runtime,
                        const emma_action_checkpoint& checkpoint) noexcept {
    try {
        if (!checkpoint.present) {
            if (runtime.action == nullptr)
                return SAO_OK;
            const int32_t status = unregister_context_providers(runtime, {}, runtime.action.get());
            if (status != SAO_OK)
                return status;
            reset_action_bridge(runtime);
            return SAO_OK;
        }
        if (runtime.action == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        runtime.action->enable_scoped = checkpoint.enable_scoped;
        runtime.action->registration_revision = checkpoint.registration_revision;
        runtime.action->handler = checkpoint.handler;
        runtime.action->disable_restore_handler = checkpoint.disable_restore_handler;
        runtime.action->closing.store(false, std::memory_order_release);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t commit_enable_action(emma_plugin_runtime& runtime,
                             const emma_action_checkpoint& checkpoint) noexcept {
    if (runtime.action == nullptr || (checkpoint.present && runtime.action->registration_revision ==
                                                                checkpoint.registration_revision)) {
        return SAO_OK;
    }
    if (!checkpoint.present) {
        runtime.action->enable_scoped = true;
        runtime.action->disable_restore_handler.reset();
        return SAO_OK;
    }
    runtime.action->enable_scoped = false;
    runtime.action->disable_restore_handler = checkpoint.handler;
    return SAO_OK;
}

int32_t remove_enable_action(emma_plugin_runtime& runtime) noexcept {
    if (runtime.action == nullptr)
        return SAO_OK;
    try {
        if (runtime.action->enable_scoped) {
            const int32_t status = unregister_context_providers(runtime, {}, runtime.action.get());
            if (status != SAO_OK)
                return status;
            reset_action_bridge(runtime);
            return SAO_OK;
        }
        if (runtime.action->disable_restore_handler == nullptr)
            return SAO_OK;
        runtime.action->handler = std::move(runtime.action->disable_restore_handler);
        runtime.action->disable_restore_handler.reset();
        runtime.action->closing.store(false, std::memory_order_release);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

emma_value register_action_handler(emma_plugin_runtime& runtime,
                                   std::vector<emma_value> arguments) {
    if (arguments.size() != 1)
        throw_context_status("ctx.register_action_handler", SAO_ERR_INVALID_ARGUMENT);
    auto candidate = require_callable(arguments, 0, "ctx.register_action_handler");
    if (runtime.context == nullptr)
        throw_context_status("ctx.register_action_handler", SAO_ERR_HANDLE_INVALID);

    if (runtime.action == nullptr) {
        auto action = std::make_unique<emma_action_bridge>();
        action->runtime = &runtime;
        action->handler = std::move(candidate);
        const int32_t status = register_action_provider(runtime, *action);
        if (status != SAO_OK) {
            action->runtime = nullptr;
            throw_context_status("ctx.register_action_handler", status);
        }
        runtime.action = std::move(action);
        return std::string(kActionProviderLocalId);
    }

    if (runtime.action->registration_revision == std::numeric_limits<std::uint64_t>::max())
        throw_context_status("ctx.register_action_handler", SAO_ERR_OS_CALL_FAILED);
    const int32_t status = register_action_provider(runtime, *runtime.action);
    if (status != SAO_OK)
        throw_context_status("ctx.register_action_handler", status);
    runtime.action->handler = std::move(candidate);
    ++runtime.action->registration_revision;
    runtime.action->closing.store(false, std::memory_order_release);
    return std::string(kActionProviderLocalId);
}

emma_value register_menu_surface(emma_plugin_runtime& runtime, std::vector<emma_value> arguments) {
    if (arguments.size() < 2 || arguments.size() > 3)
        throw_context_status("ctx.register_menu_surface", SAO_ERR_INVALID_ARGUMENT);
    const std::string surface_id = require_string(arguments, 0, "ctx.register_menu_surface");
    const std::string descriptor = serialize_value_or_throw(arguments[1]);
    const double priority =
        arguments.size() == 3 ? require_number(arguments, 2, "ctx.register_menu_surface") : 0.0;
    if (!std::isfinite(priority))
        throw_context_status("ctx.register_menu_surface", SAO_ERR_INVALID_ARGUMENT);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_menu_surface(
        runtime.context, surface_id.c_str(), descriptor.c_str(), static_cast<float>(priority));
    if (status != SAO_OK)
        throw_context_status("ctx.register_menu_surface", status);
    return surface_id;
}

emma_value register_menu_category(emma_plugin_runtime& runtime, std::vector<emma_value> arguments) {
    if (arguments.size() < 3 || arguments.size() > 4)
        throw_context_status("ctx.register_menu_category", SAO_ERR_INVALID_ARGUMENT);
    const std::string name = require_string(arguments, 0, "ctx.register_menu_category");
    const std::string icon = require_string(arguments, 1, "ctx.register_menu_category");
    auto builder = require_callable(arguments, 2, "ctx.register_menu_category");
    const double priority =
        arguments.size() == 4 ? require_number(arguments, 3, "ctx.register_menu_category") : 0.0;
    if (!valid_menu_string(name, true) || !valid_menu_string(icon, false) ||
        !std::isfinite(priority)) {
        throw_context_status("ctx.register_menu_category", SAO_ERR_INVALID_ARGUMENT);
    }
    if (runtime.context == nullptr)
        throw_context_status("ctx.register_menu_category", SAO_ERR_HANDLE_INVALID);

    auto menu = std::make_unique<emma_menu_bridge>();
    menu->runtime = &runtime;
    menu->provider_id = "menu-" + hash_suffix(name);
    menu->contribution_id = menu->provider_id;
    menu->root_id = "plugin:" + hash_suffix(runtime.plugin_id + "\n" + name);
    menu->name = name;
    menu->icon = icon;
    menu->priority = priority;
    menu->builder = std::move(builder);
    const std::string extension_id = menu->provider_id;
    runtime.menus.push_back(std::move(menu));
    const int32_t status = register_menu_provider(runtime, *runtime.menus.back());
    if (status != SAO_OK) {
        runtime.menus.back()->runtime = nullptr;
        runtime.menus.pop_back();
        throw_context_status("ctx.register_menu_category", status);
    }
    return extension_id;
}

emma_value parse_owned_json(int32_t status, char* owned_json, const char* method) {
    std::unique_ptr<char, decltype(&sao::plugins::loader::sao_plugins_ctx_free_string)> value(
        owned_json, &sao::plugins::loader::sao_plugins_ctx_free_string);
    if (status != SAO_OK)
        throw_context_status(method, status);
    json parsed;
    const bool parsed_ok = value == nullptr
                               ? parse_bounded_json("null", kMaximumJsonInputBytes, parsed)
                               : parse_bounded_json_c_string(value.get(), kMaximumJsonInputBytes,
                                                             parsed);
    emma_value converted = nullptr;
    if (!parsed_ok || !json_to_value(parsed, converted)) {
        throw_context_status(method, SAO_ERR_OS_CALL_FAILED);
    }
    return converted;
}

std::shared_ptr<emma_plugin_runtime::callback_record>
make_callback(emma_plugin_runtime* runtime, std::shared_ptr<callable> function) {
    auto callback = std::make_shared<emma_plugin_runtime::callback_record>();
    callback->runtime = runtime;
    callback->function = std::move(function);
    return callback;
}

uint64_t add_owned_resource(emma_plugin_runtime& runtime, emma_plugin_runtime::resource_kind kind,
                            uint32_t numeric_token = 0, std::string key = {},
                            std::shared_ptr<emma_plugin_runtime::callback_record> callback = {},
                            std::shared_ptr<void> attachment = {}) {
    std::lock_guard lock(runtime.resources_mutex);
    const uint64_t id = runtime.next_resource_id++;
    runtime.resources.push_back(
        {id, kind, numeric_token, std::move(key), std::move(callback), std::move(attachment)});
    return id;
}

bool remove_owned_resource(emma_plugin_runtime& runtime, uint64_t id) {
    std::lock_guard lock(runtime.resources_mutex);
    const auto found = std::find_if(runtime.resources.begin(), runtime.resources.end(),
                                    [id](const auto& resource) { return resource.id == id; });
    if (found == runtime.resources.end())
        return false;
    runtime.resources.erase(found);
    return true;
}

bool update_owned_resource(emma_plugin_runtime& runtime, uint64_t id, uint32_t numeric_token,
                           std::string key) {
    std::lock_guard lock(runtime.resources_mutex);
    const auto found = std::find_if(runtime.resources.begin(), runtime.resources.end(),
                                    [id](const auto& resource) { return resource.id == id; });
    if (found == runtime.resources.end())
        return false;
    found->numeric_token = numeric_token;
    found->key = std::move(key);
    return true;
}

template <typename Predicate>
bool find_owned_resource(emma_plugin_runtime& runtime, Predicate&& predicate,
                         emma_plugin_runtime::owned_resource& out_resource) {
    std::lock_guard lock(runtime.resources_mutex);
    const auto found = std::find_if(runtime.resources.begin(), runtime.resources.end(),
                                    std::forward<Predicate>(predicate));
    if (found == runtime.resources.end())
        return false;
    out_resource = *found;
    return true;
}

bool has_accepting_event_subscription(emma_plugin_runtime& runtime, std::string_view topic) {
    std::lock_guard resources_lock(runtime.resources_mutex);
    for (const auto& resource : runtime.resources) {
        if (resource.kind != emma_plugin_runtime::resource_kind::event_subscription ||
            resource.key != topic || resource.callback == nullptr) {
            continue;
        }
        std::lock_guard callback_lock(resource.callback->mutex);
        if (resource.callback->accepting && resource.callback->runtime == &runtime &&
            resource.callback->function != nullptr) {
            return true;
        }
    }
    return false;
}

void remember_callback_error(emma_plugin_runtime* runtime, const emma_error& error) noexcept {
    if (runtime == nullptr)
        return;
    try {
        runtime->last_error = error;
    } catch (...) {
    }
}

void invoke_callback(emma_plugin_runtime::callback_record* raw_callback,
                     std::vector<emma_value> arguments) noexcept {
    try {
        callback_lease lease;
        if (!lease.acquire(raw_callback, arguments))
            return;
        auto& callback = lease.value();
        auto* runtime = callback.runtime;
        if (runtime == nullptr)
            return;
        runtime_invocation_guard guard(runtime);
        if (!guard.acquired()) {
            emma_error error;
            error.kind = error_kind::runtime_error;
            error.status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            error.message = "Emma runtime reentry is busy";
            remember_callback_error(runtime, error);
            lease.defer(std::move(arguments));
            return;
        }
        std::unique_lock invocation_lock(runtime->invocation_mutex, std::try_to_lock);
        if (!invocation_lock.owns_lock()) {
            lease.defer(std::move(arguments));
            return;
        }
        if (!lease.claim())
            return;
        std::string message;
        emma_error error;
        (void)runtime->interp->call_function(callback.function, std::move(arguments), message,
                                             &error);
        if (error.kind != error_kind::none) {
            remember_callback_error(runtime, error);
        }
        if (callback.one_shot && !callback.timer_token.empty()) {
            (void)sao::plugins::loader::sao_plugins_ctx_complete_timer(
                runtime->context, callback.timer_token.c_str());
        }
    } catch (...) {
    }
}

void drain_pending_one_shots(emma_plugin_runtime& runtime) noexcept {
    try {
        std::vector<std::pair<std::shared_ptr<emma_plugin_runtime::callback_record>,
                              std::vector<emma_value>>>
            pending;
        {
            std::lock_guard resources_lock(runtime.resources_mutex);
            for (auto& resource : runtime.resources) {
                const auto& callback = resource.callback;
                if (callback == nullptr || !callback->one_shot)
                    continue;
                std::lock_guard callback_lock(callback->mutex);
                if (callback->one_shot_phase !=
                    emma_plugin_runtime::callback_record::one_shot_state::fired_pending) {
                    continue;
                }
                callback->one_shot_phase =
                    emma_plugin_runtime::callback_record::one_shot_state::armed;
                callback->accepting = true;
                pending.emplace_back(callback, std::move(callback->pending_arguments));
                callback->pending_arguments.clear();
            }
        }
        for (auto& [callback, arguments] : pending)
            invoke_callback(callback.get(), std::move(arguments));
    } catch (...) {
    }
}

void SAO_PLUGINS_CALL event_callback_bridge(const char*, const char* event_json_utf8,
                                            void* user_data) {
    try {
        emma_value event = nullptr;
        json parsed;
        const bool parsed_ok =
            event_json_utf8 == nullptr
                ? parse_bounded_json("null", kMaximumJsonInputBytes, parsed)
                : parse_bounded_json_c_string(event_json_utf8, kMaximumJsonInputBytes, parsed);
        if (!parsed_ok || !json_to_value(parsed, event))
            return;
        invoke_callback(static_cast<emma_plugin_runtime::callback_record*>(user_data),
                        {std::move(event)});
    } catch (...) {
    }
}

void SAO_PLUGINS_CALL hotkey_callback_bridge(void* user_data) {
    invoke_callback(static_cast<emma_plugin_runtime::callback_record*>(user_data), {});
}

void SAO_PLUGINS_CALL timer_callback_bridge(void* user_data) {
    invoke_callback(static_cast<emma_plugin_runtime::callback_record*>(user_data), {});
}

// ── ctx 扩展绑定: 通用转换 / 可选参数 / 同步调用 ───────────────────────────

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty())
        return {};
    // u8path() is deprecated in C++20; u8string_view construction is the
    // equivalent UTF-8 → native-format path conversion.
    return std::filesystem::path(std::u8string_view(
                reinterpret_cast<const char8_t*>(value.data()), value.size()))
        .wstring();
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    return path_utf8(std::filesystem::path(value));
}

[[noreturn]] void throw_context_message(const char* method, int32_t status,
                                        std::string message) {
    emma_error error;
    error.kind = error_kind::runtime_error;
    error.status = status;
    error.message = std::string(method) + ": " + std::move(message);
    throw emma_exception(std::move(error));
}

bool is_null_argument(const std::vector<emma_value>& arguments, size_t index) {
    return index >= arguments.size() ||
           std::holds_alternative<std::nullptr_t>(arguments[index]);
}

bool optional_bool_value(const std::vector<emma_value>& arguments, size_t index, bool fallback,
                         const char* method) {
    if (is_null_argument(arguments, index))
        return fallback;
    if (const auto* flag = std::get_if<bool>(&arguments[index]))
        return *flag;
    if (const auto* number = std::get_if<int64_t>(&arguments[index]))
        return *number != 0;
    throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
}

int64_t optional_integer_value(const std::vector<emma_value>& arguments, size_t index,
                               int64_t fallback, const char* method) {
    if (is_null_argument(arguments, index))
        return fallback;
    return require_integer(arguments, index, method);
}

std::string optional_string_value(const std::vector<emma_value>& arguments, size_t index,
                                  std::string fallback, const char* method) {
    if (is_null_argument(arguments, index))
        return fallback;
    return require_string(arguments, index, method);
}

std::shared_ptr<callable> nullable_callable(const std::vector<emma_value>& arguments,
                                            size_t index, const char* method) {
    if (is_null_argument(arguments, index))
        return nullptr;
    return require_callable(arguments, index, method);
}

// invoke_callback 的同步返回值版本: render hook / data source / 跨语言
// module facade 都需要把 callback 的返回值带回调用方。
// 嵌套路径 (本线程已经持有同一 runtime 的 invocation) 直接执行 ——
// invocation_mutex 已由外层持有; 不同 runtime / 其他线程照旧走
// callback_lease + invocation guard。
int32_t invoke_callable_result(emma_plugin_runtime::callback_record* raw_callback,
                               std::vector<emma_value> arguments, emma_value* out_value,
                               std::string* out_error) noexcept {
    if (out_value != nullptr)
        *out_value = nullptr;
    try {
        if (raw_callback == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        auto invoke = [&](emma_plugin_runtime* runtime,
                          const std::shared_ptr<callable>& function) -> int32_t {
            std::string message;
            emma_error error;
            emma_value result =
                runtime->interp->call_function(function, std::move(arguments), message, &error);
            if (error.kind != error_kind::none || !message.empty()) {
                if (error.kind == error_kind::none) {
                    error.kind = error_kind::runtime_error;
                    error.status = SAO_ERR_OS_CALL_FAILED;
                    error.message = std::move(message);
                }
                if (out_error != nullptr)
                    *out_error = error.message;
                remember_callback_error(runtime, error);
                const int32_t status = sao_plugins_emma_error_status(&error);
                return status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status;
            }
            if (out_value != nullptr)
                *out_value = std::move(result);
            return SAO_OK;
        };
        {
            // 同线程嵌套: 外层已持有 invocation_mutex, 直接调用。
            // 只拷贝 shared_ptr, 锁必须尽早释放 —— 嵌套 invoke 里可能再次
            // 接触同一 record 的 mutex。
            std::shared_ptr<callable> nested_function;
            emma_plugin_runtime* nested_runtime = nullptr;
            {
                std::lock_guard lock(raw_callback->mutex);
                if (raw_callback->runtime != nullptr && raw_callback->runtime->interp != nullptr &&
                    raw_callback->function != nullptr &&
                    g_active_runtime == raw_callback->runtime) {
                    nested_runtime = raw_callback->runtime;
                    nested_function = raw_callback->function;
                }
            }
            if (nested_runtime != nullptr) {
                if (!nested_runtime->callbacks_accepting.load(std::memory_order_acquire))
                    return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
                return invoke(nested_runtime, nested_function);
            }
        }
        callback_lease lease;
        std::vector<emma_value> pending = arguments;
        if (!lease.acquire(raw_callback, pending))
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* runtime = lease.value().runtime;
        if (runtime == nullptr || runtime->interp == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        runtime_invocation_guard guard(runtime);
        if (!guard.acquired()) {
            if (out_error != nullptr)
                *out_error = "Emma runtime reentry is busy";
            emma_error error;
            error.kind = error_kind::runtime_error;
            error.status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            error.message = "Emma runtime reentry is busy";
            remember_callback_error(runtime, error);
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        std::unique_lock invocation_lock(runtime->invocation_mutex, std::try_to_lock);
        if (!invocation_lock.owns_lock())
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        if (!lease.claim())
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto function = lease.value().function;
        if (function == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        return invoke(runtime, function);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── provider 回调桥 (render hook / data source / compositor input) ─────────

int32_t render_hook_bridge(const char* surface_utf8, const char* payload_json_utf8,
                           char** out_spec_json_utf8, void* user_data) noexcept {
    if (out_spec_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_spec_json_utf8 = nullptr;
    try {
        json parsed;
        const bool parsed_ok =
            payload_json_utf8 == nullptr
                ? parse_bounded_json("null", kMaximumJsonInputBytes, parsed)
                : parse_bounded_json_c_string(payload_json_utf8, kMaximumJsonInputBytes, parsed);
        emma_value payload = nullptr;
        if (!parsed_ok || !json_to_value(parsed, payload))
            return SAO_ERR_INVALID_ARGUMENT;
        emma_value result = nullptr;
        std::string error;
        const int32_t status = invoke_callable_result(
            static_cast<emma_plugin_runtime::callback_record*>(user_data),
            {std::string(surface_utf8 == nullptr ? "" : surface_utf8), std::move(payload)},
            &result, &error);
        if (status != SAO_OK)
            return status;
        std::string serialized;
        std::string message;
        if (!detail::serialize_emma_value(result, serialized, message))
            return SAO_ERR_INVALID_ARGUMENT;
        // sao_plugins_ctx_free_string 使用 delete[] —— 必须 new char[]。
        auto output = std::make_unique<char[]>(serialized.size() + 1);
        std::memcpy(output.get(), serialized.data(), serialized.size());
        output[serialized.size()] = '\0';
        *out_spec_json_utf8 = output.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// data source 的 start/stop 在 ctx teardown 时仍会被调用 (loader 逐条停
// data_sources); 插件已卸载时 bundle->runtime/stop 为 nullptr, 空转返回 OK
// 保证 teardown 链完整。bundle 本身由 process-lifetime 保留列表持有 (见
// retain_lifecycle_bundle), 直到进程退出 —— ctx 没有 data_source 反注册 ABI。
// start/stop 返回值约定: int → 原样状态码; bool true/nil → OK; bool false → 失败。
int32_t data_source_result_status(emma_plugin_runtime::callback_record* record) noexcept {
    emma_value result = nullptr;
    const int32_t status = invoke_callable_result(record, {}, &result, nullptr);
    if (status != SAO_OK)
        return status;
    if (const auto* code = std::get_if<int64_t>(&result))
        return static_cast<int32_t>(*code);
    if (const auto* flag = std::get_if<bool>(&result))
        return *flag ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
}

int32_t data_source_start_bridge(void* user_data) noexcept {
    auto* bundle = static_cast<emma_data_source_bundle*>(user_data);
    if (bundle == nullptr || bundle->runtime == nullptr || bundle->start == nullptr)
        return SAO_OK;
    return data_source_result_status(bundle->start.get());
}

int32_t data_source_stop_bridge(void* user_data) noexcept {
    auto* bundle = static_cast<emma_data_source_bundle*>(user_data);
    if (bundle == nullptr || bundle->runtime == nullptr || bundle->stop == nullptr)
        return SAO_OK;
    return data_source_result_status(bundle->stop.get());
}

void compositor_cursor_pos_bridge(float x, float y, void* user_data) noexcept {
    auto* bundle = static_cast<emma_compositor_input_bundle*>(user_data);
    if (bundle == nullptr || bundle->cursor_pos == nullptr)
        return;
    invoke_callback(bundle->cursor_pos.get(),
                    {static_cast<double>(x), static_cast<double>(y)});
}

void compositor_mouse_button_bridge(uint32_t button, bool pressed, void* user_data) noexcept {
    auto* bundle = static_cast<emma_compositor_input_bundle*>(user_data);
    if (bundle == nullptr || bundle->mouse_button == nullptr)
        return;
    invoke_callback(bundle->mouse_button.get(),
                    {static_cast<int64_t>(button), pressed});
}

void compositor_cursor_leave_bridge(void* user_data) noexcept {
    auto* bundle = static_cast<emma_compositor_input_bundle*>(user_data);
    if (bundle == nullptr || bundle->cursor_leave == nullptr)
        return;
    invoke_callback(bundle->cursor_leave.get(), {});
}

void compositor_scroll_bridge(float dx, float dy, void* user_data) noexcept {
    auto* bundle = static_cast<emma_compositor_input_bundle*>(user_data);
    if (bundle == nullptr || bundle->scroll == nullptr)
        return;
    invoke_callback(bundle->scroll.get(),
                    {static_cast<double>(dx), static_cast<double>(dy)});
}

// data source bundle 在插件卸载后必须继续存活到所属 ctx teardown —
// loader 会在 teardown 阶段调用 stop(user_data)。这里没有 data_source
// 反注册 ABI, bundle 记录随资源销毁, 但其内存由本表保留到进程结束
// (按注册次数有界)。bundle->runtime==nullptr 时 bridge 安全空转。
std::vector<std::shared_ptr<emma_data_source_bundle>>& retained_data_source_bundles() {
    static std::vector<std::shared_ptr<emma_data_source_bundle>> bundles;
    return bundles;
}

std::mutex& retained_data_source_bundles_mutex() {
    static std::mutex mutex;
    return mutex;
}

void retain_data_source_bundle(const std::shared_ptr<emma_data_source_bundle>& bundle) {
    std::lock_guard lock(retained_data_source_bundles_mutex());
    retained_data_source_bundles().push_back(bundle);
}

// ── script_value ↔ emma_value 跨语言值转换 ────────────────────────────────

using script_value_ptr = sao::plugins::script_ctx::script_value_ptr;

script_value_ptr emma_value_to_script(const emma_value& value, emma_plugin_runtime* runtime);
emma_value script_value_to_emma(const script_value_ptr& value, emma_plugin_runtime* runtime);
script_value_ptr emma_to_script_module(const emma_value& value, emma_plugin_handle_t plugin);
emma_value script_to_emma_module(const script_value_ptr& value, emma_plugin_handle_t plugin);

// 卸载失败 (active ops / BUSY) 的 module handle 记到 zombie 表, 下一次
// load_module / unload 路径重试, 避免 tombstone 泄漏。
std::mutex& module_zombie_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<emma_plugin_handle_t>& module_zombies() {
    static std::vector<emma_plugin_handle_t> zombies;
    return zombies;
}

void zombie_module_handle(emma_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return;
    std::lock_guard lock(module_zombie_mutex());
    module_zombies().push_back(plugin);
}

void drain_module_zombies() noexcept {
    std::vector<emma_plugin_handle_t> pending;
    {
        std::lock_guard lock(module_zombie_mutex());
        pending.swap(module_zombies());
    }
    for (auto* plugin : pending) {
        if (plugin == nullptr)
            continue;
        if (sao_plugins_emma_unload_script(plugin) != SAO_OK)
            zombie_module_handle(plugin);
    }
}

// module 成员访问: 用 direct_operation_lease + invocation_mutex 串行化,
// 刻意不走 runtime_invocation_guard —— guard 只允许每线程一个活动
// runtime, 会拒绝从别的 emma 插件 invocation 里发起的合法跨 runtime
// module 调用 (module facade 是跨 runtime 设计的)。lease 自身的
// active_operations 计数保证 unload 会等到访问结束。
template <typename Callback>
int32_t with_module_runtime(emma_plugin_handle_t plugin, std::string* out_error,
                            Callback&& callback) noexcept {
    try {
        direct_operation_lease lease;
        const int32_t status = acquire_direct_plugin(plugin, lease);
        if (status != SAO_OK) {
            if (out_error != nullptr)
                *out_error = "emma module handle is invalid or busy";
            return status;
        }
        auto& runtime = lease.runtime();
        std::lock_guard invocation_lock(runtime.invocation_mutex);
        return callback(runtime);
    } catch (...) {
        if (out_error != nullptr)
            *out_error = "emma module invocation failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

script_value_ptr emma_to_script_module(const emma_value& value, emma_plugin_handle_t plugin) {
    namespace sc = sao::plugins::script_ctx;
    if (const auto* flag = std::get_if<bool>(&value))
        return sc::script_value::make_boolean(*flag);
    if (const auto* integer = std::get_if<int64_t>(&value))
        return sc::script_value::make_integer(*integer);
    if (const auto* number = std::get_if<double>(&value))
        return sc::script_value::make_number(*number);
    if (const auto* text = std::get_if<std::string>(&value))
        return sc::script_value::make_string(*text);
    if (const auto* list = std::get_if<std::shared_ptr<emma_list>>(&value)) {
        std::vector<script_value_ptr> items;
        if (*list != nullptr) {
            items.reserve((*list)->items.size());
            for (const auto& item : (*list)->items)
                items.push_back(emma_to_script_module(item, plugin));
        }
        return sc::script_value::make_list(std::move(items));
    }
    if (const auto* dict = std::get_if<std::shared_ptr<emma_dict>>(&value)) {
        std::vector<std::pair<std::string, script_value_ptr>> object;
        if (*dict != nullptr) {
            object.reserve((*dict)->items.size());
            for (const auto& [key, item] : (*dict)->items)
                object.emplace_back(key, emma_to_script_module(item, plugin));
        }
        return sc::script_value::make_map(std::move(object));
    }
    if (const auto* function = std::get_if<std::shared_ptr<callable>>(&value)) {
        auto target = function != nullptr ? *function : nullptr;
        return sc::script_value::make_function(
            [plugin, target](const std::vector<script_value_ptr>& args,
                             script_value_ptr* out_value, std::string* out_error) -> int32_t {
                if (plugin == nullptr || target == nullptr) {
                    if (out_error != nullptr)
                        *out_error = "emma module callable is not bound";
                    return SAO_ERR_HANDLE_INVALID;
                }
                return with_module_runtime(
                    plugin, out_error, [&](emma_plugin_runtime& runtime) -> int32_t {
                        if (runtime.interp == nullptr)
                            return SAO_ERR_HANDLE_INVALID;
                        std::vector<emma_value> emma_args;
                        emma_args.reserve(args.size());
                        for (const auto& arg : args)
                            emma_args.push_back(script_to_emma_module(arg, plugin));
                        std::string message;
                        emma_error error;
                        emma_value result = runtime.interp->call_function(
                            target, std::move(emma_args), message, &error);
                        if (error.kind != error_kind::none || !message.empty()) {
                            if (error.kind == error_kind::none) {
                                error.kind = error_kind::runtime_error;
                                error.status = SAO_ERR_OS_CALL_FAILED;
                                error.message = std::move(message);
                            }
                            if (out_error != nullptr)
                                *out_error = error.message;
                            const int32_t status = sao_plugins_emma_error_status(&error);
                            return status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status;
                        }
                        if (out_value != nullptr)
                            *out_value = emma_to_script_module(result, plugin);
                        return SAO_OK;
                    });
            });
    }
    return sc::script_value::null_value();
}

emma_value script_to_emma_module(const script_value_ptr& value, emma_plugin_handle_t plugin) {
    namespace sc = sao::plugins::script_ctx;
    if (value == nullptr)
        return emma_value(nullptr);
    switch (value->k) {
    case sc::script_value::kind::boolean:
        return emma_value(value->boolean);
    case sc::script_value::kind::integer:
        return emma_value(value->integer);
    case sc::script_value::kind::number:
        return emma_value(value->number);
    case sc::script_value::kind::string:
    case sc::script_value::kind::bytes:
        return emma_value(value->text);
    case sc::script_value::kind::list: {
        auto list = std::make_shared<emma_list>();
        list->items.reserve(value->items.size());
        for (const auto& item : value->items)
            list->items.push_back(script_to_emma_module(item, plugin));
        return emma_value(std::move(list));
    }
    case sc::script_value::kind::map: {
        auto dict = std::make_shared<emma_dict>();
        for (const auto& [key, item] : value->object)
            dict->items.emplace(key, script_to_emma_module(item, plugin));
        return emma_value(std::move(dict));
    }
    case sc::script_value::kind::function: {
        auto remote = value;
        auto bridge = make_host_callable(
            "module.remote", [plugin, remote](std::vector<emma_value> arguments) -> emma_value {
                std::vector<script_value_ptr> script_args;
                script_args.reserve(arguments.size());
                for (const auto& argument : arguments)
                    script_args.push_back(emma_to_script_module(argument, plugin));
                script_value_ptr result;
                std::string error;
                const int32_t status = remote->call(script_args, &result, &error);
                if (status != SAO_OK)
                    throw_context_message("module.remote", status,
                                          error.empty() ? "remote call failed" : error);
                return script_to_emma_module(result, plugin);
            });
        return emma_value(std::move(bridge));
    }
    default:
        return emma_value(nullptr);
    }
}

script_value_ptr emma_value_to_script(const emma_value& value, emma_plugin_runtime* runtime) {
    namespace sc = sao::plugins::script_ctx;
    if (const auto* flag = std::get_if<bool>(&value))
        return sc::script_value::make_boolean(*flag);
    if (const auto* integer = std::get_if<int64_t>(&value))
        return sc::script_value::make_integer(*integer);
    if (const auto* number = std::get_if<double>(&value))
        return sc::script_value::make_number(*number);
    if (const auto* text = std::get_if<std::string>(&value))
        return sc::script_value::make_string(*text);
    if (const auto* list = std::get_if<std::shared_ptr<emma_list>>(&value)) {
        std::vector<script_value_ptr> items;
        if (*list != nullptr) {
            items.reserve((*list)->items.size());
            for (const auto& item : (*list)->items)
                items.push_back(emma_value_to_script(item, runtime));
        }
        return sc::script_value::make_list(std::move(items));
    }
    if (const auto* dict = std::get_if<std::shared_ptr<emma_dict>>(&value)) {
        std::vector<std::pair<std::string, script_value_ptr>> object;
        if (*dict != nullptr) {
            object.reserve((*dict)->items.size());
            for (const auto& [key, item] : (*dict)->items)
                object.emplace_back(key, emma_value_to_script(item, runtime));
        }
        return sc::script_value::make_map(std::move(object));
    }
    if (const auto* function = std::get_if<std::shared_ptr<callable>>(&value)) {
        auto record = make_callback(runtime, function != nullptr ? *function : nullptr);
        return sc::script_value::make_function(
            [record](const std::vector<script_value_ptr>& args, script_value_ptr* out_value,
                     std::string* out_error) -> int32_t {
                std::vector<emma_value> emma_args;
                emma_args.reserve(args.size());
                for (const auto& arg : args)
                    emma_args.push_back(script_to_emma_module(arg, nullptr));
                emma_value result = nullptr;
                const int32_t status =
                    invoke_callable_result(record.get(), std::move(emma_args), &result, out_error);
                if (status != SAO_OK)
                    return status;
                if (out_value != nullptr)
                    *out_value = emma_to_script_module(result, nullptr);
                return SAO_OK;
            });
    }
    return sc::script_value::null_value();
}

emma_value script_value_to_emma(const script_value_ptr& value, emma_plugin_runtime* runtime) {
    namespace sc = sao::plugins::script_ctx;
    if (value == nullptr)
        return emma_value(nullptr);
    switch (value->k) {
    case sc::script_value::kind::boolean:
        return emma_value(value->boolean);
    case sc::script_value::kind::integer:
        return emma_value(value->integer);
    case sc::script_value::kind::number:
        return emma_value(value->number);
    case sc::script_value::kind::string:
    case sc::script_value::kind::bytes:
        return emma_value(value->text);
    case sc::script_value::kind::list: {
        auto list = std::make_shared<emma_list>();
        list->items.reserve(value->items.size());
        for (const auto& item : value->items)
            list->items.push_back(script_value_to_emma(item, runtime));
        return emma_value(std::move(list));
    }
    case sc::script_value::kind::map: {
        auto dict = std::make_shared<emma_dict>();
        for (const auto& [key, item] : value->object)
            dict->items.emplace(key, script_value_to_emma(item, runtime));
        return emma_value(std::move(dict));
    }
    case sc::script_value::kind::function: {
        auto remote = value;
        auto bridge = make_host_callable(
            "script.function", [runtime, remote](std::vector<emma_value> arguments) -> emma_value {
                std::vector<script_value_ptr> script_args;
                script_args.reserve(arguments.size());
                for (const auto& argument : arguments)
                    script_args.push_back(emma_value_to_script(argument, runtime));
                script_value_ptr result;
                std::string error;
                const int32_t status = remote->call(script_args, &result, &error);
                if (status != SAO_OK)
                    throw_context_message("script.function", status,
                                          error.empty() ? "remote call failed" : error);
                return script_value_to_emma(result, runtime);
            });
        return emma_value(std::move(bridge));
    }
    default:
        return emma_value(nullptr);
    }
}

// ── runtime_bridge provider: emma .emma module facade ─────────────────────

class emma_host_script_module final : public sao::plugins::script_ctx::script_module {
  public:
    emma_host_script_module(emma_plugin_handle_t plugin, std::string id,
                            std::vector<std::string> names)
        : plugin_(plugin), id_(std::move(id)), names_(std::move(names)) {}

    ~emma_host_script_module() override {
        if (plugin_ != nullptr) {
            if (sao_plugins_emma_unload_script(plugin_) != SAO_OK)
                zombie_module_handle(plugin_);
            plugin_ = nullptr;
        }
    }

    const std::string& module_id() const noexcept override {
        return id_;
    }

    std::vector<std::string> member_names() const override {
        return names_;
    }

    int32_t get(const std::string& name, script_value_ptr* out_value,
                std::string* out_error) override {
        if (out_error != nullptr)
            out_error->clear();
        if (out_value != nullptr)
            *out_value = nullptr;
        return with_module_runtime(plugin_, out_error,
                                   [&](emma_plugin_runtime& runtime) -> int32_t {
                                       if (runtime.interp == nullptr)
                                           return SAO_ERR_HANDLE_INVALID;
                                       const emma_value member = runtime.interp->get_global(name);
                                       if (out_value != nullptr)
                                           *out_value = emma_to_script_module(member, plugin_);
                                       return SAO_OK;
                                   });
    }

    int32_t call(const std::string& name, const std::vector<script_value_ptr>& args,
                 script_value_ptr* out_value, std::string* out_error) override {
        if (out_error != nullptr)
            out_error->clear();
        if (out_value != nullptr)
            *out_value = nullptr;
        return with_module_runtime(
            plugin_, out_error, [&](emma_plugin_runtime& runtime) -> int32_t {
                if (runtime.interp == nullptr)
                    return SAO_ERR_HANDLE_INVALID;
                const emma_value member = runtime.interp->get_global(name);
                const auto* function = std::get_if<std::shared_ptr<callable>>(&member);
                if (function == nullptr || *function == nullptr) {
                    if (out_error != nullptr)
                        *out_error = "member '" + name + "' is not callable";
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                std::vector<emma_value> emma_args;
                emma_args.reserve(args.size());
                for (const auto& arg : args)
                    emma_args.push_back(script_to_emma_module(arg, plugin_));
                std::string message;
                emma_error error;
                emma_value result =
                    runtime.interp->call_function(*function, std::move(emma_args), message, &error);
                if (error.kind != error_kind::none || !message.empty()) {
                    if (error.kind == error_kind::none) {
                        error.kind = error_kind::runtime_error;
                        error.status = SAO_ERR_OS_CALL_FAILED;
                        error.message = std::move(message);
                    }
                    if (out_error != nullptr)
                        *out_error = error.message;
                    const int32_t status = sao_plugins_emma_error_status(&error);
                    return status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status;
                }
                if (out_value != nullptr)
                    *out_value = emma_to_script_module(result, plugin_);
                return SAO_OK;
            });
    }

  private:
    emma_plugin_handle_t plugin_ = nullptr;
    std::string id_;
    std::vector<std::string> names_;
};

bool emma_bridge_probe(sao::plugins::loader::plugin_context_t*, const wchar_t*, std::string&,
                       void*) noexcept {
    return true;
}

int32_t emma_bridge_load_module(sao::plugins::loader::plugin_context_t* ctx,
                                const wchar_t* abs_path, const std::string& logical_name,
                                std::shared_ptr<sao::plugins::script_ctx::script_module>* out_module,
                                std::string* out_error, void*) noexcept {
    try {
        if (out_error != nullptr)
            out_error->clear();
        if (out_module == nullptr || ctx == nullptr || abs_path == nullptr) {
            if (out_error != nullptr)
                *out_error = "invalid load_local module arguments";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        *out_module = nullptr;
        drain_module_zombies();
        const wchar_t* root_w = sao::plugins::loader::sao_plugins_ctx_path(ctx);
        if (root_w == nullptr || root_w[0] == L'\0') {
            if (out_error != nullptr)
                *out_error = "canonical plugin root unavailable";
            return SAO_ERR_HANDLE_INVALID;
        }
        const std::filesystem::path root(root_w);
        const std::filesystem::path absolute(abs_path);
        const std::filesystem::path relative = absolute.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") {
            if (out_error != nullptr)
                *out_error = "module path escapes plugin root";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto relative_u8 = relative.generic_u8string();
        const std::string relative_utf8(relative_u8.begin(), relative_u8.end());
        const char* plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(ctx);
        emma_plugin_handle_t plugin = nullptr;
        emma_error load_error;
        const int32_t status =
            sao_plugins_emma_load_script_ex(root_w, relative_utf8.c_str(),
                                            plugin_id == nullptr ? "" : plugin_id, ctx, &plugin,
                                            &load_error);
        if (status != SAO_OK) {
            if (out_error != nullptr) {
                char* formatted = nullptr;
                if (sao_plugins_emma_error_format(&load_error, &formatted) == SAO_OK &&
                    formatted != nullptr) {
                    *out_error = formatted;
                    sao_plugins_emma_free_string(formatted);
                } else {
                    *out_error = "emma module load failed";
                }
            }
            if (plugin != nullptr) {
                if (sao_plugins_emma_unload_script(plugin) != SAO_OK)
                    zombie_module_handle(plugin);
            }
            return status;
        }
        if (plugin == nullptr) {
            if (out_error != nullptr)
                *out_error = "emma module load returned no handle";
            return SAO_ERR_OS_CALL_FAILED;
        }

        std::vector<std::string> names;
        std::string capture_error;
        const int32_t capture_status =
            with_module_runtime(plugin, &capture_error, [&](emma_plugin_runtime& runtime) {
                if (runtime.interp == nullptr)
                    return SAO_ERR_HANDLE_INVALID;
                std::unordered_set<std::string> excluded{"ctx", "log"};
                const char** builtins = nullptr;
                size_t builtin_count = 0;
                if (sao_plugins_emma_list_builtins(runtime.interp.get(), &builtins,
                                                 &builtin_count) == SAO_OK) {
                    for (size_t index = 0; index < builtin_count; ++index) {
                        if (builtins[index] != nullptr)
                            excluded.emplace(builtins[index]);
                    }
                    sao_plugins_emma_free_names(builtins, builtin_count);
                }
                for (const auto& name : runtime.interp->global_names()) {
                    if (!excluded.count(name))
                        names.push_back(name);
                }
                return SAO_OK;
            });
        if (capture_status != SAO_OK) {
            if (out_error != nullptr)
                *out_error = capture_error.empty() ? "module member enumeration failed"
                                                 : capture_error;
            if (sao_plugins_emma_unload_script(plugin) != SAO_OK)
                zombie_module_handle(plugin);
            return capture_status;
        }

        *out_module = std::make_shared<emma_host_script_module>(plugin, logical_name,
                                                                std::move(names));
        return SAO_OK;
    } catch (...) {
        if (out_error != nullptr)
            *out_error = "emma module load failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

const char* const kEmmaBridgeExtensions[] = {"emma", nullptr};

sao::plugins::script_ctx::script_engine_ops g_emma_bridge_ops{
    "emma",                    // engine_name_utf8
    30,                        // priority — pymini=10 之后, python_host=90 之前
    kEmmaBridgeExtensions,     // extensions_utf8
    &emma_bridge_probe,        // probe — .emma 归 emma host, 恒 true
    &emma_bridge_load_module,  // load_module
    nullptr,                   // user_data
};

std::once_flag g_emma_bridge_once;

void ensure_emma_runtime_bridge() noexcept {
    std::call_once(g_emma_bridge_once,
                   [] { (void)sao::plugins::script_ctx::runtime_bridge_register(&g_emma_bridge_ops); });
}

// ── 反射引擎面 (binding_engine.h 契约) ─────────────────────────────────
//
// ctx.engine 为 catalog 每一条目挂一个命名 callable: "mem.read_u64" →
// ctx.engine.mem_read_u64(...)。位置实参按 desc.arg_names 顺序映射; 单个
// dict 实参且全部键落在 arg_names ∪ {"callback_channel"} 内时按 kwargs
// 逐键映射 (与 ctx.ui.* 的单 dict 约定一致), 否则整个 dict 视作第一个
// 位置实参。位置形表之外允许多出一个 string 实参 → envelope 级
// "callback_channel"。
//
// dispatch: sao_plugins_sdk_context_dispatch(ctx, method_engine_call,
// request); 结果 envelope {"status","result"} —— 非零 status 按本文件惯例
// throw_context_status, 否则返回解码 result。ctx.engine_call 为 raw
// passthrough: 返回完整 {status,result} dict 由脚本自行判定 status。
//
// SaoSdkContext 归本 host 持有 (loader 的 plugin_context_t 不是
// SaoSdkContext, 不能互转; 与 angel/csharp/csmini/python host 同模式):
// sao_sdk_context_create + sao_sdk_context_bind_platform_services。创建
// 失败只记 runtime.sdk_init_status, 不阻断脚本加载 —— engine.* 调用照旧
// 报错 (fail closed)。

void init_engine_sdk_context(emma_plugin_runtime& runtime) noexcept {
    if (runtime.sdk_context != nullptr)
        return;
    try {
        const std::string base_dir = path_utf8(runtime.plugin_root);
        SaoSdkContext* created = nullptr;
        int32_t status = sao_sdk_context_create(base_dir.c_str(),
                                                runtime.plugin_id.c_str(), &created);
        if (status != SAO_SDK_OK || created == nullptr) {
            runtime.sdk_init_status =
                status != SAO_SDK_OK ? status : SAO_ERR_OS_CALL_FAILED;
            return;
        }
        status = sao_sdk_context_bind_platform_services(created);
        if (status != SAO_SDK_OK) {
            runtime.sdk_init_status = status;
            // destroy 内部走 try_destroy, 失败进 SDK 的 quarantine 路径,
            // 不向外传播状态。
            sao_sdk_context_destroy(created);
            return;
        }
        runtime.sdk_context = created;
        runtime.sdk_init_status = SAO_OK;
    } catch (...) {
        runtime.sdk_init_status = SAO_ERR_OS_CALL_FAILED;
    }
}

// engine.on(channel, cb) 的 channel 路由 trampoline —— 每次 engine 调用的
// request.engine_callback 都指向它; user_data 是 heap 锚定的 runtime,
// channel_utf8 命中 engine_callbacks 表后 decode payload →
// invoke_callback(channel, payload)。
void SAO_PLUGINS_CALL engine_channel_bridge(const char* channel_utf8,
                                            const uint8_t* payload_json_utf8,
                                            size_t payload_size, void* user_data) {
    try {
        auto* runtime = static_cast<emma_plugin_runtime*>(user_data);
        if (runtime == nullptr)
            return;
        std::shared_ptr<emma_plugin_runtime::callback_record> record;
        {
            std::lock_guard lock(runtime->engine_callbacks_mutex);
            const auto found =
                runtime->engine_callbacks.find(channel_utf8 == nullptr ? "" : channel_utf8);
            if (found != runtime->engine_callbacks.end())
                record = found->second;
        }
        if (record == nullptr)
            return;
        json parsed;
        const bool parsed_ok =
            payload_json_utf8 == nullptr
                ? parse_bounded_json("null", kMaximumJsonInputBytes, parsed)
                : parse_bounded_json(
                      std::string_view(reinterpret_cast<const char*>(payload_json_utf8),
                                       payload_size),
                      kMaximumJsonInputBytes, parsed);
        emma_value payload = nullptr;
        if (!parsed_ok || !json_to_value(parsed, payload))
            return;
        invoke_callback(record.get(), {std::string(channel_utf8 == nullptr ? "" : channel_utf8),
                                       std::move(payload)});
    } catch (...) {
    }
}

// catalog "组.函数" → engine 成员名 ('.'→'_')。
std::string engine_member_name(const char* catalog_name) {
    std::string member;
    if (catalog_name == nullptr)
        return member;
    while (*catalog_name != '\0') {
        const char character = *catalog_name++;
        member.push_back(character == '.' ? '_' : character);
    }
    return member;
}

// 引擎面通用 dispatch: 序列化请求 → 单次 sao_plugins_sdk_context_dispatch
// → 解析 {status,result} envelope。out 缓冲按协议上限一次性分配 —
// write_result 的 BUFFER_TOO_SMALL 在 invoke 实跑后才返回, 用
// out_required 二遍探测会重复执行引擎副作用, 故不二遍调用。
json engine_dispatch_envelope(emma_plugin_runtime* runtime,
                              sao::plugins::sdk_binding::sdk_method_id method,
                              const json& request_args, const char* label) {
    namespace sdk_binding = sao::plugins::sdk_binding;
    if (runtime == nullptr)
        throw_context_status(label, SAO_ERR_INVALID_ARGUMENT);
    SaoSdkContext* sdk = runtime->sdk_context;
    if (sdk == nullptr) {
        throw_context_status(label, runtime->sdk_init_status != SAO_OK
                                        ? runtime->sdk_init_status
                                        : SAO_ERR_NOT_INITIALIZED);
    }
    std::string serialized;
    std::string message;
    if (!detail::serialize_json(request_args, serialized, message))
        throw_context_message(label, SAO_ERR_INVALID_ARGUMENT, message);
    std::lock_guard dispatch_lock(runtime->engine_dispatch_mutex);
    if (runtime->engine_result_buffer.size() < sdk_binding::kMaximumBindingJsonBytes + 1U)
        runtime->engine_result_buffer.assign(sdk_binding::kMaximumBindingJsonBytes + 1U, '\0');
    sdk_binding::sdk_context_call_request request{};
    request.args_json_utf8 = serialized.data();
    request.args_size = serialized.size();
    request.engine_callback = &engine_channel_bridge;
    request.callback_user_data = runtime;
    request.out_result_json_utf8 = runtime->engine_result_buffer.data();
    request.out_capacity = runtime->engine_result_buffer.size();
    size_t required = 0;
    request.out_required = &required;
    // scratch 是复用缓冲: 预置空串, 对 SAO_OK 但无写出的路径不误读上轮的
    // 残留 envelope。
    runtime->engine_result_buffer[0] = '\0';
    const int32_t status = sdk_binding::sao_plugins_sdk_context_dispatch(sdk, method, &request);
    if (status != SAO_OK)
        throw_context_status(label, status);
    json parsed;
    if (!parse_bounded_json_c_string(runtime->engine_result_buffer.data(),
                                     kMaximumJsonInputBytes, parsed)) {
        throw_context_status(label, SAO_ERR_OS_CALL_FAILED);
    }
    return parsed;
}

// {status,result} envelope → result emma_value; 非零 status 按惯例抛。
// 不带 status 字段的 defensively 按整包 result 返回。
emma_value engine_call_result(const json& envelope, const char* label) {
    if (envelope.is_object() && envelope.contains("status")) {
        const auto& status_node = envelope["status"];
        if (!status_node.is_number())
            throw_context_status(label, SAO_ERR_OS_CALL_FAILED);
        const int64_t status = status_node.get<int64_t>();
        if (status != SAO_OK)
            throw_context_status(label, static_cast<int32_t>(status));
        const auto found = envelope.find("result");
        emma_value converted = nullptr;
        if (!json_to_value(found == envelope.end() ? json(nullptr) : *found, converted))
            throw_context_status(label, SAO_ERR_OS_CALL_FAILED);
        return converted;
    }
    emma_value converted = nullptr;
    if (!json_to_value(envelope, converted))
        throw_context_status(label, SAO_ERR_OS_CALL_FAILED);
    return converted;
}

// 组装 {"name","args",["callback_channel"]} → method_engine_call。
emma_value engine_named_invoke(emma_plugin_runtime* runtime, const char* catalog_name,
                               json engine_args, std::string callback_channel,
                               const char* label) {
    json request_args = json::object();
    request_args["name"] = catalog_name;
    request_args["args"] = std::move(engine_args);
    if (!callback_channel.empty())
        request_args["callback_channel"] = std::move(callback_channel);
    const json envelope =
        engine_dispatch_envelope(runtime,
                                 sao::plugins::sdk_binding::sdk_method_id::method_engine_call,
                                 request_args, label);
    return engine_call_result(envelope, label);
}

// catalog 条目命名 callable: 位置实参按 arg_names 顺序映射; kwargs 形
// (单 dict 且键 ⊆ arg_names ∪ {callback_channel}) 逐键映射。
emma_value engine_named_call(emma_plugin_runtime* runtime,
                             const sao::plugins::sdk_binding::sdk_engine_function_desc* desc,
                             const char* label, std::vector<emma_value> arguments) {
    if (desc == nullptr || desc->name == nullptr)
        throw_context_status(label, SAO_ERR_INVALID_ARGUMENT);
    json engine_args = json::object();
    std::string callback_channel;
    const char* const* names = desc->arg_names;
    const size_t name_count = desc->arg_count;
    const auto is_formal_name = [&](const std::string& key) {
        for (size_t index = 0; index < name_count; ++index) {
            if (names[index] != nullptr && key == names[index])
                return true;
        }
        return false;
    };
    if (arguments.size() == 1 &&
        std::holds_alternative<std::shared_ptr<emma_dict>>(arguments[0])) {
        const auto& dictionary = std::get<std::shared_ptr<emma_dict>>(arguments[0]);
        const bool kwargs =
            dictionary == nullptr ||
            std::all_of(dictionary->items.begin(), dictionary->items.end(),
                        [&](const auto& entry) {
                            return entry.first == "callback_channel" ||
                                   is_formal_name(entry.first);
                        });
        if (kwargs) {
            if (dictionary != nullptr) {
                for (const auto& entry : dictionary->items) {
                    if (entry.first == "callback_channel") {
                        const auto* text = std::get_if<std::string>(&entry.second);
                        if (text == nullptr)
                            throw_context_status(label, SAO_ERR_INVALID_ARGUMENT);
                        callback_channel = *text;
                        continue;
                    }
                    json converted;
                    if (!value_to_json(entry.second, converted))
                        throw_context_status(label, SAO_ERR_INVALID_ARGUMENT);
                    engine_args[entry.first] = std::move(converted);
                }
            }
            return engine_named_invoke(runtime, desc->name, std::move(engine_args),
                                       std::move(callback_channel), label);
        }
    }
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (index < name_count && names != nullptr && names[index] != nullptr) {
            json converted;
            if (!value_to_json(arguments[index], converted))
                throw_context_status(label, SAO_ERR_INVALID_ARGUMENT);
            engine_args[names[index]] = std::move(converted);
            continue;
        }
        // 位置形表之外唯一允许的额外实参: 一个 string → callback_channel。
        if (index == name_count && callback_channel.empty() &&
            std::holds_alternative<std::string>(arguments[index])) {
            callback_channel = std::get<std::string>(arguments[index]);
            continue;
        }
        throw_context_status(label, SAO_ERR_INVALID_ARGUMENT);
    }
    return engine_named_invoke(runtime, desc->name, std::move(engine_args),
                               std::move(callback_channel), label);
}

// ctx.engine.list() → method_engine_list; result 为目录数组。
emma_value engine_list_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.engine.list";
    if (!arguments.empty())
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    const json envelope =
        engine_dispatch_envelope(runtime,
                                 sao::plugins::sdk_binding::sdk_method_id::method_engine_list,
                                 json::object(), method);
    return engine_call_result(envelope, method);
}

// ctx.engine.on(channel, cb): channel → record 的替换式注册; cb 收到
// (channel, payload)。第二个实参传 nil → 注销该 channel。
emma_value engine_on_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.engine.on";
    const std::string channel = require_string(arguments, 0, method);
    if (channel.empty())
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    if (is_null_argument(arguments, 1)) {
        std::shared_ptr<emma_plugin_runtime::callback_record> removed;
        {
            std::lock_guard lock(runtime->engine_callbacks_mutex);
            const auto found = runtime->engine_callbacks.find(channel);
            if (found != runtime->engine_callbacks.end()) {
                removed = found->second;
                runtime->engine_callbacks.erase(found);
            }
        }
        // 移出表后 quiesce (等在飞回调落地); 同线程在飞执行到本调用时
        // quiesce 报 BUSY —— 记录已停止接收, 转 tombstone 续命收尾。
        if (removed != nullptr && quiesce_callback(removed) != SAO_OK) {
            std::lock_guard lock(runtime->engine_callbacks_mutex);
            runtime->engine_callbacks_retired.push_back(std::move(removed));
        } else if (removed != nullptr) {
            retire_callback(removed);
        }
        return emma_value(true);
    }
    auto function = require_callable(arguments, 1, method);
    auto record = make_callback(runtime, std::move(function));
    std::shared_ptr<emma_plugin_runtime::callback_record> previous;
    {
        std::lock_guard lock(runtime->engine_callbacks_mutex);
        const auto found = runtime->engine_callbacks.find(channel);
        if (found == runtime->engine_callbacks.end()) {
            runtime->engine_callbacks.emplace(channel, record);
        } else {
            previous = found->second;
            found->second = record;
        }
    }
    // replace-on-re-register: 旧记录已下表, quiesce 后退役; quiesce 失败
    // (本线程正跑该回调) 则转 tombstone —— 它已停止接收, 不可回表顶替
    // 新注册。
    if (previous != nullptr) {
        if (quiesce_callback(previous) == SAO_OK) {
            retire_callback(previous);
        } else {
            std::lock_guard lock(runtime->engine_callbacks_mutex);
            runtime->engine_callbacks_retired.push_back(std::move(previous));
        }
    }
    return emma_value(true);
}

// ctx.engine_call(name, args[, callback_channel]): raw passthrough —— 返回
// 完整 {status,result} envelope dict 由脚本自行判定 status。
emma_value engine_call_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.engine_call";
    const std::string name = require_string(arguments, 0, method);
    json engine_args = json::object();
    if (!is_null_argument(arguments, 1)) {
        if (!std::holds_alternative<std::shared_ptr<emma_dict>>(arguments[1]) ||
            !value_to_json(arguments[1], engine_args) || !engine_args.is_object()) {
            throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
        }
    }
    const std::string callback_channel = optional_string_value(arguments, 2, {}, method);
    json request_args = json::object();
    request_args["name"] = name;
    request_args["args"] = std::move(engine_args);
    if (!callback_channel.empty())
        request_args["callback_channel"] = callback_channel;
    const json envelope =
        engine_dispatch_envelope(runtime,
                                 sao::plugins::sdk_binding::sdk_method_id::method_engine_call,
                                 request_args, method);
    emma_value converted = nullptr;
    if (!json_to_value(envelope, converted))
        throw_context_status(method, SAO_ERR_OS_CALL_FAILED);
    return converted;
}

// engine 面 teardown: 排空并退役 engine.on 的 channel 记录, 然后
// try_destroy 自有 SaoSdkContext (SAO_SDK_ERR_BUSY → loader BUSY, 供上层
// 重试)。channel 先停: sdk 销毁途中触达的末次回调查表落空或被
// invoke_callback 按 !accepting 丢弃; try_destroy 失败时 ctx 仍归
// runtime, 可整个重来。
int32_t teardown_engine_surface(emma_plugin_runtime& runtime) noexcept {
    try {
        for (;;) {
            std::shared_ptr<emma_plugin_runtime::callback_record> record;
            {
                std::lock_guard lock(runtime.engine_callbacks_mutex);
                if (runtime.engine_callbacks.empty())
                    break;
                record = runtime.engine_callbacks.begin()->second;
            }
            const int32_t status = quiesce_callback(record);
            if (status != SAO_OK) {
                resume_callback(record);
                return status;
            }
            retire_callback(record);
            std::lock_guard lock(runtime.engine_callbacks_mutex);
            for (auto it = runtime.engine_callbacks.begin();
                 it != runtime.engine_callbacks.end(); ++it) {
                if (it->second == record) {
                    runtime.engine_callbacks.erase(it);
                    break;
                }
            }
        }
        // tombstone: 已停止接收只欠 quiesce 收尾的记录 (同线程在飞时进入)。
        for (;;) {
            std::shared_ptr<emma_plugin_runtime::callback_record> record;
            {
                std::lock_guard lock(runtime.engine_callbacks_mutex);
                if (runtime.engine_callbacks_retired.empty())
                    break;
                record = runtime.engine_callbacks_retired.back();
            }
            const int32_t status = quiesce_callback(record);
            if (status != SAO_OK)
                return status;
            retire_callback(record);
            std::lock_guard lock(runtime.engine_callbacks_mutex);
            runtime.engine_callbacks_retired.pop_back();
        }
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (runtime.sdk_context == nullptr)
        return SAO_OK;
    int32_t status = sao_sdk_context_try_destroy(runtime.sdk_context);
    if (status == SAO_SDK_ERR_BUSY)
        status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    if (status == SAO_OK)
        runtime.sdk_context = nullptr;
    return status;
}

// ctx_surface: 本 host 绑定的全部 ctx 名 (canonical Python 拼写)。
// 在每个 ctx 构造时调用一次, 幂等。
void note_emma_ctx_surface() noexcept {
    static const char* const kNames[] = {
        "plugin_id",
        "path",
        "web_path",
        "assets_path",
        "should_stop",
        "time",
        "log",
        "register_ui_panel",
        "register_render_hook",
        "unregister_render_hook",
        "register_menu_category",
        "register_menu_surface",
        "register_action_handler",
        "register_parser_adapter",
        "register_exporter",
        "register_formatter",
        "register_trigger_type",
        "register_report_view",
        "register_timer",
        "register_data_source",
        "register_engine",
        "get_engine",
        "require_engine",
        "register_thread",
        "run_on_ui",
        "get_setting",
        "setting",
        "set_setting",
        "set_defaults",
        "get_snapshot",
        "snapshot_value",
        "recent_events",
        "subscribe",
        "subscribe_once",
        "unsubscribe",
        "emit",
        "on",
        "on_damage",
        "on_heal",
        "on_skill",
        "on_boss",
        "on_snapshot",
        "on_encounter_finalized",
        "register_hotkey",
        "unregister_hotkey",
        "set_interval",
        "set_timeout",
        "clear_timer",
        "notify",
        "dismiss_notify",
        "toast",
        "open_file",
        "open_window",
        "set_overlay",
        "clear_overlay",
        "request_redraw",
        "create_compositor_layer",
        "upload_compositor_frame",
        "set_compositor_layer_position",
        "set_compositor_layer_visible",
        "set_compositor_layer_input",
        "destroy_compositor_layer",
        "ensure_requirements",
        "load_local",
        "load_script",
        "event_bus",
        "engine",
        "engine_call",
        nullptr,
    };
    sao::plugins::script_ctx::ctx_surface_note_all(loader::engine_kind::emma, kNames);
    // ui.* 构建器名单由 script_ui_methods 提供。
    size_t ui_count = 0;
    const char* const* ui_methods =
        sao::plugins::script_ctx::script_ui_methods(&ui_count);
    for (size_t index = 0; index < ui_count; ++index) {
        const std::string name = std::string("ui.") + ui_methods[index];
        sao::plugins::script_ctx::ctx_surface_note(loader::engine_kind::emma, name.c_str());
    }
    sao::plugins::script_ctx::ctx_surface_note(loader::engine_kind::emma, "ui");
    // 反射引擎面成员: engine.list / engine.on + catalog 条目 ('.'→'_')。
    sao::plugins::script_ctx::ctx_surface_note(loader::engine_kind::emma, "engine.list");
    sao::plugins::script_ctx::ctx_surface_note(loader::engine_kind::emma, "engine.on");
    const size_t engine_catalog_count = sao::plugins::sdk_binding::sdk_engine_catalog_size();
    for (size_t index = 0; index < engine_catalog_count; ++index) {
        const auto* desc = sao::plugins::sdk_binding::sdk_engine_catalog_at(index);
        const std::string member = engine_member_name(desc == nullptr ? nullptr : desc->name);
        if (member.empty())
            continue;
        const std::string qualified = "engine." + member;
        sao::plugins::script_ctx::ctx_surface_note(loader::engine_kind::emma,
                                                 qualified.c_str());
    }
}

// ── ctx 扩展绑定: render hook / data source / extension / compositor ──────

emma_value register_render_hook_impl(emma_plugin_runtime* runtime,
                                     std::vector<emma_value> arguments) {
    const char* method = "ctx.register_render_hook";
    const std::string surface = require_string(arguments, 0, method);
    auto function = require_callable(arguments, 1, method);
    const double priority =
        arguments.size() > 2 ? require_number(arguments, 2, method) : 0.0;
    auto callback = make_callback(runtime, std::move(function));
    const uint64_t resource_id = add_owned_resource(
        *runtime, emma_plugin_runtime::resource_kind::render_hook, 0, surface, callback);
    uint32_t token = 0;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_render_hook(
        runtime->context, surface.c_str(), static_cast<float>(priority), render_hook_bridge,
        callback.get(), &token);
    if (status != SAO_OK) {
        (void)remove_owned_resource(*runtime, resource_id);
        retire_callback(callback);
        throw_context_status(method, status);
    }
    (void)update_owned_resource(*runtime, resource_id, token, surface);
    return static_cast<int64_t>(token);
}

emma_value unregister_render_hook_impl(emma_plugin_runtime* runtime,
                                       std::vector<emma_value> arguments) {
    const char* method = "ctx.unregister_render_hook";
    const int64_t raw_token = require_integer(arguments, 0, method);
    if (raw_token <= 0 || raw_token > std::numeric_limits<uint32_t>::max())
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    const uint32_t token = static_cast<uint32_t>(raw_token);
    emma_plugin_runtime::owned_resource resource;
    if (!find_owned_resource(
            *runtime,
            [token](const auto& resource) {
                return resource.kind == emma_plugin_runtime::resource_kind::render_hook &&
                       resource.numeric_token == token;
            },
            resource)) {
        throw_context_status(method, SAO_ERR_HANDLE_INVALID);
    }
    int32_t status = quiesce_callback(resource.callback);
    if (status == SAO_OK) {
        status = sao::plugins::loader::sao_plugins_ctx_unregister_render_hook(runtime->context,
                                                                            token);
    }
    if (!teardown_status_is_complete(status)) {
        resume_callback(resource.callback);
        throw_context_status(method, status);
    }
    retire_callback(resource.callback);
    (void)remove_owned_resource(*runtime, resource.id);
    return emma_value(true);
}

emma_value register_data_source_impl(emma_plugin_runtime* runtime,
                                     std::vector<emma_value> arguments) {
    const char* method = "ctx.register_data_source";
    const std::string source_id = require_string(arguments, 0, method);
    if (arguments.size() < 4)
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    const std::string metadata = serialize_value_or_throw(arguments[1]);
    auto start_fn = require_callable(arguments, 2, method);
    auto stop_fn = require_callable(arguments, 3, method);
    auto bundle = std::make_shared<emma_data_source_bundle>();
    bundle->runtime = runtime;
    bundle->source_id = source_id;
    bundle->start = make_callback(runtime, std::move(start_fn));
    bundle->stop = make_callback(runtime, std::move(stop_fn));
    const uint64_t resource_id = add_owned_resource(
        *runtime, emma_plugin_runtime::resource_kind::data_source, 0, source_id, bundle->stop,
        std::static_pointer_cast<void>(bundle));
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_data_source(
        runtime->context, source_id.c_str(), metadata.c_str(), data_source_start_bridge,
        data_source_stop_bridge, bundle.get());
    if (status != SAO_OK) {
        (void)remove_owned_resource(*runtime, resource_id);
        retire_callback(bundle->start);
        retire_callback(bundle->stop);
        throw_context_status(method, status);
    }
    // ctx teardown 会再次调用 stop(user_data) —— bundle 内存必须活到
    // 进程结束, 由保留表持有; 见 retained_data_source_bundles 注释。
    retain_data_source_bundle(bundle);
    return emma_value(true);
}

emma_value register_extension_impl(emma_plugin_runtime* runtime, const char* kind,
                                   std::vector<emma_value> arguments) {
    const std::string method = std::string("ctx.register_") + kind;
    const std::string extension_id = require_string(arguments, 0, method.c_str());
    if (arguments.size() < 2)
        throw_context_status(method.c_str(), SAO_ERR_INVALID_ARGUMENT);
    const std::string metadata = serialize_value_or_throw(arguments[1]);
    // register_extension ABI 要求 handler/user_data 均为 nullptr; emma
    // callable 只能保存在 runtime.extension_handlers 供宿主侧 parity 留存。
    auto handler = nullable_callable(arguments, 2, method.c_str());
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_extension(
        runtime->context, kind, extension_id.c_str(), metadata.c_str(), nullptr, nullptr);
    if (status != SAO_OK)
        throw_context_status(method.c_str(), status);
    if (handler != nullptr) {
        std::lock_guard lock(runtime->resources_mutex);
        runtime->extension_handlers[std::string(kind) + ":" + extension_id] =
            std::move(handler);
    }
    return emma_value(true);
}

emma_value set_compositor_layer_input_impl(emma_plugin_runtime* runtime,
                                           std::vector<emma_value> arguments) {
    const char* method = "ctx.set_compositor_layer_input";
    const std::string name = require_string(arguments, 0, method);
    const auto cursor_pos = nullable_callable(arguments, 1, method);
    const auto mouse_button = nullable_callable(arguments, 2, method);
    const auto cursor_leave = nullable_callable(arguments, 3, method);
    const auto scroll = nullable_callable(arguments, 4, method);
    auto bundle = std::make_shared<emma_compositor_input_bundle>();
    bundle->runtime = runtime;
    bundle->layer_name = name;
    bundle->cursor_pos = cursor_pos == nullptr ? nullptr
                                             : make_callback(runtime, std::move(cursor_pos));
    bundle->mouse_button = mouse_button == nullptr ? nullptr
                                                   : make_callback(runtime, std::move(mouse_button));
    bundle->cursor_leave = cursor_leave == nullptr ? nullptr
                                                   : make_callback(runtime, std::move(cursor_leave));
    bundle->scroll = scroll == nullptr ? nullptr : make_callback(runtime, std::move(scroll));
    const uint64_t resource_id = add_owned_resource(
        *runtime, emma_plugin_runtime::resource_kind::compositor_input, 0, name, bundle->cursor_pos,
        std::static_pointer_cast<void>(bundle));
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
        runtime->context, name.c_str(),
        bundle->cursor_pos == nullptr ? nullptr : &compositor_cursor_pos_bridge,
        bundle->mouse_button == nullptr ? nullptr : &compositor_mouse_button_bridge,
        bundle->cursor_leave == nullptr ? nullptr : &compositor_cursor_leave_bridge,
        bundle->scroll == nullptr ? nullptr : &compositor_scroll_bridge, bundle.get());
    if (status != SAO_OK) {
        (void)remove_owned_resource(*runtime, resource_id);
        retire_callback(bundle->cursor_pos);
        retire_callback(bundle->mouse_button);
        retire_callback(bundle->cursor_leave);
        retire_callback(bundle->scroll);
        throw_context_status(method, status);
    }
    return emma_value(true);
}

// compositor layer 薄封装
emma_value create_compositor_layer_impl(emma_plugin_runtime* runtime,
                                        std::vector<emma_value> arguments) {
    const char* method = "ctx.create_compositor_layer";
    const std::string name = require_string(arguments, 0, method);
    const int64_t width = optional_integer_value(arguments, 1, 0, method);
    const int64_t height = optional_integer_value(arguments, 2, 0, method);
    const int64_t x = optional_integer_value(arguments, 3, 0, method);
    const int64_t y = optional_integer_value(arguments, 4, 0, method);
    const int64_t z = optional_integer_value(arguments, 5, 0, method);
    const bool click_through = optional_bool_value(arguments, 6, false, method);
    const bool high_fps = optional_bool_value(arguments, 7, false, method);
    const int64_t target_fps = optional_integer_value(arguments, 8, 0, method);
    if (width <= 0 || height <= 0 || width > std::numeric_limits<uint32_t>::max() ||
        height > std::numeric_limits<uint32_t>::max() ||
        target_fps < 0 || target_fps > std::numeric_limits<uint32_t>::max() ||
        x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max() ||
        y < std::numeric_limits<int32_t>::min() || y > std::numeric_limits<int32_t>::max() ||
        z < std::numeric_limits<int32_t>::min() || z > std::numeric_limits<int32_t>::max()) {
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
        runtime->context, name.c_str(), static_cast<uint32_t>(width),
        static_cast<uint32_t>(height), static_cast<int32_t>(x), static_cast<int32_t>(y),
        static_cast<int32_t>(z), click_through, high_fps, static_cast<uint32_t>(target_fps));
    if (status != SAO_OK)
        throw_context_status(method, status);
    // Python 约定: {ok=true, name=name}; emma 语义里 true + name dict。
    auto result = std::make_shared<emma_dict>();
    result->items.emplace("ok", true);
    result->items.emplace("name", name);
    return emma_value(std::move(result));
}

emma_value upload_compositor_frame_impl(emma_plugin_runtime* runtime,
                                        std::vector<emma_value> arguments) {
    const char* method = "ctx.upload_compositor_frame";
    const std::string name = require_string(arguments, 0, method);
    const int64_t width = optional_integer_value(arguments, 1, 0, method);
    const int64_t height = optional_integer_value(arguments, 2, 0, method);
    if (width <= 0 || height <= 0 || width > std::numeric_limits<uint32_t>::max() ||
        height > std::numeric_limits<uint32_t>::max()) {
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    }
    if (arguments.size() <= 3)
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    std::string bytes;
    if (const auto* text = std::get_if<std::string>(&arguments[3])) {
        bytes = *text;
    } else if (const auto* list =
                   std::get_if<std::shared_ptr<emma_list>>(&arguments[3])) {
        if (*list != nullptr) {
            bytes.reserve((*list)->items.size());
            for (const auto& item : (*list)->items) {
                const auto* byte = std::get_if<int64_t>(&item);
                if (byte == nullptr || *byte < 0 || *byte > 255)
                    throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
                bytes.push_back(static_cast<char>(*byte));
            }
        }
    } else {
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    }
    // 可选 x,y → 先 set_position 再 upload (对齐 Python 语义)。
    const int64_t x = optional_integer_value(arguments, 4, 0, method);
    const int64_t y = optional_integer_value(arguments, 5, 0, method);
    if (arguments.size() > 4) {
        if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max() ||
            y < std::numeric_limits<int32_t>::min() || y > std::numeric_limits<int32_t>::max()) {
            throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
        }
        const int32_t position_status =
            sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_position(
                runtime->context, name.c_str(), static_cast<int32_t>(x),
                static_cast<int32_t>(y));
        if (position_status != SAO_OK)
            throw_context_status(method, position_status);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_upload_compositor_frame(
        runtime->context, name.c_str(),
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
        static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(true);
}

emma_value set_compositor_layer_position_impl(emma_plugin_runtime* runtime,
                                              std::vector<emma_value> arguments) {
    const char* method = "ctx.set_compositor_layer_position";
    const std::string name = require_string(arguments, 0, method);
    const int64_t x = require_integer(arguments, 1, method);
    const int64_t y = require_integer(arguments, 2, method);
    if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max() ||
        y < std::numeric_limits<int32_t>::min() || y > std::numeric_limits<int32_t>::max()) {
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_position(
        runtime->context, name.c_str(), static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(true);
}

emma_value set_compositor_layer_visible_impl(emma_plugin_runtime* runtime,
                                             std::vector<emma_value> arguments) {
    const char* method = "ctx.set_compositor_layer_visible";
    const std::string name = require_string(arguments, 0, method);
    const bool visible = optional_bool_value(arguments, 1, true, method);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
        runtime->context, name.c_str(), visible);
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(true);
}

emma_value destroy_compositor_layer_impl(emma_plugin_runtime* runtime,
                                         std::vector<emma_value> arguments) {
    const char* method = "ctx.destroy_compositor_layer";
    const std::string name = require_string(arguments, 0, method);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(
        runtime->context, name.c_str());
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(true);
}

// ── ctx 扩展绑定: file/window/engine/requirements ─────────────────────────

emma_value open_file_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.open_file";
    // filters: dict/list → JSON; string → 原样 (约定已是 JSON)
    std::string filters_json;
    if (arguments.size() > 0 && !std::holds_alternative<std::nullptr_t>(arguments[0])) {
        if (const auto* text = std::get_if<std::string>(&arguments[0])) {
            filters_json = *text;
        } else {
            filters_json = serialize_value_or_throw(arguments[0]);
        }
    }
    const std::string title = optional_string_value(arguments, 1, {}, method);
    const std::string initial_dir = optional_string_value(arguments, 2, {}, method);
    const int64_t hwnd = optional_integer_value(arguments, 3, 0, method);
    std::wstring initial_dir_w;
    if (!initial_dir.empty())
        initial_dir_w = utf8_to_wide(initial_dir);
    wchar_t* selected = nullptr;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_open_file(
        runtime->context, filters_json.empty() ? nullptr : filters_json.c_str(),
        title.empty() ? nullptr : title.c_str(),
        initial_dir_w.empty() ? nullptr : initial_dir_w.c_str(), static_cast<intptr_t>(hwnd),
        &selected);
    if (status != SAO_OK)
        throw_context_status(method, status);
    // cancel → SAO_OK + nullptr → nil
    std::unique_ptr<wchar_t, decltype(&sao::plugins::loader::sao_plugins_ctx_free_wstring)> path(
        selected, &sao::plugins::loader::sao_plugins_ctx_free_wstring);
    if (path == nullptr)
        return emma_value(nullptr);
    return emma_value(wide_to_utf8(path.get()));
}

emma_value open_window_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.open_window";
    const std::string panel_id = require_string(arguments, 0, method);
    const int64_t width = optional_integer_value(arguments, 1, 0, method);
    const int64_t height = optional_integer_value(arguments, 2, 0, method);
    if (width <= 0 || height <= 0 || width > std::numeric_limits<uint32_t>::max() ||
        height > std::numeric_limits<uint32_t>::max()) {
        throw_context_status(method, SAO_ERR_INVALID_ARGUMENT);
    }
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_open_window(
        runtime->context, panel_id.c_str(), static_cast<uint32_t>(width),
        static_cast<uint32_t>(height));
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(true);
}

// engine 句柄: emma 没有原始指针, 与 Python 一致用 int64 整数句柄。
emma_value register_engine_impl(emma_plugin_runtime* runtime,
                                std::vector<emma_value> arguments) {
    const char* method = "ctx.register_engine";
    const std::string name = require_string(arguments, 0, method);
    const int64_t engine_ptr = require_integer(arguments, 1, method);
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_engine(
        runtime->context, name.c_str(),
        reinterpret_cast<void*>(static_cast<intptr_t>(engine_ptr)));
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(true);
}

emma_value get_engine_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.get_engine";
    const std::string name = require_string(arguments, 0, method);
    void* engine =
        sao::plugins::loader::sao_plugins_ctx_get_engine(runtime->context, name.c_str());
    if (engine == nullptr) {
        if (arguments.size() > 1)
            return arguments[1];
        return emma_value(nullptr);
    }
    return emma_value(static_cast<int64_t>(reinterpret_cast<intptr_t>(engine)));
}

emma_value require_engine_impl(emma_plugin_runtime* runtime,
                               std::vector<emma_value> arguments) {
    const char* method = "ctx.require_engine";
    const std::string name = require_string(arguments, 0, method);
    void* engine =
        sao::plugins::loader::sao_plugins_ctx_get_engine(runtime->context, name.c_str());
    if (engine == nullptr)
        throw_context_message(method, SAO_ERR_HANDLE_INVALID,
                              "engine '" + name + "' is not registered");
    return emma_value(static_cast<int64_t>(reinterpret_cast<intptr_t>(engine)));
}

emma_value ensure_requirements_impl(emma_plugin_runtime* runtime,
                                    std::vector<emma_value> arguments) {
    const char* method = "ctx.ensure_requirements";
    const bool install = optional_bool_value(arguments, 0, false, method);
    char* report = nullptr;
    const int32_t status = sao::plugins::loader::sao_plugins_ctx_ensure_requirements(
        runtime->context, install, &report);
    std::unique_ptr<char, decltype(&sao::plugins::loader::sao_plugins_ctx_free_string)> owned(
        report, &sao::plugins::loader::sao_plugins_ctx_free_string);
    // 有 report 时即使 status 非 OK 也回传解析结果 (Python 语义)。
    if (owned != nullptr) {
        json parsed;
        emma_value converted = nullptr;
        if (parse_bounded_json_c_string(owned.get(), kMaximumJsonInputBytes, parsed) &&
            json_to_value(parsed, converted)) {
            return converted;
        }
    }
    if (status != SAO_OK)
        throw_context_status(method, status);
    return emma_value(nullptr);
}

// load_local → runtime_bridge_load_local (provider 分派)。
emma_value wrap_script_module(
    const std::shared_ptr<sao::plugins::script_ctx::script_module>& module,
    emma_plugin_runtime* runtime) {
    auto dict = std::make_shared<emma_dict>();
    if (module == nullptr)
        return emma_value(std::move(dict));
    dict->items.emplace("__name", module->module_id());
    dict->items.emplace(
        "__call", make_host_callable(
                      "module.__call",
                      [runtime, module](std::vector<emma_value> arguments) -> emma_value {
                          const std::string name = require_string(arguments, 0, "module.__call");
                          std::vector<sao::plugins::script_ctx::script_value_ptr> script_args;
                          script_args.reserve(arguments.size() > 0 ? arguments.size() - 1 : 0);
                          for (size_t index = 1; index < arguments.size(); ++index)
                              script_args.push_back(emma_value_to_script(arguments[index], runtime));
                          script_value_ptr result;
                          std::string error;
                          const int32_t status =
                              module->call(name, script_args, &result, &error);
                          if (status != SAO_OK)
                              throw_context_message(
                                  "module.__call", status,
                                  error.empty() ? "module call failed" : error);
                          return script_value_to_emma(result, runtime);
                      }));
    for (const auto& member_name : module->member_names()) {
        // 数据成员 → 直接取值绑定; 成员函数/get 失败 → callable 包装。
        script_value_ptr value;
        std::string get_error;
        bool bind_value = false;
        if (module->get(member_name, &value, &get_error) == SAO_OK && value != nullptr &&
            value->k != sao::plugins::script_ctx::script_value::kind::function) {
            bind_value = true;
        }
        if (bind_value) {
            dict->items.emplace(member_name, script_value_to_emma(value, runtime));
            continue;
        }
        dict->items.emplace(
            member_name,
            make_host_callable(
                ("module." + member_name).c_str(),
                [runtime, module, member_name](std::vector<emma_value> arguments) -> emma_value {
                    std::vector<sao::plugins::script_ctx::script_value_ptr> script_args;
                    script_args.reserve(arguments.size());
                    for (const auto& argument : arguments)
                        script_args.push_back(emma_value_to_script(argument, runtime));
                    script_value_ptr result;
                    std::string error;
                    const int32_t status = module->call(member_name, script_args, &result, &error);
                    if (status != SAO_OK)
                        throw_context_message("ctx.load_local member", status,
                                              error.empty() ? "module call failed" : error);
                    return script_value_to_emma(result, runtime);
                }));
    }
    return emma_value(std::move(dict));
}

emma_value load_local_impl(emma_plugin_runtime* runtime, std::vector<emma_value> arguments) {
    const char* method = "ctx.load_local";
    const std::string relative = require_string(arguments, 0, method);
    ensure_emma_runtime_bridge();
    namespace sc = sao::plugins::script_ctx;
    sc::load_local_result kind = sc::load_local_result::missing;
    std::shared_ptr<sc::script_module> module;
    std::wstring absolute;
    std::string diagnostic;
    const std::wstring root_w = runtime->plugin_root.wstring();
    const int32_t status = sc::runtime_bridge_load_local(
        runtime->context, runtime->plugin_id.c_str(), root_w.c_str(), relative.c_str(), &kind,
        &module, &absolute, &diagnostic);
    if (status != SAO_OK)
        throw_context_status(method, status);
    switch (kind) {
    case sc::load_local_result::module:
        return wrap_script_module(module, runtime);
    case sc::load_local_result::path_only:
        return emma_value(wide_to_utf8(absolute));
    case sc::load_local_result::missing:
    case sc::load_local_result::unsupported:
    default: {
        const std::string reason = diagnostic.empty()
                                       ? (kind == sc::load_local_result::missing
                                              ? "path resolution failed"
                                              : "no script provider for extension")
                                       : diagnostic;
        sao::plugins::loader::sao_plugins_ctx_log(
            runtime->context, (std::string("ctx.load_local: ") + reason).c_str());
        return emma_value(false);
    }
    }
}

bool teardown_status_is_complete(int32_t status) noexcept {
    return status == SAO_OK || status == SAO_ERR_HANDLE_INVALID;
}

int32_t teardown_resource(emma_plugin_runtime& runtime,
                          emma_plugin_runtime::owned_resource& resource) {
    int32_t status = quiesce_callback(resource.callback);
    if (status != SAO_OK)
        return status;
    switch (resource.kind) {
    case emma_plugin_runtime::resource_kind::event_subscription:
        status = sao::plugins::loader::sao_plugins_ctx_unsubscribe(runtime.context,
                                                                   resource.numeric_token);
        break;
    case emma_plugin_runtime::resource_kind::hotkey:
        status = sao::plugins::loader::sao_plugins_ctx_unregister_hotkey(runtime.context,
                                                                         resource.key.c_str());
        break;
    case emma_plugin_runtime::resource_kind::timer:
        status = sao::plugins::loader::sao_plugins_ctx_clear_timer(runtime.context,
                                                                   resource.key.c_str());
        break;
    case emma_plugin_runtime::resource_kind::notification:
        status = sao::plugins::loader::sao_plugins_ctx_dismiss_notify(runtime.context);
        break;
    case emma_plugin_runtime::resource_kind::overlay:
        status = sao::plugins::loader::sao_plugins_ctx_clear_overlay(runtime.context,
                                                                     resource.key.c_str());
        break;
    case emma_plugin_runtime::resource_kind::render_hook:
        status = sao::plugins::loader::sao_plugins_ctx_unregister_render_hook(
            runtime.context, resource.numeric_token);
        break;
    case emma_plugin_runtime::resource_kind::data_source: {
        // ctx 保留该 source 记录并在其 teardown 阶段调用 stop —— 这里只停
        // bundle 里的 start/stop 回调, bundle 内存由保留表持有;
        // bridge 见 runtime==nullptr 即安全空转。
        auto bundle = std::static_pointer_cast<emma_data_source_bundle>(resource.attachment);
        if (bundle != nullptr) {
            status = quiesce_callback(bundle->start);
            if (status != SAO_OK) {
                resume_callback(resource.callback);
                resume_callback(bundle->start);
                return status;
            }
            retire_callback(bundle->start);
            retire_callback(bundle->stop);
            bundle->runtime = nullptr;
            status = SAO_OK;
        }
        break;
    }
    case emma_plugin_runtime::resource_kind::compositor_input: {
        auto bundle =
            std::static_pointer_cast<emma_compositor_input_bundle>(resource.attachment);
        if (bundle != nullptr) {
            status = quiesce_callback(bundle->mouse_button);
            if (status == SAO_OK)
                status = quiesce_callback(bundle->cursor_leave);
            if (status == SAO_OK)
                status = quiesce_callback(bundle->scroll);
            if (status != SAO_OK) {
                resume_callback(resource.callback);
                resume_callback(bundle->mouse_button);
                resume_callback(bundle->cursor_leave);
                resume_callback(bundle->scroll);
                return status;
            }
            // 显式 detach, provider 不再回调旧 bundle (bundle 随资源释放)。
            status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
                runtime.context, bundle->layer_name.c_str(), nullptr, nullptr, nullptr,
                nullptr, nullptr);
            if (!teardown_status_is_complete(status)) {
                resume_callback(resource.callback);
                resume_callback(bundle->mouse_button);
                resume_callback(bundle->cursor_leave);
                resume_callback(bundle->scroll);
                return status;
            }
            retire_callback(bundle->cursor_pos);
            retire_callback(bundle->mouse_button);
            retire_callback(bundle->cursor_leave);
            retire_callback(bundle->scroll);
            bundle->runtime = nullptr;
        }
        break;
    }
    }
    if (!teardown_status_is_complete(status)) {
        resume_callback(resource.callback);
        return status;
    }
    retire_callback(resource.callback);
    return SAO_OK;
}

int32_t teardown_owned_resources(emma_plugin_runtime& runtime) {
    while (true) {
        emma_plugin_runtime::owned_resource resource;
        {
            std::lock_guard lock(runtime.resources_mutex);
            if (runtime.resources.empty())
                return SAO_OK;
            resource = runtime.resources.back();
        }
        const int32_t status = teardown_resource(runtime, resource);
        if (status != SAO_OK)
            return status;
        (void)remove_owned_resource(runtime, resource.id);
    }
}

emma_value make_native_context(emma_plugin_runtime* runtime) {
    loader_context_t* context = runtime->context;
    auto wrapper = std::make_shared<emma_dict>();
    wrapper->items.emplace("plugin_id", runtime->plugin_id);
    wrapper->items.emplace("path", path_utf8(runtime->plugin_root));
    wrapper->items.emplace("web_path", path_utf8(runtime->plugin_root / "web"));
    wrapper->items.emplace("assets_path", path_utf8(runtime->plugin_root / "assets"));
    wrapper->items.emplace("time", make_host_callable("ctx.time", [](std::vector<emma_value>) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return emma_value(std::chrono::duration<double>(now).count());
    }));
    wrapper->items.emplace(
        "log", make_host_callable("ctx.log", [context](std::vector<emma_value> arguments) {
            if (arguments.empty()) {
                throw_context_status("ctx.log", SAO_ERR_INVALID_ARGUMENT);
            }
            std::string message;
            for (size_t index = 0; index < arguments.size(); ++index) {
                if (index != 0)
                    message.push_back(' ');
                message += emma_value_to_string(arguments[index]);
            }
            sao::plugins::loader::sao_plugins_ctx_log(context, message.c_str());
            return emma_value(nullptr);
        }));
    wrapper->items.emplace(
        "register_ui_panel",
        make_host_callable("ctx.register_ui_panel", [context](std::vector<emma_value> arguments) {
            const std::string panel_id = require_metadata_only_panel(arguments);
            const std::string metadata = serialize_value_or_throw(arguments[1]);
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_ui_panel(
                context, panel_id.c_str(), metadata.c_str(), nullptr, nullptr, nullptr);
            if (status != SAO_OK) {
                throw_context_status("ctx.register_ui_panel", status);
            }
            return emma_value(true);
        }));
    wrapper->items.emplace("register_menu_category",
                           make_host_callable("ctx.register_menu_category",
                                              [runtime](std::vector<emma_value> arguments) {
                                                  return register_menu_category(
                                                      *runtime, std::move(arguments));
                                              }));
    wrapper->items.emplace("register_action_handler",
                           make_host_callable("ctx.register_action_handler",
                                              [runtime](std::vector<emma_value> arguments) {
                                                  return register_action_handler(
                                                      *runtime, std::move(arguments));
                                              }));
    wrapper->items.emplace("register_menu_surface",
                           make_host_callable("ctx.register_menu_surface",
                                              [runtime](std::vector<emma_value> arguments) {
                                                  return register_menu_surface(
                                                      *runtime, std::move(arguments));
                                              }));
    wrapper->items.emplace(
        "get_setting",
        make_host_callable("ctx.get_setting", [context](std::vector<emma_value> arguments) {
            const std::string key = require_string(arguments, 0, "ctx.get_setting");
            char* value = nullptr;
            const int32_t status =
                sao::plugins::loader::sao_plugins_ctx_get_setting(context, key.c_str(), &value);
            if (status == SAO_ERR_HANDLE_INVALID && arguments.size() > 1) {
                return arguments[1];
            }
            return parse_owned_json(status, value, "ctx.get_setting");
        }));
    wrapper->items.emplace("setting", wrapper->items.at("get_setting"));
    wrapper->items.emplace(
        "set_setting",
        make_host_callable("ctx.set_setting", [context](std::vector<emma_value> arguments) {
            const std::string key = require_string(arguments, 0, "ctx.set_setting");
            if (arguments.size() < 2) {
                throw_context_status("ctx.set_setting", SAO_ERR_INVALID_ARGUMENT);
            }
            const std::string serialized = serialize_value_or_throw(arguments[1]);
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_setting(
                context, key.c_str(), serialized.c_str());
            if (status != SAO_OK) {
                throw_context_status("ctx.set_setting", status);
            }
            return emma_value(nullptr);
        }));
    wrapper->items.emplace(
        "set_defaults",
        make_host_callable("ctx.set_defaults", [context](std::vector<emma_value> arguments) {
            if (arguments.empty()) {
                throw_context_status("ctx.set_defaults", SAO_ERR_INVALID_ARGUMENT);
            }
            const std::string serialized = serialize_value_or_throw(arguments[0]);
            const int32_t status =
                sao::plugins::loader::sao_plugins_ctx_set_defaults(context, serialized.c_str());
            if (status != SAO_OK) {
                throw_context_status("ctx.set_defaults", status);
            }
            return emma_value(nullptr);
        }));

    const auto subscribe = [runtime](std::vector<emma_value> arguments, bool once) -> emma_value {
        const char* method = once ? "ctx.subscribe_once" : "ctx.subscribe";
        const std::string topic = require_string(arguments, 0, method);
        auto callback = make_callback(runtime, require_callable(arguments, 1, method));
        callback->one_shot = once;
        if (once) {
            callback->one_shot_phase =
                emma_plugin_runtime::callback_record::one_shot_state::registering;
        }
        const uint64_t resource_id = add_owned_resource(
            *runtime, emma_plugin_runtime::resource_kind::event_subscription, 0, topic, callback);
        uint32_t token = 0;
        const int32_t status = once ? sao::plugins::loader::sao_plugins_ctx_subscribe_once(
                                          runtime->context, topic.c_str(), event_callback_bridge,
                                          callback.get(), &token)
                                    : sao::plugins::loader::sao_plugins_ctx_subscribe(
                                          runtime->context, topic.c_str(), event_callback_bridge,
                                          callback.get(), &token);
        if (status != SAO_OK) {
            (void)remove_owned_resource(*runtime, resource_id);
            retire_callback(callback);
            throw_context_status(method, status);
        }
        (void)update_owned_resource(*runtime, resource_id, token, topic);
        if (once) {
            std::lock_guard callback_lock(callback->mutex);
            if (callback->one_shot_phase ==
                emma_plugin_runtime::callback_record::one_shot_state::registering) {
                callback->one_shot_phase =
                    emma_plugin_runtime::callback_record::one_shot_state::armed;
            }
        }
        return static_cast<int64_t>(token);
    };
    wrapper->items.emplace(
        "subscribe",
        make_host_callable("ctx.subscribe", [subscribe](std::vector<emma_value> arguments) {
            return subscribe(std::move(arguments), false);
        }));
    wrapper->items.emplace(
        "subscribe_once",
        make_host_callable("ctx.subscribe_once",
                           [subscribe](std::vector<emma_value> arguments) {
                               return subscribe(std::move(arguments), true);
                           }));
    wrapper->items.emplace(
        "unsubscribe",
        make_host_callable("ctx.unsubscribe", [runtime](std::vector<emma_value> arguments) {
            const int64_t raw_token = require_integer(arguments, 0, "ctx.unsubscribe");
            if (raw_token <= 0 || raw_token > std::numeric_limits<uint32_t>::max()) {
                throw_context_status("ctx.unsubscribe", SAO_ERR_INVALID_ARGUMENT);
            }
            const uint32_t token = static_cast<uint32_t>(raw_token);
            emma_plugin_runtime::owned_resource resource;
            if (!find_owned_resource(
                    *runtime,
                    [token](const auto& resource) {
                        return resource.kind ==
                                   emma_plugin_runtime::resource_kind::event_subscription &&
                               resource.numeric_token == token;
                    },
                    resource)) {
                throw_context_status("ctx.unsubscribe", SAO_ERR_HANDLE_INVALID);
            }
            int32_t status = quiesce_callback(resource.callback);
            if (status == SAO_OK) {
                status = sao::plugins::loader::sao_plugins_ctx_unsubscribe(runtime->context, token);
            }
            if (!teardown_status_is_complete(status)) {
                resume_callback(resource.callback);
                throw_context_status("ctx.unsubscribe", status);
            }
            retire_callback(resource.callback);
            (void)remove_owned_resource(*runtime, resource.id);
            return emma_value(true);
        }));
    wrapper->items.emplace(
        "emit", make_host_callable("ctx.emit", [runtime](std::vector<emma_value> arguments) {
            const std::string topic = require_string(arguments, 0, "ctx.emit");
            if (g_active_runtime == runtime && has_accepting_event_subscription(*runtime, topic)) {
                throw_context_status("ctx.emit", sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
            }
            const std::string payload =
                arguments.size() > 1 ? serialize_value_or_throw(arguments[1]) : "null";
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_emit(
                runtime->context, topic.c_str(), payload.c_str());
            if (status != SAO_OK)
                throw_context_status("ctx.emit", status);
            return emma_value(true);
        }));

    wrapper->items.emplace(
        "register_hotkey",
        make_host_callable("ctx.register_hotkey", [runtime](std::vector<emma_value> arguments) {
            const std::string id = require_string(arguments, 0, "ctx.register_hotkey");
            auto function = require_callable(arguments, 1, "ctx.register_hotkey");
            const std::string default_key = require_string(arguments, 2, "ctx.register_hotkey");
            const std::string label = arguments.size() > 3
                                          ? require_string(arguments, 3, "ctx.register_hotkey")
                                          : std::string{};
            auto callback = make_callback(runtime, std::move(function));
            const uint64_t resource_id = add_owned_resource(
                *runtime, emma_plugin_runtime::resource_kind::hotkey, 0, id, callback);
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_register_hotkey(
                runtime->context, id.c_str(), default_key.c_str(), label.c_str(),
                hotkey_callback_bridge, callback.get());
            if (status != SAO_OK) {
                (void)remove_owned_resource(*runtime, resource_id);
                retire_callback(callback);
                throw_context_status("ctx.register_hotkey", status);
            }
            return id;
        }));
    wrapper->items.emplace(
        "unregister_hotkey",
        make_host_callable("ctx.unregister_hotkey", [runtime](std::vector<emma_value> arguments) {
            const std::string id = require_string(arguments, 0, "ctx.unregister_hotkey");
            emma_plugin_runtime::owned_resource resource;
            if (!find_owned_resource(
                    *runtime,
                    [&id](const auto& resource) {
                        return resource.kind == emma_plugin_runtime::resource_kind::hotkey &&
                               resource.key == id;
                    },
                    resource)) {
                throw_context_status("ctx.unregister_hotkey", SAO_ERR_HANDLE_INVALID);
            }
            int32_t status = quiesce_callback(resource.callback);
            if (status == SAO_OK) {
                status = sao::plugins::loader::sao_plugins_ctx_unregister_hotkey(runtime->context,
                                                                                 id.c_str());
            }
            if (!teardown_status_is_complete(status)) {
                resume_callback(resource.callback);
                throw_context_status("ctx.unregister_hotkey", status);
            }
            retire_callback(resource.callback);
            (void)remove_owned_resource(*runtime, resource.id);
            return emma_value(true);
        }));

    const auto register_timer = [runtime](std::vector<emma_value> arguments,
                                          bool one_shot) -> emma_value {
        const char* method = one_shot ? "ctx.set_timeout" : "ctx.set_interval";
        auto function = require_callable(arguments, 0, method);
        const double seconds = require_number(arguments, 1, method);
        auto callback = make_callback(runtime, std::move(function));
        callback->one_shot = one_shot;
        if (one_shot) {
            callback->one_shot_phase =
                emma_plugin_runtime::callback_record::one_shot_state::registering;
        }
        const uint64_t resource_id = add_owned_resource(
            *runtime, emma_plugin_runtime::resource_kind::timer, 0, {}, callback);
        char* raw_token = nullptr;
        const int32_t status =
            one_shot
                ? sao::plugins::loader::sao_plugins_ctx_set_timeout(
                      runtime->context, timer_callback_bridge, seconds, callback.get(), &raw_token)
                : sao::plugins::loader::sao_plugins_ctx_set_interval(
                      runtime->context, timer_callback_bridge, seconds, callback.get(), &raw_token);
        std::unique_ptr<char, decltype(&sao::plugins::loader::sao_plugins_ctx_free_string)>
            token_owner(raw_token, &sao::plugins::loader::sao_plugins_ctx_free_string);
        if (status != SAO_OK || raw_token == nullptr) {
            (void)remove_owned_resource(*runtime, resource_id);
            retire_callback(callback);
            throw_context_status(method, status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status);
        }
        const std::string token(raw_token);
        (void)update_owned_resource(*runtime, resource_id, 0, token);
        {
            std::lock_guard callback_lock(callback->mutex);
            callback->timer_token = token;
            if (one_shot && callback->one_shot_phase ==
                                emma_plugin_runtime::callback_record::one_shot_state::registering) {
                callback->one_shot_phase =
                    emma_plugin_runtime::callback_record::one_shot_state::armed;
            }
        }
        return token;
    };
    wrapper->items.emplace(
        "set_interval",
        make_host_callable("ctx.set_interval",
                           [register_timer](std::vector<emma_value> arguments) {
                               return register_timer(std::move(arguments), false);
                           }));
    wrapper->items.emplace(
        "set_timeout",
        make_host_callable("ctx.set_timeout",
                           [register_timer](std::vector<emma_value> arguments) {
                               return register_timer(std::move(arguments), true);
                           }));
    wrapper->items.emplace(
        "clear_timer",
        make_host_callable("ctx.clear_timer", [runtime](std::vector<emma_value> arguments) {
            const std::string token = require_string(arguments, 0, "ctx.clear_timer");
            emma_plugin_runtime::owned_resource resource;
            if (!find_owned_resource(
                    *runtime,
                    [&token](const auto& resource) {
                        return resource.kind == emma_plugin_runtime::resource_kind::timer &&
                               resource.key == token;
                    },
                    resource)) {
                throw_context_status("ctx.clear_timer", SAO_ERR_HANDLE_INVALID);
            }
            int32_t status = quiesce_callback(resource.callback);
            if (status == SAO_OK) {
                status = sao::plugins::loader::sao_plugins_ctx_clear_timer(runtime->context,
                                                                           token.c_str());
            }
            if (!teardown_status_is_complete(status)) {
                resume_callback(resource.callback);
                throw_context_status("ctx.clear_timer", status);
            }
            retire_callback(resource.callback);
            (void)remove_owned_resource(*runtime, resource.id);
            return emma_value(true);
        }));

    wrapper->items.emplace(
        "notify", make_host_callable("ctx.notify", [runtime](std::vector<emma_value> arguments) {
            const std::string title = require_string(arguments, 0, "ctx.notify");
            const std::string message = require_string(arguments, 1, "ctx.notify");
            const double duration =
                arguments.size() > 2 ? require_number(arguments, 2, "ctx.notify") : 60.0;
            const std::string kind =
                arguments.size() > 3 ? require_string(arguments, 3, "ctx.notify") : "plugin";
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_notify(
                runtime->context, title.c_str(), message.c_str(), duration, kind.c_str());
            if (status != SAO_OK)
                throw_context_status("ctx.notify", status);
            try {
                (void)add_owned_resource(*runtime,
                                         emma_plugin_runtime::resource_kind::notification);
            } catch (...) {
                (void)sao::plugins::loader::sao_plugins_ctx_dismiss_notify(runtime->context);
                throw;
            }
            return emma_value(true);
        }));
    wrapper->items.emplace(
        "dismiss_notify",
        make_host_callable("ctx.dismiss_notify", [runtime](std::vector<emma_value>) {
            const int32_t status =
                sao::plugins::loader::sao_plugins_ctx_dismiss_notify(runtime->context);
            if (status != SAO_OK) {
                throw_context_status("ctx.dismiss_notify", status);
            }
            {
                std::lock_guard lock(runtime->resources_mutex);
                runtime->resources.erase(
                    std::remove_if(runtime->resources.begin(), runtime->resources.end(),
                                   [](const auto& resource) {
                                       return resource.kind ==
                                              emma_plugin_runtime::resource_kind::notification;
                                   }),
                    runtime->resources.end());
            }
            return emma_value(true);
        }));
    wrapper->items.emplace(
        "toast", make_host_callable("ctx.toast", [runtime](std::vector<emma_value> arguments) {
            const std::string message = require_string(arguments, 0, "ctx.toast");
            const int32_t status =
                sao::plugins::loader::sao_plugins_ctx_toast(runtime->context, message.c_str());
            if (status != SAO_OK)
                throw_context_status("ctx.toast", status);
            try {
                (void)add_owned_resource(*runtime,
                                         emma_plugin_runtime::resource_kind::notification);
            } catch (...) {
                (void)sao::plugins::loader::sao_plugins_ctx_dismiss_notify(runtime->context);
                throw;
            }
            return emma_value(true);
        }));

    wrapper->items.emplace(
        "set_overlay",
        make_host_callable("ctx.set_overlay", [runtime](std::vector<emma_value> arguments) {
            const std::string surface = require_string(arguments, 0, "ctx.set_overlay");
            if (arguments.size() < 2) {
                throw_context_status("ctx.set_overlay", SAO_ERR_INVALID_ARGUMENT);
            }
            const std::string spec = serialize_value_or_throw(arguments[1]);
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_set_overlay(
                runtime->context, surface.c_str(), spec.c_str());
            if (status != SAO_OK) {
                throw_context_status("ctx.set_overlay", status);
            }
            try {
                (void)add_owned_resource(*runtime, emma_plugin_runtime::resource_kind::overlay, 0,
                                         surface);
            } catch (...) {
                (void)sao::plugins::loader::sao_plugins_ctx_clear_overlay(runtime->context,
                                                                          surface.c_str());
                throw;
            }
            return emma_value(true);
        }));
    wrapper->items.emplace(
        "clear_overlay",
        make_host_callable("ctx.clear_overlay", [runtime](std::vector<emma_value> arguments) {
            const std::string surface = require_string(arguments, 0, "ctx.clear_overlay");
            emma_plugin_runtime::owned_resource resource;
            if (!find_owned_resource(
                    *runtime,
                    [&surface](const auto& resource) {
                        return resource.kind == emma_plugin_runtime::resource_kind::overlay &&
                               resource.key == surface;
                    },
                    resource)) {
                throw_context_status("ctx.clear_overlay", SAO_ERR_HANDLE_INVALID);
            }
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_clear_overlay(
                runtime->context, surface.c_str());
            if (!teardown_status_is_complete(status)) {
                throw_context_status("ctx.clear_overlay", status);
            }
            (void)remove_owned_resource(*runtime, resource.id);
            return emma_value(true);
        }));
    wrapper->items.emplace(
        "request_redraw",
        make_host_callable("ctx.request_redraw", [context](std::vector<emma_value> arguments) {
            const std::string surface = require_string(arguments, 0, "ctx.request_redraw");
            const std::string reason = arguments.size() > 1
                                           ? require_string(arguments, 1, "ctx.request_redraw")
                                           : std::string{};
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_request_redraw(
                context, surface.c_str(), reason.c_str());
            if (status != SAO_OK) {
                throw_context_status("ctx.request_redraw", status);
            }
            return emma_value(true);
        }));

    // ── 扩展 ctx 表面 (ctx_completion slice) ─────────────────────────────

    wrapper->items.emplace(
        "should_stop",
        make_host_callable("ctx.should_stop", [context](std::vector<emma_value>) {
            return emma_value(sao::plugins::loader::sao_plugins_ctx_should_stop(context));
        }));
    wrapper->items.emplace(
        "get_snapshot",
        make_host_callable("ctx.get_snapshot", [context](std::vector<emma_value>) {
            char* snapshot = nullptr;
            const int32_t status =
                sao::plugins::loader::sao_plugins_ctx_get_snapshot(context, &snapshot);
            return parse_owned_json(status, snapshot, "ctx.get_snapshot");
        }));
    wrapper->items.emplace(
        "snapshot_value",
        make_host_callable("ctx.snapshot_value", [context](std::vector<emma_value> arguments) {
            const std::string path = require_string(arguments, 0, "ctx.snapshot_value");
            char* value = nullptr;
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_snapshot_value(
                context, path.c_str(), &value);
            if (status == SAO_ERR_HANDLE_INVALID && arguments.size() > 1) {
                return arguments[1];
            }
            return parse_owned_json(status, value, "ctx.snapshot_value");
        }));
    wrapper->items.emplace(
        "recent_events",
        make_host_callable("ctx.recent_events", [context](std::vector<emma_value> arguments) {
            const int64_t raw_limit =
                optional_integer_value(arguments, 0, 20, "ctx.recent_events");
            const std::string topic =
                optional_string_value(arguments, 1, {}, "ctx.recent_events");
            if (raw_limit < 0 || raw_limit > std::numeric_limits<uint32_t>::max())
                throw_context_status("ctx.recent_events", SAO_ERR_INVALID_ARGUMENT);
            char* events = nullptr;
            const int32_t status = sao::plugins::loader::sao_plugins_ctx_recent_events(
                context, static_cast<uint32_t>(raw_limit),
                topic.empty() ? nullptr : topic.c_str(), &events);
            return parse_owned_json(status, events, "ctx.recent_events");
        }));

    // on(topic, cb=None): cb 缺省回装饰器 callable; 已提供 → 订阅并返回该 fn。
    const auto on_impl = [subscribe](std::vector<emma_value> arguments) -> emma_value {
        const std::string topic = require_string(arguments, 0, "ctx.on");
        if (arguments.size() < 2 || std::holds_alternative<std::nullptr_t>(arguments[1])) {
            return emma_value(make_host_callable(
                "ctx.on.decorator",
                [subscribe, topic](std::vector<emma_value> inner) -> emma_value {
                    auto function = require_callable(inner, 0, "ctx.on.decorator");
                    (void)subscribe({topic, emma_value(function)}, false);
                    return emma_value(std::move(function));
                }));
        }
        auto function = require_callable(arguments, 1, "ctx.on");
        (void)subscribe({topic, emma_value(function)}, false);
        return emma_value(std::move(function));
    };
    wrapper->items.emplace(
        "on", make_host_callable(
                  "ctx.on", [on_impl](std::vector<emma_value> arguments) {
                      return on_impl(std::move(arguments));
                  }));
    const auto on_topic = [subscribe](const char* topic,
                                    std::vector<emma_value> arguments) -> emma_value {
        auto function = require_callable(arguments, 0, "ctx.on_topic");
        (void)subscribe({std::string(topic), emma_value(function)}, false);
        return emma_value(std::move(function));
    };
    const auto bind_on_topic = [on_topic](const char* topic, const char* name) {
        return make_host_callable(name, [on_topic, topic](std::vector<emma_value> args) {
            return on_topic(topic, std::move(args));
        });
    };
    wrapper->items.emplace("on_damage", bind_on_topic("damage", "ctx.on_damage"));
    wrapper->items.emplace("on_heal", bind_on_topic("heal", "ctx.on_heal"));
    wrapper->items.emplace("on_skill", bind_on_topic("skill", "ctx.on_skill"));
    wrapper->items.emplace("on_boss", bind_on_topic("boss", "ctx.on_boss"));
    wrapper->items.emplace("on_snapshot", bind_on_topic("act_snapshot", "ctx.on_snapshot"));
    wrapper->items.emplace(
        "on_encounter_finalized",
        bind_on_topic("encounter_finalized", "ctx.on_encounter_finalized"));

    // render hook
    wrapper->items.emplace(
        "register_render_hook",
        make_host_callable("ctx.register_render_hook",
                           [runtime](std::vector<emma_value> arguments) {
                               return register_render_hook_impl(runtime, std::move(arguments));
                           }));
    wrapper->items.emplace(
        "unregister_render_hook",
        make_host_callable("ctx.unregister_render_hook",
                           [runtime](std::vector<emma_value> arguments) {
                               return unregister_render_hook_impl(runtime, std::move(arguments));
                           }));

    // extension 注册族 (handler 仅留存 runtime.extension_handlers)
    const auto bind_extension = [runtime](const char* kind) {
        return make_host_callable(
            (std::string("ctx.register_") + kind).c_str(),
            [runtime, kind_string = std::string(kind)](std::vector<emma_value> arguments) {
                return register_extension_impl(runtime, kind_string.c_str(),
                                               std::move(arguments));
            });
    };
    wrapper->items.emplace("register_parser_adapter", bind_extension("parser_adapter"));
    wrapper->items.emplace("register_exporter", bind_extension("exporter"));
    wrapper->items.emplace("register_formatter", bind_extension("formatter"));
    wrapper->items.emplace("register_trigger_type", bind_extension("trigger_type"));
    wrapper->items.emplace("register_report_view", bind_extension("report_view"));
    wrapper->items.emplace("register_timer", bind_extension("timer"));
    wrapper->items.emplace(
        "register_data_source",
        make_host_callable("ctx.register_data_source",
                           [runtime](std::vector<emma_value> arguments) {
                               return register_data_source_impl(runtime, std::move(arguments));
                           }));

    // engine 句柄族 (int64 opaque handle)
    wrapper->items.emplace(
        "register_engine",
        make_host_callable("ctx.register_engine", [runtime](std::vector<emma_value> arguments) {
            return register_engine_impl(runtime, std::move(arguments));
        }));
    wrapper->items.emplace(
        "get_engine",
        make_host_callable("ctx.get_engine", [runtime](std::vector<emma_value> arguments) {
            return get_engine_impl(runtime, std::move(arguments));
        }));
    wrapper->items.emplace(
        "require_engine",
        make_host_callable("ctx.require_engine", [runtime](std::vector<emma_value> arguments) {
            return require_engine_impl(runtime, std::move(arguments));
        }));
    {
        auto engine_dict = std::make_shared<emma_dict>();
        engine_dict->items.emplace("get", wrapper->items.at("get_engine"));
        engine_dict->items.emplace("require", wrapper->items.at("require_engine"));
        engine_dict->items.emplace("register", wrapper->items.at("register_engine"));
        engine_dict->items.emplace(
            "has",
            make_host_callable("ctx.engine.has", [runtime](std::vector<emma_value> arguments) {
                const std::string name = require_string(arguments, 0, "ctx.engine.has");
                void* engine = sao::plugins::loader::sao_plugins_ctx_get_engine(
                    runtime->context, name.c_str());
                return emma_value(engine != nullptr);
            }));
        // 反射引擎面 (binding_engine.h): engine.list() 目录枚举,
        // engine.on(channel, cb) 通用回调, catalog 每一条目一个命名
        // callable; 命名冲突时既有成员优先 (fail-closed 不覆盖)。
        engine_dict->items.emplace(
            "list", make_host_callable("ctx.engine.list",
                                       [runtime](std::vector<emma_value> arguments) {
                                           return engine_list_impl(runtime, std::move(arguments));
                                       }));
        engine_dict->items.emplace(
            "on", make_host_callable("ctx.engine.on",
                                     [runtime](std::vector<emma_value> arguments) {
                                         return engine_on_impl(runtime, std::move(arguments));
                                     }));
        const size_t engine_catalog_count =
            sao::plugins::sdk_binding::sdk_engine_catalog_size();
        for (size_t index = 0; index < engine_catalog_count; ++index) {
            const auto* desc = sao::plugins::sdk_binding::sdk_engine_catalog_at(index);
            if (desc == nullptr || desc->name == nullptr)
                continue;
            const std::string member = engine_member_name(desc->name);
            if (member.empty() || engine_dict->items.contains(member))
                continue;
            const std::string label = "ctx.engine." + member;
            engine_dict->items.emplace(
                member,
                make_host_callable(label.c_str(),
                                   [runtime, desc, label](std::vector<emma_value> arguments) {
                                       return engine_named_call(runtime, desc, label.c_str(),
                                                                std::move(arguments));
                                   }));
        }
        wrapper->items.emplace("engine", emma_value(std::move(engine_dict)));
    }

    // raw passthrough: {"name","args",["callback_channel"]} → {status,result}
    wrapper->items.emplace(
        "engine_call",
        make_host_callable("ctx.engine_call", [runtime](std::vector<emma_value> arguments) {
            return engine_call_impl(runtime, std::move(arguments));
        }));

    // register_thread: Emma 无并发线程模型, 语义为 no-op true (见报告)。
    wrapper->items.emplace(
        "register_thread",
        make_host_callable("ctx.register_thread", [](std::vector<emma_value> arguments) {
            (void)require_callable(arguments, 0, "ctx.register_thread");
            return emma_value(true);
        }));
    // run_on_ui: 无 UI dispatch ABI → 等价 0s one-shot 定时器。
    wrapper->items.emplace(
        "run_on_ui",
        make_host_callable("ctx.run_on_ui", [register_timer](std::vector<emma_value> args) {
            auto function = require_callable(args, 0, "ctx.run_on_ui");
            return register_timer({emma_value(std::move(function)), 0.0}, true);
        }));

    wrapper->items.emplace(
        "open_file",
        make_host_callable("ctx.open_file", [runtime](std::vector<emma_value> arguments) {
            return open_file_impl(runtime, std::move(arguments));
        }));
    wrapper->items.emplace(
        "open_window",
        make_host_callable("ctx.open_window", [runtime](std::vector<emma_value> arguments) {
            return open_window_impl(runtime, std::move(arguments));
        }));
    wrapper->items.emplace(
        "ensure_requirements",
        make_host_callable("ctx.ensure_requirements",
                           [runtime](std::vector<emma_value> arguments) {
                               return ensure_requirements_impl(runtime, std::move(arguments));
                           }));

    // compositor layer 族
    wrapper->items.emplace(
        "create_compositor_layer",
        make_host_callable("ctx.create_compositor_layer",
                           [runtime](std::vector<emma_value> arguments) {
                               return create_compositor_layer_impl(runtime, std::move(arguments));
                           }));
    wrapper->items.emplace(
        "upload_compositor_frame",
        make_host_callable("ctx.upload_compositor_frame",
                           [runtime](std::vector<emma_value> arguments) {
                               return upload_compositor_frame_impl(runtime, std::move(arguments));
                           }));
    wrapper->items.emplace(
        "set_compositor_layer_position",
        make_host_callable(
            "ctx.set_compositor_layer_position",
            [runtime](std::vector<emma_value> arguments) {
                return set_compositor_layer_position_impl(runtime, std::move(arguments));
            }));
    wrapper->items.emplace(
        "set_compositor_layer_visible",
        make_host_callable(
            "ctx.set_compositor_layer_visible",
            [runtime](std::vector<emma_value> arguments) {
                return set_compositor_layer_visible_impl(runtime, std::move(arguments));
            }));
    wrapper->items.emplace(
        "set_compositor_layer_input",
        make_host_callable(
            "ctx.set_compositor_layer_input",
            [runtime](std::vector<emma_value> arguments) {
                return set_compositor_layer_input_impl(runtime, std::move(arguments));
            }));
    wrapper->items.emplace(
        "destroy_compositor_layer",
        make_host_callable("ctx.destroy_compositor_layer",
                           [runtime](std::vector<emma_value> arguments) {
                               return destroy_compositor_layer_impl(runtime, std::move(arguments));
                           }));

    wrapper->items.emplace(
        "load_local",
        make_host_callable("ctx.load_local", [runtime](std::vector<emma_value> arguments) {
            return load_local_impl(runtime, std::move(arguments));
        }));

    // ctx.ui.* spec 构建器 → script_ui_build
    {
        auto ui_dict = std::make_shared<emma_dict>();
        size_t ui_count = 0;
        const char* const* ui_methods =
            sao::plugins::script_ctx::script_ui_methods(&ui_count);
        for (size_t index = 0; index < ui_count; ++index) {
            const std::string method(ui_methods[index]);
            ui_dict->items.emplace(
                method,
                make_host_callable(
                    ("ctx.ui." + method).c_str(),
                    [method](std::vector<emma_value> arguments) -> emma_value {
                        json args_json = json::array();
                        if (arguments.size() == 1 &&
                            std::holds_alternative<std::shared_ptr<emma_dict>>(arguments[0])) {
                            // kwargs 形式: 单 dict → json object
                            if (!value_to_json(arguments[0], args_json))
                                throw_context_status(("ctx.ui." + method).c_str(),
                                                     SAO_ERR_INVALID_ARGUMENT);
                        } else {
                            for (const auto& argument : arguments) {
                                json item;
                                if (!value_to_json(argument, item))
                                    throw_context_status(("ctx.ui." + method).c_str(),
                                                         SAO_ERR_INVALID_ARGUMENT);
                                args_json.push_back(std::move(item));
                            }
                        }
                        json node;
                        std::string error;
                        if (!sao::plugins::script_ctx::script_ui_build(
                                method.c_str(), args_json, node, error)) {
                            throw_context_message(("ctx.ui." + method).c_str(),
                                                  SAO_ERR_INVALID_ARGUMENT,
                                                  error.empty() ? "ui build failed" : error);
                        }
                        emma_value converted = nullptr;
                        if (!json_to_value(node, converted))
                            throw_context_status(("ctx.ui." + method).c_str(),
                                                 SAO_ERR_OS_CALL_FAILED);
                        return converted;
                    }));
        }
        wrapper->items.emplace("ui", emma_value(std::move(ui_dict)));
    }

    // event_bus 子域: 复用已绑定 callable。
    {
        auto event_bus = std::make_shared<emma_dict>();
        event_bus->items.emplace("subscribe", wrapper->items.at("subscribe"));
        event_bus->items.emplace("subscribe_once", wrapper->items.at("subscribe_once"));
        event_bus->items.emplace("unsubscribe", wrapper->items.at("unsubscribe"));
        event_bus->items.emplace("emit", wrapper->items.at("emit"));
        event_bus->items.emplace("get_snapshot", wrapper->items.at("get_snapshot"));
        event_bus->items.emplace("snapshot_value", wrapper->items.at("snapshot_value"));
        event_bus->items.emplace("recent_events", wrapper->items.at("recent_events"));
        wrapper->items.emplace("event_bus", emma_value(std::move(event_bus)));
    }

    const emma_value load_script = runtime->interp->get_global("load_script");
    if (std::holds_alternative<std::shared_ptr<callable>>(load_script)) {
        wrapper->items.emplace("load_script", load_script);
    }
    // provider 注册 + ctx surface 记录 (每 ctx 构造调用, 内部幂等)。
    ensure_emma_runtime_bridge();
    note_emma_ctx_surface();
    return wrapper;
}

int32_t install_context(emma_plugin_runtime* plugin, loader_context_t* context) {
    if (plugin == nullptr || context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    plugin->context = context;
    // 反射引擎面的 host 自有 SaoSdkContext (懒挂状态; 失败不阻断加载)。
    init_engine_sdk_context(*plugin);
    const int32_t activation_status = activate_emma_binding(*plugin);
    if (activation_status != SAO_OK)
        return activation_status;
    const int32_t binding_status = sao::plugins::sdk_binding::sao_plugins_binding_emma_register_ctx(
        reinterpret_cast<sao::plugins::sdk_binding::emma_interpreter_ptr>(plugin->interp.get()),
        reinterpret_cast<sao::plugins::sdk_binding::plugin_context_ptr>(context));
    emma_value native_context = make_native_context(plugin);
    if (binding_status == SAO_OK) {
        const emma_value candidate = plugin->interp->get_global("ctx");
        const auto* dictionary = std::get_if<std::shared_ptr<emma_dict>>(&candidate);
        auto native_dictionary = std::get<std::shared_ptr<emma_dict>>(native_context);
        if (dictionary == nullptr || *dictionary == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        constexpr std::array<std::string_view, 3> core_callables{
            "log", "register_ui_panel", "register_menu_category"};
        for (const auto& [name, value] : (*dictionary)->items) {
            if (name == "plugin_id" || name == "path")
                continue;
            const bool core =
                std::find(core_callables.begin(), core_callables.end(), name) !=
                core_callables.end();
            if (core) {
                const auto* function = std::get_if<std::shared_ptr<callable>>(&value);
                if (function == nullptr || *function == nullptr ||
                    (!(*function)->host_impl &&
                     ((*function)->body.empty() || (*function)->closure == nullptr))) {
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                if (name == "register_ui_panel") {
                    const auto provider_panel = *function;
                    native_dictionary->items.insert_or_assign(
                        name, make_host_callable(
                                  "ctx.register_ui_panel",
                                  [plugin, provider_panel](std::vector<emma_value> arguments) {
                                      (void)require_metadata_only_panel(arguments);
                                      std::string message;
                                      emma_error error;
                                      emma_value result = plugin->interp->call_function(
                                          provider_panel, std::move(arguments), message, &error);
                                      if (error.kind != error_kind::none || !message.empty()) {
                                          if (error.kind == error_kind::none) {
                                              error.kind = error_kind::runtime_error;
                                              error.status = SAO_ERR_OS_CALL_FAILED;
                                              error.message = std::move(message);
                                          }
                                          throw emma_exception(std::move(error));
                                      }
                                      return result;
                                  }));
                } else {
                    native_dictionary->items.insert_or_assign(name, value);
                }
                continue;
            }
            if (!native_dictionary->items.contains(name))
                native_dictionary->items.emplace(name, value);
        }
    } else if (binding_status != sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED) {
        return binding_status;
    }
    plugin->context_value = native_context;
    plugin->interp->register_global("ctx", plugin->context_value);
    const auto* dictionary = std::get_if<std::shared_ptr<emma_dict>>(&plugin->context_value);
    if (dictionary != nullptr && *dictionary != nullptr) {
        plugin->interp->register_global("log", (*dictionary)->items.at("log"));
    }
    return SAO_OK;
}

int32_t call_named(emma_plugin_runtime* plugin, const char* name, std::vector<emma_value> arguments,
                   emma_value* out_result, bool missing_is_success) {
    if (plugin == nullptr || name == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto function = plugin->interp->get_function(name);
    if (!function) {
        if (out_result != nullptr)
            *out_result = nullptr;
        if (missing_is_success) {
            plugin->last_error = {};
            return SAO_OK;
        }
        plugin->last_error.kind = error_kind::runtime_error;
        plugin->last_error.status = SAO_ERR_HANDLE_INVALID;
        plugin->last_error.message = std::string("unknown Emma hook: ") + name;
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string message;
    emma_error error;
    emma_value result =
        plugin->interp->call_function(function, std::move(arguments), message, &error);
    if (error.kind != error_kind::none || !message.empty()) {
        if (error.kind == error_kind::none) {
            error.kind = error_kind::runtime_error;
            error.status = SAO_ERR_OS_CALL_FAILED;
            error.message = std::move(message);
        }
        plugin->last_error = std::move(error);
        return plugin->last_error.status == SAO_OK ? SAO_ERR_OS_CALL_FAILED
                                                   : plugin->last_error.status;
    }
    plugin->last_error = {};
    if (out_result != nullptr)
        *out_result = std::move(result);
    return SAO_OK;
}

void set_api_error(emma_error* out_error, error_kind kind, int32_t status, std::string message) {
    if (out_error == nullptr)
        return;
    *out_error = {};
    out_error->kind = kind;
    out_error->status = status;
    out_error->message = std::move(message);
}

int32_t finish_failed_load(std::unique_ptr<emma_plugin_s> plugin, int32_t failure_status,
                           emma_plugin_handle_t* out_plugin, emma_error* out_error) {
    if (plugin == nullptr || plugin->runtime == nullptr)
        return failure_status;
    auto& runtime = *plugin->runtime;
    runtime.callbacks_accepting.store(false, std::memory_order_release);
    const int32_t provider_status = quiesce_context_providers(runtime);
    const int32_t teardown_status =
        provider_status == SAO_OK ? teardown_owned_resources(runtime) : provider_status;
    const int32_t engine_status =
        teardown_status == SAO_OK ? teardown_engine_surface(runtime) : teardown_status;
    int32_t closing_status =
        engine_status == SAO_OK ? deactivate_emma_binding(runtime) : engine_status;
    if (closing_status == SAO_OK) {
        for (auto& menu : runtime.menus)
            menu->runtime = nullptr;
        runtime.menus.clear();
        reset_action_bridge(runtime);
        return failure_status;
    }
    if (provider_status == SAO_OK) {
        const int32_t resume_status = register_context_providers(runtime);
        if (resume_status != SAO_OK)
            closing_status = resume_status;
    }

    if (out_error != nullptr) {
        if (out_error->kind == error_kind::none)
            out_error->kind = error_kind::runtime_error;
        out_error->status = closing_status;
        if (!out_error->message.empty())
            out_error->message += "; ";
        out_error->message += "Emma failed-load rollback is still closing with status " +
                              std::to_string(closing_status);
    }

    plugin->lifecycle = emma_plugin_s::state::closing;
    const emma_plugin_handle_t handle = plugin.get();
    auto& registry = direct_plugins();
    try {
        std::lock_guard lock(registry.mutex);
        registry.plugins.emplace(handle, std::move(plugin));
    } catch (...) {
        (void)plugin.release();
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (out_plugin != nullptr)
        *out_plugin = handle;
    return closing_status;
}

struct interpreter_callback_call final {
    emma_interpreter_callback_t callback = nullptr;
    interpreter* interp = nullptr;
    void* user_data = nullptr;
};

int32_t SAO_PLUGINS_CALL invoke_interpreter_callback(void* user_data) {
    auto* call = static_cast<interpreter_callback_call*>(user_data);
    if (call == nullptr || call->callback == nullptr || call->interp == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return call->callback(call->interp, call->user_data);
}

} // namespace

emma_plugin_runtime::~emma_plugin_runtime() {
    context_value = nullptr;
    for (auto& menu : menus)
        menu->runtime = nullptr;
    menus.clear();
    reset_action_bridge(*this);
    // 反射引擎面收尾 (best-effort): sao_sdk_context_destroy 内部
    // try_destroy, BUSY 走 SDK quarantine, 不传播状态。先销毁 sdk_context
    // 再退役 channel 记录 —— 销毁途中的末次回调仍能查表并被
    // invoke_callback 按 !accepting 丢弃。
    if (sdk_context != nullptr) {
        sao_sdk_context_destroy(sdk_context);
        sdk_context = nullptr;
    }
    {
        std::lock_guard lock(engine_callbacks_mutex);
        for (auto& [channel, record] : engine_callbacks)
            retire_callback(record);
        engine_callbacks.clear();
        for (auto& record : engine_callbacks_retired)
            retire_callback(record);
        engine_callbacks_retired.clear();
    }
    // bundle 里的 runtime 指针在 teardown 已清空; 这里兜底一次。
    for (auto& resource : resources) {
        switch (resource.kind) {
        case resource_kind::data_source:
            if (auto bundle = std::static_pointer_cast<emma_data_source_bundle>(
                    resource.attachment)) {
                bundle->runtime = nullptr;
                bundle->start.reset();
                bundle->stop.reset();
            }
            break;
        case resource_kind::compositor_input:
            if (auto input = std::static_pointer_cast<emma_compositor_input_bundle>(
                    resource.attachment)) {
                input->runtime = nullptr;
                input->cursor_pos.reset();
                input->mouse_button.reset();
                input->cursor_leave.reset();
                input->scroll.reset();
            }
            break;
        default:
            break;
        }
    }
    extension_handlers.clear();
    interp.reset();
    if (owns_context_lease) {
        sao::plugins::loader::plugin_context_release_host_lease(context);
        owns_context_lease = false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_load_script(
    const wchar_t* plugin_dir, const char* entry_relative, const char* plugin_id_utf8,
    void* ctx_ptr, emma_plugin_handle_t* out_plugin) {
    return sao_plugins_emma_load_script_ex(plugin_dir, entry_relative, plugin_id_utf8, ctx_ptr,
                                           out_plugin, nullptr);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_load_script_ex(
    const wchar_t* plugin_dir, const char* entry_relative, const char* plugin_id_utf8,
    void* ctx_ptr, emma_plugin_handle_t* out_plugin, emma_error* out_error) {
    (void)plugin_dir;
    (void)plugin_id_utf8;
    if (out_plugin != nullptr)
        *out_plugin = nullptr;
    if (out_error != nullptr)
        *out_error = {};
    if (out_plugin == nullptr || entry_relative == nullptr || entry_relative[0] == '\0' ||
        ctx_ptr == nullptr) {
        set_api_error(out_error, error_kind::runtime_error, SAO_ERR_INVALID_ARGUMENT,
                      "Emma load arguments are invalid");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<emma_plugin_s> plugin;
    try {
        plugin = std::make_unique<emma_plugin_s>();
        auto* context = static_cast<loader_context_t*>(ctx_ptr);
        const char* canonical_plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(context);
        const wchar_t* canonical_plugin_root = sao::plugins::loader::sao_plugins_ctx_path(context);
        if (canonical_plugin_id == nullptr || canonical_plugin_id[0] == '\0' ||
            canonical_plugin_root == nullptr || canonical_plugin_root[0] == L'\0') {
            set_api_error(out_error, error_kind::runtime_error, SAO_ERR_HANDLE_INVALID,
                          "canonical loader context identity is unavailable");
            return SAO_ERR_HANDLE_INVALID;
        }

        std::string source;
        std::filesystem::path entry_path;
        std::string read_error;
        const std::filesystem::path plugin_root(canonical_plugin_root);
        const int32_t read_status =
            read_emma_source_file(plugin_root, entry_relative, source, entry_path, read_error);
        if (read_status != SAO_OK) {
            set_api_error(out_error, error_kind::runtime_error, read_status,
                          "Emma entry load failed: " + read_error);
            return read_status;
        }

        plugin->runtime = std::make_unique<emma_plugin_runtime>();
        auto& runtime = *plugin->runtime;
        runtime.plugin_id = canonical_plugin_id;
        runtime.plugin_root = plugin_root;
        runtime.entry_path = entry_path;
        runtime.context = context;
        int32_t status = sao::plugins::loader::plugin_context_retain_host_lease(context);
        if (status != SAO_OK) {
            set_api_error(out_error, error_kind::runtime_error, status,
                          "canonical loader context lease could not be retained");
            return status;
        }
        runtime.owns_context_lease = true;
        runtime.interp = std::make_unique<interpreter>();
        runtime.interp->set_pool(&runtime.pool);
        status = sao_plugins_emma_install_stdlib(runtime.interp.get());
        if (status != SAO_OK) {
            set_api_error(out_error, error_kind::runtime_error, status,
                          "Emma standard library installation failed");
            return finish_failed_load(std::move(plugin), status, out_plugin, out_error);
        }
        status = sao_plugins_emma_install_io_stdlib(runtime.interp.get(), plugin_root.c_str());
        if (status != SAO_OK) {
            set_api_error(out_error, error_kind::runtime_error, status,
                          "Emma IO standard library installation failed");
            return finish_failed_load(std::move(plugin), status, out_plugin, out_error);
        }
        status = install_context(&runtime, context);
        if (status != SAO_OK) {
            set_api_error(out_error, error_kind::runtime_error, status,
                          "Emma plugin context installation failed");
            return finish_failed_load(std::move(plugin), status, out_plugin, out_error);
        }

        std::vector<token> tokens;
        try {
            tokens = tokenize_source(source);
        } catch (const emma_exception& exception) {
            if (out_error != nullptr)
                *out_error = exception.error();
            return finish_failed_load(std::move(plugin),
                                      sao_plugins_emma_error_status(&exception.error()), out_plugin,
                                      out_error);
        }
        parser source_parser(tokens, &runtime.pool);
        std::vector<node_id> statements;
        std::string parse_error;
        status = source_parser.parse_program(statements, parse_error);
        if (status != SAO_OK) {
            emma_error diagnostic;
            diagnostic.kind = error_kind::parse_error;
            diagnostic.status = status;
            diagnostic.message = std::move(parse_error);
            set_error_location_from_message(diagnostic);
            if (out_error != nullptr)
                *out_error = diagnostic;
            return finish_failed_load(std::move(plugin), sao_plugins_emma_error_status(&diagnostic),
                                      out_plugin, out_error);
        }
        emma_error execution_error;
        status = runtime.interp->execute(statements, parse_error, &execution_error);
        if (status != SAO_OK) {
            if (execution_error.kind == error_kind::none) {
                execution_error.kind = error_kind::runtime_error;
                execution_error.status = status;
                execution_error.message = std::move(parse_error);
            }
            if (out_error != nullptr)
                *out_error = execution_error;
            return finish_failed_load(std::move(plugin),
                                      sao_plugins_emma_error_status(&execution_error), out_plugin,
                                      out_error);
        }
        drain_pending_one_shots(runtime);

        const emma_plugin_handle_t handle = plugin.get();
        auto& registry = direct_plugins();
        {
            std::lock_guard lock(registry.mutex);
            registry.plugins.emplace(handle, std::move(plugin));
        }
        *out_plugin = handle;
        return SAO_OK;
    } catch (const emma_exception& exception) {
        if (out_error != nullptr)
            *out_error = exception.error();
        return finish_failed_load(std::move(plugin),
                                  sao_plugins_emma_error_status(&exception.error()), out_plugin,
                                  out_error);
    } catch (const std::exception& exception) {
        if (out_error != nullptr) {
            out_error->kind = error_kind::runtime_error;
            out_error->status = SAO_ERR_OS_CALL_FAILED;
            out_error->message = exception.what();
        }
        return finish_failed_load(std::move(plugin), SAO_ERR_OS_CALL_FAILED, out_plugin, out_error);
    } catch (...) {
        if (out_error != nullptr) {
            out_error->kind = error_kind::runtime_error;
            out_error->status = SAO_ERR_OS_CALL_FAILED;
            out_error->message = "unknown Emma load failure";
        }
        return finish_failed_load(std::move(plugin), SAO_ERR_OS_CALL_FAILED, out_plugin, out_error);
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_load(emma_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(plugin, [](emma_plugin_runtime& runtime) {
            return call_named(&runtime, "on_load", {runtime.context_value}, nullptr, true);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_enable(emma_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(plugin, [](emma_plugin_runtime& runtime) {
            const std::size_t checkpoint = runtime.menus.size();
            const emma_action_checkpoint action_before = action_checkpoint(runtime);
            const int32_t status = call_named(&runtime, "on_enable", {}, nullptr, true);
            if (status == SAO_OK) {
                const int32_t menu_status = commit_enable_menus(runtime, checkpoint);
                return menu_status == SAO_OK ? commit_enable_action(runtime, action_before)
                                             : menu_status;
            }
            const int32_t action_status = rollback_action(runtime, action_before);
            const int32_t menu_status = rollback_menus(runtime, checkpoint);
            if (action_status != SAO_OK)
                return action_status;
            return menu_status == SAO_OK ? status : menu_status;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_disable(emma_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(plugin, [](emma_plugin_runtime& runtime) {
            const int32_t status = call_named(&runtime, "on_disable", {}, nullptr, true);
            if (status != SAO_OK)
                return status;
            const int32_t action_status = remove_enable_action(runtime);
            const int32_t menu_status = remove_enable_menus(runtime);
            return action_status == SAO_OK ? menu_status : action_status;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_unload(emma_plugin_handle_t plugin, bool* out_allow_unload) {
    if (out_allow_unload)
        *out_allow_unload = true;
    if (plugin == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(plugin, [out_allow_unload](emma_plugin_runtime& runtime) {
            emma_value result = nullptr;
            const int32_t status = call_named(&runtime, "on_unload", {}, &result, true);
            if (status == SAO_OK && out_allow_unload != nullptr &&
                std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                *out_allow_unload = false;
            }
            return status;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_hook(emma_plugin_handle_t plugin, const char* hook_name,
                           const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8)
        *out_result_json_utf8 = nullptr;
    if (plugin == nullptr || hook_name == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        return with_direct_plugin(plugin, [&](emma_plugin_runtime& runtime) -> int32_t {
            runtime.last_error = {};
            std::vector<emma_value> arguments;
            if (args_json_utf8 != nullptr && args_json_utf8[0] != '\0') {
                json parsed;
                if (!parse_bounded_json_c_string(args_json_utf8, kMaximumJsonInputBytes, parsed) ||
                    !parsed.is_array()) {
                    set_api_error(&runtime.last_error, error_kind::runtime_error,
                                  SAO_ERR_INVALID_ARGUMENT,
                                  "Emma hook arguments must be a JSON array");
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                arguments.reserve(parsed.size());
                for (const auto& item : parsed) {
                    emma_value converted = nullptr;
                    if (!json_to_value(item, converted)) {
                        set_api_error(&runtime.last_error, error_kind::runtime_error,
                                      sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED,
                                      "Emma hook argument cannot be converted from JSON");
                        return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
                    }
                    arguments.push_back(std::move(converted));
                }
            }
            emma_value result = nullptr;
            const int32_t status =
                call_named(&runtime, hook_name, std::move(arguments), &result, false);
            if (status != SAO_OK || out_result_json_utf8 == nullptr) {
                return status;
            }
            json serialized;
            if (!value_to_json(result, serialized)) {
                set_api_error(&runtime.last_error, error_kind::runtime_error,
                              sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED,
                              "Emma hook result cannot be converted to JSON");
                return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
            }
            std::string text;
            std::string serialization_error;
            if (!detail::serialize_json(serialized, text, serialization_error)) {
                set_api_error(&runtime.last_error, error_kind::runtime_error,
                              SAO_ERR_INVALID_ARGUMENT, std::move(serialization_error));
                return SAO_ERR_INVALID_ARGUMENT;
            }
            auto* buffer = static_cast<char*>(std::malloc(text.size() + 1));
            if (buffer == nullptr) {
                set_api_error(&runtime.last_error, error_kind::runtime_error,
                              SAO_ERR_OS_CALL_FAILED, "Emma hook result allocation failed");
                return SAO_ERR_OS_CALL_FAILED;
            }
            std::memcpy(buffer, text.data(), text.size());
            buffer[text.size()] = '\0';
            *out_result_json_utf8 = buffer;
            return SAO_OK;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_call_hook_ex(
    emma_plugin_handle_t plugin, const char* hook_name, const char* args_json_utf8,
    char** out_result_json_utf8, emma_error* out_error) {
    if (out_result_json_utf8 != nullptr)
        *out_result_json_utf8 = nullptr;
    if (out_error != nullptr)
        *out_error = {};
    try {
        const int32_t status =
            sao_plugins_emma_call_hook(plugin, hook_name, args_json_utf8, out_result_json_utf8);
        if (status == SAO_OK || out_error == nullptr)
            return status;
        emma_error error;
        if (sao_plugins_emma_get_last_error(plugin, &error) == SAO_OK) {
            *out_error = std::move(error);
        } else {
            out_error->kind = error_kind::runtime_error;
            out_error->status = status;
            out_error->message = "Emma hook call failed with status " + std::to_string(status);
        }
        return status;
    } catch (...) {
        if (out_error != nullptr) {
            out_error->kind = error_kind::runtime_error;
            out_error->status = SAO_ERR_OS_CALL_FAILED;
            out_error->message = "unknown Emma hook failure";
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_get_last_error(emma_plugin_handle_t plugin, emma_error* out_error) {
    if (out_error != nullptr)
        *out_error = {};
    if (plugin == nullptr || out_error == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        return with_direct_plugin(plugin, [out_error](emma_plugin_runtime& runtime) {
            if (runtime.last_error.kind == error_kind::none) {
                return SAO_ERR_HANDLE_INVALID;
            }
            *out_error = runtime.last_error;
            return SAO_OK;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_emma_has_hook(emma_plugin_handle_t plugin, const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr)
        return false;
    try {
        bool result = false;
        const int32_t status = with_direct_plugin(plugin, [&](emma_plugin_runtime& runtime) {
            result = static_cast<bool>(runtime.interp->get_function(hook_name));
            return SAO_OK;
        });
        return status == SAO_OK && result;
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_with_interpreter(
    emma_plugin_handle_t plugin, emma_interpreter_callback_t callback, void* user_data) {
    if (callback == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        return with_direct_plugin(plugin, [&](emma_plugin_runtime& runtime) {
            interpreter_callback_call call{callback, runtime.interp.get(), user_data};
            return sao::plugins::sdk_binding::sao_plugins_binding_barrier(
                &invoke_interpreter_callback, &call, nullptr);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unload_script(emma_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    try {
        auto& registry = direct_plugins();
        emma_plugin_runtime* runtime_to_teardown = nullptr;
        {
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found == registry.plugins.end() || found->second == nullptr ||
                found->second->runtime == nullptr ||
                found->second->lifecycle == emma_plugin_s::state::destroyed) {
                return SAO_ERR_HANDLE_INVALID;
            }
            if (found->second->lifecycle == emma_plugin_s::state::tearing_down) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            if (found->second->lifecycle == emma_plugin_s::state::open) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            found->second->runtime->callbacks_accepting.store(false, std::memory_order_release);
            if (found->second->active_operations != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second->lifecycle = emma_plugin_s::state::tearing_down;
            runtime_to_teardown = found->second->runtime.get();
        }

        const int32_t provider_status = quiesce_context_providers(*runtime_to_teardown);
        if (provider_status != SAO_OK) {
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found != registry.plugins.end() && found->second != nullptr &&
                found->second->runtime.get() == runtime_to_teardown) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            return provider_status;
        }

        const int32_t teardown_status = teardown_owned_resources(*runtime_to_teardown);
        if (teardown_status != SAO_OK) {
            const int32_t resume_status = register_context_providers(*runtime_to_teardown);
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found != registry.plugins.end() && found->second != nullptr &&
                found->second->runtime.get() == runtime_to_teardown) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            return resume_status == SAO_OK ? teardown_status : resume_status;
        }

        const int32_t engine_status = teardown_engine_surface(*runtime_to_teardown);
        if (engine_status != SAO_OK) {
            const int32_t resume_status = register_context_providers(*runtime_to_teardown);
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found != registry.plugins.end() && found->second != nullptr &&
                found->second->runtime.get() == runtime_to_teardown) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            return resume_status == SAO_OK ? engine_status : resume_status;
        }

        const int32_t binding_status = deactivate_emma_binding(*runtime_to_teardown);
        if (binding_status != SAO_OK) {
            const int32_t resume_status = register_context_providers(*runtime_to_teardown);
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found != registry.plugins.end() && found->second != nullptr &&
                found->second->runtime.get() == runtime_to_teardown) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            return resume_status == SAO_OK ? binding_status : resume_status;
        }

        for (auto& menu : runtime_to_teardown->menus)
            menu->runtime = nullptr;
        runtime_to_teardown->menus.clear();
        reset_action_bridge(*runtime_to_teardown);

        std::unique_ptr<emma_plugin_runtime> runtime;
        {
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found == registry.plugins.end() || found->second == nullptr ||
                found->second->runtime.get() != runtime_to_teardown) {
                return SAO_ERR_HANDLE_INVALID;
            }
            found->second->lifecycle = emma_plugin_s::state::destroyed;
            runtime = std::move(found->second->runtime);
        }
        runtime.reset();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── host-level 内存源码便利函数 ──
//
// task 描述里说的 sao_emma_host_execute/call_function 走这个门。签名式对齐:
//   sao_plugins_emma_execute_source(interp, source, source_len, out_result_utf8)
//   sao_plugins_emma_call_by_name(interp, fn_name, out_result_utf8)
//
// 供 test / repl 用。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_execute_source(interpreter* interp, const char* source_utf8, size_t source_len,
                                ast_pool* out_pool, char** out_error_utf8) {
    if (out_error_utf8)
        *out_error_utf8 = nullptr;
    if (interp == nullptr || source_utf8 == nullptr || out_pool == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (source_len > kMaximumEmmaSourceBytes) {
        constexpr std::string_view message = "Emma source exceeds the 8 MiB size limit";
        if (out_error_utf8 != nullptr) {
            auto* buffer = static_cast<char*>(std::malloc(message.size() + 1));
            if (buffer == nullptr)
                return SAO_ERR_OS_CALL_FAILED;
            std::memcpy(buffer, message.data(), message.size());
            buffer[message.size()] = '\0';
            *out_error_utf8 = buffer;
        }
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        interp->set_pool(out_pool);
        std::vector<token> tokens = tokenize_source(std::string_view(source_utf8, source_len));
        parser source_parser(tokens, out_pool);
        std::vector<node_id> statements;
        std::string error;
        int32_t status = source_parser.parse_program(statements, error);
        if (status == SAO_OK) {
            status = interp->execute(statements, error);
        }
        if (status != SAO_OK && out_error_utf8 != nullptr) {
            auto* buffer = static_cast<char*>(std::malloc(error.size() + 1));
            if (buffer == nullptr)
                return SAO_ERR_OS_CALL_FAILED;
            std::memcpy(buffer, error.data(), error.size());
            buffer[error.size()] = '\0';
            *out_error_utf8 = buffer;
        }
        return status;
    } catch (const emma_exception& exception) {
        if (out_error_utf8 != nullptr) {
            const std::string& message = exception.error().message;
            auto* buffer = static_cast<char*>(std::malloc(message.size() + 1));
            if (buffer == nullptr)
                return SAO_ERR_OS_CALL_FAILED;
            std::memcpy(buffer, message.data(), message.size());
            buffer[message.size()] = '\0';
            *out_error_utf8 = buffer;
        }
        return sao_plugins_emma_error_status(&exception.error());
    } catch (...) {
        constexpr std::string_view message = "unknown Emma source execution failure";
        if (out_error_utf8 != nullptr) {
            auto* buffer = static_cast<char*>(std::malloc(message.size() + 1));
            if (buffer == nullptr)
                return SAO_ERR_OS_CALL_FAILED;
            std::memcpy(buffer, message.data(), message.size());
            buffer[message.size()] = '\0';
            *out_error_utf8 = buffer;
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_emma_free_string(char* s) {
    if (s != nullptr)
        std::free(s);
}

} // namespace sao::plugins::emma_host
