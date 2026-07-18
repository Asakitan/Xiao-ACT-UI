#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "entity_provider_internal.h"
#include "plugin_internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
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
};

namespace {

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

int32_t plugin_context_destroy(plugin_context_t* ctx) noexcept {
    if (ctx == nullptr) return SAO_OK;
    if (plugin_context_entity_provider_is_current_thread(ctx)) {
        return SAO_PLUGINS_ERR_BUSY;
    }
    try {
        std::vector<std::shared_ptr<entity_provider_state>> providers;
        {
            std::lock_guard lock(ctx->mutex);
            providers = ctx->entity_providers;
        }
        const int32_t status = destroy_entity_providers(providers);
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
        context->path = std::filesystem::u8path(manifest.source_path).native();
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
    const auto status = sao_plugins_ctx_subscribe(ctx, topic_utf8, callback, user_data, out_token);
    if (status == SAO_OK) {
        try {
            std::lock_guard lock(ctx->mutex);
            const auto iterator = std::find_if(ctx->subscriptions.begin(), ctx->subscriptions.end(),
                [out_token](const auto& item) { return item.token == *out_token; });
            if (iterator != ctx->subscriptions.end()) iterator->once = true;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    return status;
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
sao_plugins_ctx_register_render_hook(plugin_context_t*, const char*, float, render_hook_fn, void*, uint32_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    return SAO_PLUGINS_ERR_UNSUPPORTED;
}
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_unregister_render_hook(plugin_context_t*, uint32_t) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_overlay(plugin_context_t*, const char*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_clear_overlay(plugin_context_t*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_request_redraw(plugin_context_t*, const char*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }

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
sao_plugins_ctx_register_hotkey(plugin_context_t*, const char*, const char*, const char*, hotkey_callback_fn, void*) {
    return SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_unregister_hotkey(plugin_context_t*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_interval(plugin_context_t*, timer_callback_fn, double, void*, char** out_token) { if (out_token) *out_token = nullptr; return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_set_timeout(plugin_context_t*, timer_callback_fn, double, void*, char** out_token) { if (out_token) *out_token = nullptr; return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_clear_timer(plugin_context_t*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ctx_notify(plugin_context_t*, const char*, const char*, double, const char*) {
    return SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_dismiss_notify(plugin_context_t*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_toast(plugin_context_t*, const char*) { return SAO_PLUGINS_ERR_UNSUPPORTED; }

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
