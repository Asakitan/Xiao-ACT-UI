#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/emma_host/emma_call.h"
#include "sao/plugins/emma_host/emma_error.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/sdk_binding/binding_common.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sao::plugins::emma_host;
using namespace sao::plugins::loader;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class temp_tree final {
  public:
    explicit temp_tree(const wchar_t* suffix) {
        static std::atomic_uint64_t sequence{0};
        root = fs::temp_directory_path() /
               (L"sao_emma_context_" + std::to_wstring(GetCurrentProcessId()) + L"_" + suffix +
                L"_" + std::to_wstring(sequence.fetch_add(1)));
        std::error_code error;
        fs::remove_all(root, error);
        REQUIRE(fs::create_directories(root));
    }

    ~temp_tree() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
};

std::string path_utf8(const fs::path& path) {
    const std::wstring wide = path.native();
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string result(static_cast<size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), result.data(), required, nullptr,
                                nullptr) == required);
    return result;
}

void write_text(const fs::path& path, const std::string& text) {
    REQUIRE((fs::create_directories(path.parent_path()) || fs::is_directory(path.parent_path())));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(const temp_tree& tree, const char* plugin_id,
                              const char* entry = "entry.emma") {
    plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = entry;
    manifest.language = engine_kind::emma;
    manifest.enabled = false;
    manifest.source_path = path_utf8(tree.root);
    return manifest;
}

plugin_handle_t add_plugin(const plugin_manifest& manifest) {
    plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &plugin) ==
            SAO_OK);
    REQUIRE(plugin != nullptr);
    return plugin;
}

void remove_plugin(plugin_handle_t plugin) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), plugin) == SAO_OK);
}

json call_json(emma_plugin_handle_t plugin, const char* hook) {
    char* result = nullptr;
    REQUIRE(sao_plugins_emma_call_hook(plugin, hook, "[]", &result) == SAO_OK);
    REQUIRE(result != nullptr);
    const json parsed = json::parse(result);
    sao_plugins_emma_free_string(result);
    return parsed;
}

std::string nested_array_json(size_t depth) {
    return std::string(depth, '[') + "0" + std::string(depth, ']');
}

std::string flat_array_json(size_t values) {
    std::string result;
    result.reserve(values * 2 + 1);
    result.push_back('[');
    for (size_t index = 0; index < values; ++index) {
        if (index != 0)
            result.push_back(',');
        result.push_back('0');
    }
    result.push_back(']');
    return result;
}

struct callback_slot {
    plugin_context_platform_token_t token = 0;
    bool one_shot = false;
    timer_callback_fn timer = nullptr;
    hotkey_callback_fn hotkey = nullptr;
    void* user_data = nullptr;
};

struct platform_fixture {
    plugin_context_platform_token_t next_token = 1;
    std::vector<callback_slot> callbacks;
    std::vector<std::string> teardown;
    size_t redraw_count = 0;
    size_t retain_count = 0;
    size_t release_count = 0;
    int32_t hotkey_unregister_status = SAO_OK;
    int32_t timer_unregister_status = SAO_OK;
    bool fire_one_shot_timer_synchronously = false;

    static platform_fixture& from(void* user_data) {
        return *static_cast<platform_fixture*>(user_data);
    }

    static void SAO_PLUGINS_CALL retain(void* user_data) {
        ++from(user_data).retain_count;
    }

    static void SAO_PLUGINS_CALL release(void* user_data) {
        ++from(user_data).release_count;
    }

