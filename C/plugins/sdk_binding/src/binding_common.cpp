// binding_common.cpp — 多语言 SDK binding 共享实现
//
// 提供:
//   1. json_node 与 JSON 文本双向转换
//   2. provider 调用与跨语言回调生命周期保护
//   3. plugin_binding_s 定义 (5 语言共享的 runtime binding 状态)
//   4. sao_plugins_sdk_bind_call 中央 dispatcher
//   5. test 辅助 API (has_hotkey / event_count / last_log)

#include "sao/plugins/sdk_binding/binding_common.h"

#include "sao/plugins/loader/plugin_context.h"

#include <algorithm>
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

#if defined(_WIN32) && defined(_MSC_VER)
#include <excpt.h>
#endif

namespace sao::plugins::sdk_binding {

namespace {

using ordered_json = nlohmann::ordered_json;

json_node json_to_node(const ordered_json& value) {
    if (value.is_null())
        return json_node{nullptr};
    if (value.is_boolean())
        return json_node{value.get<bool>()};
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return json_node{value.get<int64_t>()};
    }
    if (value.is_number_float())
        return json_node{value.get<double>()};
    if (value.is_string())
        return json_node{value.get<std::string>()};
    if (value.is_array()) {
        std::vector<json_node> output;
        output.reserve(value.size());
        for (const auto& item : value)
            output.push_back(json_to_node(item));
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
    return std::visit(
        [](const auto& value) -> ordered_json {
            using value_t = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_t, std::vector<json_node>>) {
                auto output = ordered_json::array();
                for (const auto& item : value)
                    output.push_back(node_to_json(item));
                return output;
            } else if constexpr (std::is_same_v<value_t,
                                                std::vector<std::pair<std::string, json_node>>>) {
                auto output = ordered_json::object();
                for (const auto& [key, item] : value)
                    output[key] = node_to_json(item);
                return output;
            } else {
                return ordered_json(value);
            }
        },
        node.value);
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
    enum class language_kind : uint8_t { python = 0, lua, angel, emma, csharp };
    language_kind lang = language_kind::python;

    language_host_kind host_kind = language_host_kind::python;
    language_host_adapter_vtable host{};
    void* provider_plugin = nullptr;
    std::string last_error;

    enum class lifecycle_state : uint8_t {
        loading,
        ready,
        unloading,
        dead,
    } lifecycle = lifecycle_state::loading;
    uint64_t host_generation = 0;
    size_t active_calls = 0;
    uint64_t callback_sequence = 0;
    int32_t callback_release_status = SAO_OK;

    // 关联的 sao_plugins_ctx (来自 loader/plugin_context.h)
    void* ctx = nullptr;

    // 关联的语言侧 handle (PyObject*/lua_State*/asIScriptEngine*/...)
    void* lang_state = nullptr;

    // 记录 add_hotkey 注册，供兼容查询与测试内省。
    std::unordered_set<std::string> hotkeys;

    // 记录 publish_event: topic → count。
    std::unordered_map<std::string, uint32_t> event_counts;

    // 最近一条 log，供测试内省。
    std::string last_log;

    // 记录最近一次 plugin_id 查询结果。
    std::string last_plugin_id_query;

    struct callback_record {
        void* callback_user_data = nullptr;
        release_callback_fn legacy_release = nullptr;
        void(SAO_PLUGINS_CALL* provider_release)(void*, void*) = nullptr;
        void* provider_user_data = nullptr;
        language_host_kind language = language_host_kind::python;
        uint64_t sequence = 0;
        bool claimed = false;
    };
    std::vector<std::shared_ptr<callback_record>> callbacks;

