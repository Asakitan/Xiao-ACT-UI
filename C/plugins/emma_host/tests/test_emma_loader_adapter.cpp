#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/emma_host/emma_call.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_loader_adapter.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <windows.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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
               (L"sao_emma_adapter_" +
                std::to_wstring(GetCurrentProcessId()) + L"_" + suffix +
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
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string result(static_cast<size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                static_cast<int>(wide.size()), result.data(), required, nullptr,
                nullptr) == required);
    return result;
}

void write_text(const fs::path& path, const std::string& text) {
    REQUIRE((fs::create_directories(path.parent_path()) ||
             fs::is_directory(path.parent_path())));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(const temp_tree& tree,
                              const char* plugin_id,
                              const char* entry = "nested/plugin.emma") {
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
    REQUIRE(sao_plugins_registry_add_plugin(
                sao_plugins_registry_instance(), &manifest, &plugin) ==
            SAO_OK);
    REQUIRE(plugin != nullptr);
    return plugin;
}

void remove_plugin(plugin_handle_t plugin) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(),
                                        plugin) == SAO_OK);
}

bool has_panel(const char* plugin_id, const char* panel_id) {
    const auto panels = snapshot_extensions(sao_plugins_registry_instance(),
                                            extension_kind::ui_panel);
    for (const auto& panel : panels) {
        if (panel.plugin_id == plugin_id && panel.id == panel_id) return true;
    }
    return false;
}

