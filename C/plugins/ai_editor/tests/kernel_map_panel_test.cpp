// Kernel Map Bridge panel — message-dispatch contract tests.
//
// These tests exercise KernelMapPanelProvider::handle_message() with a
// mock IKernelMapBridge that records every call.  No WebView2 is spun
// up; the real bridge is replaced with a proxy shim so we do not need
// the peer's kernel_map_bridge.cpp linked in.

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/kernel_map_panel_provider.h"
#include "../src/mcp_management_panel_provider.h"
#include "../src/native_utils.h"
#include "../src/webview_panel_registry.h"

namespace {

using Json = nlohmann::json;
using sao::ai_editor::native::IKernelMapBridge;
using sao::ai_editor::native::KernelMapPanelProvider;
using sao::ai_editor::native::McpManagementPanelProvider;
using sao::ai_editor::native::NativePanelProvider;
using sao::ai_editor::native::WebviewPanelOwner;
using sao::ai_editor::native::WebviewPanelRegistry;
using sao::ai_editor::native::WebviewPanelState;

class MockKernelMapBridge final : public IKernelMapBridge {
public:
    bool available = true;

    // Call recorder — every dispatch appends to this vector so tests
    // can assert on ordering and arguments.
    struct Call {
        std::string method;
        Json args = Json::object();
    };
    mutable std::vector<Call> calls;

    // Programmable replies.  Tests set these before invoking the
    // provider so the mock returns the exact payload expected.
    Json status_reply = Json{{"active", true}, {"mapped_count", 3}};
    Json enumerate_reply = Json{
        {"bases",
         Json::array({"0xFFFFC00088880000", "0xFFFFC00088890000",
                      "0xFFFFC000888A0000"})}};
    Json activate_reply = Json{{"ok", true}};
    Json deactivate_reply = Json{{"ok", true}};
    Json map_reply = Json{{"ok", true},
                          {"target_base", "0xFFFFC00088880000"}};
    Json unmap_reply = Json{{"ok", true},
                            {"target_base", "0xFFFFC00088880000"}};

    bool is_available() const noexcept override { return available; }

    Json status() override {
        calls.push_back({"status"});
        return status_reply;
    }
    Json enumerate() override {
        calls.push_back({"enumerate"});
        return enumerate_reply;
    }
    Json activate(const ActivateConfig& config) override {
        Json args;
        args["invoke_result_slot_va"] =
            static_cast<int64_t>(config.invoke_result_slot_va);
        args["idle_timeout_ms"] = config.idle_timeout_ms;
        args["pool_tag_seed"] =
            static_cast<int64_t>(config.pool_tag_seed);
        calls.push_back({"activate", std::move(args)});
        return activate_reply;
    }
    Json deactivate() override {
        calls.push_back({"deactivate"});
        return deactivate_reply;
    }
    Json map(const std::vector<uint8_t>& driver_bytes) override {
        Json args;
        args["length"] = static_cast<int64_t>(driver_bytes.size());
        calls.push_back({"map", std::move(args)});
        return map_reply;
    }
    Json unmap(uint64_t target_base) override {
        Json args;
        args["target_base"] = static_cast<int64_t>(target_base);
        calls.push_back({"unmap", std::move(args)});
        return unmap_reply;
    }
};

// Convenience: build a fresh provider + mock + return both by ref.
struct Fixture {
    KernelMapPanelProvider provider;
    MockKernelMapBridge* mock = nullptr;

    Fixture() {
        auto mock_owned = std::make_unique<MockKernelMapBridge>();
        mock = mock_owned.get();
        provider.install_bridge(std::move(mock_owned));
    }
};

// Dispatch helper — feeds the JSON directly into handle_message() and
// returns the reply for assertion.
Json dispatch(KernelMapPanelProvider& provider, const Json& message) {
    Json reply;
    const int32_t rc = provider.handle_message(message, reply);
    REQUIRE(rc == SAO_AI_EDITOR_OK);
    return reply;
}

}  // namespace

TEST_CASE("panel: status_message_returns_bridge_status",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    fx.mock->status_reply = Json{{"active", true}, {"mapped_count", 7}};

    const Json reply = dispatch(fx.provider,
                                Json{{"cmd", "status"},
                                     {"requestId", "req-1"}});

    REQUIRE(reply["status"] == "ok");
    REQUIRE(reply["cmd"] == "status");
    REQUIRE(reply["requestId"] == "req-1");
    REQUIRE(reply["payload"]["active"].get<bool>());
    REQUIRE(reply["payload"]["mapped_count"].get<int>() == 7);
    REQUIRE(fx.mock->calls.size() == 1);
    REQUIRE(fx.mock->calls[0].method == "status");
}

