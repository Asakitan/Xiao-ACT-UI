#include <catch2/catch_test_macros.hpp>

#include "process_selector_panel_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace Panel = sao::launcher::process_selector_panel;

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

    std::size_t layer_count() const {
        std::size_t count = 0;
        REQUIRE(sao_ui_compositor_list_layers(handle_, nullptr, 0, &count) == SAO_STATUS_OK);
        return count;
    }

  private:
    sao_ui_compositor_handle_t handle_{};
};

Panel::ProcessRecord process(std::uint32_t pid, std::uint64_t start_time, std::string path,
                             std::uint32_t parent = 1U, std::string base_name = {}) {
    return {pid, parent, start_time, std::move(path), std::move(base_name)};
}

std::optional<Json> find_action_node(const Json& value, std::string_view action) {
    if (value.is_object()) {
        const auto action_value = value.find("action");
        if (action_value != value.end() && action_value->is_string() &&
            action_value->get_ref<const std::string&>() == action) {
            return value;
        }
        for (auto member = value.begin(); member != value.end(); ++member) {
            if (auto found = find_action_node(member.value(), action); found.has_value())
                return found;
        }
    } else if (value.is_array()) {
        for (const Json& child : value) {
            if (auto found = find_action_node(child, action); found.has_value())
                return found;
        }
    }
    return std::nullopt;
}

