#include <catch2/catch_test_macros.hpp>

#include "license_panel_internal.h"
#include "sao/license/sdk/license_status.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace Panel = sao::launcher::license_panel;

class HeadlessCompositor final {
  public:
    HeadlessCompositor() {
        REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &handle_) == SAO_STATUS_OK);
        REQUIRE(handle_ != nullptr);
    }

    ~HeadlessCompositor() {
        if (handle_ != nullptr)
            (void)sao_ui_compositor_try_destroy(handle_);
    }

    sao_ui_compositor_handle_t get() const noexcept {
        return handle_;
    }

  private:
    sao_ui_compositor_handle_t handle_{};
};

Panel::Operations make_operations() {
    Panel::Operations operations;
    operations.activate = [](std::string_view) { return SAO_STATUS_OK; };
    operations.get_hwid = [](std::string& output) {
        output.assign(64U, 'a');
        return SAO_STATUS_OK;
    };
    operations.get_status = [](std::string& tier, std::uint64_t& expiry_ms) {
        tier = "pro";
        expiry_ms = 123000U;
        return SAO_STATUS_OK;
    };
    operations.copy_to_clipboard = [](std::string_view) { return SAO_STATUS_OK; };
    operations.refresh_license = [] { return SAO_STATUS_OK; };
    return operations;
}

const Json* find_action(const Json& value, std::string_view action) {
    if (value.is_object()) {
        const auto found = value.find("action");
        if (found != value.end() && found->is_string() &&
            found->get_ref<const std::string&>() == action) {
            return &value;
        }
        for (const auto& [key, child] : value.items()) {
            (void)key;
            if (const Json* match = find_action(child, action); match != nullptr)
                return match;
        }
    } else if (value.is_array()) {
        for (const Json& child : value) {
            if (const Json* match = find_action(child, action); match != nullptr)
                return match;
        }
    }
    return nullptr;
}