TEST_CASE("panel: enumerate_returns_bases_array",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    fx.mock->enumerate_reply = Json{
        {"bases",
         Json::array({"0xFFFFC00088880000", "0xFFFFC00088890000",
                      "0xFFFFC000888A0000"})}};

    const Json reply = dispatch(fx.provider,
                                Json{{"cmd", "enumerate"}});

    REQUIRE(reply["status"] == "ok");
    REQUIRE(reply["payload"]["bases"].is_array());
    REQUIRE(reply["payload"]["bases"].size() == 3);
    REQUIRE(reply["payload"]["bases"][0].get<std::string>() ==
            "0xFFFFC00088880000");
    REQUIRE(fx.mock->calls.size() == 1);
    REQUIRE(fx.mock->calls[0].method == "enumerate");
}

TEST_CASE("panel: unmap_forwards_target_base",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    fx.mock->unmap_reply = Json{{"ok", true},
                                {"target_base", "0xFFFFC00088890000"}};

    const Json reply = dispatch(
        fx.provider,
        Json{{"cmd", "unmap"},
             {"args", Json{{"target_base", "0xFFFFC00088890000"}}},
             {"requestId", "req-unmap"}});

    REQUIRE(reply["status"] == "ok");
    REQUIRE(reply["cmd"] == "unmap");
    REQUIRE(reply["requestId"] == "req-unmap");
    REQUIRE(fx.mock->calls.size() == 1);
    REQUIRE(fx.mock->calls[0].method == "unmap");
    REQUIRE(fx.mock->calls[0].args["target_base"].get<int64_t>() ==
            static_cast<int64_t>(0xFFFFC00088890000ULL));
}

TEST_CASE("panel: activate_forwards_config",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;

    SECTION("defaults when args omitted") {
        const Json reply = dispatch(fx.provider,
                                    Json{{"cmd", "activate"}});
        REQUIRE(reply["status"] == "ok");
        REQUIRE(fx.mock->calls.size() == 1);
        REQUIRE(fx.mock->calls[0].method == "activate");
        REQUIRE(fx.mock->calls[0].args["invoke_result_slot_va"]
                    .get<int64_t>() == 0);
        REQUIRE(fx.mock->calls[0].args["idle_timeout_ms"].get<int>() ==
                60000);
        REQUIRE(fx.mock->calls[0].args["pool_tag_seed"].get<int64_t>() ==
                0);
    }

    SECTION("hex slot va parses through parse_hex_address") {
        const Json reply = dispatch(
            fx.provider,
            Json{{"cmd", "activate"},
                 {"args", Json{{"invoke_result_slot_va",
                                "0xFFFF800012345000"},
                               {"idle_timeout_ms", 30000},
                               {"pool_tag_seed", "0xDEADBEEF"}}}});
        REQUIRE(reply["status"] == "ok");
        REQUIRE(fx.mock->calls.size() == 1);
        REQUIRE(fx.mock->calls[0].method == "activate");
        REQUIRE(fx.mock->calls[0].args["invoke_result_slot_va"]
                    .get<int64_t>() ==
                static_cast<int64_t>(0xFFFF800012345000ULL));
        REQUIRE(fx.mock->calls[0].args["idle_timeout_ms"].get<int>() ==
                30000);
        REQUIRE(fx.mock->calls[0].args["pool_tag_seed"].get<int64_t>() ==
                0xDEADBEEF);
    }
}

TEST_CASE("panel: deactivate_forwards_no_args",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    fx.mock->deactivate_reply = Json{{"ok", true}, {"rc", 0}};

    const Json reply = dispatch(fx.provider,
                                Json{{"cmd", "deactivate"},
                                     {"requestId", "req-deact"}});

    REQUIRE(reply["status"] == "ok");
    REQUIRE(reply["cmd"] == "deactivate");
    REQUIRE(fx.mock->calls.size() == 1);
    REQUIRE(fx.mock->calls[0].method == "deactivate");
    REQUIRE(fx.mock->calls[0].args.empty());
}

