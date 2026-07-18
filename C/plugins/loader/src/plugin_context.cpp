#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "entity_provider_internal.h"
#include "plugin_internal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::loader {
namespace {

using json = nlohmann::json;

struct event_subscription {
    uint32_t token = 0;
    std::string topic;
    event_callback_fn callback = nullptr;
    void* user_data = nullptr;
    bool once = false;
};

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
};

struct platform_resource_record {
    platform_resource_kind kind = platform_resource_kind::timer;
    uint32_t loader_token = 0;
    plugin_context_platform_token_t provider_token = 0;
    std::string key;
};

struct platform_provider_record {
    plugin_context_platform_provider value{};
    bool present = false;
    size_t active_contexts = 0;
};

std::mutex g_platform_provider_mutex;
platform_provider_record g_platform_provider;

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

bool valid_json(const char* text, json& output) {
    if (text == nullptr) return false;
    try {
        output = json::parse(text);
        return true;
    } catch (...) {
        return false;
    }
}

extension_kind parse_extension_kind(const char* value, bool& valid) {
    valid = true;
    if (std::strcmp(value, "parser_adapter") == 0) return extension_kind::parser_adapter;
    if (std::strcmp(value, "exporter") == 0) return extension_kind::exporter;
    if (std::strcmp(value, "formatter") == 0) return extension_kind::formatter;
    if (std::strcmp(value, "trigger_type") == 0) return extension_kind::trigger_type;
    if (std::strcmp(value, "report_view") == 0) return extension_kind::report_view;
    if (std::strcmp(value, "timer") == 0) return extension_kind::timer;
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
    std::vector<event_subscription> subscriptions;
    std::unordered_map<std::string, json> settings;
    std::unordered_map<std::string, json> last_events;
    std::vector<json> recent_events;
    std::unordered_map<std::string, void*> engines;
    std::unordered_map<std::string, data_source_record> data_sources;
    std::vector<std::shared_ptr<entity_provider_state>> entity_providers;
    plugin_context_platform_provider platform_provider{};
    plugin_context_platform_session_t platform_session = nullptr;
    bool platform_bound = false;
    bool platform_closing = false;
    bool platform_quiesced = false;
    size_t platform_active_calls = 0;
    std::condition_variable platform_calls_drained;
    std::vector<platform_resource_record> platform_resources;
};

namespace {

class platform_call_lease {
public:
    platform_call_lease() = default;
    ~platform_call_lease() { release(); }

    platform_call_lease(const platform_call_lease&) = delete;
    platform_call_lease& operator=(const platform_call_lease&) = delete;