bool service_until_idle(Panel::Owner& owner, Panel::Snapshot& snapshot, int attempts = 1000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (owner.service_ui() != SAO_STATUS_OK)
            return false;
        if (owner.snapshot(snapshot) != SAO_STATUS_OK)
            return false;
        if (!snapshot.busy)
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

TEST_CASE("License JSON spec exposes Skip and inherits the global theme",
          "[launcher][license][json][skip][theme][focused]") {
    HeadlessCompositor compositor;
    Panel::Owner owner(compositor.get(), make_operations());
    REQUIRE(owner.open() == SAO_STATUS_OK);

    Panel::Snapshot snapshot{};
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    const Json spec = Json::parse(snapshot.rendered_spec_json);
    CHECK(spec["version"] == 1);
    REQUIRE(find_action(spec, Panel::kSkipAction) != nullptr);
    REQUIRE(find_action(spec, Panel::kRefreshAction) != nullptr);
    CHECK(snapshot.rendered_spec_json.find('#') == std::string::npos);

    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_find_by_id(compositor.get(), Panel::kPanelId, &panel) == SAO_STATUS_OK);
    SaoPanelDescriptor descriptor{};
    REQUIRE(sao_ui_panel_get_descriptor(panel, &descriptor) == SAO_STATUS_OK);
    CHECK(descriptor.theme_override_json_utf8 == nullptr);

    REQUIRE(owner.dispatch_action(Panel::kSkipAction) == SAO_STATUS_OK);
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    CHECK(snapshot.activated);
    CHECK(snapshot.tier == "free");
    CHECK(snapshot.status_text.find("Skipped") != std::string::npos);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("License Refresh invokes the backend and preserves exact errors",
          "[launcher][license][refresh][error][focused]") {
    HeadlessCompositor compositor;
    std::atomic<sao_status_t> refresh_status{
        static_cast<sao_status_t>(SAO_LICENSE_ERR_NETWORK)};
    std::atomic<int> refresh_calls{};
    Panel::Operations operations = make_operations();
    operations.refresh_license = [&] {
        refresh_calls.fetch_add(1);
        return refresh_status.load();
    };
    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
        Panel::Snapshot snapshot{};
        REQUIRE(service_until_idle(owner, snapshot));
        refresh_calls.store(0);

        REQUIRE(owner.dispatch_action(Panel::kRefreshAction) == SAO_STATUS_OK);
        REQUIRE(service_until_idle(owner, snapshot));
    CHECK(refresh_calls.load() == 1);
    CHECK(snapshot.last_status == static_cast<sao_status_t>(SAO_LICENSE_ERR_NETWORK));
    CHECK(snapshot.error_text.find("Network error") != std::string::npos);

    refresh_status.store(SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action(Panel::kRefreshAction) == SAO_STATUS_OK);
        REQUIRE(service_until_idle(owner, snapshot));
    CHECK(refresh_calls.load() == 2);
    CHECK(snapshot.activated);
    CHECK(snapshot.tier == "pro");
    CHECK(snapshot.error_text.empty());

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("License rejects malformed bounded JSON and key text",
          "[launcher][license][json][bounds][focused]") {
    HeadlessCompositor compositor;
    Panel::Owner owner(compositor.get(), make_operations());
    REQUIRE(owner.open() == SAO_STATUS_OK);

    CHECK(owner.dispatch_action(Panel::kKeyInputAction, "{") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(owner.dispatch_action(Panel::kKeyInputAction, "[]") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(owner.dispatch_action(Panel::kKeyInputAction, R"({"value":7})") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const std::string oversized_key(257U, 'K');
    CHECK(owner.dispatch_action(Panel::kKeyInputAction,
                                Json{{"value", oversized_key}}.dump()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    std::string embedded_nul = R"({"value":"KEY"})";
    embedded_nul.push_back('\0');
    CHECK(owner.dispatch_action(
              Panel::kKeyInputAction,
              std::string_view(embedded_nul.data(), embedded_nul.size())) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const std::string oversized_payload(4097U, 'x');
    CHECK(owner.dispatch_action(Panel::kKeyInputAction, oversized_payload) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("License activation worker keeps teardown retryable and publishes completion",
          "[launcher][license][activation][concurrency][teardown][focused]") {
    HeadlessCompositor compositor;
    std::atomic<bool> release{};
    std::atomic<bool> key_matches{};
    std::atomic<int> refresh_calls{};
    Panel::Operations operations = make_operations();
    operations.activate = [&](std::string_view key) {
        key_matches.store(key == "KEY-123");
        while (!release.load())
            std::this_thread::sleep_for(1ms);
        return SAO_STATUS_OK;
    };
    operations.refresh_license = [&] {
        refresh_calls.fetch_add(1);
        return SAO_STATUS_OK;
    };
    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));
    refresh_calls.store(0);
    REQUIRE(owner.dispatch_action(Panel::kKeyInputAction,
                                  R"({"value":"KEY-123"})") == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action(Panel::kActivateAction) == SAO_STATUS_OK);

    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    CHECK(snapshot.busy);
    CHECK(owner.dispatch_action(Panel::kActivateAction) == SAO_UI_PANEL_STATUS_ERR_BUSY);
    CHECK(owner.take_offline() == SAO_UI_PANEL_STATUS_ERR_BUSY);

    release.store(true);
    REQUIRE(service_until_idle(owner, snapshot));
    CHECK(snapshot.last_status == SAO_STATUS_ERR_CANCELLED);
    CHECK(key_matches.load());
    CHECK(snapshot.activated);
    CHECK(snapshot.status_text.empty());
    CHECK(snapshot.rendered_spec_json.find("Activating...") == std::string::npos);
    CHECK(refresh_calls.load() == 0);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("License owner-thread and operation leases reject concurrent retirement",
          "[launcher][license][owner_thread][lease][focused]") {
    HeadlessCompositor compositor;
    Panel::Owner* owner_address = nullptr;
    sao_status_t nested_retirement = SAO_STATUS_OK;
    Panel::Operations operations = make_operations();
    operations.copy_to_clipboard = [&](std::string_view) {
        REQUIRE(owner_address != nullptr);
        nested_retirement = owner_address->take_offline();
        return SAO_STATUS_OK;
    };
    Panel::Owner owner(compositor.get(), std::move(operations));
    owner_address = &owner;
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action(Panel::kCopyHwidAction) == SAO_STATUS_OK);
    CHECK(nested_retirement == SAO_UI_PANEL_STATUS_ERR_BUSY);

    std::atomic<sao_status_t> close_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> action_status{SAO_STATUS_OK};
    std::thread foreign([&] {
        close_status.store(owner.close());
        action_status.store(owner.dispatch_action(Panel::kRefreshAction));
    });
    foreign.join();
    CHECK(close_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(action_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);

    REQUIRE(owner.dispatch_event_for_testing(SAO_UI_PANEL_EVENT_CLOSE) == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    CHECK_FALSE(snapshot.visible);
    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    CHECK(owner.take_offline() == SAO_STATUS_OK);
}