TEST_CASE("panel: unknown_message_returns_error",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;

    Json reply;
    const int32_t rc = fx.provider.handle_message(
        Json{{"cmd", "totally_bogus_command"}, {"requestId", "req-x"}},
        reply);
    REQUIRE(rc == SAO_AI_EDITOR_OK);
    REQUIRE(reply["status"] == "error");
    REQUIRE(reply["cmd"] == "totally_bogus_command");
    REQUIRE(reply["requestId"] == "req-x");
    REQUIRE(fx.mock->calls.empty());

    SECTION("missing cmd is an invalid argument") {
        Json malformed_reply;
        const int32_t bad_rc = fx.provider.handle_message(
            Json{{"args", Json::object()}}, malformed_reply);
        REQUIRE(bad_rc == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    }
}

TEST_CASE("panel: refresh_re_polls_status_and_enumerate",
          "[plugins][ai_editor][kernel_map_panel]") {
    // Refresh alone is a no-op on the native side.  The panel js issues
    // a companion status + enumerate pair; this test emulates that pair
    // to assert the full round trip.
    Fixture fx;
    fx.mock->status_reply = Json{{"active", false}, {"mapped_count", 0}};
    fx.mock->enumerate_reply = Json{{"bases", Json::array()}};

    const Json refresh_reply = dispatch(fx.provider,
                                        Json{{"cmd", "refresh"}});
    REQUIRE(refresh_reply["status"] == "ok");
    REQUIRE(refresh_reply["cmd"] == "refresh");
    // Refresh does not touch the bridge — that's the JS pump's job.
    REQUIRE(fx.mock->calls.empty());

    const Json status_reply = dispatch(fx.provider,
                                       Json{{"cmd", "status"}});
    const Json enumerate_reply = dispatch(fx.provider,
                                          Json{{"cmd", "enumerate"}});
    REQUIRE(status_reply["status"] == "ok");
    REQUIRE(status_reply["payload"]["active"].get<bool>() == false);
    REQUIRE(enumerate_reply["status"] == "ok");
    REQUIRE(enumerate_reply["payload"]["bases"].size() == 0);
    REQUIRE(fx.mock->calls.size() == 2);
    REQUIRE(fx.mock->calls[0].method == "status");
    REQUIRE(fx.mock->calls[1].method == "enumerate");
}

TEST_CASE(
    "panel: load_driver_reports_not_implemented_when_picker_missing",
    "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    // No install_file_picker() call — the provider must reject the
    // command cleanly and NOT invoke the bridge.

    const Json reply = dispatch(fx.provider,
                                Json{{"cmd", "load_driver"},
                                     {"requestId", "req-load"}});

    REQUIRE(reply["status"] == "not_implemented");
    REQUIRE(reply["reason"].get<std::string>() ==
            "no file picker in current build");
    REQUIRE(reply["cmd"] == "load_driver");
    REQUIRE(reply["requestId"] == "req-load");
    REQUIRE(fx.mock->calls.empty());
}

TEST_CASE("panel: unavailable_bridge_short_circuits_errors",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    fx.mock->available = false;

    for (const std::string cmd :
         {"status", "enumerate", "activate", "deactivate", "unmap",
          "load_driver"}) {
        Json message = Json{{"cmd", cmd}, {"requestId", "req-" + cmd}};
        if (cmd == "unmap") {
            message["args"] = Json{{"target_base", "0xFFFFC00088880000"}};
        }
        Json reply;
        const int32_t rc = fx.provider.handle_message(message, reply);
        REQUIRE(rc == SAO_AI_EDITOR_OK);
        REQUIRE(reply["status"] == "error");
        REQUIRE(reply["reason"].get<std::string>() == "bridge unavailable");
    }
    // Bridge must never have been called while flagged unavailable.
    REQUIRE(fx.mock->calls.empty());
}

TEST_CASE("panel: register_with_runtime_is_idempotent",
          "[plugins][ai_editor][kernel_map_panel]") {
    Fixture fx;
    WebviewPanelRegistry registry;

    const int32_t first =
        fx.provider.register_with_runtime(registry, /*assets_root*/ "");
    REQUIRE(first == SAO_AI_EDITOR_OK);
    REQUIRE(fx.provider.is_registered());
    REQUIRE(registry.list_alive().size() == 1);
    REQUIRE(registry.list_alive()[0].panel_id ==
            std::string(KernelMapPanelProvider::panel_id()));
    REQUIRE(registry.list_alive()[0].view_type ==
            std::string(KernelMapPanelProvider::view_type()));

    // A second call is a no-op — no new panel is created.
    const int32_t second =
        fx.provider.register_with_runtime(registry, /*assets_root*/ "");
    REQUIRE(second == SAO_AI_EDITOR_OK);
    REQUIRE(registry.list_alive().size() == 1);
    REQUIRE(registry.total_created() == 1);
}

TEST_CASE("panel: load_driver_invokes_bridge_when_picker_present",
          "[plugins][ai_editor][kernel_map_panel]") {
    // Prove the not-implemented reply IS conditional on the delegate.
    // The provider reads the file locally, so we write a tiny fake PE
    // into the temp dir and hand its path to the picker.
    wchar_t temp_dir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, temp_dir) > 0);
    const std::filesystem::path path =
        std::filesystem::path(temp_dir) /
        (L"sao_kernel_map_panel_test_" +
         std::to_wstring(GetCurrentProcessId()) + L".bin");
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.is_open());
        const std::string bytes(1024, 'x');
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    Fixture fx;
    fx.provider.install_file_picker(
        [utf8_path = path.string()]() { return utf8_path; });

    const Json reply = dispatch(fx.provider,
                                Json{{"cmd", "load_driver"},
                                     {"requestId", "req-load"}});

    REQUIRE(reply["status"] == "ok");
    REQUIRE(fx.mock->calls.size() == 1);
    REQUIRE(fx.mock->calls[0].method == "map");
    REQUIRE(fx.mock->calls[0].args["length"].get<int64_t>() == 1024);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("panel: MCP management implements the native provider contract",
          "[plugins][ai_editor][mcp_management_panel]") {
    McpManagementPanelProvider provider;
    NativePanelProvider* contract = &provider;
    REQUIRE(contract->provider_panel_id() == McpManagementPanelProvider::panel_id());

    bool navigated = false;
    int snapshots = 0;
    provider.install_snapshot_provider([&] {
        ++snapshots;
        return Json{{"registration", {{"name", "kernel_map"}, {"status", 0}}},
                    {"servers", Json::array({{{"name", "kernel_map"}}})},
                    {"tools", Json::array({{{"server", "kernel_map"},
                                             {"name", "helperStatus"},
                                             {"requires_confirm", false}}})}};
    });
    provider.install_kernel_map_navigator([&] {
        navigated = true;
        return true;
    });

    const std::filesystem::path assets_root =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "assets" / "ai_editor" / "mcp_management_panel";
    WebviewPanelRegistry registry;
    REQUIRE(contract->register_with_runtime(registry, assets_root.string()) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(provider.is_registered());
    REQUIRE(registry.total_created(WebviewPanelOwner::native_runtime) == 1);
    const auto state = registry.snapshot(std::string(provider.panel_id()));
    REQUIRE(state.has_value());
    CHECK(state->owner == WebviewPanelOwner::native_runtime);
    CHECK(state->view_type == std::string(provider.view_type()));
    CHECK(state->html.find("helperStatus") != std::string::npos);
    CHECK(state->html.find("Open Kernel Map panel") != std::string::npos);
    CHECK(state->html.find("acquireVsCodeApi") != std::string::npos);

    Json reply;
    REQUIRE(contract->handle_message(
                Json{{"cmd", "snapshot"}, {"requestId", "mcp-1"}}, reply) ==
            SAO_AI_EDITOR_OK);
    CHECK(reply["status"] == "ok");
    CHECK(reply["requestId"] == "mcp-1");
    CHECK(reply["payload"]["servers"].size() == 1);
    CHECK(reply["payload"]["tools"][0]["name"] == "helperStatus");
    CHECK(snapshots == 1);

    REQUIRE(contract->handle_message(
                Json{{"cmd", "open_kernel_map"}, {"requestId", "mcp-2"}}, reply) ==
            SAO_AI_EDITOR_OK);
    CHECK(reply["status"] == "ok");
    CHECK(reply["payload"]["panelId"] == "kernel-map-builtin");
    CHECK(reply["payload"]["opened"] == true);
    CHECK(navigated);

    REQUIRE(contract->unregister_from_runtime(registry) == SAO_AI_EDITOR_OK);
    CHECK_FALSE(provider.is_registered());
    const auto disposed = registry.snapshot(std::string(provider.panel_id()));
    REQUIRE(disposed.has_value());
    CHECK(disposed->disposed);
}