bool service_until_idle(Panel::Owner& owner, Panel::Snapshot& snapshot, int attempts = 1000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (owner.service_ui() != SAO_STATUS_OK)
            return false;
        if (owner.snapshot(snapshot) != SAO_STATUS_OK)
            return false;
        if (!snapshot.loading)
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool wait_until(const std::function<bool()>& predicate, int attempts = 1000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

TEST_CASE("process selector normalizes UTF-8 bounded snapshots and filters games",
          "[launcher][process_selector][utf8][filter][focused]") {
    HeadlessCompositor compositor;
    Panel::Operations operations{};
    operations.current_process_id = 99U;
    operations.enumerate_snapshot = [](std::vector<Panel::ProcessRecord>& output) {
        std::string embedded_nul = R"(C:\bad.exe)";
        embedded_nul.push_back('\0');
        embedded_nul.append("tail");
        std::string invalid_utf8 = R"(C:\bad-)";
        invalid_utf8.push_back(static_cast<char>(0xc3));
        invalid_utf8.push_back('(');
        invalid_utf8.append(".exe");
        output = {
            process(0U, 1U, R"(C:\zero.exe)"),
            process(4U, 2U, R"(C:\System)"),
            process(99U, 3U, R"(C:\SaoAuto.exe)"),
            process(10U, 100U, R"(C:\Tools\Alpha.exe)"),
            process(20U, 200U, R"(D:\Games\Nova\NovaClient.exe)"),
            process(30U, 300U, std::move(embedded_nul)),
            process(40U, 400U, std::move(invalid_utf8)),
            process(50U, 500U, std::string(32768U, 'x')),
        };
        return SAO_STATUS_OK;
    };
    operations.query_process = [](std::uint32_t, Panel::ProcessRecord&) {
        return SAO_STATUS_ERR_NOT_FOUND;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_OK; };

    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.refresh() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));
    REQUIRE(snapshot.all_processes.size() == 2U);
    CHECK(snapshot.all_processes[0].pid == 10U);
    CHECK(snapshot.all_processes[0].base_name_utf8 == "Alpha.exe");
    CHECK(snapshot.all_processes[1].pid == 20U);

    REQUIRE(owner.set_filter(Panel::FilterMode::likely_game) == SAO_STATUS_OK);
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    REQUIRE(snapshot.visible_processes.size() == 1U);
    CHECK(snapshot.visible_processes.front().pid == 20U);
    CHECK(Panel::is_likely_game_process(snapshot.visible_processes.front()));

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("process selector JSON spec inherits global theme and carries stable identity",
          "[launcher][process_selector][json][identity][theme][focused]") {
    HeadlessCompositor compositor;
    const Panel::ProcessRecord target =
        process(321U, 987654321U, R"(D:\Games\Photon\PhotonGame.exe)", 44U);
    Panel::Operations operations{};
    operations.current_process_id = 999U;
    operations.enumerate_snapshot = [&](std::vector<Panel::ProcessRecord>& output) {
        output = {target};
        return SAO_STATUS_OK;
    };
    operations.query_process = [&](std::uint32_t, Panel::ProcessRecord& output) {
        output = target;
        return SAO_STATUS_OK;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_OK; };

    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));
    const Json spec = Json::parse(snapshot.rendered_spec_json);
    const auto action = find_action_node(spec, Panel::kAttachAction);
    REQUIRE(action.has_value());
    CHECK((*action)["payload"]["pid"] == target.pid);
    CHECK((*action)["payload"]["start_time_100ns"] == target.start_time_100ns);
    CHECK(snapshot.rendered_spec_json.find('#') == std::string::npos);

    sao_ui_panel_handle_t panel = nullptr;
    REQUIRE(sao_ui_panel_find_by_id(compositor.get(), Panel::kPanelId, &panel) == SAO_STATUS_OK);
    SaoPanelDescriptor descriptor{};
    REQUIRE(sao_ui_panel_get_descriptor(panel, &descriptor) == SAO_STATUS_OK);
    CHECK(descriptor.theme_override_json_utf8 == nullptr);

    REQUIRE(owner.dispatch_action(Panel::kAttachAction,
                                  (*action)["payload"].dump()) == SAO_STATUS_OK);
    REQUIRE(service_until_idle(owner, snapshot));
    REQUIRE(snapshot.attached_process.has_value());
    CHECK(snapshot.attached_process->identity() == target.identity());
    CHECK(snapshot.status_text.find("Attached PID 321") != std::string::npos);

    const std::string rendered = snapshot.rendered_spec_json;
    REQUIRE(owner.service_ui() == SAO_STATUS_OK);
    REQUIRE(owner.service_ui() == SAO_STATUS_OK);
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    CHECK(snapshot.rendered_spec_json == rendered);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("process selector worker rejects PID reuse before attach",
          "[launcher][process_selector][worker][pid_reuse][focused]") {
    HeadlessCompositor compositor;
    std::vector<Panel::ProcessRecord> current{
        process(700U, 1000U, R"(D:\Games\Astra\Astra-Win64-Shipping.exe)"),
    };
    std::atomic<int> attach_calls{};
    Panel::Operations operations{};
    operations.current_process_id = 999U;
    operations.enumerate_snapshot = [&](std::vector<Panel::ProcessRecord>& output) {
        output = current;
        return SAO_STATUS_OK;
    };
    operations.query_process = [&](std::uint32_t, Panel::ProcessRecord& output) {
        output = current.front();
        return SAO_STATUS_OK;
    };
    operations.attach = [&](std::uint32_t) {
        attach_calls.fetch_add(1);
        return SAO_STATUS_OK;
    };

    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));
    current.front().start_time_100ns = 2000U;

    REQUIRE(owner.attach({700U, 1000U}) == SAO_STATUS_OK);
    REQUIRE(service_until_idle(owner, snapshot));
    CHECK(snapshot.last_status == SAO_STATUS_ERR_PROCESS_GONE);
    CHECK(snapshot.status_text.find("identity changed") != std::string::npos);
    CHECK(attach_calls.load() == 0);
    CHECK_FALSE(snapshot.attached_process.has_value());

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("process selector attach runs off the owner thread and makes teardown retryable",
          "[launcher][process_selector][worker][concurrency][teardown][focused]") {
    HeadlessCompositor compositor;
    const Panel::ProcessRecord target =
        process(808U, 8080U, R"(D:\Games\Async\AsyncGame.exe)");
    const std::thread::id owner_thread = std::this_thread::get_id();
    std::thread::id query_thread;
    std::atomic<bool> entered{};
    std::atomic<bool> release{};
    Panel::Operations operations{};
    operations.current_process_id = 999U;
    operations.enumerate_snapshot = [&](std::vector<Panel::ProcessRecord>& output) {
        output = {target};
        return SAO_STATUS_OK;
    };
    operations.query_process = [&](std::uint32_t, Panel::ProcessRecord& output) {
        query_thread = std::this_thread::get_id();
        entered.store(true);
        while (!release.load())
            std::this_thread::sleep_for(1ms);
        output = target;
        return SAO_STATUS_OK;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_OK; };

    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));
    REQUIRE(owner.attach(target.identity()) == SAO_STATUS_OK);
    REQUIRE(wait_until([&] { return entered.load(); }));
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    CHECK(snapshot.loading);
    CHECK(query_thread != owner_thread);
    CHECK(owner.take_offline() == SAO_UI_PANEL_STATUS_ERR_BUSY);

    release.store(true);
    REQUIRE(service_until_idle(owner, snapshot));
    CHECK(snapshot.last_status == SAO_STATUS_OK);
    REQUIRE(snapshot.attached_process.has_value());
    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("process selector rejects malformed JSON, NUL, and oversized payloads",
          "[launcher][process_selector][json][bounds][focused]") {
    HeadlessCompositor compositor;
    Panel::Operations operations{};
    operations.current_process_id = 999U;
    operations.enumerate_snapshot = [](std::vector<Panel::ProcessRecord>& output) {
        output.clear();
        return SAO_STATUS_OK;
    };
    operations.query_process = [](std::uint32_t, Panel::ProcessRecord&) {
        return SAO_STATUS_ERR_NOT_FOUND;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_OK; };
    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));

    CHECK(owner.dispatch_action(Panel::kFilterAction, "{") == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(owner.dispatch_action(Panel::kFilterAction, R"({"mode":7})") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(owner.dispatch_action(Panel::kAttachAction,
                                R"({"pid":-1,"start_time_100ns":1})") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(owner.dispatch_action(Panel::kAttachAction,
                                R"({"pid":1,"start_time_100ns":1.5})") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    std::string embedded_nul = R"({"mode":"all"})";
    embedded_nul.push_back('\0');
    CHECK(owner.dispatch_action(
              Panel::kFilterAction,
              std::string_view(embedded_nul.data(), embedded_nul.size())) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    std::string invalid_utf8 = R"({"mode":")";
    invalid_utf8.push_back(static_cast<char>(0xc3));
    invalid_utf8.push_back('(');
    invalid_utf8.append(R"("})");
    CHECK(owner.dispatch_action(Panel::kFilterAction, invalid_utf8) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const std::string oversized_payload(16U * 1024U + 1U, 'x');
    CHECK(owner.dispatch_action(Panel::kFilterAction, oversized_payload) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const std::string oversized_action(65U, 'x');
    CHECK(owner.dispatch_action(oversized_action, "{}") == SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}

TEST_CASE("process selector owner-thread rejection preserves a retryable panel",
          "[launcher][process_selector][owner_thread][retirement][focused]") {
    HeadlessCompositor compositor;
    Panel::Operations operations{};
    operations.current_process_id = 999U;
    operations.enumerate_snapshot = [](std::vector<Panel::ProcessRecord>& output) {
        output = {process(81U, 8100U, R"(D:\Games\Threaded\ThreadedGame.exe)")};
        return SAO_STATUS_OK;
    };
    operations.query_process = [](std::uint32_t, Panel::ProcessRecord&) {
        return SAO_STATUS_ERR_NOT_FOUND;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_OK; };
    Panel::Owner owner(compositor.get(), std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    Panel::Snapshot snapshot{};
    REQUIRE(service_until_idle(owner, snapshot));

    std::atomic<sao_status_t> close_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> offline_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> action_status{SAO_STATUS_OK};
    std::thread foreign([&] {
        close_status.store(owner.close());
        offline_status.store(owner.take_offline());
        action_status.store(owner.dispatch_action(Panel::kRefreshAction));
    });
    foreign.join();
    CHECK(close_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(offline_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(action_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(compositor.layer_count() == 1U);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    CHECK(compositor.layer_count() == 0U);
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until_idle(owner, snapshot));
    CHECK(compositor.layer_count() == 1U);
    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
}
