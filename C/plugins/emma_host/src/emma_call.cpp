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
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_emma.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
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

struct emma_plugin_runtime {
    struct callback_record;

    ~emma_plugin_runtime();

    enum class resource_kind {
        event_subscription,
        hotkey,
        timer,
        notification,
        overlay,
    };

    struct owned_resource {
        uint64_t id = 0;
        resource_kind kind = resource_kind::event_subscription;
        uint32_t numeric_token = 0;
        std::string key;
        std::shared_ptr<callback_record> callback;
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
    std::atomic_bool callbacks_accepting{true};
    std::mutex resources_mutex;
    std::vector<owned_resource> resources;
    std::vector<std::unique_ptr<emma_menu_bridge>> menus;
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
        if (callback_->one_shot_phase !=
            emma_plugin_runtime::callback_record::one_shot_state::armed) {
            return false;
        }
        callback_->one_shot_phase = emma_plugin_runtime::callback_record::one_shot_state::claimed;
        callback_->accepting = false;
        return true;
    }

    void defer(std::vector<emma_value> arguments) {
        if (callback_ == nullptr)
            return;
        std::lock_guard lock(callback_->mutex);
        if (callback_->one_shot &&
            callback_->one_shot_phase ==
                emma_plugin_runtime::callback_record::one_shot_state::armed) {
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

int32_t SAO_PLUGINS_CALL native_menu_snapshot(sao::plugins::loader::entity_menu_row* rows,
                                              std::uint32_t capacity, std::uint32_t* out_count,
                                              std::uint64_t* out_revision, void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr ||
        (capacity > 0 && rows == nullptr)) {
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
        if (capacity < bridge.rows.size())
            return SAO_ERR_BUFFER_TOO_SMALL;
        for (std::size_t index = 0; index < bridge.rows.size(); ++index) {
            const auto& source = bridge.rows[index];
            rows[index] = {
                sizeof(sao::plugins::loader::entity_menu_row),
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
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL native_menu_action(const char* action_id_utf8, const char*,
                                            void* user_data) {
    if (action_id_utf8 == nullptr || user_data == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
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
        if (found == bridge.actions.end() || found->second == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        std::string message;
        emma_error error;
        (void)runtime->interp->call_function(found->second, {}, message, &error);
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
        runtime->last_error = {};
        return SAO_OK;
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

        sao::plugins::loader::context_entity_provider_descriptor provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = menu.provider_id.c_str();
        provider.snapshot = native_menu_snapshot;
        provider.action_handler = native_menu_action;
        provider.user_data = &menu;
        provider.root_contribution = &root;
        return sao::plugins::loader::sao_plugins_ctx_register_entity_provider(runtime.context,
                                                                              &provider);
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

int32_t quiesce_menu_providers(emma_plugin_runtime& runtime) noexcept {
    try {
        return unregister_menu_providers(runtime, menu_range(runtime, 0));
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
                            std::shared_ptr<emma_plugin_runtime::callback_record> callback = {}) {
    std::lock_guard lock(runtime.resources_mutex);
    const uint64_t id = runtime.next_resource_id++;
    runtime.resources.push_back({id, kind, numeric_token, std::move(key), std::move(callback)});
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
        make_host_callable("ctx.subscribe", [subscribe](std::vector<emma_value> arguments) mutable {
            return subscribe(std::move(arguments), false);
        }));
    wrapper->items.emplace(
        "subscribe_once",
        make_host_callable("ctx.subscribe_once",
                           [subscribe](std::vector<emma_value> arguments) mutable {
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
                           [register_timer](std::vector<emma_value> arguments) mutable {
                               return register_timer(std::move(arguments), false);
                           }));
    wrapper->items.emplace(
        "set_timeout",
        make_host_callable("ctx.set_timeout",
                           [register_timer](std::vector<emma_value> arguments) mutable {
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

    const emma_value load_script = runtime->interp->get_global("load_script");
    if (std::holds_alternative<std::shared_ptr<callable>>(load_script)) {
        wrapper->items.emplace("load_script", load_script);
    }
    return wrapper;
}

int32_t install_context(emma_plugin_runtime* plugin, loader_context_t* context) {
    if (plugin == nullptr || context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    plugin->context = context;
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
    const int32_t menu_status = quiesce_menu_providers(runtime);
    const int32_t teardown_status =
        menu_status == SAO_OK ? teardown_owned_resources(runtime) : menu_status;
    int32_t closing_status =
        teardown_status == SAO_OK ? deactivate_emma_binding(runtime) : teardown_status;
    if (closing_status == SAO_OK) {
        for (auto& menu : runtime.menus)
            menu->runtime = nullptr;
        runtime.menus.clear();
        return failure_status;
    }
    if (menu_status == SAO_OK) {
        const int32_t resume_status = register_menu_providers(runtime);
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
            const int32_t status = call_named(&runtime, "on_enable", {}, nullptr, true);
            if (status == SAO_OK)
                return commit_enable_menus(runtime, checkpoint);
            const int32_t rollback_status = rollback_menus(runtime, checkpoint);
            return rollback_status == SAO_OK ? status : rollback_status;
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
            return status == SAO_OK ? remove_enable_menus(runtime) : status;
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

        const int32_t menu_status = quiesce_menu_providers(*runtime_to_teardown);
        if (menu_status != SAO_OK) {
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found != registry.plugins.end() && found->second != nullptr &&
                found->second->runtime.get() == runtime_to_teardown) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            return menu_status;
        }

        const int32_t teardown_status = teardown_owned_resources(*runtime_to_teardown);
        if (teardown_status != SAO_OK) {
            const int32_t resume_status = register_menu_providers(*runtime_to_teardown);
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found != registry.plugins.end() && found->second != nullptr &&
                found->second->runtime.get() == runtime_to_teardown) {
                found->second->lifecycle = emma_plugin_s::state::closing;
            }
            return resume_status == SAO_OK ? teardown_status : resume_status;
        }

        const int32_t binding_status = deactivate_emma_binding(*runtime_to_teardown);
        if (binding_status != SAO_OK) {
            const int32_t resume_status = register_menu_providers(*runtime_to_teardown);
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