class direct_hook_gate final {
public:
    emma_value wait(std::vector<emma_value>) {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return int64_t{7};
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

int32_t SAO_PLUGINS_CALL install_direct_hook_gate(interpreter* interp,
                                                  void* user_data) {
    if (interp == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* gate = static_cast<direct_hook_gate*>(user_data);
    auto callable = std::make_shared<sao::plugins::emma_host::callable>();
    callable->name = "wait_for_unload";
    callable->host_impl = [gate](std::vector<emma_value> arguments) {
        return gate->wait(std::move(arguments));
    };
    interp->register_global("wait_for_unload", std::move(callable));
    return SAO_OK;
}

} // namespace

TEST_CASE("Emma direct unload closes active hooks without UAF",
          "[plugins][emma][direct][concurrency]") {
    temp_tree tree(L"direct_concurrency");
    write_text(tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn blocking_hook()
    return wait_for_unload()
end
fn quick_hook()
    return 1
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.direct.concurrent");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);

    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(
                tree.root.c_str(), manifest.entry.c_str(),
                manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(plugin != nullptr);

    direct_hook_gate gate;
    REQUIRE(sao_plugins_emma_with_interpreter(
                plugin, install_direct_hook_gate, &gate) == SAO_OK);

    std::atomic_int hook_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread hook_thread([&] {
        hook_status.store(sao_plugins_emma_call_hook(
            plugin, "blocking_hook", "[]", nullptr));
    });
    REQUIRE(gate.wait_until_entered(std::chrono::seconds(2)));

    CHECK(sao_plugins_emma_unload_script(plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_emma_call_hook(plugin, "quick_hook", "[]", nullptr) ==
          SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_emma_call_on_enable(plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK_FALSE(sao_plugins_emma_has_hook(plugin, "quick_hook"));
    CHECK(sao_plugins_emma_with_interpreter(
              plugin, install_direct_hook_gate, &gate) ==
          SAO_PLUGINS_ERR_BUSY);

    gate.release();
    hook_thread.join();
    REQUIRE(hook_status.load() == SAO_OK);

    std::barrier start{3};
    std::atomic_int first_unload{SAO_ERR_OS_CALL_FAILED};
    std::atomic_int second_unload{SAO_ERR_OS_CALL_FAILED};
    std::jthread first([&] {
        start.arrive_and_wait();
        first_unload.store(sao_plugins_emma_unload_script(plugin));
    });
    std::jthread second([&] {
        start.arrive_and_wait();
        second_unload.store(sao_plugins_emma_unload_script(plugin));
    });
    start.arrive_and_wait();
    first.join();
    second.join();

    const int32_t first_status = first_unload.load();
    const int32_t second_status = second_unload.load();
    CHECK(((first_status == SAO_OK &&
            second_status == SAO_ERR_HANDLE_INVALID) ||
           (first_status == SAO_ERR_HANDLE_INVALID &&
            second_status == SAO_OK)));
    CHECK(sao_plugins_emma_unload_script(plugin) == SAO_ERR_HANDLE_INVALID);
    CHECK(sao_plugins_emma_call_on_disable(plugin) ==
          SAO_ERR_HANDLE_INVALID);

    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}

TEST_CASE("Emma generic loader adapter closes lifecycle, context and JSON",
          "[plugins][emma][adapter]") {
    emma_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_emma_register_loader_adapter(&owner) == SAO_OK);
    REQUIRE(owner != nullptr);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);

    emma_loader_adapter_owner_t duplicate =
        reinterpret_cast<emma_loader_adapter_owner_t>(1);
    CHECK(sao_plugins_emma_register_loader_adapter(&duplicate) ==
          SAO_PLUGINS_ERR_ALREADY_EXISTS);
    CHECK(duplicate == nullptr);
    CHECK(sao_plugins_emma_unregister_loader_adapter(nullptr) ==
          SAO_ERR_INVALID_ARGUMENT);

    temp_tree lifecycle_tree(L"lifecycle");
    write_text(lifecycle_tree.root / L"nested" / L"actual-entry.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("emma adapter loaded")
    ctx.register_ui_panel("main", {title: "Emma Panel"}, nil, nil)
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    return true
end
)EMMA");
    auto lifecycle_manifest = make_manifest(
        lifecycle_tree, "emma.adapter.lifecycle", "nested/actual-entry.emma");
    plugin_handle_t lifecycle_plugin = add_plugin(lifecycle_manifest);

    REQUIRE(sao_plugins_lifecycle_load(lifecycle_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(lifecycle_plugin) ==
          lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 1);
    CHECK(has_panel("emma.adapter.lifecycle", "main"));
    CHECK(sao_plugins_emma_unregister_loader_adapter(owner) ==
          SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(lifecycle_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_disable(lifecycle_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(lifecycle_plugin) == SAO_OK);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    CHECK_FALSE(has_panel("emma.adapter.lifecycle", "main"));
    remove_plugin(lifecycle_plugin);

    temp_tree veto_tree(L"veto");
    write_text(veto_tree.root / L"nested" / L"plugin.emma", R"EMMA(
let unload_attempts = 0
fn on_load(ctx)
    ctx.log("veto fixture loaded")
end
fn on_unload()
    unload_attempts = unload_attempts + 1
    return unload_attempts > 1
end
)EMMA");
    const auto veto_manifest = make_manifest(veto_tree, "emma.adapter.veto");
    plugin_handle_t veto_plugin = add_plugin(veto_manifest);
    REQUIRE(sao_plugins_lifecycle_load(veto_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_unload(veto_plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(veto_plugin) ==
          lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 1);
    REQUIRE(sao_plugins_lifecycle_unload(veto_plugin) == SAO_OK);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(veto_plugin);

    temp_tree failed_tree(L"failed");
    write_text(failed_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn on_load(ctx)
    ctx.register_ui_panel("rollback", {title: "Rollback"}, nil, nil)
    missing_function()
end
)EMMA");
    const auto failed_manifest = make_manifest(
        failed_tree, "emma.adapter.failed");
    plugin_handle_t failed_plugin = add_plugin(failed_manifest);
    CHECK(sao_plugins_lifecycle_load(failed_plugin) ==
          SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(failed_plugin) ==
          lifecycle_state::failed);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    CHECK_FALSE(has_panel("emma.adapter.failed", "rollback"));
    plugin_context_t* failed_context = reinterpret_cast<plugin_context_t*>(1);
    CHECK(sao_plugins_lifecycle_get_context(failed_plugin, &failed_context) ==
          SAO_ERR_NOT_INITIALIZED);
    CHECK(failed_context == nullptr);
    remove_plugin(failed_plugin);

    temp_tree json_tree(L"json");
    write_text(json_tree.root / L"nested" / L"json.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("direct JSON fixture loaded")
end
fn echo(number, object, items)
    return {number: number, object: object, items: items}
end
fn unsupported_result()
    return unsupported_result
end
)EMMA");
    const auto json_manifest = make_manifest(
        json_tree, "emma.adapter.json", "nested/json.emma");
    plugin_handle_t json_loader_plugin = add_plugin(json_manifest);
    plugin_context_t* direct_context =
        sao_plugins_ctx_create(json_loader_plugin);
    REQUIRE(direct_context != nullptr);
    emma_plugin_handle_t direct_plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(
                json_tree.root.c_str(), "nested/json.emma",
                json_manifest.plugin_id.c_str(), direct_context,
                &direct_plugin) == SAO_OK);
    REQUIRE(direct_plugin != nullptr);
    REQUIRE(sao_plugins_emma_call_on_load(direct_plugin) == SAO_OK);

    char* result = nullptr;
    REQUIRE(sao_plugins_emma_call_hook(
                direct_plugin, "echo",
                R"([7,{"name":"Aldina"},[true,null,2.5]])", &result) ==
            SAO_OK);
    REQUIRE(result != nullptr);
    const json parsed = json::parse(result);
    sao_plugins_emma_free_string(result);
    result = nullptr;
    CHECK(parsed == json{{"number", 7},
                         {"object", {{"name", "Aldina"}}},
                         {"items", {true, nullptr, 2.5}}});
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "echo", "{bad",
                                     &result) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(result == nullptr);
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "echo", "{}", &result) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "missing", "[]", &result) ==
          SAO_ERR_HANDLE_INVALID);
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "unsupported_result", "[]",
                                     &result) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(result == nullptr);
    REQUIRE(sao_plugins_emma_unload_script(direct_plugin) == SAO_OK);
    sao_plugins_ctx_destroy(direct_context);
    remove_plugin(json_loader_plugin);

    temp_tree concurrent_tree(L"concurrent");
    write_text(concurrent_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("slow fixture loaded")
end
fn on_enable()
    ctx.register_ui_panel("slow_started", {title: "Slow Started"}, nil, nil)
    let index = 0
    while index < 2000000
        index = index + 1
    end
end
)EMMA");
    const auto concurrent_manifest = make_manifest(
        concurrent_tree, "emma.adapter.concurrent");
    plugin_handle_t concurrent_plugin = add_plugin(concurrent_manifest);
    REQUIRE(sao_plugins_lifecycle_load(concurrent_plugin) == SAO_OK);

    temp_tree quick_tree(L"quick");
    write_text(quick_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("quick fixture loaded")
end
fn on_enable()
    return true
end
)EMMA");
    const auto quick_manifest = make_manifest(quick_tree, "emma.adapter.quick");
    plugin_handle_t quick_plugin = add_plugin(quick_manifest);
    REQUIRE(sao_plugins_lifecycle_load(quick_plugin) == SAO_OK);

    std::atomic_int concurrent_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread concurrent_call([&] {
        concurrent_status.store(
            sao_plugins_lifecycle_enable(concurrent_plugin));
    });
    const auto state_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!has_panel("emma.adapter.concurrent", "slow_started") &&
           std::chrono::steady_clock::now() < state_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(has_panel("emma.adapter.concurrent", "slow_started"));
    REQUIRE(sao_plugins_lifecycle_state(concurrent_plugin) ==
            lifecycle_state::enabling);
    CHECK(sao_plugins_lifecycle_unload(concurrent_plugin) ==
          SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(quick_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(concurrent_plugin) ==
          lifecycle_state::enabling);
    concurrent_call.join();
    CHECK(concurrent_status.load() == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(quick_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(concurrent_plugin) == SAO_OK);
    remove_plugin(quick_plugin);
    remove_plugin(concurrent_plugin);

    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_OK);
    CHECK(sao_plugins_emma_unregister_loader_adapter(owner) ==
          SAO_ERR_HANDLE_INVALID);
}