    std::mutex mu;
};

namespace {

struct host_record {
    language_host_adapter_vtable adapter{};
    bool present = false;
    uint64_t generation = 0;
    size_t active_calls = 0;
    size_t active_bindings = 0;
};

std::mutex g_hosts_mutex;
std::array<host_record, static_cast<size_t>(language_host_kind::count_)> g_hosts;
uint64_t g_next_host_generation = 1;

std::mutex g_bindings_mutex;
std::vector<plugin_binding_s*> g_bindings;

std::mutex g_callbacks_mutex;
std::vector<std::shared_ptr<plugin_binding_s::callback_record>> g_unscoped_callbacks;

thread_local plugin_binding_s* g_current_binding = nullptr;

constexpr bool valid_language(language_host_kind language) noexcept {
    return static_cast<size_t>(language) < static_cast<size_t>(language_host_kind::count_);
}

bool adapter_has_release_callback(const language_host_adapter_vtable& adapter) noexcept {
    constexpr size_t required_size =
        offsetof(language_host_adapter_vtable, release_callback) + sizeof(adapter.release_callback);
    return adapter.struct_size >= required_size && adapter.release_callback != nullptr;
}

language_host_adapter_vtable
snapshot_adapter(const language_host_adapter_vtable* adapter) noexcept {
    language_host_adapter_vtable snapshot{};
    const size_t copy_size = std::min<size_t>(adapter->struct_size, sizeof(snapshot));
    std::memcpy(&snapshot, adapter, copy_size);
    if (!adapter_has_release_callback(snapshot)) {
        snapshot.release_callback = nullptr;
    }
    return snapshot;
}

int32_t unsupported() noexcept {
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

class current_binding_scope {
  public:
    explicit current_binding_scope(plugin_binding_s* binding) noexcept
        : previous_(g_current_binding) {
        g_current_binding = binding;
    }

    ~current_binding_scope() {
        g_current_binding = previous_;
    }

    current_binding_scope(const current_binding_scope&) = delete;
    current_binding_scope& operator=(const current_binding_scope&) = delete;

  private:
    plugin_binding_s* previous_ = nullptr;
};

class host_call_lease {
  public:
    host_call_lease() = default;
    ~host_call_lease() {
        reset();
    }

    host_call_lease(const host_call_lease&) = delete;
    host_call_lease& operator=(const host_call_lease&) = delete;

    int32_t acquire(language_host_kind language, uint64_t expected_generation = 0) noexcept {
        if (!valid_language(language))
            return SAO_ERR_INVALID_ARGUMENT;
        try {
            std::lock_guard lock(g_hosts_mutex);
            auto& record = g_hosts[static_cast<size_t>(language)];
            if (!record.present ||
                (expected_generation != 0 && record.generation != expected_generation)) {
                return unsupported();
            }
            ++record.active_calls;
            language_ = language;
            generation_ = record.generation;
            adapter_ = record.adapter;
            active_ = true;
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    int32_t promote_binding() noexcept {
        if (!active_)
            return SAO_ERR_NOT_INITIALIZED;
        try {
            std::lock_guard lock(g_hosts_mutex);
            auto& record = g_hosts[static_cast<size_t>(language_)];
            if (!record.present || record.generation != generation_) {
                return unsupported();
            }
            ++record.active_bindings;
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    const language_host_adapter_vtable& adapter() const noexcept {
        return adapter_;
    }

    uint64_t generation() const noexcept {
        return generation_;
    }

    void reset() noexcept {
        if (!active_)
            return;
        try {
            std::lock_guard lock(g_hosts_mutex);
            auto& record = g_hosts[static_cast<size_t>(language_)];
            if (record.generation == generation_ && record.active_calls > 0) {
                --record.active_calls;
            }
            active_ = false;
        } catch (...) {
        }
    }

  private:
    language_host_adapter_vtable adapter_{};
    language_host_kind language_ = language_host_kind::python;
    uint64_t generation_ = 0;
    bool active_ = false;
};

void release_host_binding(language_host_kind language, uint64_t generation) noexcept {
    try {
        std::lock_guard lock(g_hosts_mutex);
        auto& record = g_hosts[static_cast<size_t>(language)];
        if (record.generation == generation && record.active_bindings > 0) {
            --record.active_bindings;
        }
    } catch (...) {
    }
}

class binding_call_lease {
  public:
    binding_call_lease() = default;
    ~binding_call_lease() {
        reset();
    }

    binding_call_lease(const binding_call_lease&) = delete;
    binding_call_lease& operator=(const binding_call_lease&) = delete;

    int32_t acquire(plugin_binding_s* binding, bool allow_loading = false) noexcept {
        if (binding == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        try {
            std::lock_guard registry_lock(g_bindings_mutex);
            if (std::find(g_bindings.begin(), g_bindings.end(), binding) == g_bindings.end()) {
                return SAO_ERR_HANDLE_INVALID;
            }
            std::lock_guard binding_lock(binding->mu);
            if (binding->lifecycle != plugin_binding_s::lifecycle_state::ready &&
                !(allow_loading &&
                  binding->lifecycle == plugin_binding_s::lifecycle_state::loading)) {
                return binding->lifecycle == plugin_binding_s::lifecycle_state::unloading
                           ? loader::SAO_PLUGINS_ERR_BUSY
                           : SAO_ERR_HANDLE_INVALID;
            }
            ++binding->active_calls;
            binding_ = binding;
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    plugin_binding_s* get() const noexcept {
        return binding_;
    }

    void adopt_locked(plugin_binding_s* binding) noexcept {
        binding_ = binding;
    }

    void reset() noexcept {
        if (binding_ == nullptr)
            return;
        try {
            std::lock_guard lock(binding_->mu);
            if (binding_->active_calls > 0)
                --binding_->active_calls;
            binding_ = nullptr;
        } catch (...) {
        }
    }

  private:
    plugin_binding_s* binding_ = nullptr;
};

int32_t acquire_callback_owner(language_host_kind language, void* runtime,
                               binding_call_lease& lease) noexcept {
    if (runtime == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        if (g_current_binding != nullptr && g_current_binding->host_kind == language &&
            g_current_binding->lang_state == runtime) {
            return lease.acquire(g_current_binding, true);
        }

        plugin_binding_s* candidate = nullptr;
        {
            std::lock_guard registry_lock(g_bindings_mutex);
            for (auto* binding : g_bindings) {
                std::lock_guard binding_lock(binding->mu);
                if (binding->lifecycle != plugin_binding_s::lifecycle_state::ready ||
                    binding->host_kind != language || binding->lang_state != runtime) {
                    continue;
                }
                if (candidate != nullptr) {
                    return loader::SAO_PLUGINS_ERR_BUSY;
                }
                candidate = binding;
            }
        }
        return candidate == nullptr ? SAO_ERR_NOT_INITIALIZED : lease.acquire(candidate);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

struct available_call {
    language_host_adapter_vtable adapter{};
    bool available = false;
};

int32_t SAO_PLUGINS_CALL call_available(void* opaque) {
    auto* call = static_cast<available_call*>(opaque);
    call->available = call->adapter.available(call->adapter.user_data);
    return SAO_OK;
}

bool host_available(const language_host_adapter_vtable& adapter) noexcept {
    if (adapter.available == nullptr)
        return true;
    available_call call{adapter, false};
    return sao_plugins_binding_barrier(&call_available, &call, nullptr) == SAO_OK && call.available;
}

char* duplicate_error(const char* message) noexcept {
    if (message == nullptr || message[0] == '\0')
        return nullptr;
    const size_t length = std::strlen(message);
    auto copy = std::unique_ptr<char[]>(new (std::nothrow) char[length + 1]);
    if (!copy)
        return nullptr;
    std::memcpy(copy.get(), message, length + 1);
    return copy.release();
}

#if defined(_WIN32) && defined(_MSC_VER)
int seh_filter(unsigned int exception_code) noexcept {
    constexpr unsigned int kMsvcCppException = 0xe06d7363U;
    return exception_code == kMsvcCppException ? EXCEPTION_CONTINUE_SEARCH
                                               : EXCEPTION_EXECUTE_HANDLER;
}

int32_t invoke_with_seh(barrier_fn fn, void* user_data, bool* out_faulted) {
    __try {
        return fn(user_data);
    } __except (seh_filter(GetExceptionCode())) {
        *out_faulted = true;
        return SAO_ERR_OS_CALL_FAILED;
    }
}
#endif

int32_t copy_to_caller(std::string_view value, char* output, size_t capacity,
                       size_t* required) noexcept {
    if (required != nullptr)
        *required = value.size() + 1;
    if (output == nullptr || capacity < value.size() + 1) {
        if (output != nullptr && capacity > 0)
            output[0] = '\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = '\0';
    return SAO_OK;
}

std::shared_ptr<plugin_binding_s::callback_record>
make_legacy_callback(void* user_data, release_callback_fn release_fn) {
    auto callback = std::make_shared<plugin_binding_s::callback_record>();
    callback->callback_user_data = user_data;
    callback->legacy_release = release_fn;
    return callback;
}

std::shared_ptr<plugin_binding_s::callback_record>
make_provider_callback(plugin_binding_s* binding, void* callback_user_data) {
    auto callback = std::make_shared<plugin_binding_s::callback_record>();
    callback->callback_user_data = callback_user_data;
    callback->provider_release = binding->host.release_callback;
    callback->provider_user_data = binding->host.user_data;
    callback->language = binding->host_kind;
    return callback;
}

struct release_call {
    std::shared_ptr<plugin_binding_s::callback_record> callback;
};

int32_t SAO_PLUGINS_CALL call_release(void* opaque) {
    auto* call = static_cast<release_call*>(opaque);
    if (call->callback->provider_release != nullptr) {
        call->callback->provider_release(call->callback->callback_user_data,
                                         call->callback->provider_user_data);
    } else {
        call->callback->legacy_release(call->callback->callback_user_data);
    }
    return SAO_OK;
}

int32_t release_callback_record(
    const std::shared_ptr<plugin_binding_s::callback_record>& callback) noexcept {
    release_call call{callback};
    return sao_plugins_binding_barrier(&call_release, &call, nullptr);
}

int32_t release_callbacks(
    const std::vector<std::shared_ptr<plugin_binding_s::callback_record>>& callbacks) noexcept {
    int32_t first_error = SAO_OK;
    for (auto iterator = callbacks.rbegin(); iterator != callbacks.rend(); ++iterator) {
        const int32_t status = release_callback_record(*iterator);
        if (first_error == SAO_OK && status != SAO_OK)
            first_error = status;
    }
    return first_error;
}

void record_callback_release_status(plugin_binding_s* binding, int32_t status) noexcept {
    if (binding == nullptr || status == SAO_OK)
        return;
    try {
        std::lock_guard lock(binding->mu);
        if (binding->callback_release_status == SAO_OK) {
            binding->callback_release_status = status;
        }
    } catch (...) {
    }
}

std::vector<std::shared_ptr<plugin_binding_s::callback_record>>
claim_all_callbacks(plugin_binding_s* binding) {
    std::vector<std::shared_ptr<plugin_binding_s::callback_record>> claimed;
    std::lock_guard lock(binding->mu);
    claimed.swap(binding->callbacks);
    for (const auto& callback : claimed) {
        callback->claimed = true;
    }
    return claimed;
}

std::shared_ptr<plugin_binding_s::callback_record>
claim_callback(language_host_kind language, void* user_data, binding_call_lease& lease) noexcept {
    if (user_data == nullptr)
        return {};
    try {
        std::lock_guard registry_lock(g_bindings_mutex);
        plugin_binding_s* owner = nullptr;
        std::shared_ptr<plugin_binding_s::callback_record> candidate;
        bool ambiguous = false;
        const auto inspect = [&](plugin_binding_s* binding) {
            if (ambiguous || binding->host_kind != language)
                return;
            std::lock_guard binding_lock(binding->mu);
            if (binding->lifecycle != plugin_binding_s::lifecycle_state::ready &&
                binding->lifecycle != plugin_binding_s::lifecycle_state::loading) {
                return;
            }
            for (const auto& callback : binding->callbacks) {
                if (callback->claimed || callback->callback_user_data != user_data) {
                    continue;
                }
                if (candidate != nullptr) {
                    candidate.reset();
                    owner = nullptr;
                    ambiguous = true;
                    return;
                }
                candidate = callback;
                owner = binding;
            }
        };
        if (g_current_binding != nullptr)
            inspect(g_current_binding);
        if (candidate == nullptr && !ambiguous) {
            for (auto* binding : g_bindings) {
                if (binding == g_current_binding)
                    continue;
                inspect(binding);
                if (ambiguous)
                    return {};
            }
        }
        if (candidate == nullptr || owner == nullptr)
            return {};
        {
            std::lock_guard owner_lock(owner->mu);
            if (candidate->claimed ||
                (owner->lifecycle != plugin_binding_s::lifecycle_state::ready &&
                 owner->lifecycle != plugin_binding_s::lifecycle_state::loading)) {
                return {};
            }
            candidate->claimed = true;
            const auto found =
                std::find(owner->callbacks.begin(), owner->callbacks.end(), candidate);
            if (found == owner->callbacks.end())
                return {};
            owner->callbacks.erase(found);
            ++owner->active_calls;
            lease.adopt_locked(owner);
        }
        return candidate;
    } catch (...) {
        return {};
    }
}

uint64_t callback_watermark(plugin_binding_s* binding) noexcept {
    try {
        std::lock_guard lock(binding->mu);
        return binding->callback_sequence;
    } catch (...) {
        return 0;
    }
}

bool callback_tracked_after(plugin_binding_s* binding, void* user_data,
                            uint64_t watermark) noexcept {
    try {
        std::lock_guard lock(binding->mu);
        return std::any_of(binding->callbacks.begin(), binding->callbacks.end(),
                           [user_data, watermark](const auto& callback) {
                               return callback->sequence > watermark &&
                                      callback->callback_user_data == user_data &&
                                      !callback->claimed;
                           });
    } catch (...) {
        return false;
    }
}

std::shared_ptr<plugin_binding_s::callback_record>
claim_callback_tracked_after(plugin_binding_s* binding, void* user_data,
                             uint64_t watermark) noexcept {
    if (binding == nullptr || user_data == nullptr)
        return {};
    try {
        std::lock_guard lock(binding->mu);
        const auto found = std::find_if(binding->callbacks.begin(), binding->callbacks.end(),
                                        [user_data, watermark](const auto& callback) {
                                            return callback->sequence > watermark &&
                                                   callback->callback_user_data == user_data &&
                                                   !callback->claimed;
                                        });
        if (found == binding->callbacks.end())
            return {};
        auto callback = *found;
        callback->claimed = true;
        binding->callbacks.erase(found);
        return callback;
    } catch (...) {
        return {};
    }
}

int32_t rollback_wrapped_callback(
    plugin_binding_s* binding, void* user_data, uint64_t watermark,
    const std::shared_ptr<plugin_binding_s::callback_record>& fallback) noexcept {
    auto callback = claim_callback_tracked_after(binding, user_data, watermark);
    if (callback == nullptr)
        callback = fallback;
    if (callback == nullptr)
        return SAO_OK;
    callback->callback_user_data = user_data;
    const int32_t status = release_callback_record(callback);
    record_callback_release_status(binding, status);
    return status;
}

int32_t append_callback(plugin_binding_s* binding,
                        const std::shared_ptr<plugin_binding_s::callback_record>& callback) {
    try {
        std::lock_guard lock(binding->mu);
        if (binding->lifecycle != plugin_binding_s::lifecycle_state::loading &&
            binding->lifecycle != plugin_binding_s::lifecycle_state::ready) {
            return loader::SAO_PLUGINS_ERR_BUSY;
        }
        callback->sequence = ++binding->callback_sequence;
        binding->callbacks.push_back(callback);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t register_binding(plugin_binding_s* binding) {
    try {
        std::lock_guard lock(g_bindings_mutex);
        g_bindings.push_back(binding);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void unregister_binding(plugin_binding_s* binding) noexcept {
    try {
        std::lock_guard lock(g_bindings_mutex);
        const auto found = std::find(g_bindings.begin(), g_bindings.end(), binding);
        if (found != g_bindings.end())
            g_bindings.erase(found);
    } catch (...) {
    }
}

class binding_registration_guard {
  public:
    binding_registration_guard(plugin_binding_s* binding, language_host_kind language,
                               uint64_t generation) noexcept
        : binding_(binding), language_(language), generation_(generation) {}

    ~binding_registration_guard() {
        if (registered_)
            unregister_binding(binding_);
        if (promoted_)
            release_host_binding(language_, generation_);
    }

    binding_registration_guard(const binding_registration_guard&) = delete;
    binding_registration_guard& operator=(const binding_registration_guard&) = delete;

    void mark_promoted() noexcept {
        promoted_ = true;
    }
    void mark_registered() noexcept {
        registered_ = true;
    }
    void dismiss() noexcept {
        promoted_ = false;
        registered_ = false;
    }

  private:
    plugin_binding_s* binding_ = nullptr;
    language_host_kind language_ = language_host_kind::python;
    uint64_t generation_ = 0;
    bool promoted_ = false;
    bool registered_ = false;
};

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_register_language_host(const language_host_adapter_vtable* adapter) {
    if (adapter == nullptr || adapter->struct_size < SAO_LANGUAGE_HOST_ADAPTER_V1_SIZE ||
        adapter->abi_version != SAO_LANGUAGE_HOST_PROVIDER_ABI_VERSION ||
        !valid_language(adapter->language) || adapter->load_plugin == nullptr ||
        adapter->unload_plugin == nullptr || adapter->invoke == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (adapter->language == language_host_kind::python &&
        (adapter->flags & SAO_LANGUAGE_HOST_PROVIDER_ISOLATED_PYTHON_ABI) == 0) {
        return unsupported();
    }
    try {
        std::lock_guard lock(g_hosts_mutex);
        auto& record = g_hosts[static_cast<size_t>(adapter->language)];
        if (record.present || record.active_calls != 0 || record.active_bindings != 0) {
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        record.adapter = snapshot_adapter(adapter);
        record.present = true;
        record.generation = g_next_host_generation++;
        if (record.generation == 0) {
            record.generation = g_next_host_generation++;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_unregister_language_host(language_host_kind language) {
    if (!valid_language(language))
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_hosts_mutex);
        auto& record = g_hosts[static_cast<size_t>(language)];
        if (!record.present)
            return SAO_ERR_HANDLE_INVALID;
        if (record.active_calls != 0 || record.active_bindings != 0) {
            return loader::SAO_PLUGINS_ERR_BUSY;
        }
        record.adapter = {};
        record.present = false;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_language_host_available(language_host_kind language) {
    try {
        host_call_lease lease;
        return lease.acquire(language) == SAO_OK && host_available(lease.adapter());
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_barrier(barrier_fn fn, void* user_data, char** out_error_utf8) {
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (fn == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(_WIN32) && defined(_MSC_VER)
        bool faulted = false;
        const int32_t status = invoke_with_seh(fn, user_data, &faulted);
        if (faulted && out_error_utf8 != nullptr) {
            *out_error_utf8 = duplicate_error("language host raised a structured exception");
        }
        return status;
#else
        return fn(user_data);
#endif
    } catch (const std::exception& error) {
        if (out_error_utf8 != nullptr) {
            *out_error_utf8 = duplicate_error(error.what());
        }
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        if (out_error_utf8 != nullptr) {
            *out_error_utf8 = duplicate_error("language host crossed exception barrier");
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_binding_free_error(char* error) {
    delete[] error;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_track_callback(void* user_data, release_callback_fn release_fn) {
    if (user_data == nullptr || release_fn == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        auto callback = make_legacy_callback(user_data, release_fn);
        if (g_current_binding != nullptr) {
            return append_callback(g_current_binding, callback);
        }
        std::lock_guard lock(g_callbacks_mutex);
        g_unscoped_callbacks.push_back(std::move(callback));
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_binding_release_all_callbacks(void) {
    try {
        if (g_current_binding != nullptr) {
            const int32_t status = release_callbacks(claim_all_callbacks(g_current_binding));
            record_callback_release_status(g_current_binding, status);
            return;
        }
        std::vector<std::shared_ptr<plugin_binding_s::callback_record>> callbacks;
        {
            std::lock_guard lock(g_callbacks_mutex);
            callbacks.swap(g_unscoped_callbacks);
            for (const auto& callback : callbacks) {
                callback->claimed = true;
            }
        }
        (void)release_callbacks(callbacks);
    } catch (...) {
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_release_callback(language_host_kind language, void* callback_user_data) {
    if (!valid_language(language) || callback_user_data == nullptr)
        return;
    try {
        binding_call_lease lease;
        auto callback = claim_callback(language, callback_user_data, lease);
        if (callback == nullptr)
            return;
        current_binding_scope scope(lease.get());
        const int32_t status = release_callback_record(callback);
        record_callback_release_status(lease.get(), status);
    } catch (...) {
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
    {sdk_method_id::method_on_encounter_finalized, "on_encounter_finalized"},
    {sdk_method_id::method_emit, "emit"},
    {sdk_method_id::method_get_snapshot, "get_snapshot"},
    {sdk_method_id::method_snapshot_value, "snapshot_value"},
    {sdk_method_id::method_recent_events, "recent_events"},
    {sdk_method_id::method_get_setting, "get_setting"},
    {sdk_method_id::method_setting, "setting"},
    {sdk_method_id::method_set_setting, "set_setting"},
    {sdk_method_id::method_set_defaults, "set_defaults"},
    {sdk_method_id::method_register_parser_adapter, "register_parser_adapter"},
    {sdk_method_id::method_register_exporter, "register_exporter"},
    {sdk_method_id::method_register_formatter, "register_formatter"},
    {sdk_method_id::method_register_trigger_type, "register_trigger_type"},
    {sdk_method_id::method_register_report_view, "register_report_view"},
    {sdk_method_id::method_register_timer, "register_timer"},
    {sdk_method_id::method_register_ui_panel, "register_ui_panel"},
    {sdk_method_id::method_register_render_hook, "register_render_hook"},
    {sdk_method_id::method_set_overlay, "set_overlay"},
    {sdk_method_id::method_clear_overlay, "clear_overlay"},
    {sdk_method_id::method_register_hotkey, "register_hotkey"},
    {sdk_method_id::method_register_engine, "register_engine"},
    {sdk_method_id::method_register_data_source, "register_data_source"},
    {sdk_method_id::method_register_menu_category, "register_menu_category"},
    {sdk_method_id::method_register_menu_surface, "register_menu_surface"},
    {sdk_method_id::method_register_action_handler, "register_action_handler"},
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
    {sdk_method_id::method_create_compositor_layer, "create_compositor_layer"},
    {sdk_method_id::method_upload_compositor_frame, "upload_compositor_frame"},
    {sdk_method_id::method_set_compositor_layer_mmf_source, "set_compositor_layer_mmf_source"},
    {sdk_method_id::method_set_compositor_layer_shared_texture_source,
     "set_compositor_layer_shared_texture_source"},
    {sdk_method_id::method_set_compositor_layer_position, "set_compositor_layer_position"},
    {sdk_method_id::method_set_compositor_layer_visible, "set_compositor_layer_visible"},
    {sdk_method_id::method_set_compositor_layer_input, "set_compositor_layer_input"},
    {sdk_method_id::method_destroy_compositor_layer, "destroy_compositor_layer"},
    {sdk_method_id::method_compositor_gpu_interop_available, "compositor_gpu_interop_available"},
    {sdk_method_id::method_compositor_layer_shared_texture_active,
     "compositor_layer_shared_texture_active"},
    {sdk_method_id::method_compositor_display_refresh_hz, "compositor_display_refresh_hz"},
    {sdk_method_id::method_get_engine, "get_engine"},
    {sdk_method_id::method_require_engine, "require_engine"},
    {sdk_method_id::method_call_engine, "call_engine"},
    {sdk_method_id::method_call_runtime, "call_runtime"},
    {sdk_method_id::method_ensure_requirements, "ensure_requirements"},
    {sdk_method_id::method_load_local, "load_local"},
};
static_assert(std::size(k_names) == static_cast<size_t>(sdk_method_id::method_count_));

// 简单字符串范围复制 (args_ptr 可能没 \0)
std::string bytes_to_string(const char* ptr, size_t size) {
    if (ptr == nullptr || size == 0)
        return {};
    return std::string(ptr, size);
}
} // namespace

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_method_name(sdk_method_id method) {
    for (const auto& e : k_names) {
        if (e.id == method)
            return e.name;
    }
    return "";
}

extern "C" SAO_PLUGINS_API sdk_method_id SAO_PLUGINS_CALL
sao_plugins_binding_method_from_name(const char* name) {
    if (name == nullptr || *name == '\0')
        return sdk_method_id::method_count_;
    for (const auto& e : k_names) {
        if (std::strcmp(e.name, name) == 0)
            return e.id;
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

int32_t SAO_PLUGINS_CALL call_load(void* opaque) {
    auto* call = static_cast<load_call*>(opaque);
    return call->adapter.load_plugin(call->context, call->runtime, call->output,
                                     call->adapter.user_data);
}

struct unload_call {
    language_host_adapter_vtable adapter{};
    void* plugin = nullptr;
};

int32_t SAO_PLUGINS_CALL call_unload(void* opaque) {
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

int32_t SAO_PLUGINS_CALL call_invoke(void* opaque) {
    auto* call = static_cast<invoke_call*>(opaque);
    return call->binding->host.invoke(call->binding->provider_plugin, call->method, call->arguments,
                                      call->arguments_size, call->output, call->output_capacity,
                                      call->required, call->error, call->error_capacity,
                                      call->binding->host.user_data);
}

struct dispatch_call {
    language_host_adapter_vtable adapter{};
    language_binding_operation operation{};
    language_binding_request* request = nullptr;
};

int32_t SAO_PLUGINS_CALL call_dispatch(void* opaque) {
    auto* call = static_cast<dispatch_call*>(opaque);
    return call->adapter.dispatch(call->operation, call->request, call->adapter.user_data);
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_load(language_host_kind language, void* plugin_ctx, void* runtime,
                                plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (!valid_language(language) || plugin_ctx == nullptr || runtime == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        host_call_lease host_lease;
        int32_t status = host_lease.acquire(language);
        if (status != SAO_OK || !host_available(host_lease.adapter())) {
            return unsupported();
        }

        auto binding = std::unique_ptr<plugin_binding_s>(new (std::nothrow) plugin_binding_s{});
        if (!binding)
            return SAO_ERR_OS_CALL_FAILED;
        binding->ctx = plugin_ctx;
        binding->lang_state = runtime;
        binding->host_kind = language;
        binding->host = host_lease.adapter();
        binding->host_generation = host_lease.generation();
        binding->lang = static_cast<plugin_binding_s::language_kind>(language);
        binding_registration_guard registration(binding.get(), language, binding->host_generation);

        status = host_lease.promote_binding();
        if (status != SAO_OK)
            return status;
        registration.mark_promoted();
        status = register_binding(binding.get());
        if (status != SAO_OK)
            return status;
        registration.mark_registered();

        void* provider_plugin = nullptr;
        load_call call{binding->host, plugin_ctx, runtime, &provider_plugin};
        char* error = nullptr;
        {
            current_binding_scope scope(binding.get());
            status = sao_plugins_binding_barrier(&call_load, &call, &error);
        }
        sao_plugins_binding_free_error(error);
        if (status != SAO_OK || provider_plugin == nullptr) {
            if (provider_plugin != nullptr) {
                unload_call rollback{binding->host, provider_plugin};
                current_binding_scope scope(binding.get());
                const int32_t rollback_status =
                    sao_plugins_binding_barrier(&call_unload, &rollback, nullptr);
                if (rollback_status != SAO_OK) {
                    binding->provider_plugin = provider_plugin;
                    {
                        std::lock_guard lock(binding->mu);
                        binding->lifecycle = plugin_binding_s::lifecycle_state::unloading;
                    }
                    binding.release();
                    registration.dismiss();
                    return rollback_status;
                }
            }
            // A successful provider unload has already torn down the language
            // runtime. Drop stale ownership records without calling into it.
            (void)claim_all_callbacks(binding.get());
            return status == SAO_OK ? SAO_ERR_NOT_INITIALIZED : status;
        }

        binding->provider_plugin = provider_plugin;
        {
            std::lock_guard lock(binding->mu);
            binding->lifecycle = plugin_binding_s::lifecycle_state::ready;
        }
        *out_plugin = binding.release();
        registration.dismiss();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_unload(plugin_binding_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        {
            std::lock_guard registry_lock(g_bindings_mutex);
            if (std::find(g_bindings.begin(), g_bindings.end(), plugin) == g_bindings.end()) {
                return SAO_ERR_HANDLE_INVALID;
            }
            std::lock_guard binding_lock(plugin->mu);
            if (plugin->lifecycle != plugin_binding_s::lifecycle_state::ready) {
                return plugin->lifecycle == plugin_binding_s::lifecycle_state::unloading
                           ? loader::SAO_PLUGINS_ERR_BUSY
                           : SAO_ERR_HANDLE_INVALID;
            }
            if (plugin->active_calls != 0) {
                return loader::SAO_PLUGINS_ERR_BUSY;
            }
            plugin->lifecycle = plugin_binding_s::lifecycle_state::unloading;
        }

        host_call_lease host_lease;
        int32_t status = host_lease.acquire(plugin->host_kind, plugin->host_generation);
        if (status != SAO_OK) {
            std::lock_guard lock(plugin->mu);
            plugin->lifecycle = plugin_binding_s::lifecycle_state::ready;
            return status;
        }

        unload_call call{plugin->host, plugin->provider_plugin};
        char* error = nullptr;
        {
            current_binding_scope scope(plugin);
            status = sao_plugins_binding_barrier(&call_unload, &call, &error);
        }
        if (status != SAO_OK) {
            std::lock_guard lock(plugin->mu);
            plugin->last_error = error != nullptr ? error : "language host unload failed";
            plugin->lifecycle = plugin_binding_s::lifecycle_state::ready;
            sao_plugins_binding_free_error(error);
            return status;
        }
        sao_plugins_binding_free_error(error);

        auto remaining_callbacks = claim_all_callbacks(plugin);
        int32_t release_status = remaining_callbacks.empty() ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
        {
            std::lock_guard lock(plugin->mu);
            if (release_status == SAO_OK && plugin->callback_release_status != SAO_OK) {
                release_status = plugin->callback_release_status;
            }
        }
        unregister_binding(plugin);
        {
            std::lock_guard lock(plugin->mu);
            plugin->lifecycle = plugin_binding_s::lifecycle_state::dead;
        }
        release_host_binding(plugin->host_kind, plugin->host_generation);
        delete plugin;
        return release_status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_plugin_invoke(
    plugin_binding_handle_t plugin, const char* method_name_utf8, const uint8_t* args_json_utf8,
    size_t args_size, uint8_t* out_result_json_utf8, size_t out_capacity, size_t* out_required) {
    if (out_required != nullptr)
        *out_required = 0;
    if (plugin == nullptr || method_name_utf8 == nullptr || method_name_utf8[0] == '\0' ||
        out_required == nullptr || (args_size > 0 && args_json_utf8 == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        binding_call_lease binding_lease;
        int32_t status = binding_lease.acquire(plugin);
        if (status != SAO_OK)
            return status;
        host_call_lease host_lease;
        status = host_lease.acquire(plugin->host_kind, plugin->host_generation);
        if (status != SAO_OK)
            return status;
        std::array<char, 512> provider_error{};
        invoke_call call{plugin,       method_name_utf8,      args_json_utf8,
                         args_size,    out_result_json_utf8,  out_capacity,
                         out_required, provider_error.data(), provider_error.size()};
        char* barrier_error = nullptr;
        {
            current_binding_scope scope(plugin);
            status = sao_plugins_binding_barrier(&call_invoke, &call, &barrier_error);
        }
        {
            std::lock_guard lock(plugin->mu);
            if (provider_error[0] != '\0')
                plugin->last_error = provider_error.data();
            else if (barrier_error != nullptr)
                plugin->last_error = barrier_error;
            else if (status == SAO_OK || status == SAO_ERR_BUFFER_TOO_SMALL) {
                plugin->last_error.clear();
            } else {
                plugin->last_error = "language host invoke failed";
            }
        }
        sao_plugins_binding_free_error(barrier_error);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_plugin_last_error(plugin_binding_handle_t plugin, char* out_error_utf8,
                                      size_t out_capacity, size_t* out_required) {
    if (plugin == nullptr || out_required == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        binding_call_lease lease;
        const int32_t status = lease.acquire(plugin);
        if (status != SAO_OK)
            return status;
        std::lock_guard lock(plugin->mu);
        return copy_to_caller(plugin->last_error, out_error_utf8, out_capacity, out_required);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_binding_dispatch_provider(
    language_host_kind language, language_binding_operation operation,
    language_binding_request* request) {
    if (request == nullptr || !valid_language(language)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        if (operation == language_binding_operation::callback_wrap &&
            request->out_callback != nullptr && request->out_user_data != nullptr) {
            *request->out_callback = nullptr;
            *request->out_user_data = nullptr;
        }
        host_call_lease host_lease;
        int32_t status = host_lease.acquire(language);
        if (status != SAO_OK || !host_available(host_lease.adapter()) ||
            host_lease.adapter().dispatch == nullptr) {
            return unsupported();
        }

        binding_call_lease binding_lease;
        std::shared_ptr<plugin_binding_s::callback_record> callback;
        uint64_t watermark = 0;
        if (operation == language_binding_operation::callback_wrap) {
            if (request->runtime == nullptr || request->out_callback == nullptr ||
                request->out_user_data == nullptr) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            status = acquire_callback_owner(language, request->runtime, binding_lease);
            if (status != SAO_OK)
                return status;
            if (adapter_has_release_callback(host_lease.adapter())) {
                callback = make_provider_callback(binding_lease.get(), nullptr);
            }
            watermark = callback_watermark(binding_lease.get());
        }

        dispatch_call call{host_lease.adapter(), operation, request};
        {
            current_binding_scope scope(binding_lease.get());
            status = sao_plugins_binding_barrier(&call_dispatch, &call, nullptr);
        }
        if (operation != language_binding_operation::callback_wrap) {
            return status;
        }
        if (status != SAO_OK) {
            if (*request->out_user_data != nullptr) {
                current_binding_scope scope(binding_lease.get());
                (void)rollback_wrapped_callback(binding_lease.get(), *request->out_user_data,
                                                watermark, callback);
            }
            *request->out_callback = nullptr;
            *request->out_user_data = nullptr;
            return status;
        }
        if (*request->out_callback == nullptr || *request->out_user_data == nullptr) {
            if (*request->out_user_data != nullptr) {
                current_binding_scope scope(binding_lease.get());
                (void)rollback_wrapped_callback(binding_lease.get(), *request->out_user_data,
                                                watermark, callback);
            }
            *request->out_callback = nullptr;
            *request->out_user_data = nullptr;
            return SAO_ERR_NOT_INITIALIZED;
        }
        if (callback_tracked_after(binding_lease.get(), *request->out_user_data, watermark)) {
            return SAO_OK;
        }
        if (callback == nullptr) {
            *request->out_callback = nullptr;
            *request->out_user_data = nullptr;
            return unsupported();
        }
        callback->callback_user_data = *request->out_user_data;
        status = append_callback(binding_lease.get(), callback);
        if (status != SAO_OK) {
            current_binding_scope scope(binding_lease.get());
            (void)release_callback_record(callback);
            *request->out_callback = nullptr;
            *request->out_user_data = nullptr;
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── 中央 dispatcher ─────────────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_sdk_bind_call(plugin_binding_handle_t plugin, sdk_method_id method_id,
                          const char* args_ptr, size_t args_size, char* ret_ptr, size_t ret_size) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        binding_call_lease lease;
        const int32_t lease_status = lease.acquire(plugin);
        if (lease_status != SAO_OK)
            return lease_status;

        switch (method_id) {
        case sdk_method_id::method_log: {
            std::string msg = bytes_to_string(args_ptr, args_size);
            loader::sao_plugins_ctx_log(static_cast<loader::plugin_context_t*>(plugin->ctx),
                                        msg.c_str());
            std::lock_guard lock(plugin->mu);
            plugin->last_log = std::move(msg);
            return SAO_OK;
        }

        case sdk_method_id::prop_plugin_id: {
            const char* plugin_id = loader::sao_plugins_ctx_plugin_id(
                static_cast<loader::plugin_context_t*>(plugin->ctx));
            if (plugin_id == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const std::string copied_plugin_id(plugin_id);
            if (ret_ptr != nullptr && ret_size > 0) {
                if (copied_plugin_id.size() + 1 > ret_size) {
                    ret_ptr[0] = '\0';
                    return SAO_ERR_BUFFER_TOO_SMALL;
                }
                std::memcpy(ret_ptr, copied_plugin_id.c_str(), copied_plugin_id.size() + 1);
            }
            std::lock_guard lock(plugin->mu);
            plugin->last_plugin_id_query = copied_plugin_id;
            return SAO_OK;
        }

        case sdk_method_id::method_register_hotkey: {
            std::string key = bytes_to_string(args_ptr, args_size);
            if (key.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_register_hotkey(
                static_cast<loader::plugin_context_t*>(plugin->ctx), key.c_str(), "", key.c_str(),
                nullptr, nullptr);
            if (status == SAO_OK) {
                std::lock_guard lock(plugin->mu);
                plugin->hotkeys.insert(std::move(key));
            }
            return status;
        }

        case sdk_method_id::method_emit: {
            std::string topic = bytes_to_string(args_ptr, args_size);
            if (topic.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_emit(
                static_cast<loader::plugin_context_t*>(plugin->ctx), topic.c_str(), "null");
            if (status == SAO_OK) {
                std::lock_guard lock(plugin->mu);
                plugin->event_counts[topic] += 1;
            }
            return status;
        }

        default:
            return unsupported();
        }
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_sdk_bind_call_ex(
    plugin_binding_handle_t plugin, sdk_method_id method_id, const char* args_ptr, size_t args_size,
    char* ret_ptr, size_t ret_size, size_t* out_required) {
    if (out_required == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_required = 0;
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (method_id == sdk_method_id::prop_plugin_id) {
        binding_call_lease lease;
        const int32_t lease_status = lease.acquire(plugin);
        if (lease_status != SAO_OK)
            return lease_status;
        const char* plugin_id =
            loader::sao_plugins_ctx_plugin_id(static_cast<loader::plugin_context_t*>(plugin->ctx));
        if (plugin_id == nullptr)
            return SAO_ERR_NOT_INITIALIZED;
        return copy_to_caller(plugin_id, ret_ptr, ret_size, out_required);
    }
    return sao_plugins_sdk_bind_call(plugin, method_id, args_ptr, args_size, ret_ptr, ret_size);
}

// ── test 辅助 ────────────────────────────────────────────────

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_binding_test_has_hotkey(plugin_binding_handle_t plugin, const char* hotkey_id) {
    if (plugin == nullptr || hotkey_id == nullptr)
        return false;
    binding_call_lease lease;
    if (lease.acquire(plugin) != SAO_OK)
        return false;
    std::lock_guard<std::mutex> lk(plugin->mu);
    return plugin->hotkeys.count(hotkey_id) > 0;
}

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_binding_test_event_count(plugin_binding_handle_t plugin, const char* topic) {
    if (plugin == nullptr || topic == nullptr)
        return 0;
    binding_call_lease lease;
    if (lease.acquire(plugin) != SAO_OK)
        return 0;
    std::lock_guard<std::mutex> lk(plugin->mu);
    auto it = plugin->event_counts.find(topic);
    return it == plugin->event_counts.end() ? 0u : it->second;
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_binding_test_last_log(plugin_binding_handle_t plugin) {
    if (plugin == nullptr)
        return "";
    // 注意: 返回内部 string 引用, 调用方不能修改。
    return plugin->last_log.c_str();
}

// ── activate / deactivate 通用工厂 (各语言复用) ──────────────

namespace detail {

// int lang: 0=python, 1=lua, 2=angel, 3=emma, 4=csharp
plugin_binding_handle_t make_binding(void* ctx, void* lang_state, int lang) {
    plugin_binding_handle_t binding = nullptr;
    const auto language = static_cast<language_host_kind>(lang);
    return sao_plugins_binding_plugin_load(language, ctx, lang_state, &binding) == SAO_OK ? binding
                                                                                          : nullptr;
}

void free_binding(plugin_binding_handle_t plugin) {
    (void)sao_plugins_binding_plugin_unload(plugin);
}

} // namespace detail

} // namespace sao::plugins::sdk_binding
