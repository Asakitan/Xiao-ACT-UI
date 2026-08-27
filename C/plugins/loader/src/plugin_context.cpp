#include "sao/plugins/loader/plugin_context.h"
#include "entity_provider_internal.h"
#include "plugin_internal.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::loader {
namespace {

using json = nlohmann::json;

struct event_subscription_state {
    uint32_t token = 0;
    std::string topic;
    event_callback_fn callback = nullptr;
    void* user_data = nullptr;
    bool once = false;
    std::mutex mutex;
    std::condition_variable idle;
    bool accepting = true;
    size_t in_flight = 0;
};

constexpr size_t kMaximumEventCallbackNesting = 64;
thread_local std::array<event_subscription_state*, kMaximumEventCallbackNesting>
    g_active_event_subscriptions{};
thread_local size_t g_active_event_subscription_depth = 0;

bool event_subscription_active_on_current_thread(
    const event_subscription_state& subscription) noexcept {
    return std::find(g_active_event_subscriptions.begin(),
                     g_active_event_subscriptions.begin() + g_active_event_subscription_depth,
                     &subscription) !=
           g_active_event_subscriptions.begin() + g_active_event_subscription_depth;
}

class event_invocation_lease {
  public:
    event_invocation_lease() = default;
    ~event_invocation_lease() {
        release();
    }

    event_invocation_lease(const event_invocation_lease&) = delete;
    event_invocation_lease& operator=(const event_invocation_lease&) = delete;

    bool acquire(const std::shared_ptr<event_subscription_state>& subscription) noexcept {
        if (subscription == nullptr ||
            g_active_event_subscription_depth == kMaximumEventCallbackNesting) {
            return false;
        }
        try {
            std::lock_guard lock(subscription->mutex);
            if (!subscription->accepting)
                return false;
            if (subscription->once)
                subscription->accepting = false;
            ++subscription->in_flight;
            subscription_ = subscription;
            g_active_event_subscriptions[g_active_event_subscription_depth++] = subscription.get();
            return true;
        } catch (...) {
            return false;
        }
    }

    event_subscription_state& value() const noexcept {
        return *subscription_;
    }

  private:
    void release() noexcept {
        if (subscription_ == nullptr)
            return;
        if (g_active_event_subscription_depth > 0 &&
            g_active_event_subscriptions[g_active_event_subscription_depth - 1] ==
                subscription_.get()) {
            g_active_event_subscriptions[--g_active_event_subscription_depth] = nullptr;
        }
        try {
            {
                std::lock_guard lock(subscription_->mutex);
                if (subscription_->in_flight > 0)
                    --subscription_->in_flight;
            }
            subscription_->idle.notify_all();
        } catch (...) {
        }
        subscription_.reset();
    }