    static int32_t SAO_PLUGINS_CALL create_session(void* user_data,
                                                   const plugin_context_platform_session_spec*,
                                                   plugin_context_platform_session_t* out_session) {
        if (out_session == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *out_session = user_data;
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL quiesce_session(void*, plugin_context_platform_session_t) {
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL destroy_session(void*, plugin_context_platform_session_t) {
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL register_hotkey(void* user_data,
                                                    plugin_context_platform_session_t, const char*,
                                                    const char*, const char*,
                                                    hotkey_callback_fn callback,
                                                    void* callback_user_data,
                                                    plugin_context_platform_token_t* out_token) {
        if (callback == nullptr || out_token == nullptr) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        auto& fixture = from(user_data);
        const auto token = fixture.next_token++;
        fixture.callbacks.push_back({token, false, nullptr, callback, callback_user_data});
        *out_token = token;
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL unregister_hotkey(void* user_data,
                                                      plugin_context_platform_session_t,
                                                      plugin_context_platform_token_t token) {
        auto& fixture = from(user_data);
        fixture.teardown.push_back("hotkey:" + std::to_string(token));
        if (fixture.hotkey_unregister_status != SAO_OK)
            return fixture.hotkey_unregister_status;
        std::erase_if(fixture.callbacks,
                      [token](const callback_slot& slot) { return slot.token == token; });
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL register_timer(void* user_data,
                                                   plugin_context_platform_session_t, double,
                                                   bool one_shot, timer_callback_fn callback,
                                                   void* callback_user_data,
                                                   plugin_context_platform_token_t* out_token) {
        if (callback == nullptr || out_token == nullptr) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        auto& fixture = from(user_data);
        const auto token = fixture.next_token++;
        fixture.callbacks.push_back({token, one_shot, callback, nullptr, callback_user_data});
        *out_token = token;
        if (one_shot && fixture.fire_one_shot_timer_synchronously)
            callback(callback_user_data);
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL unregister_timer(void* user_data,
                                                     plugin_context_platform_session_t,
                                                     plugin_context_platform_token_t token) {
        auto& fixture = from(user_data);
        fixture.teardown.push_back("timer:" + std::to_string(token));
        if (fixture.timer_unregister_status != SAO_OK)
            return fixture.timer_unregister_status;
        std::erase_if(fixture.callbacks,
                      [token](const callback_slot& slot) { return slot.token == token; });
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL show_notify(void* user_data, plugin_context_platform_session_t,
                                                const char*, const char*, double, const char*,
                                                plugin_context_platform_token_t* out_token) {
        if (out_token == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *out_token = from(user_data).next_token++;
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL dismiss_notify(void* user_data,
                                                   plugin_context_platform_session_t,
                                                   plugin_context_platform_token_t token) {
        from(user_data).teardown.push_back("notify:" + std::to_string(token));
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL set_overlay(void* user_data, plugin_context_platform_session_t,
                                                const char*, const char*,
                                                plugin_context_platform_token_t* out_token) {
        if (out_token == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *out_token = from(user_data).next_token++;
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL clear_overlay(void* user_data,
                                                  plugin_context_platform_session_t,
                                                  plugin_context_platform_token_t token) {
        from(user_data).teardown.push_back("overlay:" + std::to_string(token));
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL request_redraw(void* user_data,
                                                   plugin_context_platform_session_t, const char*,
                                                   const char*) {
        ++from(user_data).redraw_count;
        return SAO_OK;
    }

    plugin_context_platform_provider provider() {
        plugin_context_platform_provider value{};
        value.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
        value.struct_size = sizeof(value);
        value.user_data = this;
        value.retain = retain;
        value.release = release;
        value.create_session = create_session;
        value.quiesce_session = quiesce_session;
        value.destroy_session = destroy_session;
        value.register_hotkey = register_hotkey;
        value.unregister_hotkey = unregister_hotkey;
        value.register_timer = register_timer;
        value.unregister_timer = unregister_timer;
        value.show_notify = show_notify;
        value.dismiss_notify = dismiss_notify;
        value.set_overlay = set_overlay;
        value.clear_overlay = clear_overlay;
        value.request_redraw = request_redraw;
        return value;
    }
};

struct custom_emma_provider_fixture {
    enum class malformed_context_kind {
        none,
        non_dictionary,
        null_log,
        non_callable_panel,
        empty_menu_callable,
    } malformed_context = malformed_context_kind::none;

    size_t context_bind_calls = 0;
    size_t load_calls = 0;
    size_t unload_calls = 0;
    size_t log_calls = 0;
    size_t panel_calls = 0;
    size_t menu_calls = 0;
    std::shared_ptr<callable> menu_builder;
    emma_value menu_result = nullptr;
    int32_t unload_status = SAO_OK;

    static int32_t SAO_PLUGINS_CALL load_plugin(void* context, void* runtime, void** out_plugin,
                                                void* user_data) {
        if (context == nullptr || runtime == nullptr || out_plugin == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        auto& fixture = *static_cast<custom_emma_provider_fixture*>(user_data);
        ++fixture.load_calls;
        *out_plugin = context;
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL unload_plugin(void* plugin, void* user_data) {
        if (plugin == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        auto& fixture = *static_cast<custom_emma_provider_fixture*>(user_data);
        ++fixture.unload_calls;
        return fixture.unload_status;
    }

    static int32_t SAO_PLUGINS_CALL invoke(void*, const char*, const uint8_t*, size_t, uint8_t*,
                                           size_t, size_t*, char*, size_t, void*) {
        return SAO_PLUGINS_ERR_UNSUPPORTED;
    }

    static int32_t SAO_PLUGINS_CALL
    dispatch(sao::plugins::sdk_binding::language_binding_operation operation,
             sao::plugins::sdk_binding::language_binding_request* request, void* user_data) {
        if (operation != sao::plugins::sdk_binding::language_binding_operation::context_bind ||
            request == nullptr || request->runtime == nullptr) {
            return SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        auto& fixture = *static_cast<custom_emma_provider_fixture*>(user_data);
        ++fixture.context_bind_calls;
        auto* interp = static_cast<interpreter*>(request->runtime);
        if (fixture.malformed_context == malformed_context_kind::non_dictionary) {
            interp->register_global("ctx", true);
            return SAO_OK;
        }
        auto context = std::make_shared<emma_dict>();
        auto log = std::make_shared<callable>();
        log->name = "custom.log";
        log->host_impl = [&fixture](std::vector<emma_value>) {
            ++fixture.log_calls;
            return emma_value(nullptr);
        };
        auto panel = std::make_shared<callable>();
        panel->name = "custom.register_ui_panel";
        panel->host_impl = [&fixture](std::vector<emma_value>) {
            ++fixture.panel_calls;
            return emma_value(true);
        };
        auto menu = std::make_shared<callable>();
        menu->name = "custom.register_menu_category";
        menu->host_impl = [&fixture](std::vector<emma_value> arguments) {
            if (arguments.size() < 3) {
                return emma_value(false);
            }
            const auto* builder = std::get_if<std::shared_ptr<callable>>(&arguments[2]);
            if (builder == nullptr || *builder == nullptr) {
                return emma_value(false);
            }
            fixture.menu_builder = *builder;
            ++fixture.menu_calls;
            return emma_value(std::string("custom-menu"));
        };
        context->items.emplace("log", std::move(log));
        context->items.emplace("register_ui_panel", std::move(panel));
        context->items.emplace("register_menu_category", std::move(menu));
        context->items.emplace("custom_provider", true);
        context->items.emplace("plugin_id", std::string("forged.provider.id"));
        context->items.emplace("path", std::string("C:\\forged\\provider"));
        context->items.emplace("get_setting", std::string("poisoned native method"));
        switch (fixture.malformed_context) {
        case malformed_context_kind::null_log:
            context->items.insert_or_assign("log", std::shared_ptr<callable>{});
            break;
        case malformed_context_kind::non_callable_panel:
            context->items.insert_or_assign("register_ui_panel", int64_t{7});
            break;
        case malformed_context_kind::empty_menu_callable:
            context->items.insert_or_assign("register_menu_category",
                                            std::make_shared<callable>());
            break;
        case malformed_context_kind::none:
        case malformed_context_kind::non_dictionary:
            break;
        }
        interp->register_global("ctx", std::move(context));
        return SAO_OK;
    }

    static int32_t SAO_PLUGINS_CALL invoke_menu_builder(interpreter* interp, void* user_data) {
        if (interp == nullptr || user_data == nullptr) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        auto& fixture = *static_cast<custom_emma_provider_fixture*>(user_data);
        if (fixture.menu_builder == nullptr) {
            return SAO_ERR_NOT_INITIALIZED;
        }
        std::string message;
        emma_error error;
        fixture.menu_result = interp->call_function(fixture.menu_builder, {}, message, &error);
        if (error.kind != error_kind::none) {
            return error.status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : error.status;
        }
        return message.empty() ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    }

    sao::plugins::sdk_binding::language_host_adapter_vtable provider() {
        sao::plugins::sdk_binding::language_host_adapter_vtable value{};
        value.language = sao::plugins::sdk_binding::language_host_kind::emma;
        value.load_plugin = load_plugin;
        value.unload_plugin = unload_plugin;
        value.invoke = invoke;
        value.dispatch = dispatch;
        value.user_data = this;
        return value;
    }
};

} // namespace

TEST_CASE("Emma stdlib imports stay in plugin sandbox and preserve errors",
          "[plugins][emma][stdlib][error]") {
    temp_tree tree(L"stdlib");
    write_text(tree.root / L"nested" / L"helper.emma", R"EMMA(
fn imported_value()
    return 42
end
)EMMA");
    write_text(tree.root / L"entry.emma", R"EMMA(
load_script("nested/helper.emma")
fn probe()
    let encoded = json_encode({name: "Emma", items: [1, true]})
    return {imported: imported_value(), decoded: json_decode(encoded), now: time()}
end
fn escape()
    return import("../outside.emma")
end
fn unsupported()
    return ctx.notify("Title", "Message")
end
fn panel_callback()
    return true
end
fn rejected_panel_callback()
    return ctx.register_ui_panel("callback-panel", {title: "Callback"}, panel_callback, nil)
end
fn rejected_panel_arity()
    return ctx.register_ui_panel("extra-panel", {title: "Extra"}, nil, nil, nil)
end
)EMMA");
    write_text(tree.root / L"lex.emma", "let value = @\n");
    write_text(tree.root / L"parse.emma", "fn broken()\nreturn 1\n");
    write_text(tree.root / L"runtime.emma", "missing_function()\n");

    const auto manifest = make_manifest(tree, "emma.context.stdlib");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);

    for (const auto& [entry, expected_kind] : std::vector<std::pair<const char*, error_kind>>{
             {"lex.emma", error_kind::lex_error},
             {"parse.emma", error_kind::parse_error},
             {"runtime.emma", error_kind::runtime_error}}) {
        emma_plugin_handle_t failed = reinterpret_cast<emma_plugin_handle_t>(1);
        emma_error error;
        const int32_t status = sao_plugins_emma_load_script_ex(
            tree.root.c_str(), entry, manifest.plugin_id.c_str(), context, &failed, &error);
        CHECK(status != SAO_OK);
        CHECK(failed == nullptr);
        CHECK(error.kind == expected_kind);
        CHECK_FALSE(error.message.empty());
    }

    emma_plugin_handle_t plugin = nullptr;
    emma_error load_error;
    REQUIRE(sao_plugins_emma_load_script_ex(tree.root.c_str(), "entry.emma",
                                            manifest.plugin_id.c_str(), context, &plugin,
                                            &load_error) == SAO_OK);
    const json probe = call_json(plugin, "probe");
    CHECK(probe["imported"] == 42);
    CHECK(probe["decoded"] == json{{"name", "Emma"}, {"items", {1, true}}});
    CHECK(probe["now"].is_number());

    emma_error call_error;
    CHECK(sao_plugins_emma_call_hook_ex(plugin, "escape", "[]", nullptr, &call_error) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(call_error.kind == error_kind::runtime_error);
    CHECK(call_error.message.find("plugin root") != std::string::npos);

    CHECK(sao_plugins_emma_call_hook_ex(plugin, "unsupported", "[]", nullptr, &call_error) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(call_error.kind == error_kind::runtime_error);
    CHECK(call_error.status == SAO_PLUGINS_ERR_UNSUPPORTED);

    CHECK(sao_plugins_emma_call_hook_ex(plugin, "rejected_panel_callback", "[]", nullptr,
                                        &call_error) == SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(call_error.kind == error_kind::runtime_error);
    CHECK(call_error.status == SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao_plugins_emma_call_hook_ex(plugin, "rejected_panel_arity", "[]", nullptr,
                                        &call_error) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(call_error.kind == error_kind::runtime_error);
    CHECK(call_error.status == SAO_ERR_INVALID_ARGUMENT);

    emma_error forged;
    forged.kind = static_cast<error_kind>(255);
    CHECK(sao_plugins_emma_error_status(&forged) == SAO_ERR_INVALID_ARGUMENT);
    char* formatted = reinterpret_cast<char*>(1);
    CHECK(sao_plugins_emma_error_format(&forged, &formatted) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(formatted == nullptr);

    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}

TEST_CASE("Emma native context owns callbacks and tears resources down in reverse",
          "[plugins][emma][context][lifecycle]") {
    platform_fixture fixture;
    auto provider = fixture.provider();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    temp_tree tree(L"native");
    write_text(tree.root / L"entry.emma", R"EMMA(
let event_total = 0
let hotkey_total = 0
let timer_total = 0
let subscription = 0
fn event_callback(event)
    event_total = event_total + event.payload.amount
end
fn hotkey_callback()
    hotkey_total = hotkey_total + 1
end
fn timer_callback()
    timer_total = timer_total + 1
end
fn on_load(ctx)
    ctx.set_defaults({level: 3})
    ctx.set_setting("name", "Emma")
    subscription = ctx.subscribe("demo", event_callback)
    ctx.subscribe_once("once", event_callback)
    ctx.emit("demo", {amount: 1})
    ctx.emit("once", {amount: 2})
    ctx.emit("once", {amount: 100})
    ctx.register_hotkey("main", hotkey_callback, "Ctrl+E", "Main")
    ctx.set_interval(timer_callback, 1.0)
    ctx.set_timeout(timer_callback, 1.0)
    ctx.notify("Title", "Message", 2.0, "info")
    ctx.toast("Toast")
    ctx.set_overlay("main", {visible: true})
    ctx.request_redraw("main", "test")
end
fn snapshot()
    return {events: event_total, hotkeys: hotkey_total, timers: timer_total,
            level: ctx.get_setting("level"), name: ctx.get_setting("name"),
            missing: ctx.get_setting("missing", 7)}
end
fn stop_events()
    ctx.unsubscribe(subscription)
    ctx.emit("demo", {amount: 100})
    return event_total
end
)EMMA");

    const auto manifest = make_manifest(tree, "emma.context.native");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(sao_plugins_emma_call_on_load(plugin) == SAO_OK);
    REQUIRE(fixture.callbacks.size() == 3);
    REQUIRE(fixture.redraw_count == 1);

    const auto hotkey =
        *std::find_if(fixture.callbacks.begin(), fixture.callbacks.end(),
                      [](const callback_slot& slot) { return slot.hotkey != nullptr; });
    hotkey.hotkey(hotkey.user_data);
    const auto interval = *std::find_if(
        fixture.callbacks.begin(), fixture.callbacks.end(),
        [](const callback_slot& slot) { return slot.timer != nullptr && !slot.one_shot; });
    interval.timer(interval.user_data);
    const auto timeout = *std::find_if(
        fixture.callbacks.begin(), fixture.callbacks.end(),
        [](const callback_slot& slot) { return slot.timer != nullptr && slot.one_shot; });
    timeout.timer(timeout.user_data);
    timeout.timer(timeout.user_data);
    std::erase_if(fixture.callbacks, [](const callback_slot& slot) { return slot.one_shot; });

    const json snapshot = call_json(plugin, "snapshot");
    CHECK(snapshot == json{{"events", 2},
                           {"hotkeys", 1},
                           {"timers", 2},
                           {"level", 3},
                           {"name", "Emma"},
                           {"missing", 7}});
    CHECK(call_json(plugin, "stop_events") == 2);

    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    CHECK(fixture.teardown ==
          std::vector<std::string>{"overlay:6", "notify:5", "notify:4", "timer:2", "hotkey:1"});

    sao_plugins_ctx_destroy(context);
    CHECK(fixture.retain_count == fixture.release_count);
    remove_plugin(loader_plugin);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}

TEST_CASE("Emma callback teardown failures retain userdata and retry from closing",
          "[plugins][emma][context][teardown]") {
    platform_fixture fixture;
    auto provider = fixture.provider();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    temp_tree tree(L"teardown_retry");
    write_text(tree.root / L"entry.emma", R"EMMA(
let calls = 0
fn on_timer()
    calls = calls + 1
end
ctx.set_interval(on_timer, 1.0)
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.teardown.retry");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);

    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(fixture.callbacks.size() == 1);
    const callback_slot retained = fixture.callbacks.front();

    SECTION("BUSY") {
        fixture.timer_unregister_status = SAO_PLUGINS_ERR_BUSY;
    }
    SECTION("UNSUPPORTED") {
        fixture.timer_unregister_status = SAO_PLUGINS_ERR_UNSUPPORTED;
    }

    CHECK(sao_plugins_emma_unload_script(plugin) == fixture.timer_unregister_status);
    CHECK(sao_plugins_emma_call_on_enable(plugin) == SAO_PLUGINS_ERR_BUSY);
    retained.timer(retained.user_data);
    CHECK(fixture.callbacks.size() == 1);

    fixture.timer_unregister_status = SAO_OK;
    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    CHECK(fixture.callbacks.empty());
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}

TEST_CASE("Emma failed load rollback keeps provider callback alive until retry",
          "[plugins][emma][context][rollback]") {
    platform_fixture fixture;
    fixture.timer_unregister_status = SAO_PLUGINS_ERR_BUSY;
    auto provider = fixture.provider();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    temp_tree tree(L"failed_rollback");
    write_text(tree.root / L"entry.emma", R"EMMA(
fn callback()
    return true
end
ctx.set_interval(callback, 1.0)
missing_function()
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.failed.rollback");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);

    emma_plugin_handle_t closing = nullptr;
    emma_error error;
    CHECK(sao_plugins_emma_load_script_ex(tree.root.c_str(), "entry.emma",
                                          manifest.plugin_id.c_str(), context, &closing,
                                          &error) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(closing != nullptr);
    REQUIRE(fixture.callbacks.size() == 1);
    const callback_slot retained = fixture.callbacks.front();
    retained.timer(retained.user_data);
    CHECK(sao_plugins_emma_call_on_load(closing) == SAO_PLUGINS_ERR_BUSY);

    fixture.timer_unregister_status = SAO_OK;
    REQUIRE(sao_plugins_emma_unload_script(closing) == SAO_OK);
    CHECK(fixture.callbacks.empty());
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}

TEST_CASE("Emma timeout acquisition is exactly once under concurrency",
          "[plugins][emma][context][oneshot][concurrency]") {
    platform_fixture fixture;
    auto provider = fixture.provider();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    temp_tree tree(L"oneshot_concurrent");
    write_text(tree.root / L"entry.emma", R"EMMA(
let calls = 0
fn callback()
    calls = calls + 1
end
fn on_load(ctx)
    ctx.set_timeout(callback, 1.0)
end
fn count()
    return calls
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.oneshot.concurrent");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(sao_plugins_emma_call_on_load(plugin) == SAO_OK);
    REQUIRE(fixture.callbacks.size() == 1);
    const callback_slot timeout = fixture.callbacks.front();

    std::barrier start{3};
    std::jthread first([&] {
        start.arrive_and_wait();
        timeout.timer(timeout.user_data);
    });
    std::jthread second([&] {
        start.arrive_and_wait();
        timeout.timer(timeout.user_data);
    });
    start.arrive_and_wait();
    first.join();
    second.join();
    CHECK(call_json(plugin, "count") == 1);
    std::erase_if(fixture.callbacks, [](const callback_slot& slot) { return slot.one_shot; });

    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}

TEST_CASE("Emma synchronous timeout fire is deferred until registration is armed",
          "[plugins][emma][context][oneshot][synchronous]") {
    platform_fixture fixture;
    fixture.fire_one_shot_timer_synchronously = true;
    auto provider = fixture.provider();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    temp_tree tree(L"oneshot_synchronous");
    write_text(tree.root / L"entry.emma", R"EMMA(
let calls = 0
fn callback()
    calls = calls + 1
end
fn on_load(ctx)
    ctx.set_timeout(callback, 1.0)
end
fn count()
    return calls
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.oneshot.synchronous");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(sao_plugins_emma_call_on_load(plugin) == SAO_OK);
    CHECK(call_json(plugin, "count") == 1);

    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}

TEST_CASE("Emma callback self-unregister returns without invocation-inflight deadlock",
          "[plugins][emma][context][deadlock]") {
    platform_fixture fixture;
    auto provider = fixture.provider();
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    temp_tree tree(L"self_unregister");
    write_text(tree.root / L"entry.emma", R"EMMA(
let token = ""
fn callback()
    ctx.clear_timer(token)
end
fn on_load(ctx)
    token = ctx.set_interval(callback, 1.0)
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.self.unregister");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(sao_plugins_emma_call_on_load(plugin) == SAO_OK);
    REQUIRE(fixture.callbacks.size() == 1);
    const callback_slot interval = fixture.callbacks.front();

    auto callback = std::async(std::launch::async, [&] { interval.timer(interval.user_data); });
    REQUIRE(callback.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    callback.get();

    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}

TEST_CASE("Emma source entry import and memory execution share the 8 MiB limit",
          "[plugins][emma][stdlib][size]") {
    constexpr size_t limit = 8u * 1024u * 1024u;
    temp_tree tree(L"source_limit");
    std::string exact = "fn accepted()\n    return true\nend\n";
    exact.resize(limit, ' ');
    write_text(tree.root / L"exact.emma", exact);
    write_text(tree.root / L"large.emma", std::string(limit + 1, ' '));
    write_text(tree.root / L"entry.emma", R"EMMA(
fn import_large()
    return import("large.emma")
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.source.limit", "entry.emma");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);

    emma_plugin_handle_t exact_plugin = nullptr;
    emma_error error;
    REQUIRE(sao_plugins_emma_load_script_ex(tree.root.c_str(), "exact.emma",
                                            manifest.plugin_id.c_str(), context, &exact_plugin,
                                            &error) == SAO_OK);
    REQUIRE(sao_plugins_emma_unload_script(exact_plugin) == SAO_OK);

    emma_plugin_handle_t oversized = reinterpret_cast<emma_plugin_handle_t>(1);
    CHECK(sao_plugins_emma_load_script_ex(tree.root.c_str(), "large.emma",
                                          manifest.plugin_id.c_str(), context, &oversized,
                                          &error) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(oversized == nullptr);
    CHECK(error.kind == error_kind::runtime_error);
    CHECK(error.message.find("8 MiB") != std::string::npos);

    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    CHECK(sao_plugins_emma_call_hook_ex(plugin, "import_large", "[]", nullptr, &error) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(error.kind == error_kind::runtime_error);
    CHECK(error.message.find("8 MiB") != std::string::npos);
    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);

    interpreter interp;
    ast_pool pool;
    char* execute_error = reinterpret_cast<char*>(1);
    const std::string memory_source(limit + 1, ' ');
    CHECK(sao_plugins_emma_execute_source(&interp, memory_source.data(), memory_source.size(),
                                          &pool, &execute_error) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(execute_error != nullptr);
    CHECK(std::string(execute_error).find("8 MiB") != std::string::npos);
    sao_plugins_emma_free_string(execute_error);

    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}

TEST_CASE("Emma preserves custom sdk_binding context providers",
          "[plugins][emma][sdk-binding][provider]") {
    custom_emma_provider_fixture fixture;
    auto provider = fixture.provider();
    REQUIRE(sao::plugins::sdk_binding::sao_plugins_binding_register_language_host(&provider) ==
            SAO_OK);

    temp_tree tree(L"custom_provider");
    write_text(tree.root / L"entry.emma", R"EMMA(
fn build_menu()
    return [{action_id: "native-setting", label: ctx.get_setting("native_label", "fallback")}]
end
fn panel_callback()
    return true
end
fn custom_context()
    return {provider: ctx.custom_provider, plugin_id: ctx.plugin_id, path: ctx.path,
            native_setting: ctx.get_setting("native_label", "fallback"),
            menu: ctx.register_menu_category("Custom", "", build_menu)}
end
fn rejected_panel_callback()
    return ctx.register_ui_panel("callback-panel", {title: "Callback"}, panel_callback, nil)
end
fn on_load(ctx)
    ctx.log("custom log")
    ctx.register_ui_panel("custom", {title: "Custom"}, nil, nil)
    ctx.register_menu_category("Custom", "", build_menu)
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.custom.provider");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    REQUIRE(sao_plugins_ctx_set_setting(context, "native_label", "\"native-label\"") ==
            SAO_OK);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(sao_plugins_emma_call_on_load(plugin) == SAO_OK);
    CHECK(fixture.load_calls == 1);
    CHECK(fixture.context_bind_calls == 1);
    CHECK(fixture.log_calls == 1);
    CHECK(fixture.panel_calls == 1);
    CHECK(fixture.menu_calls == 1);
    CHECK(call_json(plugin, "custom_context") ==
            json{{"provider", true},
               {"plugin_id", manifest.plugin_id},
               {"path", path_utf8(tree.root)},
               {"native_setting", "native-label"},
               {"menu", "custom-menu"}});
        emma_error panel_error;
        CHECK(sao_plugins_emma_call_hook_ex(plugin, "rejected_panel_callback", "[]", nullptr,
                                &panel_error) == SAO_PLUGINS_ERR_UNSUPPORTED);
        CHECK(panel_error.kind == error_kind::runtime_error);
        CHECK(panel_error.status == SAO_PLUGINS_ERR_UNSUPPORTED);
        CHECK(fixture.panel_calls == 1);
    REQUIRE(sao_plugins_emma_with_interpreter(plugin,
                                              custom_emma_provider_fixture::invoke_menu_builder,
                                              &fixture) == SAO_OK);
    const auto* menu_rows = std::get_if<std::shared_ptr<emma_list>>(&fixture.menu_result);
    REQUIRE(menu_rows != nullptr);
    REQUIRE(*menu_rows != nullptr);
    REQUIRE((*menu_rows)->items.size() == 1);
    const auto* menu_row = std::get_if<std::shared_ptr<emma_dict>>(&(*menu_rows)->items[0]);
    REQUIRE(menu_row != nullptr);
    REQUIRE(*menu_row != nullptr);
    const auto label = (*menu_row)->items.find("label");
    REQUIRE(label != (*menu_row)->items.end());
    REQUIRE(std::holds_alternative<std::string>(label->second));
    CHECK(std::get<std::string>(label->second) == "native-label");
    fixture.unload_status = SAO_PLUGINS_ERR_BUSY;
    CHECK(sao_plugins_emma_unload_script(plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK(fixture.unload_calls == 1);
    CHECK(sao_plugins_emma_call_on_enable(plugin) == SAO_PLUGINS_ERR_BUSY);
    fixture.unload_status = SAO_OK;
    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    CHECK(fixture.unload_calls == 2);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
    REQUIRE(sao::plugins::sdk_binding::sao_plugins_binding_unregister_language_host(
                sao::plugins::sdk_binding::language_host_kind::emma) == SAO_OK);
}

TEST_CASE("Emma rejects malformed custom context provider core callables transactionally",
          "[plugins][emma][sdk-binding][provider][validation]") {
    using malformed_kind = custom_emma_provider_fixture::malformed_context_kind;
    const std::vector<std::pair<malformed_kind, const char*>> cases{
        {malformed_kind::non_dictionary, "non-dictionary"},
        {malformed_kind::null_log, "null-log"},
        {malformed_kind::non_callable_panel, "non-callable-panel"},
        {malformed_kind::empty_menu_callable, "empty-menu-callable"},
    };

    for (const auto& [malformed, name] : cases) {
        CAPTURE(name);
        custom_emma_provider_fixture fixture;
        fixture.malformed_context = malformed;
        auto provider = fixture.provider();
        REQUIRE(sao::plugins::sdk_binding::sao_plugins_binding_register_language_host(&provider) ==
                SAO_OK);

        temp_tree tree(L"malformed_provider");
        write_text(tree.root / L"entry.emma", "fn probe()\n    return true\nend\n");
        const std::string plugin_id = std::string("emma.context.malformed.") + name;
        const auto manifest = make_manifest(tree, plugin_id.c_str());
        plugin_handle_t loader_plugin = add_plugin(manifest);
        plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
        REQUIRE(context != nullptr);

        emma_plugin_handle_t plugin = nullptr;
        emma_error error;
        const int32_t status = sao_plugins_emma_load_script_ex(
            tree.root.c_str(), "entry.emma", manifest.plugin_id.c_str(), context, &plugin, &error);
        CHECK(status == SAO_ERR_INVALID_ARGUMENT);
        CHECK(error.kind == error_kind::runtime_error);
        CHECK(error.status == SAO_ERR_INVALID_ARGUMENT);
        if (plugin != nullptr)
            REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);

        sao_plugins_ctx_destroy(context);
        remove_plugin(loader_plugin);
        REQUIRE(sao::plugins::sdk_binding::sao_plugins_binding_unregister_language_host(
                    sao::plugins::sdk_binding::language_host_kind::emma) == SAO_OK);
    }
}

TEST_CASE("Emma JSON ingress enforces depth node and byte budgets transactionally",
          "[plugins][emma][context][json][budget]") {
    temp_tree tree(L"json_ingress_budget");
    write_text(tree.root / L"entry.emma", R"EMMA(
let hook_calls = 0
let setting_calls = 0
let event_calls = 0
fn touch_hook(value)
    hook_calls = hook_calls + 1
    return hook_calls
end
fn read_budgeted_setting()
    let value = ctx.get_setting("budgeted")
    setting_calls = setting_calls + 1
    return value
end
fn on_event(event)
    event_calls = event_calls + 1
end
fn on_load(ctx)
    ctx.subscribe("budget-event", on_event)
end
fn ingress_counts()
    return {hooks: hook_calls, settings: setting_calls, events: event_calls}
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.json.ingress");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), "entry.emma",
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(sao_plugins_emma_call_on_load(plugin) == SAO_OK);

    const std::string hook_depth_boundary = nested_array_json(64);
    REQUIRE(sao_plugins_emma_call_hook(plugin, "touch_hook", hook_depth_boundary.c_str(),
                                       nullptr) == SAO_OK);
    const std::string hook_excessive_depth = nested_array_json(65);
    emma_error error;
    CHECK(sao_plugins_emma_call_hook_ex(plugin, "touch_hook", hook_excessive_depth.c_str(),
                                        nullptr, &error) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(call_json(plugin, "ingress_counts") ==
          json{{"hooks", 1}, {"settings", 0}, {"events", 0}});

    const std::string hook_node_boundary = flat_array_json(16383);
    REQUIRE(sao_plugins_emma_call_hook(plugin, "touch_hook", hook_node_boundary.c_str(),
                                       nullptr) == SAO_OK);
    const std::string excessive_hook_nodes = flat_array_json(16384);
    CHECK(sao_plugins_emma_call_hook_ex(plugin, "touch_hook", excessive_hook_nodes.c_str(),
                                        nullptr, &error) == SAO_ERR_INVALID_ARGUMENT);
    constexpr size_t json_byte_limit = 8U * 1024U * 1024U;
        const std::string hook_byte_boundary =
                "[\"" + std::string(json_byte_limit - 4, 'x') + "\"]";
        REQUIRE(sao_plugins_emma_call_hook(plugin, "touch_hook", hook_byte_boundary.c_str(),
                                                                             nullptr) == SAO_OK);
    const std::string excessive_hook_bytes = "[\"" + std::string(json_byte_limit, 'x') + "\"]";
    CHECK(sao_plugins_emma_call_hook_ex(plugin, "touch_hook", excessive_hook_bytes.c_str(),
                                        nullptr, &error) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(call_json(plugin, "ingress_counts") ==
            json{{"hooks", 3}, {"settings", 0}, {"events", 0}});

    const std::string setting_depth_boundary = nested_array_json(64);
    REQUIRE(sao_plugins_ctx_set_setting(context, "budgeted", setting_depth_boundary.c_str()) ==
            SAO_OK);
    REQUIRE(sao_plugins_emma_call_hook(plugin, "read_budgeted_setting", "[]", nullptr) == SAO_OK);
    const std::string setting_excessive_depth = nested_array_json(65);
    CHECK(sao_plugins_ctx_set_setting(context, "budgeted", setting_excessive_depth.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    const std::string excessive_setting_nodes = flat_array_json(16384);
    CHECK(sao_plugins_ctx_set_setting(context, "budgeted", excessive_setting_nodes.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    const std::string excessive_setting_bytes = json(std::string(json_byte_limit, 'x')).dump();
    CHECK(sao_plugins_ctx_set_setting(context, "budgeted", excessive_setting_bytes.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_emma_call_hook(plugin, "read_budgeted_setting", "[]", nullptr) == SAO_OK);
    CHECK(call_json(plugin, "ingress_counts") ==
          json{{"hooks", 3}, {"settings", 2}, {"events", 0}});

    const std::string event_depth_boundary = nested_array_json(63);
    REQUIRE(sao_plugins_ctx_emit(context, "budget-event", event_depth_boundary.c_str()) == SAO_OK);
    const std::string event_excessive_depth = nested_array_json(64);
    REQUIRE(sao_plugins_ctx_emit(context, "budget-event", event_excessive_depth.c_str()) ==
            SAO_OK);
    const std::string event_node_boundary = flat_array_json(16381);
    REQUIRE(sao_plugins_ctx_emit(context, "budget-event", event_node_boundary.c_str()) == SAO_OK);
    const std::string excessive_event_nodes = flat_array_json(16382);
    REQUIRE(sao_plugins_ctx_emit(context, "budget-event", excessive_event_nodes.c_str()) ==
            SAO_OK);
    CHECK(call_json(plugin, "ingress_counts") ==
          json{{"hooks", 3}, {"settings", 2}, {"events", 2}});

    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}

TEST_CASE("Emma uses canonical loader identity and plugin root for nested entries",
          "[plugins][emma][context][canonical]") {
    temp_tree tree(L"canonical_nested");
    write_text(tree.root / L"nested" / L"entry.emma", R"EMMA(
fn identity()
    return {plugin_id: ctx.plugin_id, path: ctx.path}
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.context.canonical", "nested/entry.emma");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(L"C:\\forged", "nested/entry.emma", "forged.id", context,
                                         &plugin) == SAO_OK);
    CHECK(call_json(plugin, "identity") ==
          json{{"plugin_id", manifest.plugin_id}, {"path", path_utf8(tree.root)}});
    REQUIRE(sao_plugins_emma_unload_script(plugin) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}

TEST_CASE("Emma secure source open rejects reparse files", "[plugins][emma][stdlib][reparse]") {
    temp_tree tree(L"reparse");
    write_text(tree.root / L"real.emma", "fn value()\n    return 1\nend\n");
    write_text(tree.root / L"real-dir" / L"entry.emma", "fn value()\n    return 2\nend\n");
    const fs::path link = tree.root / L"linked.emma";
    if (!CreateSymbolicLinkW(link.c_str(), (tree.root / L"real.emma").c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        SKIP("Windows symbolic-link creation is unavailable");
    }
    const fs::path directory_link = tree.root / L"linked-dir";
    REQUIRE(CreateSymbolicLinkW(directory_link.c_str(), (tree.root / L"real-dir").c_str(),
                                SYMBOLIC_LINK_FLAG_DIRECTORY |
                                    SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    const auto manifest = make_manifest(tree, "emma.context.reparse", "linked.emma");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);
    emma_plugin_handle_t plugin = reinterpret_cast<emma_plugin_handle_t>(1);
    emma_error error;
    CHECK(sao_plugins_emma_load_script_ex(tree.root.c_str(), "linked.emma",
                                          manifest.plugin_id.c_str(), context, &plugin,
                                          &error) == SAO_ERR_HANDLE_INVALID);
    CHECK(plugin == nullptr);
    CHECK(error.kind == error_kind::runtime_error);
    CHECK(error.message.find("reparse") != std::string::npos);

    CHECK(sao_plugins_emma_load_script_ex(tree.root.c_str(), "linked-dir/entry.emma",
                                          manifest.plugin_id.c_str(), context, &plugin,
                                          &error) == SAO_ERR_HANDLE_INVALID);
    CHECK(plugin == nullptr);
    CHECK(error.kind == error_kind::runtime_error);
    CHECK(error.message.find("reparse") != std::string::npos);
    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}