    bool acquire(plugin_context_t* ctx, bool allow_closing = false) noexcept {
        if (ctx == nullptr) return false;
        try {
            std::lock_guard lock(ctx->mutex);
            if (!ctx->platform_bound ||
                (ctx->platform_closing && !allow_closing)) {
                return false;
            }
            ctx_ = ctx;
            provider_ = ctx->platform_provider;
            session_ = ctx->platform_session;
            ++ctx->platform_active_calls;
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
        if (ctx_ == nullptr) return;
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

uint32_t allocate_context_token_locked(plugin_context_t* ctx) {
    for (uint64_t attempt = 0;
         attempt < static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
         ++attempt) {
        const uint32_t token = ctx->next_token++;
        if (token != 0 &&
            std::none_of(ctx->platform_resources.begin(),
                         ctx->platform_resources.end(),
                         [token](const platform_resource_record& resource) {
                             return resource.loader_token == token;
                         })) {
            return token;
        }
    }
    return 0;
}

bool has_platform_key_locked(plugin_context_t* ctx,
                             platform_resource_kind kind,
                             std::string_view key) {
    return std::any_of(
        ctx->platform_resources.begin(), ctx->platform_resources.end(),
        [kind, key](const platform_resource_record& resource) {
            return resource.kind == kind && resource.key == key;
        });
}

int32_t unregister_platform_resource(
    const plugin_context_platform_provider& provider,
    plugin_context_platform_session_t session,
    const platform_resource_record& resource) noexcept {
    try {
        switch (resource.kind) {
        case platform_resource_kind::hotkey:
            return provider.unregister_hotkey == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.unregister_hotkey(
                             provider.user_data, session,
                             resource.provider_token);
        case platform_resource_kind::timer:
            return provider.unregister_timer == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.unregister_timer(
                             provider.user_data, session,
                             resource.provider_token);
        case platform_resource_kind::notify:
            return provider.dismiss_notify == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.dismiss_notify(
                             provider.user_data, session,
                             resource.provider_token);
        case platform_resource_kind::render_hook:
            return provider.unregister_render_hook == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.unregister_render_hook(
                             provider.user_data, session,
                             resource.provider_token);
        case platform_resource_kind::overlay:
            return provider.clear_overlay == nullptr
                       ? SAO_PLUGINS_ERR_UNSUPPORTED
                       : provider.clear_overlay(
                             provider.user_data, session,
                             resource.provider_token);
        }
    } catch (...) {
    }
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t quiesce_platform_session(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return SAO_OK;
    plugin_context_platform_provider provider{};
    plugin_context_platform_session_t session = nullptr;
    try {
        {
            std::unique_lock lock(ctx->mutex);
            if (!ctx->platform_bound || ctx->platform_quiesced) return SAO_OK;
            ctx->platform_closing = true;
            ctx->platform_calls_drained.wait(
                lock, [ctx] { return ctx->platform_active_calls == 0; });
            provider = ctx->platform_provider;
            session = ctx->platform_session;
        }
        if (provider.quiesce_session == nullptr) {
            return SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        const int32_t status =
            provider.quiesce_session(provider.user_data, session);
        if (status == SAO_OK) {
            std::lock_guard lock(ctx->mutex);
            ctx->platform_quiesced = true;
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_release_platform(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return SAO_OK;
    int32_t status = quiesce_platform_session(ctx);
    if (status != SAO_OK) return status;

    while (true) {
        plugin_context_platform_provider provider{};
        plugin_context_platform_session_t session = nullptr;
        platform_resource_record resource;
        {
            std::lock_guard lock(ctx->mutex);
            if (!ctx->platform_bound) return SAO_OK;
            if (ctx->platform_resources.empty()) break;
            provider = ctx->platform_provider;
            session = ctx->platform_session;
            resource = ctx->platform_resources.back();
        }
        status = unregister_platform_resource(provider, session, resource);
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) {
            return status;
        }
        {
            std::lock_guard lock(ctx->mutex);
            const auto found = std::find_if(
                ctx->platform_resources.rbegin(),
                ctx->platform_resources.rend(),
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
    try {
        status = provider.destroy_session == nullptr
                     ? SAO_PLUGINS_ERR_UNSUPPORTED
                     : provider.destroy_session(provider.user_data, session);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) return status;

    try {
        if (provider.release != nullptr) provider.release(provider.user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    {
        std::lock_guard lock(ctx->mutex);
        ctx->platform_provider = {};
        ctx->platform_session = nullptr;
        ctx->platform_bound = false;
    }
    {
        std::lock_guard lock(g_platform_provider_mutex);
        if (g_platform_provider.active_contexts > 0) {
            --g_platform_provider.active_contexts;
        }
    }
    return SAO_OK;
}

int32_t add_extension(plugin_context_t* ctx, extension_kind kind,
                      const char* id, const char* payload) {
    if (ctx == nullptr || id == nullptr || id[0] == '\0' || payload == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        json parsed;
        if (!valid_json(payload, parsed)) return SAO_ERR_INVALID_ARGUMENT;
        extension_record record;
        record.kind = kind;
        record.id = id;
        record.title = parsed.value("title", std::string(id));
        record.description = parsed.value("description", std::string{});
        record.route = parsed.value("route", std::string{});
        record.payload_json = parsed.dump();
        return sao_plugins_registry_add_extension(sao_plugins_registry_instance(), ctx->plugin, &record);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace

int32_t plugin_context_quiesce_platform(plugin_context_t* ctx) noexcept {
    return quiesce_platform_session(ctx);
}

void plugin_context_request_stop(plugin_context_t* ctx) noexcept {
    if (ctx != nullptr) ctx->stop_requested.store(true);
}

void plugin_context_clear_stop(plugin_context_t* ctx) noexcept {
    if (ctx != nullptr) ctx->stop_requested.store(false);
}

bool plugin_context_entity_provider_is_current_thread(
    plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return false;
    try {
        std::lock_guard lock(ctx->mutex);
        return entity_provider_is_current_thread(ctx->entity_providers);
    } catch (...) {
        return true;
    }
}

int32_t plugin_context_quiesce_entity_providers(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return SAO_OK;
    try {
        std::vector<std::shared_ptr<entity_provider_state>> providers;
        {
            std::lock_guard lock(ctx->mutex);
            providers = ctx->entity_providers;
        }
        return deactivate_entity_providers(providers);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_resume_entity_providers(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::vector<std::shared_ptr<entity_provider_state>> providers;
        {
            std::lock_guard lock(ctx->mutex);
            providers = ctx->entity_providers;
        }
        return activate_entity_providers(providers);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_release_resources(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return SAO_OK;
    if (plugin_context_entity_provider_is_current_thread(ctx)) {
        return SAO_PLUGINS_ERR_BUSY;
    }
    try {
        const int32_t quiesce_status = quiesce_platform_session(ctx);
        if (quiesce_status != SAO_OK) return quiesce_status;
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
            if (data_source.stop == nullptr) continue;
            int32_t status = SAO_ERR_OS_CALL_FAILED;
            try {
                status = data_source.stop(data_source.user_data);
            } catch (...) {
                status = SAO_ERR_OS_CALL_FAILED;
            }
            if (status != SAO_OK) return status;
        }
        const int32_t provider_status = destroy_entity_providers(providers);
        if (provider_status != SAO_OK) return provider_status;
        const int32_t platform_status = plugin_context_release_platform(ctx);
        if (platform_status != SAO_OK) return platform_status;
        {
            std::lock_guard lock(ctx->mutex);
            ctx->subscriptions.clear();
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
    if (ctx == nullptr) return SAO_OK;
    try {
        const int32_t status = plugin_context_release_resources(ctx);
        if (status != SAO_OK) return status;
        {
            std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
            if (ctx->plugin_owner->context == ctx) {
                ctx->plugin_owner->context = nullptr;
            }
        }
        delete ctx;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_platform_provider(
    const plugin_context_platform_provider* provider) {
    if (provider == nullptr ||
        (provider->abi_version >> 16u) !=
            SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < kPlatformProviderMinimumSize) {
        return provider == nullptr ? SAO_ERR_INVALID_ARGUMENT
                                   : SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
    plugin_context_platform_provider copy{};
    std::memcpy(&copy, provider,
                std::min<size_t>(provider->struct_size, sizeof(copy)));
    if (copy.retain == nullptr || copy.release == nullptr ||
        copy.create_session == nullptr || copy.quiesce_session == nullptr ||
        copy.destroy_session == nullptr) {
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

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_platform_provider() {
    try {
        std::lock_guard lock(g_platform_provider_mutex);
        if (!g_platform_provider.present) return SAO_ERR_HANDLE_INVALID;
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
    if (retained == nullptr) return nullptr;
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
            if (!g_platform_provider.present) return context.release();
            context->platform_provider = g_platform_provider.value;
            ++g_platform_provider.active_contexts;
        }
        bool retained_provider = false;
        try {
            context->platform_provider.retain(
                context->platform_provider.user_data);
            retained_provider = true;
            plugin_context_platform_session_spec spec{};
            spec.struct_size = sizeof(spec);
            spec.plugin_id_utf8 = context->plugin_id.c_str();
            spec.plugin_path_utf8 = context->plugin_path_utf8.c_str();
            spec.plugin_version_utf8 = context->plugin_version.c_str();
            const int32_t status = context->platform_provider.create_session(
                context->platform_provider.user_data, &spec,
                &context->platform_session);
            if (status != SAO_OK || context->platform_session == nullptr) {
                if (retained_provider) {
                    context->platform_provider.release(
                        context->platform_provider.user_data);
                }
                std::lock_guard lock(g_platform_provider_mutex);
                if (g_platform_provider.active_contexts > 0) {
                    --g_platform_provider.active_contexts;
                }
                return nullptr;
            }
            context->platform_bound = true;
        } catch (...) {
            try {
                if (retained_provider) {
                    context->platform_provider.release(
                        context->platform_provider.user_data);
                }
            } catch (...) {
            }
            std::lock_guard lock(g_platform_provider_mutex);
            if (g_platform_provider.active_contexts > 0) {
                --g_platform_provider.active_contexts;
            }
            return nullptr;
        }
        return context.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_destroy(plugin_context_t* ctx) {
    if (ctx == nullptr) return;
    {
        std::lock_guard plugin_lock(ctx->plugin_owner->mutex);
        if (ctx->plugin_owner->context == ctx) return;
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

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_log(plugin_context_t* ctx, const char* utf8_message) {
    if (ctx != nullptr && utf8_message != nullptr) std::clog << "[plugin:" << ctx->plugin_id << "] " << utf8_message << '\n';
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_subscribe(plugin_context_t* ctx,
                          const char* topic_utf8,
                          event_callback_fn callback,
                          void* user_data,
                          uint32_t* out_token) {
    if (ctx == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0' || callback == nullptr || out_token == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto token = ctx->next_token++;
        ctx->subscriptions.push_back({token, topic_utf8, callback, user_data, false});
        *out_token = token;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_subscribe_once(plugin_context_t* ctx, const char* topic_utf8,
                               event_callback_fn callback, void* user_data,
                               uint32_t* out_token) {
    if (ctx == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0' ||
        callback == nullptr || out_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(ctx->mutex);
        const auto token = ctx->next_token++;
        ctx->subscriptions.push_back(
            {token, topic_utf8, callback, user_data, true});
        *out_token = token;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unsubscribe(plugin_context_t* ctx, uint32_t token) {
    if (ctx == nullptr || token == 0) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto old_size = ctx->subscriptions.size();
        std::erase_if(ctx->subscriptions, [token](const auto& item) { return item.token == token; });
        return ctx->subscriptions.size() != old_size ? SAO_OK : SAO_ERR_HANDLE_INVALID;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_emit(plugin_context_t* ctx,
                     const char* topic_utf8,
                     const char* payload_json_utf8) {
    if (ctx == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0') return SAO_ERR_INVALID_ARGUMENT;
    json payload;
    if (!valid_json(payload_json_utf8 ? payload_json_utf8 : "null", payload)) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::vector<event_subscription> callbacks;
        const json envelope = {{"topic", topic_utf8}, {"payload", payload}};
        const auto serialized = envelope.dump();
        {
            std::lock_guard lock(ctx->mutex);
            ctx->last_events[topic_utf8] = envelope;
            ctx->recent_events.push_back(envelope);
            if (ctx->recent_events.size() > 256) ctx->recent_events.erase(ctx->recent_events.begin());
            for (const auto& subscription : ctx->subscriptions) {
                if (subscription.topic == topic_utf8 || subscription.topic == "*") callbacks.push_back(subscription);
            }
            std::erase_if(ctx->subscriptions, [&callbacks](const auto& item) {
                return item.once && std::any_of(callbacks.begin(), callbacks.end(),
                                                [&item](const auto& called) { return called.token == item.token; });
            });
        }
        for (const auto& callback : callbacks) {
            try {
                callback.callback(topic_utf8, serialized.c_str(), callback.user_data);
            } catch (...) {
                sao_plugins_isolation_record_failure(ctx->plugin, "event callback crossed exception boundary");
            }
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_get_snapshot(plugin_context_t* ctx, char** out_snapshot_json) {
    if (ctx == nullptr || out_snapshot_json == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_snapshot_json = nullptr;
    try {
        json snapshot = json::object();
        std::lock_guard lock(ctx->mutex);
        for (const auto& [topic, event] : ctx->last_events) snapshot[topic] = event["payload"];
        *out_snapshot_json = duplicate_string(snapshot.dump());
        return *out_snapshot_json != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_snapshot_value(plugin_context_t* ctx, const char* path_utf8,
                               char** out_value_json) {
    if (ctx == nullptr || path_utf8 == nullptr || out_value_json == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_value_json = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto iterator = ctx->last_events.find(path_utf8);
        if (iterator == ctx->last_events.end()) return SAO_ERR_HANDLE_INVALID;
        *out_value_json = duplicate_string(iterator->second["payload"].dump());
        return *out_value_json != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_recent_events(plugin_context_t* ctx, uint32_t limit,
                              const char* topic_utf8, char** out_events_json) {
    if (ctx == nullptr || out_events_json == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_events_json = nullptr;
    try {
        json result = json::array();
        std::lock_guard lock(ctx->mutex);
        for (auto iterator = ctx->recent_events.rbegin(); iterator != ctx->recent_events.rend() && result.size() < limit; ++iterator) {
            if (topic_utf8 == nullptr || topic_utf8[0] == '\0' || iterator->at("topic") == topic_utf8) result.push_back(*iterator);
        }
        std::reverse(result.begin(), result.end());
        *out_events_json = duplicate_string(result.dump());
        return *out_events_json != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_get_setting(plugin_context_t* ctx,
                            const char* key,
                            char** out_json_utf8) {
    if (ctx == nullptr || key == nullptr || out_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_json_utf8 = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto iterator = ctx->settings.find(key);
        if (iterator == ctx->settings.end()) return SAO_ERR_HANDLE_INVALID;
        *out_json_utf8 = duplicate_string(iterator->second.dump());
        return *out_json_utf8 != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_setting(plugin_context_t* ctx,
                            const char* key,
                            const char* value_json_utf8) {
    if (ctx == nullptr || key == nullptr || key[0] == '\0') return SAO_ERR_INVALID_ARGUMENT;
    json value;
    if (!valid_json(value_json_utf8, value)) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        ctx->settings[key] = std::move(value);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_defaults(plugin_context_t* ctx, const char* defaults_json_utf8) {
    if (ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    json defaults;
    if (!valid_json(defaults_json_utf8, defaults) || !defaults.is_object()) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        for (auto& [key, value] : defaults.items()) ctx->settings.try_emplace(key, value);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_ui_panel(plugin_context_t* ctx,
                                  const char* panel_id,
                                  const char* meta_json_utf8,
                                  render_callback_fn render,
                                  action_callback_fn on_action,
                                  void* user_data) {
    if (render != nullptr || on_action != nullptr || user_data != nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    return add_extension(ctx, extension_kind::ui_panel, panel_id, meta_json_utf8);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_render_hook(plugin_context_t* ctx,
                                     const char* surface_utf8,
                                     float priority,
                                     render_hook_fn hook,
                                     void* user_data,
                                     uint32_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0' ||
        !std::isfinite(priority) || hook == nullptr || out_token == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.register_render_hook == nullptr ||
        provider.unregister_render_hook == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.register_render_hook(
            provider.user_data, lease.session(), surface_utf8, priority, hook,
            user_data, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) return status;
    if (provider_token == 0) return SAO_ERR_HANDLE_INVALID;
    uint32_t loader_token = 0;
    try {
        std::lock_guard lock(ctx->mutex);
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            ctx->platform_resources.push_back({
                platform_resource_kind::render_hook, loader_token,
                provider_token, {}});
        }
    } catch (...) {
        loader_token = 0;
    }
    if (loader_token == 0) {
        (void)provider.unregister_render_hook(
            provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_token = loader_token;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_render_hook(plugin_context_t* ctx,
                                       uint32_t token) {
    if (ctx == nullptr || token == 0) return SAO_ERR_INVALID_ARGUMENT;
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found = std::find_if(
            ctx->platform_resources.begin(), ctx->platform_resources.end(),
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
    if (!lease.acquire(ctx, true)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status = unregister_platform_resource(
        lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources,
                  [&resource](const platform_resource_record& candidate) {
                      return candidate.kind == resource.kind &&
                             candidate.loader_token == resource.loader_token;
                  });
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_overlay(plugin_context_t* ctx,
                            const char* surface_utf8,
                            const char* spec_json_utf8) {
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0' ||
        spec_json_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    json ignored;
    if (!valid_json(spec_json_utf8, ignored)) return SAO_ERR_INVALID_ARGUMENT;
    platform_call_lease lease;
    if (!lease.acquire(ctx)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.set_overlay == nullptr || provider.clear_overlay == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    {
        std::lock_guard lock(ctx->mutex);
        if (has_platform_key_locked(ctx, platform_resource_kind::overlay,
                                    surface_utf8)) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.set_overlay(
            provider.user_data, lease.session(), surface_utf8, spec_json_utf8,
            &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) return status;
    if (provider_token == 0) return SAO_ERR_HANDLE_INVALID;
    bool recorded = false;
    try {
        std::lock_guard lock(ctx->mutex);
        if (!has_platform_key_locked(ctx, platform_resource_kind::overlay,
                                     surface_utf8)) {
            const uint32_t token = allocate_context_token_locked(ctx);
            if (token != 0) {
                ctx->platform_resources.push_back({
                    platform_resource_kind::overlay, token, provider_token,
                    surface_utf8});
                recorded = true;
            }
        }
    } catch (...) {
    }
    if (!recorded) {
        (void)provider.clear_overlay(
            provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_clear_overlay(plugin_context_t* ctx,
                              const char* surface_utf8) {
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found = std::find_if(
            ctx->platform_resources.begin(), ctx->platform_resources.end(),
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
    if (!lease.acquire(ctx, true)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status = unregister_platform_resource(
        lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources,
                  [&resource](const platform_resource_record& candidate) {
                      return candidate.kind == resource.kind &&
                             candidate.loader_token == resource.loader_token;
                  });
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_request_redraw(plugin_context_t* ctx,
                               const char* surface_utf8,
                               const char* reason_utf8) {
    if (ctx == nullptr || surface_utf8 == nullptr || surface_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.request_redraw == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    try {
        return provider.request_redraw(
            provider.user_data, lease.session(), surface_utf8,
            reason_utf8 == nullptr ? "" : reason_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_extension(plugin_context_t* ctx, const char* kind_utf8,
                                   const char* extension_id_utf8, const char* metadata_json_utf8,
                                   void* handler, void* user_data) {
    if (kind_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (handler != nullptr || user_data != nullptr) return SAO_PLUGINS_ERR_UNSUPPORTED;
    bool valid = false;
    const auto kind = parse_extension_kind(kind_utf8, valid);
    return valid ? add_extension(ctx, kind, extension_id_utf8, metadata_json_utf8)
                 : SAO_ERR_INVALID_ARGUMENT;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_menu_category(plugin_context_t* ctx, const char* name_utf8,
                                       const char* icon_utf8, void* builder, float priority,
                                       void* user_data) {
    if (builder != nullptr || user_data != nullptr) return SAO_PLUGINS_ERR_UNSUPPORTED;
    try {
        json metadata = {{"title", name_utf8 ? name_utf8 : ""}, {"icon", icon_utf8 ? icon_utf8 : ""}, {"priority", priority}};
        return add_extension(ctx, extension_kind::menu_category, name_utf8, metadata.dump().c_str());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_menu_surface(plugin_context_t* ctx, const char* surface_id_utf8,
                                      const char* descriptor_json_utf8, float) {
    return add_extension(ctx, extension_kind::menu_category, surface_id_utf8, descriptor_json_utf8);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_data_source(plugin_context_t* ctx, const char* source_id_utf8,
                                     const char* metadata_json_utf8,
                                     data_source_start_fn start, data_source_stop_fn stop,
                                     void* user_data) {
    if (ctx == nullptr || start == nullptr || stop == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    const auto status = add_extension(ctx, extension_kind::data_source, source_id_utf8, metadata_json_utf8);
    if (status != SAO_OK) return status;
    try {
        std::lock_guard lock(ctx->mutex);
        ctx->data_sources[source_id_utf8] = {start, stop, user_data};
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t plugin_context_register_entity_providers(
    plugin_context_t* ctx,
    const native_entity_provider_descriptor* providers,
    size_t count) noexcept {
    if (ctx == nullptr || (count > 0 && providers == nullptr)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    constexpr size_t kMaximumProvidersPerPlugin = 256;
    if (count > kMaximumProvidersPerPlugin) return SAO_ERR_INVALID_ARGUMENT;
    std::vector<std::shared_ptr<entity_provider_state>> registered;
    try {
        registered.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto& descriptor = providers[index];
            if (descriptor.struct_size < sizeof(native_entity_provider_descriptor)) {
                (void)destroy_entity_providers(registered);
                return SAO_ERR_INVALID_ARGUMENT;
            }
            std::shared_ptr<entity_provider_state> provider;
            const int32_t status = register_entity_provider(
                ctx->plugin_owner, ctx->plugin_id,
                descriptor.provider_id_utf8, descriptor.snapshot,
                descriptor.action_handler, descriptor.user_data, provider);
            if (status != SAO_OK) {
                (void)destroy_entity_providers(registered);
                return status;
            }
            registered.push_back(std::move(provider));
        }
        std::lock_guard lock(ctx->mutex);
        ctx->entity_providers.insert(ctx->entity_providers.end(),
                                     registered.begin(), registered.end());
        return SAO_OK;
    } catch (...) {
        (void)destroy_entity_providers(registered);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_engine(plugin_context_t* ctx, const char* name_utf8, void* engine_ptr) {
    if (ctx == nullptr || name_utf8 == nullptr || name_utf8[0] == '\0' || engine_ptr == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(ctx->mutex);
        if (ctx->engines.contains(name_utf8)) return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        ctx->engines.emplace(name_utf8, engine_ptr);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ctx_get_engine(plugin_context_t* ctx, const char* name_utf8) {
    if (ctx == nullptr || name_utf8 == nullptr) return nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        const auto iterator = ctx->engines.find(name_utf8);
        return iterator == ctx->engines.end() ? nullptr : iterator->second;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_register_hotkey(plugin_context_t* ctx,
                                const char* hotkey_id,
                                const char* default_key,
                                const char* label,
                                hotkey_callback_fn callback,
                                void* user_data) {
    if (ctx == nullptr || hotkey_id == nullptr || hotkey_id[0] == '\0' ||
        default_key == nullptr || default_key[0] == '\0' || callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.register_hotkey == nullptr ||
        provider.unregister_hotkey == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    {
        std::lock_guard lock(ctx->mutex);
        if (has_platform_key_locked(ctx, platform_resource_kind::hotkey,
                                    hotkey_id)) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.register_hotkey(
            provider.user_data, lease.session(), hotkey_id, default_key,
            label == nullptr ? "" : label, callback, user_data,
            &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) return status;
    if (provider_token == 0) return SAO_ERR_HANDLE_INVALID;
    bool recorded = false;
    try {
        std::lock_guard lock(ctx->mutex);
        if (!has_platform_key_locked(ctx, platform_resource_kind::hotkey,
                                     hotkey_id)) {
            const uint32_t token = allocate_context_token_locked(ctx);
            if (token != 0) {
                ctx->platform_resources.push_back({
                    platform_resource_kind::hotkey, token, provider_token,
                    hotkey_id});
                recorded = true;
            }
        }
    } catch (...) {
    }
    if (!recorded) {
        (void)provider.unregister_hotkey(
            provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_unregister_hotkey(plugin_context_t* ctx,
                                  const char* hotkey_id) {
    if (ctx == nullptr || hotkey_id == nullptr || hotkey_id[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_resource_record resource;
    {
        std::lock_guard lock(ctx->mutex);
        const auto found = std::find_if(
            ctx->platform_resources.begin(), ctx->platform_resources.end(),
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
    if (!lease.acquire(ctx, true)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status = unregister_platform_resource(
        lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources,
                  [&resource](const platform_resource_record& candidate) {
                      return candidate.kind == resource.kind &&
                             candidate.loader_token == resource.loader_token;
                  });
    return SAO_OK;
}

namespace {

int32_t register_timer(plugin_context_t* ctx, timer_callback_fn callback,
                       double seconds, void* user_data, bool one_shot,
                       char** out_token) {
    if (out_token != nullptr) *out_token = nullptr;
    if (ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    platform_call_lease lease;
    if (!lease.acquire(ctx)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    if (callback == nullptr || out_token == nullptr ||
        !std::isfinite(seconds) || seconds <= 0.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto& provider = lease.provider();
    if (provider.register_timer == nullptr ||
        provider.unregister_timer == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.register_timer(
            provider.user_data, lease.session(), seconds, one_shot, callback,
            user_data, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) return status;
    if (provider_token == 0) return SAO_ERR_HANDLE_INVALID;

    uint32_t loader_token = 0;
    char* token_copy = nullptr;
    try {
        std::lock_guard lock(ctx->mutex);
        loader_token = allocate_context_token_locked(ctx);
        if (loader_token != 0) {
            const std::string token_text = std::to_string(loader_token);
            token_copy = duplicate_string(token_text);
            if (token_copy != nullptr) {
                ctx->platform_resources.push_back({
                    platform_resource_kind::timer, loader_token,
                    provider_token, token_text});
            }
        }
    } catch (...) {
        delete[] token_copy;
        token_copy = nullptr;
    }
    if (token_copy == nullptr) {
        (void)provider.unregister_timer(
            provider.user_data, lease.session(), provider_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_token = token_copy;
    return SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_interval(plugin_context_t* ctx,
                             timer_callback_fn callback,
                             double seconds,
                             void* user_data,
                             char** out_token) {
    return register_timer(ctx, callback, seconds, user_data, false, out_token);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_set_timeout(plugin_context_t* ctx,
                            timer_callback_fn callback,
                            double seconds,
                            void* user_data,
                            char** out_token) {
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
                return candidate.kind == platform_resource_kind::timer &&
                       candidate.key == token;
            });
        if (found == ctx->platform_resources.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        resource = *found;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx, true)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const int32_t status = unregister_platform_resource(
        lease.provider(), lease.session(), resource);
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) return status;
    std::lock_guard lock(ctx->mutex);
    std::erase_if(ctx->platform_resources,
                  [&resource](const platform_resource_record& candidate) {
                      return candidate.kind == resource.kind &&
                             candidate.loader_token == resource.loader_token;
                  });
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_notify(plugin_context_t* ctx,
                       const char* title_utf8,
                       const char* message_utf8,
                       double duration_s,
                       const char* kind_utf8) {
    if (ctx == nullptr || message_utf8 == nullptr ||
        !std::isfinite(duration_s) || duration_s <= 0.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    platform_call_lease lease;
    if (!lease.acquire(ctx)) return SAO_PLUGINS_ERR_UNSUPPORTED;
    const auto& provider = lease.provider();
    if (provider.show_notify == nullptr || provider.dismiss_notify == nullptr) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }
    plugin_context_platform_token_t provider_token = 0;
    int32_t status = SAO_ERR_OS_CALL_FAILED;
    try {
        status = provider.show_notify(
            provider.user_data, lease.session(),
            title_utf8 == nullptr ? "" : title_utf8, message_utf8, duration_s,
            kind_utf8 == nullptr ? "info" : kind_utf8, &provider_token);
    } catch (...) {
        status = SAO_ERR_OS_CALL_FAILED;
    }
    if (status != SAO_OK) return status;
    if (provider_token == 0) return SAO_ERR_HANDLE_INVALID;
    try {
        std::lock_guard lock(ctx->mutex);
        const uint32_t token = allocate_context_token_locked(ctx);
        if (token != 0) {
            ctx->platform_resources.push_back({
                platform_resource_kind::notify, token, provider_token, {}});
            return SAO_OK;
        }
    } catch (...) {
    }
    (void)provider.dismiss_notify(
        provider.user_data, lease.session(), provider_token);
    return SAO_ERR_OS_CALL_FAILED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_dismiss_notify(plugin_context_t* ctx) {
    if (ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    while (true) {
        platform_resource_record resource;
        {
            std::lock_guard lock(ctx->mutex);
            const auto found = std::find_if(
                ctx->platform_resources.rbegin(),
                ctx->platform_resources.rend(),
                [](const platform_resource_record& candidate) {
                    return candidate.kind == platform_resource_kind::notify;
                });
            if (found == ctx->platform_resources.rend()) return SAO_OK;
            resource = *found;
        }
        platform_call_lease lease;
        if (!lease.acquire(ctx, true)) return SAO_PLUGINS_ERR_UNSUPPORTED;
        const int32_t status = unregister_platform_resource(
            lease.provider(), lease.session(), resource);
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID) return status;
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
    return sao_plugins_ctx_notify(
        ctx, "", message_utf8, 3.0, "info");
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_open_file(plugin_context_t*, const char*, const char*, const wchar_t*, intptr_t,
                          wchar_t** out_selected_path) {
    if (out_selected_path != nullptr) *out_selected_path = nullptr;
    return SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_open_window(plugin_context_t*, const char*, uint32_t, uint32_t) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_create_compositor_layer(plugin_context_t*, const char*, uint32_t, uint32_t, int32_t, int32_t, int32_t, bool, bool, uint32_t) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_upload_compositor_frame(plugin_context_t*, const char*, const uint8_t*, size_t, uint32_t, uint32_t) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_compositor_layer_position(plugin_context_t*, const char*, int32_t, int32_t) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_compositor_layer_visible(plugin_context_t*, const char*, bool) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_destroy_compositor_layer(plugin_context_t*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_compositor_layer_input(plugin_context_t*, const char*, compositor_cursor_pos_fn, compositor_mouse_button_fn, compositor_cursor_leave_fn, compositor_scroll_fn, void*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_ensure_requirements(plugin_context_t* ctx, bool install, char** out_report_json) {
    if (ctx == nullptr || out_report_json == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_report_json = nullptr;
    try {
        deps_bootstrap_record record;
        const auto status = sao_plugins_deps_ensure(ctx->path.c_str(), install, &record);
        json report = {{"status", status}, {"paths", json::array()}, {"dependencies", record.deps_summary}};
        for (const auto& path : record.added_paths) report["paths"].push_back(std::filesystem::path(path).u8string());
        *out_report_json = duplicate_string(report.dump());
        return *out_report_json == nullptr ? SAO_ERR_OS_CALL_FAILED : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_load_local(plugin_context_t* ctx, const char* relative_path, wchar_t** out_absolute_path) {
    if (ctx == nullptr || relative_path == nullptr || out_absolute_path == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_absolute_path = nullptr;
    try {
        const auto target = std::filesystem::weakly_canonical(std::filesystem::path(ctx->path) / std::filesystem::u8path(relative_path));
        if (!path_is_within_base(ctx->path, target.native()) || !std::filesystem::is_regular_file(target)) return SAO_ERR_HANDLE_INVALID;
        *out_absolute_path = duplicate_wstring(target.native());
        return *out_absolute_path != nullptr ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_free_string(char* str) {
    delete[] str;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ctx_free_wstring(wchar_t* str) {
    delete[] str;
}

} // namespace sao::plugins::loader