    std::shared_ptr<event_subscription_state> subscription_;
};

int32_t
quiesce_event_subscription(const std::shared_ptr<event_subscription_state>& subscription) noexcept {
    if (subscription == nullptr)
        return SAO_OK;
    try {
        std::unique_lock lock(subscription->mutex);
        subscription->accepting = false;
        if (event_subscription_active_on_current_thread(*subscription)) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        subscription->idle.wait(lock, [&subscription] { return subscription->in_flight == 0; });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

struct data_source_record {
    data_source_start_fn start = nullptr;
    data_source_stop_fn stop = nullptr;
    void* user_data = nullptr;
};

enum class platform_resource_kind : uint8_t {
    hotkey,
    timer,
    notify,
    render_hook,
    overlay,
    compositor_layer,
    file_result,
    window,
};

struct platform_resource_record {
    platform_resource_kind kind = platform_resource_kind::timer;
    uint32_t loader_token = 0;
    plugin_context_platform_token_t provider_token = 0;
    std::string key;
    bool pending_destroy = false;
    size_t active_calls = 0;
    bool one_shot = false;
    uint32_t compositor_width = 0;
    uint32_t compositor_height = 0;
};

struct platform_provider_record {
    plugin_context_platform_provider value{};
    bool present = false;
    size_t active_contexts = 0;
};

std::mutex g_platform_provider_mutex;
platform_provider_record g_platform_provider;

constexpr size_t kMaximumPlatformCallbackNesting = 64;
thread_local std::array<plugin_context_t*, kMaximumPlatformCallbackNesting>
    g_active_platform_contexts{};
thread_local size_t g_active_platform_context_depth = 0;

constexpr size_t kPlatformProviderMinimumSize =
    offsetof(plugin_context_platform_provider, register_hotkey);

char* duplicate_string(const std::string& value) noexcept {
    try {
        auto result = std::make_unique<char[]>(value.size() + 1);
        std::copy(value.begin(), value.end(), result.get());
        result[value.size()] = '\0';
        return result.release();
    } catch (...) {
        return nullptr;
    }
}

wchar_t* duplicate_wstring(const std::wstring& value) noexcept {
    try {
        auto result = std::make_unique<wchar_t[]>(value.size() + 1);
        std::copy(value.begin(), value.end(), result.get());
        result[value.size()] = L'\0';
        return result.release();
    } catch (...) {
        return nullptr;
    }
}

template <typename T>
int32_t copy_external_entity_struct(const T* source, size_t required_prefix_size, T& out) noexcept {
    if (source == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(_MSC_VER)
    __try {
#endif
        uint32_t struct_size = 0;
        std::memcpy(&struct_size, source, sizeof(struct_size));
        if (struct_size < required_prefix_size)
            return SAO_PLUGINS_ERR_ABI_MISMATCH;
        out = {};
        std::memcpy(&out, source, (std::min)(static_cast<size_t>(struct_size), sizeof(out)));
        return SAO_OK;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#endif
}

bool bounded_c_string_length(const char* value, size_t maximum_bytes,
                             size_t& out_length) noexcept {
    if (value == nullptr)
        return false;
#if defined(_MSC_VER)
    __try {
#endif
        size_t length = 0;
        while (length <= maximum_bytes && value[length] != '\0')
            ++length;
        if (length > maximum_bytes)
            return false;
        out_length = length;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool copy_c_string_bytes(char* destination, const char* source, size_t size) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        std::memcpy(destination, source, size);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool valid_utf8(std::string_view value) noexcept {
    size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        size_t continuation_count = 0;
        uint32_t code_point = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size())
            return false;
        for (size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

constexpr size_t kMaximumContextJsonBytes = 1024 * 1024;
constexpr size_t kMaximumJsonNestingDepth = 64;
constexpr size_t kMaximumJsonNodes = 16384;

class bounded_context_json_sax final : public json::json_sax_t {
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
    bool number_float(number_float_t value, const string_t&) override {
        return std::isfinite(value) && consume_node();
    }
    bool string(string_t& value) override {
        return consume_node() && value.find('\0') == string_t::npos;
    }
    bool binary(binary_t&) override {
        return consume_node();
    }
    bool start_object(std::size_t) override {
        return start_container();
    }
    bool key(string_t& value) override {
        return value.find('\0') == string_t::npos;
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
        if (nodes_ >= kMaximumJsonNodes)
            return false;
        ++nodes_;
        return true;
    }

    bool start_container() noexcept {
        if (depth_ >= kMaximumJsonNestingDepth || !consume_node())
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

    size_t depth_ = 0;
    size_t nodes_ = 0;
};

struct parsed_context_json {
    std::string text;
    json value;
};

bool parse_context_json(const char* text, parsed_context_json& output,
                        bool allow_empty = false) noexcept {
    if (text == nullptr)
        return false;
    try {
        size_t length = 0;
        if (!bounded_c_string_length(text, kMaximumContextJsonBytes, length))
            return false;
        std::string candidate_text(length, '\0');
        if (length > 0 && !copy_c_string_bytes(candidate_text.data(), text, length))
            return false;
        if (!valid_utf8(candidate_text))
            return false;
        if (candidate_text.empty()) {
            if (!allow_empty)
                return false;
            output = {std::move(candidate_text), json{}};
            return true;
        }
        bounded_context_json_sax sax;
        if (!json::sax_parse(candidate_text.begin(), candidate_text.end(), &sax))
            return false;
        auto candidate_value = json::parse(candidate_text.begin(), candidate_text.end());
        output = {std::move(candidate_text), std::move(candidate_value)};
        return true;
    } catch (...) {
        return false;
    }
}

int32_t copy_entity_provider_id(const char* value, std::string& out) noexcept {
    try {
        constexpr size_t kMaximumEntityProviderIdBytes = 16 * 1024;
        size_t length = 0;
        if (!bounded_c_string_length(value, kMaximumEntityProviderIdBytes, length) || length == 0)
            return SAO_ERR_INVALID_ARGUMENT;
        std::string candidate(length, '\0');
        if (!copy_c_string_bytes(candidate.data(), value, length) || !valid_utf8(candidate)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        out = std::move(candidate);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t copy_compositor_layer_name(const char* value, std::string& out) noexcept {
    try {
        size_t length = 0;
        if (!bounded_c_string_length(value, SAO_PLUGIN_CONTEXT_COMPOSITOR_LAYER_NAME_MAX_BYTES,
                                     length) ||
            length == 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::string candidate(length, '\0');
        if (!copy_c_string_bytes(candidate.data(), value, length) || !valid_utf8(candidate))
            return SAO_ERR_INVALID_ARGUMENT;
        out = std::move(candidate);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extension_kind parse_extension_kind(const char* value, bool& valid) {
    valid = true;
    if (std::strcmp(value, "parser_adapter") == 0)
        return extension_kind::parser_adapter;
    if (std::strcmp(value, "exporter") == 0)
        return extension_kind::exporter;
    if (std::strcmp(value, "formatter") == 0)
        return extension_kind::formatter;
    if (std::strcmp(value, "trigger_type") == 0)
        return extension_kind::trigger_type;
    if (std::strcmp(value, "report_view") == 0)
        return extension_kind::report_view;
    if (std::strcmp(value, "timer") == 0)
        return extension_kind::timer;
    valid = false;
    return extension_kind::ui_panel;
}

} // namespace

struct plugin_context_s {
    plugin_handle_t plugin = nullptr;
    std::shared_ptr<plugin_handle_s> plugin_owner;
    std::string plugin_id;
    std::string plugin_path_utf8;
    std::string plugin_version;
    std::wstring path;
    std::atomic_bool stop_requested{false};
    std::mutex mutex;
    uint32_t next_token = 1;
    std::vector<std::shared_ptr<event_subscription_state>> subscriptions;
    std::unordered_map<std::string, json> settings;
    std::unordered_map<std::string, json> last_events;
    std::vector<json> recent_events;
    std::unordered_map<std::string, void*> engines;
    std::unordered_map<std::string, data_source_record> data_sources;
    std::vector<std::shared_ptr<entity_provider_state>> entity_providers;
    bool entity_provider_publication_complete = false;
    plugin_context_platform_provider platform_provider{};
    plugin_context_platform_session_t platform_session = nullptr;
    bool platform_bound = false;
    bool platform_session_destroyed = false;
    bool platform_provider_released = false;
    bool platform_active_counted = false;
    bool platform_closing = false;
    bool platform_quiesced = false;
    size_t platform_active_calls = 0;
    std::condition_variable platform_calls_drained;
    std::vector<platform_resource_record> platform_resources;
    plugin_context_t* platform_quarantine_next = nullptr;
};

namespace {

struct context_lifetime_state {
    size_t active_registrations = 0;
    size_t active_host_leases = 0;
    bool closing = false;
};

std::mutex g_context_lifetime_mutex;
std::condition_variable g_context_lifetime_idle;
std::unordered_map<plugin_context_t*, context_lifetime_state> g_context_lifetimes;

std::mutex g_platform_quarantine_drain_mutex;
std::atomic<plugin_context_t*> g_platform_quarantine_head{nullptr};

bool register_context_lifetime(plugin_context_t* ctx) noexcept {
    try {
        std::lock_guard lock(g_context_lifetime_mutex);
        return g_context_lifetimes.emplace(ctx, context_lifetime_state{}).second;
    } catch (...) {
        return false;
    }
}

void unregister_context_lifetime(plugin_context_t* ctx) noexcept {
    try {
        std::lock_guard lock(g_context_lifetime_mutex);
        g_context_lifetimes.erase(ctx);
    } catch (...) {
    }
}

class context_registration_lease {
  public:
    ~context_registration_lease() {
        release();
    }

    context_registration_lease(const context_registration_lease&) = delete;
    context_registration_lease& operator=(const context_registration_lease&) = delete;

    context_registration_lease() = default;

    bool acquire(plugin_context_t* ctx) noexcept {
        if (ctx == nullptr)
            return false;
        try {
            std::lock_guard lock(g_context_lifetime_mutex);
            const auto found = g_context_lifetimes.find(ctx);
            if (found == g_context_lifetimes.end() || found->second.closing)
                return false;
            ++found->second.active_registrations;
            ctx_ = ctx;
            return true;
        } catch (...) {
            return false;
        }
    }

  private:
    void release() noexcept {
        if (ctx_ == nullptr)
            return;
        try {
            std::lock_guard lock(g_context_lifetime_mutex);
            const auto found = g_context_lifetimes.find(ctx_);
            if (found != g_context_lifetimes.end() && found->second.active_registrations > 0) {
                --found->second.active_registrations;
                if (found->second.active_registrations == 0)
                    g_context_lifetime_idle.notify_all();
            }
        } catch (...) {
        }
        ctx_ = nullptr;
    }

    plugin_context_t* ctx_ = nullptr;
};

int32_t begin_context_destruction(plugin_context_t* ctx) noexcept {
    try {
        std::unique_lock lock(g_context_lifetime_mutex);
        const auto found = g_context_lifetimes.find(ctx);
        if (found == g_context_lifetimes.end() || found->second.closing)
            return SAO_ERR_HANDLE_INVALID;
        found->second.closing = true;
        g_context_lifetime_idle.wait(lock, [ctx] {
            const auto current = g_context_lifetimes.find(ctx);
            return current == g_context_lifetimes.end() ||
                   current->second.active_registrations == 0;
        });
        const auto current = g_context_lifetimes.find(ctx);
        if (current == g_context_lifetimes.end())
            return SAO_ERR_HANDLE_INVALID;
        if (current->second.active_host_leases != 0) {
            current->second.closing = false;
            return SAO_PLUGINS_ERR_BUSY;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void cancel_context_destruction(plugin_context_t* ctx) noexcept {
    try {
        std::lock_guard lock(g_context_lifetime_mutex);
        const auto found = g_context_lifetimes.find(ctx);
        if (found != g_context_lifetimes.end())
            found->second.closing = false;
    } catch (...) {
    }
}

void finish_context_destruction(plugin_context_t* ctx) noexcept {
    unregister_context_lifetime(ctx);
}

class platform_call_lease {
  public:
    platform_call_lease() = default;
    ~platform_call_lease() {
        release();
    }

    platform_call_lease(const platform_call_lease&) = delete;
    platform_call_lease& operator=(const platform_call_lease&) = delete;

    bool acquire(plugin_context_t* ctx, bool allow_closing = false) noexcept {
        if (ctx == nullptr || g_active_platform_context_depth == kMaximumPlatformCallbackNesting)
            return false;
        try {
            std::lock_guard lock(ctx->mutex);
            if (!ctx->platform_bound || (ctx->platform_closing && !allow_closing)) {
                return false;
            }
            ctx_ = ctx;
            provider_ = ctx->platform_provider;
            session_ = ctx->platform_session;
            ++ctx->platform_active_calls;
            g_active_platform_contexts[g_active_platform_context_depth++] = ctx;
            return true;
        } catch (...) {
            return false;
        }
    }

    const plugin_context_platform_provider& provider() const noexcept {
        return provider_;
    }

    plugin_context_platform_session_t session() const noexcept {
        return session_;
    }

  private:
    void release() noexcept {
        if (ctx_ == nullptr)
            return;
        if (g_active_platform_context_depth > 0 &&
            g_active_platform_contexts[g_active_platform_context_depth - 1] == ctx_) {
            g_active_platform_contexts[--g_active_platform_context_depth] = nullptr;
        }
        try {
            std::lock_guard lock(ctx_->mutex);
            if (ctx_->platform_active_calls > 0) {
                --ctx_->platform_active_calls;
            }
            if (ctx_->platform_active_calls == 0) {
                ctx_->platform_calls_drained.notify_all();
            }
        } catch (...) {
        }
        ctx_ = nullptr;
    }

    plugin_context_t* ctx_ = nullptr;
    plugin_context_platform_provider provider_{};
    plugin_context_platform_session_t session_ = nullptr;
};

bool SAO_PLUGINS_CALL enter_platform_callback(void* user_data) noexcept {
    auto* ctx = static_cast<plugin_context_t*>(user_data);
    if (ctx == nullptr || g_active_platform_context_depth == kMaximumPlatformCallbackNesting)
        return false;
    try {
        std::lock_guard lock(ctx->mutex);
        if (!ctx->platform_bound || ctx->platform_closing || ctx->platform_quiesced ||
            ctx->platform_session_destroyed) {
            return false;
        }
        ++ctx->platform_active_calls;
        g_active_platform_contexts[g_active_platform_context_depth++] = ctx;
        return true;
    } catch (...) {
        return false;
    }
}

void SAO_PLUGINS_CALL leave_platform_callback(void* user_data) noexcept {
    auto* ctx = static_cast<plugin_context_t*>(user_data);
    if (ctx == nullptr)
        return;
    const auto begin = g_active_platform_contexts.begin();
    const auto end = begin + g_active_platform_context_depth;
    const auto found = std::find(std::make_reverse_iterator(end),
                                 std::make_reverse_iterator(begin), ctx);
    if (found == std::make_reverse_iterator(begin))
        return;
    const size_t index = static_cast<size_t>(std::distance(begin, found.base()) - 1);
    for (size_t current = index + 1; current < g_active_platform_context_depth; ++current)
        g_active_platform_contexts[current - 1] = g_active_platform_contexts[current];
    g_active_platform_contexts[--g_active_platform_context_depth] = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        if (ctx->platform_active_calls > 0)
            --ctx->platform_active_calls;
        if (ctx->platform_active_calls == 0)
            ctx->platform_calls_drained.notify_all();
    } catch (...) {
    }
}

bool platform_context_active_on_current_thread(plugin_context_t* ctx) noexcept {
    return std::find(g_active_platform_contexts.begin(),
                     g_active_platform_contexts.begin() + g_active_platform_context_depth,
                     ctx) != g_active_platform_contexts.begin() + g_active_platform_context_depth;
}

uint32_t allocate_context_token_locked(plugin_context_t* ctx) {
    for (uint64_t attempt = 0;
         attempt < static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()); ++attempt) {
        const uint32_t token = ctx->next_token++;
        if (token != 0 &&
            std::none_of(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [token](const platform_resource_record& resource) {
                             return resource.loader_token == token;
                         })) {
            return token;
        }
    }
    return 0;
}

bool has_platform_key_locked(plugin_context_t* ctx, platform_resource_kind kind,
                             std::string_view key) {
    return std::any_of(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                       [kind, key](const platform_resource_record& resource) {
                           return resource.kind == kind && resource.key == key;
                       });
}

int32_t map_platform_status(int32_t status) noexcept {
    switch (status) {
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK:
        return SAO_OK;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT:
        return SAO_ERR_INVALID_ARGUMENT;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_INITIALIZED:
        return SAO_ERR_NOT_INITIALIZED;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_FOUND:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_SURFACE_INVALID:
        return SAO_ERR_HANDLE_INVALID;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUFFER_TOO_SMALL:
        return SAO_ERR_BUFFER_TOO_SMALL;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_IMPLEMENTED:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNSUPPORTED:
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUSY:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_CANCELLED:
        return SAO_PLUGINS_ERR_BUSY;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ABI_MISMATCH:
        return SAO_PLUGINS_ERR_ABI_MISMATCH;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ACCESS_DENIED:
        return SAO_PLUGINS_ERR_NOT_OWNER;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ALREADY_EXISTS:
        return SAO_PLUGINS_ERR_ALREADY_EXISTS;
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_TIMEOUT:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_OS_CALL_FAILED:
    case SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_DEVICE_LOST:
    default:
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t find_compositor_layer_locked(plugin_context_t* ctx, std::string_view name,
                                     platform_resource_record& out_resource) {
    const auto found =
        std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                     [name](const platform_resource_record& candidate) {
                         return candidate.kind == platform_resource_kind::compositor_layer &&
                                candidate.key == name;
                     });
    if (found == ctx->platform_resources.end())
        return SAO_ERR_HANDLE_INVALID;
    if (found->provider_token == 0 || found->pending_destroy) {
        return SAO_PLUGINS_ERR_BUSY;
    }
    out_resource = *found;
    return SAO_OK;
}

class compositor_resource_lease {
  public:
    compositor_resource_lease() = default;
    ~compositor_resource_lease() {
        release();
    }

    compositor_resource_lease(const compositor_resource_lease&) = delete;
    compositor_resource_lease& operator=(const compositor_resource_lease&) = delete;

    int32_t acquire(plugin_context_t* ctx, std::string_view name) noexcept {
        if (ctx == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        try {
            std::lock_guard lock(ctx->mutex);
            platform_resource_record resource;
            const int32_t status = find_compositor_layer_locked(ctx, name, resource);
            if (status != SAO_OK)
                return status;
            const auto found =
                std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                             [&resource](const platform_resource_record& candidate) {
                                 return candidate.kind == resource.kind &&
                                        candidate.loader_token == resource.loader_token;
                             });
            if (found == ctx->platform_resources.end()) {
                return SAO_ERR_HANDLE_INVALID;
            }
            ++found->active_calls;
            ctx_ = ctx;
            resource_ = resource;
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    const platform_resource_record& resource() const noexcept {
        return resource_;
    }

  private:
    void release() noexcept {
        if (ctx_ == nullptr)
            return;
        try {
            std::lock_guard lock(ctx_->mutex);
            const auto found =
                std::find_if(ctx_->platform_resources.begin(), ctx_->platform_resources.end(),
                             [this](const platform_resource_record& candidate) {
                                 return candidate.kind == resource_.kind &&
                                        candidate.loader_token == resource_.loader_token;
                             });
            if (found != ctx_->platform_resources.end() && found->active_calls > 0) {
                --found->active_calls;
            }
        } catch (...) {
        }
        ctx_ = nullptr;
    }

    plugin_context_t* ctx_ = nullptr;
    platform_resource_record resource_{};
};

bool valid_compositor_dimensions(uint32_t width, uint32_t height,
                                 uint32_t target_fps = 0) noexcept {
    constexpr uint32_t kMaximumSignedDimension =
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    return width > 0 && height > 0 && width <= kMaximumSignedDimension &&
           height <= kMaximumSignedDimension && target_fps <= kMaximumSignedDimension;
}

bool valid_bgra_frame(const uint8_t* bgra_bytes, size_t bytes_len, uint32_t width,
                      uint32_t height) noexcept {
    if (bgra_bytes == nullptr || !valid_compositor_dimensions(width, height)) {
        return false;
    }
    constexpr size_t kBytesPerPixel = 4;
    if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / kBytesPerPixel) {
        return false;
    }
    const size_t stride = static_cast<size_t>(width) * kBytesPerPixel;
    if (static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / stride) {
        return false;
    }
    return bytes_len == stride * static_cast<size_t>(height);
}

int32_t unregister_platform_resource(const plugin_context_platform_provider& provider,
                                     plugin_context_platform_session_t session,
                                     const platform_resource_record& resource) noexcept {
    try {
        switch (resource.kind) {
        case platform_resource_kind::hotkey:
            return provider.unregister_hotkey == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.unregister_hotkey(provider.user_data, session,
                                                    resource.provider_token);
        case platform_resource_kind::timer:
            return provider.unregister_timer == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.unregister_timer(provider.user_data, session,
                                                   resource.provider_token);
        case platform_resource_kind::notify:
            return provider.dismiss_notify == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.dismiss_notify(provider.user_data, session,
                                                 resource.provider_token);
        case platform_resource_kind::render_hook:
            return provider.unregister_render_hook == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.unregister_render_hook(provider.user_data, session,
                                                         resource.provider_token);
        case platform_resource_kind::overlay:
            return provider.clear_overlay == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.clear_overlay(provider.user_data, session,
                                                resource.provider_token);
        case platform_resource_kind::compositor_layer:
            return provider.destroy_compositor_layer == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : map_platform_status(provider.destroy_compositor_layer(
                             provider.user_data, session, resource.provider_token));
        case platform_resource_kind::file_result:
            return provider.release_file_result == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : map_platform_status(provider.release_file_result(
                             provider.user_data, session, resource.provider_token));
        case platform_resource_kind::window:
            return provider.close_window == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : map_platform_status(provider.close_window(provider.user_data, session,
                                                                   resource.provider_token));
        }
    } catch (...) {
    }
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t prepare_platform_session(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    if (platform_context_active_on_current_thread(ctx))
        return SAO_PLUGINS_ERR_BUSY;
    try {
        std::unique_lock lock(ctx->mutex);
        if (!ctx->platform_bound || ctx->platform_quiesced)
            return SAO_OK;
        ctx->platform_closing = true;
        ctx->platform_calls_drained.wait(lock, [ctx] { return ctx->platform_active_calls == 0; });
        ctx->platform_closing = false;
        return SAO_OK;
    } catch (...) {
        try {
            std::lock_guard lock(ctx->mutex);
            if (!ctx->platform_quiesced)
                ctx->platform_closing = false;
        } catch (...) {
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t quiesce_platform_session(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    if (platform_context_active_on_current_thread(ctx))
        return SAO_PLUGINS_ERR_BUSY;
    plugin_context_platform_provider provider{};
    plugin_context_platform_session_t session = nullptr;
    try {
        {
            std::unique_lock lock(ctx->mutex);
            if (!ctx->platform_bound || ctx->platform_quiesced || ctx->platform_session_destroyed)
                return SAO_OK;
            ctx->platform_closing = true;
            ctx->platform_calls_drained.wait(lock,
                                             [ctx] { return ctx->platform_active_calls == 0; });
            provider = ctx->platform_provider;
            session = ctx->platform_session;
        }
        if (provider.quiesce_session == nullptr) {
            return SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        const int32_t status = provider.quiesce_session(provider.user_data, session);
        if (status == SAO_OK) {
            std::lock_guard lock(ctx->mutex);
            ctx->platform_quiesced = true;
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t quiesce_event_subscriptions(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    try {
        std::vector<std::shared_ptr<event_subscription_state>> subscriptions;
        {
            std::lock_guard lock(ctx->mutex);
            subscriptions = ctx->subscriptions;
        }
        for (const auto& subscription : subscriptions) {
            const int32_t status = quiesce_event_subscription(subscription);
            if (status != SAO_OK)
                return status;
        }
        std::lock_guard lock(ctx->mutex);
        std::erase_if(ctx->subscriptions, [&subscriptions](const auto& candidate) {
            return std::find(subscriptions.begin(), subscriptions.end(), candidate) !=
                   subscriptions.end();
        });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_release_platform(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    int32_t status = quiesce_platform_session(ctx);
    if (status != SAO_OK)
        return status;

    while (true) {
        plugin_context_platform_provider provider{};
        plugin_context_platform_session_t session = nullptr;
        platform_resource_record resource;
        {
            std::lock_guard lock(ctx->mutex);
            if (!ctx->platform_bound)
                return SAO_OK;
            if (ctx->platform_resources.empty())
                break;
            provider = ctx->platform_provider;
            session = ctx->platform_session;
            resource = ctx->platform_resources.back();
            ctx->platform_resources.back().pending_destroy = true;
        }
        status = unregister_platform_resource(provider, session, resource);
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
            std::lock_guard lock(ctx->mutex);
            const auto found =
                std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                             [&resource](const platform_resource_record& candidate) {
                                 return candidate.kind == resource.kind &&
                                        candidate.loader_token == resource.loader_token;
                             });
            if (found != ctx->platform_resources.end()) {
                found->pending_destroy = false;
            }
            return status;
        }
        {
            std::lock_guard lock(ctx->mutex);
            const auto found =
                std::find_if(ctx->platform_resources.rbegin(), ctx->platform_resources.rend(),
                             [&resource](const platform_resource_record& candidate) {
                                 return candidate.kind == resource.kind &&
                                        candidate.loader_token == resource.loader_token &&
                                        candidate.provider_token == resource.provider_token;
                             });
            if (found != ctx->platform_resources.rend()) {
                ctx->platform_resources.erase(std::next(found).base());
            }
        }
    }

    plugin_context_platform_provider provider{};
    plugin_context_platform_session_t session = nullptr;
    {
        std::lock_guard lock(ctx->mutex);
        provider = ctx->platform_provider;
        session = ctx->platform_session;
    }
    bool session_destroyed = false;
    bool provider_released = false;
    {
        std::lock_guard lock(ctx->mutex);
        session_destroyed = ctx->platform_session_destroyed;
        provider_released = ctx->platform_provider_released;
    }
    if (!session_destroyed) {
        try {
            status = provider.destroy_session == nullptr
                         ? SAO_PLUGINS_ERR_UNSUPPORTED
                         : provider.destroy_session(provider.user_data, session);
        } catch (...) {
            status = SAO_ERR_OS_CALL_FAILED;
        }
        if (status != SAO_OK)
            return status;
        std::lock_guard lock(ctx->mutex);
        ctx->platform_session_destroyed = true;
        ctx->platform_session = nullptr;
    }
    if (!provider_released) {
        try {
            if (provider.release != nullptr)
                provider.release(provider.user_data);
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        std::lock_guard lock(ctx->mutex);
        ctx->platform_provider_released = true;
    }
    bool release_active_count = false;
    {
        std::lock_guard lock(ctx->mutex);
        ctx->platform_provider = {};
        ctx->platform_bound = false;
        release_active_count = ctx->platform_active_counted;
        ctx->platform_active_counted = false;
    }
    if (release_active_count) {
        std::lock_guard lock(g_platform_provider_mutex);
        if (g_platform_provider.active_contexts > 0)
            --g_platform_provider.active_contexts;
    }
    return SAO_OK;
}

void quarantine_platform_context(std::unique_ptr<plugin_context_t> context) noexcept {
    if (context == nullptr)
        return;
    auto* owned = context.release();
    auto* head = g_platform_quarantine_head.load(std::memory_order_relaxed);
    do {
        owned->platform_quarantine_next = head;
    } while (!g_platform_quarantine_head.compare_exchange_weak(
        head, owned, std::memory_order_release, std::memory_order_relaxed));
}

int32_t drain_platform_quarantine() noexcept {
    try {
        std::lock_guard drain_lock(g_platform_quarantine_drain_mutex);
        auto* context = g_platform_quarantine_head.exchange(nullptr, std::memory_order_acq_rel);
        while (context != nullptr) {
            auto* next = context->platform_quarantine_next;
            const int32_t status = plugin_context_release_platform(context);
            if (status != SAO_OK) {
                while (context != nullptr) {
                    next = context->platform_quarantine_next;
                    quarantine_platform_context(std::unique_ptr<plugin_context_t>(context));
                    context = next;
                }
                return status;
            }
            delete context;
            context = next;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t add_extension(plugin_context_t* ctx, extension_kind kind, const char* id,
                      const char* payload) {
    if (ctx == nullptr || id == nullptr || id[0] == '\0' || payload == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        parsed_context_json input;
        if (!parse_context_json(payload, input))
            return SAO_ERR_INVALID_ARGUMENT;
        const auto& parsed = input.value;
        extension_record record;
        record.kind = kind;
        record.id = id;
        record.title = parsed.value("title", std::string(id));
        record.description = parsed.value("description", std::string{});
        record.route = parsed.value("route", std::string{});
        record.payload_json = parsed.dump();
        return sao_plugins_registry_add_extension(sao_plugins_registry_instance(), ctx->plugin,
                                                  &record);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace

int32_t plugin_context_retain_host_lease(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_context_lifetime_mutex);
        const auto found = g_context_lifetimes.find(ctx);
        if (found == g_context_lifetimes.end() || found->second.closing) {
            return SAO_ERR_HANDLE_INVALID;
        }
        ++found->second.active_host_leases;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void plugin_context_release_host_lease(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return;
    try {
        std::lock_guard lock(g_context_lifetime_mutex);
        const auto found = g_context_lifetimes.find(ctx);
        if (found == g_context_lifetimes.end() || found->second.active_host_leases == 0) {
            return;
        }
        --found->second.active_host_leases;
        if (found->second.active_registrations == 0 && found->second.active_host_leases == 0) {
            g_context_lifetime_idle.notify_all();
        }
    } catch (...) {
    }
}

int32_t plugin_context_quiesce_platform(plugin_context_t* ctx) noexcept {
    return prepare_platform_session(ctx);
}

void plugin_context_request_stop(plugin_context_t* ctx) noexcept {
    if (ctx != nullptr)
        ctx->stop_requested.store(true);
}

void plugin_context_clear_stop(plugin_context_t* ctx) noexcept {
    if (ctx != nullptr)
        ctx->stop_requested.store(false);
}

bool plugin_context_entity_provider_is_current_thread(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return false;
    try {
        std::lock_guard lock(ctx->mutex);
        return entity_provider_is_current_thread(ctx->entity_providers);
    } catch (...) {
        return true;
    }
}

bool plugin_context_event_is_current_thread(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return false;
    try {
        std::lock_guard lock(ctx->mutex);
        return std::any_of(ctx->subscriptions.begin(), ctx->subscriptions.end(),
                           [](const auto& subscription) {
                               return subscription != nullptr &&
                                      event_subscription_active_on_current_thread(*subscription);
                           });
    } catch (...) {
        return true;
    }
}

bool plugin_context_platform_is_current_thread(plugin_context_t* ctx) noexcept {
    return ctx != nullptr && platform_context_active_on_current_thread(ctx);
}

int32_t plugin_context_quiesce_entity_providers(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    try {
        std::vector<std::shared_ptr<entity_provider_state>> providers;
        {
            std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
            std::lock_guard context_lock(ctx->mutex);
            ctx->entity_provider_publication_complete = false;
            providers = ctx->entity_providers;
        }
        return deactivate_entity_providers(providers);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_resume_entity_providers(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
        std::lock_guard context_lock(ctx->mutex);
        if (ctx->plugin_owner->context != ctx ||
            ctx->plugin_owner->state != lifecycle_state::enabling) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        const int32_t status = activate_entity_providers(ctx->entity_providers);
        if (status == SAO_OK)
            ctx->entity_provider_publication_complete = true;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_release_resources(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    if (plugin_context_entity_provider_is_current_thread(ctx)) {
        return SAO_PLUGINS_ERR_BUSY;
    }
    try {
        const int32_t event_status = quiesce_event_subscriptions(ctx);
        if (event_status != SAO_OK)
            return event_status;
        const int32_t quiesce_status = quiesce_platform_session(ctx);
        if (quiesce_status != SAO_OK)
            return quiesce_status;
        std::vector<data_source_record> data_sources;
        std::vector<std::shared_ptr<entity_provider_state>> providers;
        {
            std::lock_guard lock(ctx->mutex);
            data_sources.reserve(ctx->data_sources.size());
            for (const auto& [_, data_source] : ctx->data_sources) {
                data_sources.push_back(data_source);
            }
            providers = ctx->entity_providers;
        }
        for (const auto& data_source : data_sources) {
            if (data_source.stop == nullptr)
                continue;
            int32_t status = SAO_ERR_OS_CALL_FAILED;
            try {
                status = data_source.stop(data_source.user_data);
            } catch (...) {
                status = SAO_ERR_OS_CALL_FAILED;
            }
            if (status != SAO_OK)
                return status;
        }
        const int32_t provider_status = destroy_entity_providers(providers);
        if (provider_status != SAO_OK)
            return provider_status;
        const int32_t platform_status = plugin_context_release_platform(ctx);
        if (platform_status != SAO_OK)
            return platform_status;
        {
            std::lock_guard lock(ctx->mutex);
            ctx->engines.clear();
            ctx->data_sources.clear();
            ctx->entity_providers.clear();
        }
        plugin_remove_extensions(ctx->plugin);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_destroy(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_OK;
    bool destruction_started = false;
    try {
        const int32_t begin_status = begin_context_destruction(ctx);
        if (begin_status != SAO_OK)
            return begin_status;
        destruction_started = true;
        const int32_t status = plugin_context_release_resources(ctx);
        if (status != SAO_OK) {
            cancel_context_destruction(ctx);
            return status;
        }
        {
            std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
            if (ctx->plugin_owner->context == ctx) {
                ctx->plugin_owner->context = nullptr;
            }
        }
        finish_context_destruction(ctx);
        delete ctx;
        return SAO_OK;
    } catch (...) {
        if (destruction_started)
            cancel_context_destruction(ctx);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_platform_provider(const plugin_context_platform_provider* provider) {
    if (provider == nullptr ||
        (provider->abi_version >> 16u) != SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < kPlatformProviderMinimumSize) {
        return provider == nullptr ? SAO_ERR_INVALID_ARGUMENT : SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
    plugin_context_platform_provider copy{};
    std::memcpy(&copy, provider, std::min<size_t>(provider->struct_size, sizeof(copy)));
    if (copy.retain == nullptr || copy.release == nullptr || copy.create_session == nullptr ||
        copy.quiesce_session == nullptr || copy.destroy_session == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(g_platform_provider_mutex);
        if (g_platform_provider.present) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        if (g_platform_provider.active_contexts != 0) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        g_platform_provider.value = copy;
        g_platform_provider.present = true;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_unregister_platform_provider() {
    const int32_t quarantine_status = drain_platform_quarantine();
    if (quarantine_status != SAO_OK)
        return quarantine_status;
    try {
        std::lock_guard lock(g_platform_provider_mutex);
        if (!g_platform_provider.present)
            return SAO_ERR_HANDLE_INVALID;
        if (g_platform_provider.active_contexts != 0) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        g_platform_provider.value = {};
        g_platform_provider.present = false;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API plugin_context_t* SAO_PLUGINS_CALL
sao_plugins_ctx_create(plugin_handle_t plugin) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr)
        return nullptr;
    try {
        const auto manifest = manifest_snapshot(plugin);
        auto context = std::make_unique<plugin_context_t>();
        context->plugin = plugin;
        context->plugin_owner = retained;
        context->plugin_id = manifest.plugin_id;
        context->plugin_path_utf8 = manifest.source_path;
        context->plugin_version = manifest.version;
        context->path = std::filesystem::u8path(manifest.source_path).native();
        {
            std::lock_guard lock(g_platform_provider_mutex);
            if (!g_platform_provider.present) {
                if (!register_context_lifetime(context.get()))
                    return nullptr;
                return context.release();
            }
            context->platform_provider = g_platform_provider.value;
            ++g_platform_provider.active_contexts;
            context->platform_active_counted = true;
        }
        try {
            context->platform_provider.retain(context->platform_provider.user_data);
        } catch (...) {
            std::lock_guard lock(g_platform_provider_mutex);
            if (g_platform_provider.active_contexts > 0)
                --g_platform_provider.active_contexts;
            context->platform_active_counted = false;
            return nullptr;
        }
        context->platform_bound = true;
        plugin_context_platform_session_spec spec{};
        const uint32_t provider_minor = context->platform_provider.abi_version & 0xffffu;
        spec.struct_size = provider_minor >= 3u
                               ? sizeof(spec)
                               : SAO_PLUGIN_CONTEXT_PLATFORM_SESSION_SPEC_V1_2_SIZE;
        spec.plugin_id_utf8 = context->plugin_id.c_str();
        spec.plugin_path_utf8 = context->plugin_path_utf8.c_str();
        spec.plugin_version_utf8 = context->plugin_version.c_str();
        if (provider_minor >= 3u) {
            spec.callback_gate_user_data = context.get();
            spec.enter_callback = &enter_platform_callback;
            spec.leave_callback = &leave_platform_callback;
        }
        int32_t create_status = SAO_ERR_OS_CALL_FAILED;
        try {
            create_status = context->platform_provider.create_session(
                context->platform_provider.user_data, &spec, &context->platform_session);
        } catch (...) {
            create_status = SAO_ERR_OS_CALL_FAILED;
        }
        if (create_status != SAO_OK || context->platform_session == nullptr) {
            context->platform_quiesced = true;
            context->platform_session_destroyed = context->platform_session == nullptr;
            if (plugin_context_release_platform(context.get()) != SAO_OK)
                quarantine_platform_context(std::move(context));
            return nullptr;
        }
        if (!register_context_lifetime(context.get())) {
            if (plugin_context_release_platform(context.get()) != SAO_OK)
                quarantine_platform_context(std::move(context));
            return nullptr;
        }
        return context.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ctx_destroy(plugin_context_t* ctx) {
    if (ctx == nullptr)
        return;
    {
        context_registration_lease lifetime;
        if (!lifetime.acquire(ctx))
            return;
        std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
        if (ctx->plugin_owner->context == ctx)
            return;
    }
    (void)plugin_context_destroy(ctx);
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_ctx_plugin_id(plugin_context_t* ctx) {
    return ctx == nullptr ? "" : ctx->plugin_id.c_str();
}

extern "C" SAO_PLUGINS_API const wchar_t* SAO_PLUGINS_CALL
sao_plugins_ctx_path(plugin_context_t* ctx) {
    return ctx == nullptr ? L"" : ctx->path.c_str();
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ctx_should_stop(plugin_context_t* ctx) {
    return ctx == nullptr || ctx->stop_requested.load() || ctx->plugin->stop_requested.load();
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ctx_log(plugin_context_t* ctx,
                                                                     const char* utf8_message) {
    if (ctx != nullptr && utf8_message != nullptr)
        std::clog << "[plugin:" << ctx->plugin_id << "] " << utf8_message << '\n';
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_subscribe(plugin_context_t* ctx, const char* topic_utf8, event_callback_fn callback,
                          void* user_data, uint32_t* out_token) {
    if (ctx == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0' || callback == nullptr ||
        out_token == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto token = ctx->next_token++;
        auto subscription = std::make_shared<event_subscription_state>();
        subscription->token = token;
        subscription->topic = topic_utf8;
        subscription->callback = callback;
        subscription->user_data = user_data;
        ctx->subscriptions.push_back(std::move(subscription));
        *out_token = token;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_subscribe_once(plugin_context_t* ctx, const char* topic_utf8,
                               event_callback_fn callback, void* user_data, uint32_t* out_token) {
    if (ctx == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0' || callback == nullptr ||
        out_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(ctx->mutex);
        const auto token = ctx->next_token++;
        auto subscription = std::make_shared<event_subscription_state>();
        subscription->token = token;
        subscription->topic = topic_utf8;
        subscription->callback = callback;
        subscription->user_data = user_data;
        subscription->once = true;
        ctx->subscriptions.push_back(std::move(subscription));
        *out_token = token;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unsubscribe(plugin_context_t* ctx, uint32_t token) {
    if (ctx == nullptr || token == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::shared_ptr<event_subscription_state> subscription;
        {
            std::lock_guard lock(ctx->mutex);
            const auto found =
                std::find_if(ctx->subscriptions.begin(), ctx->subscriptions.end(),
                             [token](const auto& item) { return item->token == token; });
            if (found == ctx->subscriptions.end())
                return SAO_ERR_HANDLE_INVALID;
            subscription = *found;
        }
        const int32_t status = quiesce_event_subscription(subscription);
        if (status != SAO_OK)
            return status;
        std::lock_guard lock(ctx->mutex);
        const auto old_size = ctx->subscriptions.size();
        std::erase(ctx->subscriptions, subscription);
        return ctx->subscriptions.size() != old_size ? SAO_OK : SAO_ERR_HANDLE_INVALID;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_emit(plugin_context_t* ctx, const char* topic_utf8, const char* payload_json_utf8) {
    if (ctx == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0')
        return SAO_ERR_INVALID_ARGUMENT;
    parsed_context_json input;
    if (!parse_context_json(payload_json_utf8 ? payload_json_utf8 : "null", input))
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::vector<std::shared_ptr<event_subscription_state>> callbacks;
        const json envelope = {{"topic", topic_utf8}, {"payload", input.value}};
        const auto serialized = envelope.dump();
        {
            std::lock_guard lock(ctx->mutex);
            auto candidate_last_events = ctx->last_events;
            auto candidate_recent_events = ctx->recent_events;
            candidate_last_events[topic_utf8] = envelope;
            candidate_recent_events.push_back(envelope);
            if (candidate_recent_events.size() > 256)
                candidate_recent_events.erase(candidate_recent_events.begin());
            for (const auto& subscription : ctx->subscriptions) {
                if (subscription->topic == topic_utf8 || subscription->topic == "*")
                    callbacks.push_back(subscription);
            }
            ctx->last_events.swap(candidate_last_events);
            ctx->recent_events.swap(candidate_recent_events);
        }
        for (const auto& callback : callbacks) {
            const bool once = callback->once;
            {
                event_invocation_lease lease;
                if (!lease.acquire(callback))
                    continue;
                try {
                    lease.value().callback(topic_utf8, serialized.c_str(), lease.value().user_data);
                } catch (...) {
                    sao_plugins_isolation_record_failure(
                        ctx->plugin, "event callback crossed exception boundary");
                }
            }
            if (once) {
                std::lock_guard lock(ctx->mutex);
                std::erase(ctx->subscriptions, callback);
            }
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_get_snapshot(plugin_context_t* ctx, char** out_snapshot_json) {
    if (ctx == nullptr || out_snapshot_json == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_snapshot_json = nullptr;
    try {
        json snapshot = json::object();
        std::lock_guard lock(ctx->mutex);
        for (const auto& [topic, event] : ctx->last_events)
            snapshot[topic] = event["payload"];
        *out_snapshot_json = duplicate_string(snapshot.dump());
        return *out_snapshot_json != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_snapshot_value(
    plugin_context_t* ctx, const char* path_utf8, char** out_value_json) {
    if (ctx == nullptr || path_utf8 == nullptr || out_value_json == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_value_json = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto iterator = ctx->last_events.find(path_utf8);
        if (iterator == ctx->last_events.end())
            return SAO_ERR_HANDLE_INVALID;
        *out_value_json = duplicate_string(iterator->second["payload"].dump());
        return *out_value_json != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_recent_events(
    plugin_context_t* ctx, uint32_t limit, const char* topic_utf8, char** out_events_json) {
    if (ctx == nullptr || out_events_json == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_events_json = nullptr;
    try {
        json result = json::array();
        std::lock_guard lock(ctx->mutex);
        for (auto iterator = ctx->recent_events.rbegin();
             iterator != ctx->recent_events.rend() && result.size() < limit; ++iterator) {
            if (topic_utf8 == nullptr || topic_utf8[0] == '\0' ||
                iterator->at("topic") == topic_utf8)
                result.push_back(*iterator);
        }
        std::reverse(result.begin(), result.end());
        *out_events_json = duplicate_string(result.dump());
        return *out_events_json != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_get_setting(plugin_context_t* ctx, const char* key, char** out_json_utf8) {
    if (ctx == nullptr || key == nullptr || out_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_json_utf8 = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto iterator = ctx->settings.find(key);
        if (iterator == ctx->settings.end())
            return SAO_ERR_HANDLE_INVALID;
        *out_json_utf8 = duplicate_string(iterator->second.dump());
        return *out_json_utf8 != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_setting(plugin_context_t* ctx, const char* key, const char* value_json_utf8) {
    if (ctx == nullptr || key == nullptr || key[0] == '\0')
        return SAO_ERR_INVALID_ARGUMENT;
    parsed_context_json input;
    if (!parse_context_json(value_json_utf8, input))
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        auto candidate = ctx->settings;
        candidate.insert_or_assign(key, std::move(input.value));
        ctx->settings.swap(candidate);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_defaults(plugin_context_t* ctx, const char* defaults_json_utf8) {
    if (ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    parsed_context_json input;
    if (!parse_context_json(defaults_json_utf8, input) || !input.value.is_object())
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        auto candidate = ctx->settings;
        for (const auto& [key, value] : input.value.items())
            candidate.try_emplace(key, value);
        ctx->settings.swap(candidate);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_ui_panel(
    plugin_context_t* ctx, const char* panel_id, const char* meta_json_utf8,
    render_callback_fn render, action_callback_fn on_action, void* user_data) {
    if (render != nullptr || on_action != nullptr || user_data != nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    return add_extension(ctx, extension_kind::ui_panel, panel_id, meta_json_utf8);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_render_hook(
    plugin_context_t* ctx, const char* surface_utf8, float priority, render_hook_fn hook,
    void* user_data, uint32_t* out_token) {
    if (out_token != nullptr)
        *out_token = 0;
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0' ||
        !std::isfinite(priority) || hook == nullptr || out_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.register_render_hook == nullptr || provider.unregister_render_hook == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.register_render_hook(provider.user_data, lease.session(), surface_utf8,
                                               priority, hook, user_data, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK)
        return status;
    if (provider_token == 0)
        return SAO_ERR_HANDLE_INVALID;
    uint32_t loader_token = 0;
    try {
        std::lock_guard lock(ctx->mutex);
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            ctx->platform_resources.push_back(
                {platform_resource_kind::render_hook, loader_token, provider_token, {}});
        }
    } catch (...) {
        loader_token = 0;
    }
    if (loader_token == 0) {
        (void)provider.unregister_render_hook(provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_token = loader_token;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_render_hook(plugin_context_t* ctx, uint32_t token) {
    if (ctx == nullptr || token == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [token](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::render_hook &&
                                    candidate.loader_token == token;
                         });
        if (found == ctx->platform_resources.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        resource = *found;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx, true))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status =
        unregister_platform_resource(lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
        return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources, [&resource](const platform_resource_record& candidate) {
        return candidate.kind == resource.kind && candidate.loader_token == resource.loader_token;
    });
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_overlay(
    plugin_context_t* ctx, const char* surface_utf8, const char* spec_json_utf8) {
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0' ||
        spec_json_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    parsed_context_json input;
    if (!parse_context_json(spec_json_utf8, input))
        return SAO_ERR_INVALID_ARGUMENT;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.set_overlay == nullptr || provider.clear_overlay == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    {
        std::lock_guard lock(ctx->mutex);
        if (has_platform_key_locked(ctx, platform_resource_kind::overlay, surface_utf8)) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.set_overlay(provider.user_data, lease.session(), surface_utf8,
                                      input.text.c_str(), &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK)
        return status;
    if (provider_token == 0)
        return SAO_ERR_HANDLE_INVALID;
    bool recorded = false;
    try {
        std::lock_guard lock(ctx->mutex);
        if (!has_platform_key_locked(ctx, platform_resource_kind::overlay, surface_utf8)) {
            const uint32_t token = allocate_context_token_locked(ctx);
            if (token != 0) {
                ctx->platform_resources.push_back(
                    {platform_resource_kind::overlay, token, provider_token, surface_utf8});
                recorded = true;
            }
        }
    } catch (...) {
    }
    if (!recorded) {
        (void)provider.clear_overlay(provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_clear_overlay(plugin_context_t* ctx, const char* surface_utf8) {
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [surface_utf8](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::overlay &&
                                    candidate.key == surface_utf8;
                         });
        if (found == ctx->platform_resources.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        resource = *found;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx, true))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status =
        unregister_platform_resource(lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
        return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources, [&resource](const platform_resource_record& candidate) {
        return candidate.kind == resource.kind && candidate.loader_token == resource.loader_token;
    });
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_request_redraw(
    plugin_context_t* ctx, const char* surface_utf8, const char* reason_utf8) {
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.request_redraw == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    try {
        return provider.request_redraw(provider.user_data, lease.session(), surface_utf8,
                                       reason_utf8 == nullptr ? "" : reason_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_extension(
    plugin_context_t* ctx, const char* kind_utf8, const char* extension_id_utf8,
    const char* metadata_json_utf8, void* handler, void* user_data) {
    if (kind_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (handler != nullptr || user_data != nullptr)
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    bool valid = false;
    const auto kind = parse_extension_kind(kind_utf8, valid);
    return valid ? add_extension(ctx, kind, extension_id_utf8, metadata_json_utf8)
                 : SAO_ERR_INVALID_ARGUMENT;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_menu_category(
    plugin_context_t* ctx, const char* name_utf8, const char* icon_utf8, void* builder,
    float priority, void* user_data) {
    if (builder != nullptr || user_data != nullptr)
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    try {
        json metadata = {{"title", name_utf8 ? name_utf8 : ""},
                         {"icon", icon_utf8 ? icon_utf8 : ""},
                         {"priority", priority}};
        return add_extension(ctx, extension_kind::menu_category, name_utf8,
                             metadata.dump().c_str());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_menu_surface(plugin_context_t*, const char*, const char*, float) {
    return SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_data_source(
    plugin_context_t* ctx, const char* source_id_utf8, const char* metadata_json_utf8,
    data_source_start_fn start, data_source_stop_fn stop, void* user_data) {
    if (ctx == nullptr || start == nullptr || stop == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const auto status =
        add_extension(ctx, extension_kind::data_source, source_id_utf8, metadata_json_utf8);
    if (status != SAO_OK)
        return status;
    try {
        std::lock_guard lock(ctx->mutex);
        ctx->data_sources[source_id_utf8] = {start, stop, user_data};
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_register_entity_providers(plugin_context_t* ctx,
                                                 const native_entity_provider_descriptor* providers,
                                                 size_t count) noexcept {
    return plugin_context_register_entity_provider_arrays(ctx, providers, count, nullptr, 0, 0);
}

int32_t plugin_context_register_entity_provider_arrays(
    plugin_context_t* ctx, const native_entity_provider_descriptor* providers_v1,
    size_t provider_v1_count, const void* providers_v2, size_t provider_v2_count,
    uint32_t provider_v2_stride_bytes) noexcept {
    if (ctx == nullptr || (provider_v1_count > 0 && providers_v1 == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (provider_v2_count == 0) {
        if (providers_v2 != nullptr || provider_v2_stride_bytes != 0)
            return SAO_ERR_INVALID_ARGUMENT;
    } else if (providers_v2 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    } else if (provider_v2_stride_bytes < kNativeEntityProviderDescriptorV2RequiredPrefixSize) {
        return SAO_PLUGINS_ERR_ABI_MISMATCH;
    } else if (provider_v2_stride_bytes % alignof(native_entity_provider_descriptor_v2) != 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    } else if (provider_v2_stride_bytes > kMaximumNativeEntityProviderDescriptorSize ||
               provider_v2_count > kMaximumNativeEntityProviderDescriptorSpanBytes / provider_v2_stride_bytes) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (provider_v1_count > kMaximumEntityProvidersPerContext ||
        provider_v2_count > kMaximumEntityProvidersPerContext - provider_v1_count) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const size_t total_count = provider_v1_count + provider_v2_count;
    if (provider_v2_count > 0) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(providers_v2);
        constexpr uintptr_t kMaximumAddress = (std::numeric_limits<uintptr_t>::max)();
        if (provider_v2_count > kMaximumAddress / provider_v2_stride_bytes) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const uintptr_t provider_v2_bytes =
            static_cast<uintptr_t>(provider_v2_count) * provider_v2_stride_bytes;
        if (base > kMaximumAddress - provider_v2_bytes) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    context_registration_lease lifetime;
    if (!lifetime.acquire(ctx))
        return SAO_ERR_HANDLE_INVALID;
    std::vector<std::shared_ptr<entity_provider_state>> registered;
    try {
        std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
        std::lock_guard context_lock(ctx->mutex);
        if (ctx->plugin_owner->context != ctx)
            return SAO_ERR_HANDLE_INVALID;
        if (ctx->entity_providers.size() > kMaximumEntityProvidersPerContext ||
            total_count > kMaximumEntityProvidersPerContext - ctx->entity_providers.size()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        ctx->entity_providers.reserve(ctx->entity_providers.size() + total_count);
        registered.reserve(total_count);
        const auto rollback_registered = [&registered](int32_t status) noexcept {
            const int32_t rollback_status = destroy_entity_providers(registered);
            return rollback_status == SAO_OK ? status : rollback_status;
        };
        uintptr_t descriptor_address = reinterpret_cast<uintptr_t>(providers_v1);
        size_t descriptor_span = 0;
        for (size_t index = 0; index < provider_v1_count; ++index) {
            native_entity_provider_descriptor descriptor{};
            const int32_t descriptor_status = copy_external_entity_struct(
                reinterpret_cast<const native_entity_provider_descriptor*>(descriptor_address),
                kNativeEntityProviderDescriptorRequiredPrefixSize, descriptor);
            if (descriptor_status != SAO_OK) {
                return rollback_registered(descriptor_status);
            }
            if (descriptor.struct_size > kMaximumNativeEntityProviderDescriptorSize ||
                descriptor_span > kMaximumNativeEntityProviderDescriptorSpanBytes -
                                    descriptor.struct_size ||
                descriptor.struct_size >
                    (std::numeric_limits<uintptr_t>::max)() - descriptor_address) {
                return rollback_registered(SAO_ERR_INVALID_ARGUMENT);
            }
            std::shared_ptr<entity_provider_state> provider;
            const int32_t status = register_entity_provider(
                ctx->plugin_owner, ctx->plugin_id, descriptor.provider_id_utf8, descriptor.snapshot,
                descriptor.action_handler, descriptor.user_data, nullptr, provider);
            if (status != SAO_OK) {
                return rollback_registered(status);
            }
            registered.push_back(std::move(provider));
            descriptor_address += descriptor.struct_size;
            descriptor_span += descriptor.struct_size;
        }

        const uintptr_t descriptor_v2_base = reinterpret_cast<uintptr_t>(providers_v2);
        for (size_t index = 0; index < provider_v2_count; ++index) {
            const uintptr_t current_address =
                descriptor_v2_base + index * static_cast<uintptr_t>(provider_v2_stride_bytes);
            native_entity_provider_descriptor_v2 descriptor{};
            const int32_t descriptor_status = copy_external_entity_struct(
                reinterpret_cast<const native_entity_provider_descriptor_v2*>(current_address),
                kNativeEntityProviderDescriptorV2RequiredPrefixSize, descriptor);
            if (descriptor_status != SAO_OK) {
                return rollback_registered(descriptor_status);
            }
            if (descriptor.struct_size < sizeof(descriptor))
                descriptor.root_contribution = nullptr;
            if (descriptor.struct_size > provider_v2_stride_bytes) {
                return rollback_registered(SAO_ERR_INVALID_ARGUMENT);
            }
            std::shared_ptr<entity_provider_state> provider;
            const int32_t status = register_entity_provider_v2(
                ctx->plugin_owner, ctx->plugin_id, descriptor.provider_id_utf8,
                descriptor.snapshot_v2, descriptor.action_handler, descriptor.user_data,
                descriptor.root_contribution, provider);
            if (status != SAO_OK) {
                return rollback_registered(status);
            }
            registered.push_back(std::move(provider));
        }
        ctx->entity_providers.insert(ctx->entity_providers.end(), registered.begin(),
                                     registered.end());
        return SAO_OK;
    } catch (...) {
        (void)destroy_entity_providers(registered);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

namespace {

int32_t SAO_PLUGINS_CALL empty_entity_snapshot_v2(
    void* rows, uint32_t capacity, uint32_t row_stride_bytes, uint32_t* out_count,
    uint64_t* out_revision, entity_snapshot_content_token_t* out_content_token,
    uint32_t* out_row_stride_bytes, void*) {
    if (out_count == nullptr || out_revision == nullptr || out_content_token == nullptr ||
        out_row_stride_bytes == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (rows != nullptr || capacity != 0 || row_stride_bytes != 0)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    *out_revision = 1;
    *out_content_token = 0x53414f4143545632ULL;
    *out_row_stride_bytes = 0;
    return SAO_OK;
}

int32_t register_context_entity_provider(
    plugin_context_t* ctx, const char* provider_id_utf8, entity_snapshot_callback_fn snapshot_v1,
    entity_snapshot_callback_v2_fn snapshot_v2, entity_action_handler_fn action_handler,
    void* user_data, const entity_root_contribution_descriptor* root_contribution) noexcept {
    if (ctx == nullptr || provider_id_utf8 == nullptr ||
        (snapshot_v1 == nullptr) == (snapshot_v2 == nullptr) || action_handler == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    context_registration_lease lifetime;
    if (!lifetime.acquire(ctx))
        return SAO_ERR_HANDLE_INVALID;
    std::shared_ptr<entity_provider_state> provider;
    try {
        std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
        std::lock_guard context_lock(ctx->mutex);
        if (ctx->plugin_owner->context != ctx) {
            return SAO_ERR_HANDLE_INVALID;
        }
        const lifecycle_state state = ctx->plugin_owner->state;
        if (state != lifecycle_state::loading && state != lifecycle_state::loaded_disabled &&
            state != lifecycle_state::enabling && state != lifecycle_state::loaded_active) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        if (state == lifecycle_state::enabling && ctx->entity_provider_publication_complete) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        if (ctx->entity_providers.size() >= kMaximumEntityProvidersPerContext)
            return SAO_ERR_INVALID_ARGUMENT;

        int32_t status =
            snapshot_v1 != nullptr
                ? register_entity_provider(ctx->plugin_owner, ctx->plugin_id, provider_id_utf8,
                                           snapshot_v1, action_handler, user_data,
                                           root_contribution, provider)
                : register_entity_provider_v2(ctx->plugin_owner, ctx->plugin_id, provider_id_utf8,
                                              snapshot_v2, action_handler, user_data,
                                              root_contribution, provider);
        if (status != SAO_OK)
            return status;

        ctx->entity_providers.reserve(ctx->entity_providers.size() + 1);
        if (state == lifecycle_state::loaded_active) {
            status = activate_entity_providers({provider});
            if (status != SAO_OK) {
                (void)destroy_entity_providers({provider});
                return status;
            }
        }
        ctx->entity_providers.push_back(provider);
        return SAO_OK;
    } catch (...) {
        if (provider != nullptr) {
            (void)destroy_entity_providers({provider});
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider(
    plugin_context_t* ctx, const context_entity_provider_descriptor* descriptor) {
    if (ctx == nullptr || descriptor == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    context_entity_provider_descriptor current{};
    const int32_t descriptor_status = copy_external_entity_struct(
        descriptor, kContextEntityProviderDescriptorRequiredPrefixSize, current);
    if (descriptor_status != SAO_OK)
        return descriptor_status;
    return register_context_entity_provider(ctx, current.provider_id_utf8, current.snapshot,
                                            nullptr, current.action_handler, current.user_data,
                                            current.root_contribution);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider_v2(
    plugin_context_t* ctx, const context_entity_provider_descriptor_v2* descriptor) {
    if (ctx == nullptr || descriptor == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    context_entity_provider_descriptor_v2 current{};
    const int32_t descriptor_status = copy_external_entity_struct(
        descriptor, kContextEntityProviderDescriptorV2RequiredPrefixSize, current);
    if (descriptor_status != SAO_OK)
        return descriptor_status;
    return register_context_entity_provider(ctx, current.provider_id_utf8, nullptr,
                                            current.snapshot, current.action_handler,
                                            current.user_data, current.root_contribution);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider_v3(
    plugin_context_t* ctx, const context_entity_provider_descriptor_v3* descriptor) {
    if (ctx == nullptr || descriptor == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    context_entity_provider_descriptor_v3 current{};
    const int32_t descriptor_status = copy_external_entity_struct(
        descriptor, kContextEntityProviderDescriptorV3RequiredPrefixSize, current);
    if (descriptor_status != SAO_OK)
        return descriptor_status;
    if (current.struct_size <
        offsetof(context_entity_provider_descriptor_v3, flags) + sizeof(current.flags)) {
        current.flags = 0;
    }
    if (current.struct_size <
        offsetof(context_entity_provider_descriptor_v3, reserved) + sizeof(current.reserved)) {
        current.reserved = 0;
    }
    if (current.action_handler != nullptr || current.action_handler_v2 == nullptr ||
        (current.flags & ~kContextEntityProviderV3ActionOnly) != 0 || current.reserved != 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const bool action_only = (current.flags & kContextEntityProviderV3ActionOnly) != 0;
    if ((action_only && (current.snapshot != nullptr || current.user_data != nullptr ||
                         current.root_contribution != nullptr)) ||
        (!action_only && current.snapshot == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    std::string local_provider_id;
    const int32_t provider_id_status =
        copy_entity_provider_id(current.provider_id_utf8, local_provider_id);
    if (provider_id_status != SAO_OK)
        return provider_id_status;
    if (local_provider_id.find('/') != std::string::npos) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    context_registration_lease lifetime;
    if (!lifetime.acquire(ctx))
        return SAO_ERR_HANDLE_INVALID;
    std::shared_ptr<entity_provider_state> provider;
    try {
        std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
        std::lock_guard context_lock(ctx->mutex);
        if (ctx->plugin_owner->context != ctx)
            return SAO_ERR_HANDLE_INVALID;
        const lifecycle_state state = ctx->plugin_owner->state;
        if (state != lifecycle_state::loading && state != lifecycle_state::loaded_disabled &&
            state != lifecycle_state::enabling && state != lifecycle_state::loaded_active) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        if (state == lifecycle_state::enabling && ctx->entity_provider_publication_complete)
            return SAO_PLUGINS_ERR_BUSY;

        const std::string qualified_provider_id = ctx->plugin_id + "/" + local_provider_id;
        const auto existing =
            std::find_if(ctx->entity_providers.begin(), ctx->entity_providers.end(),
                         [&qualified_provider_id](const auto& candidate) {
                             return entity_provider_has_id(candidate, qualified_provider_id);
                         });
        if (existing != ctx->entity_providers.end()) {
            return action_only ? replace_entity_provider_action_v2(
                                     *existing, current.action_handler_v2, current.action_user_data)
                               : SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        if (ctx->entity_providers.size() >= kMaximumEntityProvidersPerContext)
            return SAO_ERR_INVALID_ARGUMENT;

        const int32_t status = register_entity_provider_v3(
            ctx->plugin_owner, ctx->plugin_id, current.provider_id_utf8,
            action_only ? empty_entity_snapshot_v2 : current.snapshot, current.action_handler_v2,
            current.user_data, current.action_user_data,
            action_only ? nullptr : current.root_contribution, provider);
        if (status != SAO_OK)
            return status;

        ctx->entity_providers.reserve(ctx->entity_providers.size() + 1);
        if (state == lifecycle_state::loaded_active) {
            const int32_t activate_status = activate_entity_providers({provider});
            if (activate_status != SAO_OK) {
                (void)destroy_entity_providers({provider});
                return activate_status;
            }
        }
        ctx->entity_providers.push_back(provider);
        return SAO_OK;
    } catch (...) {
        if (provider != nullptr)
            (void)destroy_entity_providers({provider});
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_unregister_entity_providers(plugin_context_t* ctx,
                                                   const char* const* provider_ids_utf8,
                                                   size_t count) noexcept {
    if (ctx == nullptr || (count > 0 && provider_ids_utf8 == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (count > kMaximumEntityProvidersPerContext)
        return SAO_ERR_INVALID_ARGUMENT;
    context_registration_lease lifetime;
    if (!lifetime.acquire(ctx))
        return SAO_ERR_HANDLE_INVALID;
    try {
        std::unordered_set<std::string> provider_ids;
        provider_ids.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            std::string provider_id;
            if (copy_entity_provider_id(provider_ids_utf8[index], provider_id) != SAO_OK ||
                !provider_ids.emplace(std::move(provider_id)).second) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
        }

        std::vector<std::shared_ptr<entity_provider_state>> providers;
        {
            std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
            std::lock_guard context_lock(ctx->mutex);
            if (ctx->plugin_owner->context != ctx) {
                return SAO_ERR_HANDLE_INVALID;
            }
            providers.reserve(provider_ids.size());
            for (const auto& provider : ctx->entity_providers) {
                const auto found =
                    std::find_if(provider_ids.begin(), provider_ids.end(),
                                 [&provider](const std::string& provider_id) {
                                     return entity_provider_has_id(provider, provider_id);
                                 });
                if (found != provider_ids.end())
                    providers.push_back(provider);
            }
        }
        if (providers.empty())
            return SAO_OK;

        const int32_t status = destroy_entity_providers(providers);
        if (status != SAO_OK)
            return status;

        std::unordered_set<const entity_provider_state*> removed;
        removed.reserve(providers.size());
        for (const auto& provider : providers)
            removed.emplace(provider.get());
        {
            std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
            std::lock_guard context_lock(ctx->mutex);
            std::erase_if(ctx->entity_providers, [&removed](const auto& provider) {
                return removed.contains(provider.get());
            });
            if (ctx->entity_providers.empty()) {
                ctx->entity_provider_publication_complete = false;
            }
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_engine(plugin_context_t* ctx, const char* name_utf8, void* engine_ptr) {
    if (ctx == nullptr || name_utf8 == nullptr || name_utf8[0] == '\0' || engine_ptr == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        if (ctx->engines.contains(name_utf8))
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        ctx->engines.emplace(name_utf8, engine_ptr);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ctx_get_engine(plugin_context_t* ctx, const char* name_utf8) {
    if (ctx == nullptr || name_utf8 == nullptr)
        return nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto iterator = ctx->engines.find(name_utf8);
        return iterator == ctx->engines.end() ? nullptr : iterator->second;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_hotkey(
    plugin_context_t* ctx, const char* hotkey_id, const char* default_key, const char* label,
    hotkey_callback_fn callback, void* user_data) {
    if (ctx == nullptr || hotkey_id == nullptr || hotkey_id[0] == '\0' || default_key == nullptr ||
        default_key[0] == '\0' || callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.register_hotkey == nullptr || provider.unregister_hotkey == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    {
        std::lock_guard lock(ctx->mutex);
        if (has_platform_key_locked(ctx, platform_resource_kind::hotkey, hotkey_id)) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.register_hotkey(provider.user_data, lease.session(), hotkey_id,
                                          default_key, label == nullptr ? "" : label, callback,
                                          user_data, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK)
        return status;
    if (provider_token == 0)
        return SAO_ERR_HANDLE_INVALID;
    bool recorded = false;
    try {
        std::lock_guard lock(ctx->mutex);
        if (!has_platform_key_locked(ctx, platform_resource_kind::hotkey, hotkey_id)) {
            const uint32_t token = allocate_context_token_locked(ctx);
            if (token != 0) {
                ctx->platform_resources.push_back(
                    {platform_resource_kind::hotkey, token, provider_token, hotkey_id});
                recorded = true;
            }
        }
    } catch (...) {
    }
    if (!recorded) {
        (void)provider.unregister_hotkey(provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_hotkey(plugin_context_t* ctx, const char* hotkey_id) {
    if (ctx == nullptr || hotkey_id == nullptr || hotkey_id[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [hotkey_id](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::hotkey &&
                                    candidate.key == hotkey_id;
                         });
        if (found == ctx->platform_resources.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        resource = *found;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx, true))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status =
        unregister_platform_resource(lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
        return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources, [&resource](const platform_resource_record& candidate) {
        return candidate.kind == resource.kind && candidate.loader_token == resource.loader_token;
    });
    return SAO_OK;
}

namespace {

int32_t register_timer(plugin_context_t* ctx, timer_callback_fn callback, double seconds,
                       void* user_data, bool one_shot, char** out_token) {
    if (out_token != nullptr)
        *out_token = nullptr;
    if (ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    if (callback == nullptr || out_token == nullptr || !std::isfinite(seconds) || seconds <= 0.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto& provider = lease.provider();
    if (provider.register_timer == nullptr || provider.unregister_timer == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.register_timer(provider.user_data, lease.session(), seconds, one_shot,
                                         callback, user_data, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK)
        return status;
    if (provider_token == 0)
        return SAO_ERR_HANDLE_INVALID;

    uint32_t loader_token = 0;
    char* token_copy = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            const std::string token_text = std::to_string(loader_token);
            token_copy = duplicate_string(token_text);
            if (token_copy != nullptr) {
                platform_resource_record resource;
                resource.kind = platform_resource_kind::timer;
                resource.loader_token = loader_token;
                resource.provider_token = provider_token;
                resource.key = token_text;
                resource.one_shot = one_shot;
                ctx->platform_resources.push_back(std::move(resource));
            }
        }
    } catch (...) {
        delete[] token_copy;
        token_copy = nullptr;
    }
    if (token_copy == nullptr) {
        (void)provider.unregister_timer(provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_token = token_copy;
    return SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_interval(plugin_context_t* ctx, timer_callback_fn callback, double seconds,
                             void* user_data, char** out_token) {
    return register_timer(ctx, callback, seconds, user_data, false, out_token);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_timeout(plugin_context_t* ctx, timer_callback_fn callback, double seconds,
                            void* user_data, char** out_token) {
    return register_timer(ctx, callback, seconds, user_data, true, out_token);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_clear_timer(plugin_context_t* ctx, const char* token) {
    if (ctx == nullptr || token == nullptr || token[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found = std::find_if(
            ctx->platform_resources.begin(), ctx->platform_resources.end(),
            [token](const platform_resource_record& candidate) {
                return candidate.kind == platform_resource_kind::timer && candidate.key == token;
            });
        if (found == ctx->platform_resources.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        resource = *found;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx, true))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status =
        unregister_platform_resource(lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
        return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources, [&resource](const platform_resource_record& candidate) {
        return candidate.kind == resource.kind && candidate.loader_token == resource.loader_token;
    });
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_complete_timer(plugin_context_t* ctx, const char* token) {
    if (ctx == nullptr || token == nullptr || token[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(ctx->mutex);
        const auto found = std::find_if(
            ctx->platform_resources.begin(), ctx->platform_resources.end(),
            [token](const platform_resource_record& candidate) {
                return candidate.kind == platform_resource_kind::timer && candidate.key == token;
            });
        if (found == ctx->platform_resources.end())
            return SAO_ERR_HANDLE_INVALID;
        if (!found->one_shot)
            return SAO_ERR_INVALID_ARGUMENT;
        ctx->platform_resources.erase(found);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_notify(plugin_context_t* ctx,
                                                                           const char* title_utf8,
                                                                           const char* message_utf8,
                                                                           double duration_s,
                                                                           const char* kind_utf8) {
    if (ctx == nullptr || message_utf8 == nullptr || !std::isfinite(duration_s) ||
        duration_s <= 0.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.show_notify == nullptr || provider.dismiss_notify == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.show_notify(
            provider.user_data, lease.session(), title_utf8 == nullptr ? "" : title_utf8,
            message_utf8, duration_s, kind_utf8 == nullptr ? "info" : kind_utf8, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK)
        return status;
    if (provider_token == 0)
        return SAO_ERR_HANDLE_INVALID;
    try {
        std::lock_guard lock(ctx->mutex);
        const uint32_t token = allocate_context_token_locked(ctx);
        if (token != 0) {
            ctx->platform_resources.push_back(
                {platform_resource_kind::notify, token, provider_token, {}});
            return SAO_OK;
        }
    } catch (...) {
    }
    (void)provider.dismiss_notify(provider.user_data, lease.session(), provider_token);
    return SAO_ERR_OS_CALL_FAILED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_dismiss_notify(plugin_context_t* ctx) {
    if (ctx == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    while (true) {
        platform_resource_record resource;
        {
            std::lock_guard lock(ctx->mutex);
            const auto found =
                std::find_if(ctx->platform_resources.rbegin(), ctx->platform_resources.rend(),
                             [](const platform_resource_record& candidate) {
                                 return candidate.kind == platform_resource_kind::notify;
                             });
            if (found == ctx->platform_resources.rend())
                return SAO_OK;
            resource = *found;
        }
        platform_call_lease lease;
        if (!lease.acquire(ctx, true))
            return SAO_PLUGINS_ERR_UNSUPPORTED;
        const int32_t status =
            unregister_platform_resource(lease.provider(), lease.session(), resource);
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
            return status;
        std::lock_guard lock(ctx->mutex);
        std::erase_if(ctx->platform_resources,
                      [&resource](const platform_resource_record& candidate) {
                          return candidate.kind == resource.kind &&
                                 candidate.loader_token == resource.loader_token;
                      });
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_toast(plugin_context_t* ctx, const char* message_utf8) {
    return sao_plugins_ctx_notify(ctx, "", message_utf8, 3.0, "info");
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_open_file(
    plugin_context_t* ctx, const char* filters_json_utf8, const char* title_utf8,
    const wchar_t* initial_dir, intptr_t hwnd_owner, wchar_t** out_selected_path) {
    if (out_selected_path != nullptr)
        *out_selected_path = nullptr;
    if (ctx == nullptr || out_selected_path == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    parsed_context_json filters;
    if (filters_json_utf8 != nullptr) {
        if (!parse_context_json(filters_json_utf8, filters, true)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.open_file == nullptr || provider.release_file_result == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }

    uint32_t loader_token = 0;
    try {
        std::lock_guard lock(ctx->mutex);
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            ctx->platform_resources.push_back(
                {platform_resource_kind::file_result, loader_token, 0, {}});
        }
    } catch (...) {
        loader_token = 0;
    }
    if (loader_token == 0)
        return SAO_ERR_OS_CALL_FAILED;

    plugin_context_open_file_spec spec{};
    spec.struct_size = sizeof(spec);
    spec.filters_json_utf8 = filters_json_utf8 == nullptr ? "" : filters.text.c_str();
    spec.title_utf8 = title_utf8 == nullptr ? "" : title_utf8;
    spec.initial_dir = initial_dir == nullptr ? L"" : initial_dir;
    spec.hwnd_owner = hwnd_owner;
    const wchar_t* provider_path = nullptr;
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = map_platform_status(provider.open_file(provider.user_data, lease.session(), &spec,
                                                        &provider_path, &provider_token));
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (provider_token != 0) {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [loader_token](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::file_result &&
                                    candidate.loader_token == loader_token;
                         });
        if (found == ctx->platform_resources.end())
            return SAO_ERR_HANDLE_INVALID;
        found->provider_token = provider_token;
    }
    if (status != SAO_OK || provider_token == 0) {
        if (provider_token != 0) {
            platform_resource_record resource;
            resource.kind = platform_resource_kind::file_result;
            resource.loader_token = loader_token;
            resource.provider_token = provider_token;
            const int32_t rollback_status =
                unregister_platform_resource(provider, lease.session(), resource);
            if (rollback_status != SAO_OK && rollback_status != SAO_ERR_HANDLE_INVALID) {
                return rollback_status;
            }
        }
        std::lock_guard lock(ctx->mutex);
        std::erase_if(ctx->platform_resources,
                      [loader_token](const platform_resource_record& candidate) {
                          return candidate.kind == platform_resource_kind::file_result &&
                                 candidate.loader_token == loader_token;
                      });
        if (status != SAO_OK)
            return status;
        return provider_path == nullptr || provider_path[0] == L'\0' ? SAO_OK
                                                                     : SAO_ERR_HANDLE_INVALID;
    }

    const bool has_selected_path = provider_path != nullptr && provider_path[0] != L'\0';
    wchar_t* selected_path = nullptr;
    if (has_selected_path) {
        selected_path = duplicate_wstring(provider_path);
    }
    platform_resource_record resource;
    resource.kind = platform_resource_kind::file_result;
    resource.loader_token = loader_token;
    resource.provider_token = provider_token;
    status = unregister_platform_resource(provider, lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
        delete[] selected_path;
        return status;
    }
    {
        std::lock_guard lock(ctx->mutex);
        std::erase_if(ctx->platform_resources,
                      [loader_token](const platform_resource_record& candidate) {
                          return candidate.kind == platform_resource_kind::file_result &&
                                 candidate.loader_token == loader_token;
                      });
    }
    if (has_selected_path && selected_path == nullptr) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_selected_path = selected_path;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_open_window(
    plugin_context_t* ctx, const char* panel_id_utf8, uint32_t width, uint32_t height) {
    if (ctx == nullptr || panel_id_utf8 == nullptr || panel_id_utf8[0] == '\0' ||
        !valid_compositor_dimensions(width, height)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.open_window == nullptr || provider.close_window == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }

    uint32_t loader_token = 0;
    try {
        std::lock_guard lock(ctx->mutex);
        if (has_platform_key_locked(ctx, platform_resource_kind::window, panel_id_utf8)) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            ctx->platform_resources.push_back(
                {platform_resource_kind::window, loader_token, 0, panel_id_utf8});
        }
    } catch (...) {
        loader_token = 0;
    }
    if (loader_token == 0)
        return SAO_ERR_OS_CALL_FAILED;

    plugin_context_open_window_spec spec{};
    spec.struct_size = sizeof(spec);
    spec.panel_id_utf8 = panel_id_utf8;
    spec.width = width;
    spec.height = height;
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = map_platform_status(
            provider.open_window(provider.user_data, lease.session(), &spec, &provider_token));
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (provider_token != 0) {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [loader_token](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::window &&
                                    candidate.loader_token == loader_token;
                         });
        if (found == ctx->platform_resources.end())
            return SAO_ERR_HANDLE_INVALID;
        found->provider_token = provider_token;
    }
    if (status != SAO_OK || provider_token == 0) {
        if (provider_token != 0) {
            platform_resource_record resource;
            resource.kind = platform_resource_kind::window;
            resource.loader_token = loader_token;
            resource.provider_token = provider_token;
            const int32_t rollback_status =
                unregister_platform_resource(provider, lease.session(), resource);
            if (rollback_status != SAO_OK && rollback_status != SAO_ERR_HANDLE_INVALID) {
                return rollback_status;
            }
        }
        std::lock_guard lock(ctx->mutex);
        std::erase_if(ctx->platform_resources,
                      [loader_token](const platform_resource_record& candidate) {
                          return candidate.kind == platform_resource_kind::window &&
                                 candidate.loader_token == loader_token;
                      });
        return status == SAO_OK ? SAO_ERR_HANDLE_INVALID : status;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_create_compositor_layer(
    plugin_context_t* ctx, const char* name_utf8, uint32_t width, uint32_t height, int32_t x,
    int32_t y, int32_t z, bool click_through, bool high_fps, uint32_t target_fps) {
    if (ctx == nullptr || !valid_compositor_dimensions(width, height, target_fps)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string layer_name;
    const int32_t name_status = copy_compositor_layer_name(name_utf8, layer_name);
    if (name_status != SAO_OK)
        return name_status;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.create_compositor_layer == nullptr ||
        provider.destroy_compositor_layer == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    uint32_t loader_token = 0;
    try {
        std::lock_guard lock(ctx->mutex);
        if (has_platform_key_locked(ctx, platform_resource_kind::compositor_layer, layer_name)) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            platform_resource_record placeholder;
            placeholder.kind = platform_resource_kind::compositor_layer;
            placeholder.loader_token = loader_token;
            placeholder.pending_destroy = true;
            placeholder.compositor_width = width;
            placeholder.compositor_height = height;
            ctx->platform_resources.push_back(std::move(placeholder));
        }
    } catch (...) {
        loader_token = 0;
    }
    if (loader_token == 0)
        return SAO_ERR_OS_CALL_FAILED;

    plugin_context_compositor_layer_spec spec{};
    spec.struct_size = sizeof(spec);
    spec.name_utf8 = layer_name.c_str();
    spec.width = width;
    spec.height = height;
    spec.x = x;
    spec.y = y;
    spec.z = z;
    spec.click_through = click_through;
    spec.high_fps = high_fps;
    spec.target_fps = target_fps;
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = map_platform_status(provider.create_compositor_layer(
            provider.user_data, lease.session(), &spec, &provider_token));
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (provider_token != 0) {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [loader_token](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::compositor_layer &&
                                    candidate.loader_token == loader_token;
                         });
        if (found == ctx->platform_resources.end())
            return SAO_ERR_HANDLE_INVALID;
        found->provider_token = provider_token;
    }
    if (status != SAO_OK || provider_token == 0) {
        int32_t rollback_status = SAO_OK;
        if (provider_token != 0) {
            try {
                rollback_status = map_platform_status(provider.destroy_compositor_layer(
                    provider.user_data, lease.session(), provider_token));
            } catch (...) {
                rollback_status = SAO_ERR_OS_CALL_FAILED;
            }
        }
        if (provider_token == 0 || rollback_status == SAO_OK ||
            rollback_status == SAO_ERR_HANDLE_INVALID) {
            std::lock_guard lock(ctx->mutex);
            std::erase_if(ctx->platform_resources,
                          [loader_token](const platform_resource_record& candidate) {
                              return candidate.kind ==
                                         platform_resource_kind::compositor_layer &&
                                     candidate.loader_token == loader_token;
                          });
        }
        if (rollback_status != SAO_OK && rollback_status != SAO_ERR_HANDLE_INVALID)
            return rollback_status;
        return status == SAO_OK ? SAO_ERR_HANDLE_INVALID : status;
    }

    bool recorded = false;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [loader_token](const platform_resource_record& candidate) {
                             return candidate.kind == platform_resource_kind::compositor_layer &&
                                    candidate.loader_token == loader_token;
                         });
        if (found != ctx->platform_resources.end() &&
            !has_platform_key_locked(ctx, platform_resource_kind::compositor_layer, layer_name)) {
            found->key = layer_name;
            found->pending_destroy = false;
            recorded = true;
        }
    } catch (...) {
    }
    if (!recorded) {
        int32_t rollback_status = SAO_ERR_OS_CALL_FAILED;
        try {
            rollback_status = map_platform_status(provider.destroy_compositor_layer(
                provider.user_data, lease.session(), provider_token));
        } catch (...) {
            rollback_status = SAO_ERR_OS_CALL_FAILED;
        }
        if (rollback_status == SAO_OK || rollback_status == SAO_ERR_HANDLE_INVALID) {
            std::lock_guard lock(ctx->mutex);
            std::erase_if(ctx->platform_resources,
                          [loader_token](const platform_resource_record& candidate) {
                              return candidate.kind ==
                                         platform_resource_kind::compositor_layer &&
                                     candidate.loader_token == loader_token;
                          });
        } else {
            std::lock_guard lock(ctx->mutex);
            const auto found =
                std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                             [loader_token](const platform_resource_record& candidate) {
                                 return candidate.kind ==
                                            platform_resource_kind::compositor_layer &&
                                        candidate.loader_token == loader_token;
                             });
            if (found != ctx->platform_resources.end())
                found->pending_destroy = false;
        }
        return rollback_status == SAO_OK || rollback_status == SAO_ERR_HANDLE_INVALID
                   ? SAO_ERR_OS_CALL_FAILED
                   : rollback_status;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_upload_compositor_frame(
    plugin_context_t* ctx, const char* name_utf8, const uint8_t* bgra_bytes, size_t bytes_len,
    uint32_t width, uint32_t height) {
    if (ctx == nullptr || !valid_bgra_frame(bgra_bytes, bytes_len, width, height)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string layer_name;
    const int32_t name_status = copy_compositor_layer_name(name_utf8, layer_name);
    if (name_status != SAO_OK)
        return name_status;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.upload_compositor_frame == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    compositor_resource_lease resource_lease;
    const int32_t resource_status = resource_lease.acquire(ctx, layer_name);
    if (resource_status != SAO_OK)
        return resource_status;
    if (resource_lease.resource().compositor_width != width ||
        resource_lease.resource().compositor_height != height) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        return map_platform_status(provider.upload_compositor_frame(
            provider.user_data, lease.session(), resource_lease.resource().provider_token,
            bgra_bytes, bytes_len, width, height));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_compositor_layer_position(
    plugin_context_t* ctx, const char* name_utf8, int32_t x, int32_t y) {
    if (ctx == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string layer_name;
    const int32_t name_status = copy_compositor_layer_name(name_utf8, layer_name);
    if (name_status != SAO_OK)
        return name_status;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.set_compositor_layer_position == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    compositor_resource_lease resource_lease;
    const int32_t resource_status = resource_lease.acquire(ctx, layer_name);
    if (resource_status != SAO_OK)
        return resource_status;
    try {
        return map_platform_status(provider.set_compositor_layer_position(
            provider.user_data, lease.session(), resource_lease.resource().provider_token, x, y));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_compositor_layer_visible(
    plugin_context_t* ctx, const char* name_utf8, bool visible) {
    if (ctx == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string layer_name;
    const int32_t name_status = copy_compositor_layer_name(name_utf8, layer_name);
    if (name_status != SAO_OK)
        return name_status;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.set_compositor_layer_visible == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    compositor_resource_lease resource_lease;
    const int32_t resource_status = resource_lease.acquire(ctx, layer_name);
    if (resource_status != SAO_OK)
        return resource_status;
    try {
        return map_platform_status(provider.set_compositor_layer_visible(
            provider.user_data, lease.session(), resource_lease.resource().provider_token,
            visible));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_destroy_compositor_layer(plugin_context_t* ctx, const char* name_utf8) {
    if (ctx == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string layer_name;
    const int32_t name_status = copy_compositor_layer_name(name_utf8, layer_name);
    if (name_status != SAO_OK)
        return name_status;
    platform_call_lease lease;
    if (!lease.acquire(ctx, true))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.destroy_compositor_layer == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const int32_t status = find_compositor_layer_locked(ctx, layer_name, resource);
        if (status != SAO_OK)
            return status;
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [&resource](const platform_resource_record& candidate) {
                             return candidate.kind == resource.kind &&
                                    candidate.loader_token == resource.loader_token;
                         });
        if (found == ctx->platform_resources.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (found->active_calls != 0)
            return SAO_PLUGINS_ERR_BUSY;
        found->pending_destroy = true;
    }

    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = map_platform_status(provider.destroy_compositor_layer(
            provider.user_data, lease.session(), resource.provider_token));
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    {
        std::lock_guard lock(ctx->mutex);
        const auto found =
            std::find_if(ctx->platform_resources.begin(), ctx->platform_resources.end(),
                         [&resource](const platform_resource_record& candidate) {
                             return candidate.kind == resource.kind &&
                                    candidate.loader_token == resource.loader_token;
                         });
        if (status == SAO_OK || status == SAO_ERR_HANDLE_INVALID) {
            if (found != ctx->platform_resources.end()) {
                ctx->platform_resources.erase(found);
            }
            return SAO_OK;
        }
        if (found != ctx->platform_resources.end()) {
            found->pending_destroy = false;
        }
    }
    return status;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_compositor_layer_input(
    plugin_context_t* ctx, const char* name_utf8, compositor_cursor_pos_fn cursor_pos,
    compositor_mouse_button_fn mouse_button, compositor_cursor_leave_fn cursor_leave,
    compositor_scroll_fn scroll, void* user_data) {
    if (ctx == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string layer_name;
    const int32_t name_status = copy_compositor_layer_name(name_utf8, layer_name);
    if (name_status != SAO_OK)
        return name_status;
    platform_call_lease lease;
    if (!lease.acquire(ctx))
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.set_compositor_layer_input == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    compositor_resource_lease resource_lease;
    const int32_t resource_status = resource_lease.acquire(ctx, layer_name);
    if (resource_status != SAO_OK)
        return resource_status;
    plugin_context_compositor_input_spec spec{};
    spec.struct_size = sizeof(spec);
    spec.cursor_pos = cursor_pos;
    spec.mouse_button = mouse_button;
    spec.cursor_leave = cursor_leave;
    spec.scroll = scroll;
    spec.user_data = user_data;
    try {
        return map_platform_status(provider.set_compositor_layer_input(
            provider.user_data, lease.session(), resource_lease.resource().provider_token, &spec));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_ensure_requirements(plugin_context_t* ctx, bool install, char** out_report_json) {
    if (ctx == nullptr || out_report_json == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_report_json = nullptr;
    try {
        deps_bootstrap_record record;
        const auto status = sao_plugins_deps_ensure(ctx->path.c_str(), install, &record);
        json report = {
            {"status", status}, {"paths", json::array()}, {"dependencies", record.deps_summary}};
        for (const auto& path : record.added_paths)
            report["paths"].push_back(std::filesystem::path(path).u8string());
        *out_report_json = duplicate_string(report.dump());
        return *out_report_json == nullptr ? SAO_ERR_OS_CALL_FAILED : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_load_local(
    plugin_context_t* ctx, const char* relative_path, wchar_t** out_absolute_path) {
    if (ctx == nullptr || relative_path == nullptr || out_absolute_path == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_absolute_path = nullptr;
    try {
        const auto target = std::filesystem::weakly_canonical(
            std::filesystem::path(ctx->path) / std::filesystem::u8path(relative_path));
        if (!path_is_within_base(ctx->path, target.native()) ||
            !std::filesystem::is_regular_file(target))
            return SAO_ERR_HANDLE_INVALID;
        *out_absolute_path = duplicate_wstring(target.native());
        return *out_absolute_path != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ctx_free_string(char* str) {
    delete[] str;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ctx_free_wstring(wchar_t* str) {
    delete[] str;
}

} // namespace sao::plugins::loader
