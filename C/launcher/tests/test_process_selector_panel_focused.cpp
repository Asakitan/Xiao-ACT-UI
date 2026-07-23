#include <catch2/catch_test_macros.hpp>

#include "process_selector_panel_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_sdk.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using sao::launcher::process_selector_panel::FilterMode;
using sao::launcher::process_selector_panel::Operations;
using sao::launcher::process_selector_panel::Owner;
using sao::launcher::process_selector_panel::ProcessIdentity;
using sao::launcher::process_selector_panel::ProcessRecord;
using sao::launcher::process_selector_panel::Snapshot;

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

    HeadlessCompositor(const HeadlessCompositor&) = delete;
    HeadlessCompositor& operator=(const HeadlessCompositor&) = delete;

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

ProcessRecord process(std::uint32_t pid, std::uint64_t start_time, std::string path,
                      std::uint32_t parent = 1, std::string base_name = {}) {
    return ProcessRecord{pid, parent, start_time, std::move(path), std::move(base_name)};
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

std::vector<std::uint32_t> visible_pids(const Snapshot& snapshot) {
    std::vector<std::uint32_t> pids;
    pids.reserve(snapshot.visible_processes.size());
    std::ranges::transform(snapshot.visible_processes, std::back_inserter(pids),
                           &ProcessRecord::pid);
    return pids;
}

} // namespace

TEST_CASE("process selector sorts, excludes and filters deterministic snapshots",
          "[launcher][process_selector][focused]") {
    HeadlessCompositor compositor;
    int enumerate_calls = 0;
    Operations operations{};
    operations.current_process_id = 99;
    operations.enumerate_snapshot = [&](std::vector<ProcessRecord>& out) {
        ++enumerate_calls;
        out = {
            process(0, 1, R"(C:\zero.exe)"),
            process(4, 2, R"(C:\System)"),
            process(99, 3, R"(C:\SaoAuto.exe)"),
            process(88, 4, ""),
            process(30, 300, R"(C:\Tools\Zeta.exe)"),
            process(20, 200, R"(C:\Tools\alpha.exe)"),
            process(10, 100, R"(C:\Tools\Alpha.exe)"),
            process(40, 400, R"(D:\Games\Nova\NovaClient.exe)"),
        };
        return SAO_STATUS_OK;
    };
    operations.query_process = [](std::uint32_t, ProcessRecord&) {
        return SAO_STATUS_ERR_NOT_FOUND;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_OK; };

    {
        Owner owner(compositor.get(), std::move(operations));
        REQUIRE(owner.refresh() == SAO_STATUS_OK);
        Snapshot snapshot{};
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        REQUIRE(enumerate_calls == 1);
        REQUIRE(snapshot.all_processes.size() == 4);
        CHECK(snapshot.all_processes[0].pid == 10);
        CHECK(snapshot.all_processes[1].pid == 20);
        CHECK(snapshot.all_processes[2].pid == 40);
        CHECK(snapshot.all_processes[3].pid == 30);
        CHECK(snapshot.all_processes[0].base_name_utf8 == "Alpha.exe");
        CHECK(snapshot.all_processes[2].base_name_utf8 == "NovaClient.exe");
        CHECK(visible_pids(snapshot) == std::vector<std::uint32_t>{10, 20, 40, 30});

        const int calls_before_filter = enumerate_calls;
        REQUIRE(owner.set_filter(FilterMode::likely_game) == SAO_STATUS_OK);
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        CHECK(enumerate_calls == calls_before_filter);
        CHECK(snapshot.filter == FilterMode::likely_game);
        CHECK(visible_pids(snapshot) == std::vector<std::uint32_t>{40});
        CHECK(sao::launcher::process_selector_panel::is_likely_game_process(
            process(40, 400, R"(D:\Games\Nova\NovaClient.exe)")));
        CHECK_FALSE(sao::launcher::process_selector_panel::is_likely_game_process(
            process(41, 401, R"(C:\Windows\explorer.exe)")));
    }
}

TEST_CASE("process selector rejects PID reuse before injected attach",
          "[launcher][process_selector][identity][focused]") {
    HeadlessCompositor compositor;
    std::vector<ProcessRecord> current{
        process(700, 1000, R"(D:\Games\Astra\Astra-Win64-Shipping.exe)"),
    };
    int enumerate_calls = 0;
    int query_calls = 0;
    int attach_calls = 0;
    Operations operations{};
    operations.current_process_id = 999;
    operations.enumerate_snapshot = [&](std::vector<ProcessRecord>& out) {
        ++enumerate_calls;
        out = current;
        return SAO_STATUS_OK;
    };
    operations.query_process = [&](std::uint32_t pid, ProcessRecord& out) {
        ++query_calls;
        const auto found = std::ranges::find(current, pid, &ProcessRecord::pid);
        if (found == current.end())
            return SAO_STATUS_ERR_PROCESS_GONE;
        out = *found;
        return SAO_STATUS_OK;
    };
    operations.attach = [&](std::uint32_t) {
        ++attach_calls;
        return SAO_STATUS_OK;
    };

    {
        Owner owner(compositor.get(), std::move(operations));
        REQUIRE(owner.open() == SAO_STATUS_OK);
        REQUIRE(enumerate_calls == 1);

        current[0].start_time_100ns = 2000;
        CHECK(owner.attach(ProcessIdentity{700, 1000}) == SAO_STATUS_ERR_PROCESS_GONE);
        CHECK(enumerate_calls == 1);
        CHECK(query_calls == 1);
        CHECK(attach_calls == 0);

        Snapshot snapshot{};
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        CHECK(snapshot.last_status == SAO_STATUS_ERR_PROCESS_GONE);
        CHECK(snapshot.status_text.find("identity changed") != std::string::npos);
        CHECK_FALSE(snapshot.attached_process.has_value());
    }
}

TEST_CASE("process selector reuses one headless panel and emits identity payloads",
          "[launcher][process_selector][panel][payload][focused]") {
    HeadlessCompositor compositor;
    const ProcessRecord target = process(321, 987654321, R"(D:\Games\Photon\PhotonGame.exe)", 44);
    int enumerate_calls = 0;
    int query_calls = 0;
    std::vector<std::uint32_t> attached_pids;
    Operations operations{};
    operations.current_process_id = 999;
    operations.enumerate_snapshot = [&](std::vector<ProcessRecord>& out) {
        ++enumerate_calls;
        out = {target};
        return SAO_STATUS_OK;
    };
    operations.query_process = [&](std::uint32_t pid, ProcessRecord& out) {
        ++query_calls;
        if (pid != target.pid)
            return SAO_STATUS_ERR_PROCESS_GONE;
        out = target;
        return SAO_STATUS_OK;
    };
    operations.attach = [&](std::uint32_t pid) {
        attached_pids.push_back(pid);
        return SAO_STATUS_OK;
    };

    {
        Owner owner(compositor.get(), std::move(operations));
        REQUIRE(owner.open() == SAO_STATUS_OK);
        REQUIRE(enumerate_calls == 1);
        REQUIRE(compositor.layer_count() == 1);

        sao_ui_panel_handle_t first_panel = nullptr;
        REQUIRE(sao_ui_panel_find_by_id(compositor.get(),
                                        sao::launcher::process_selector_panel::kPanelId,
                                        &first_panel) == SAO_STATUS_OK);
        REQUIRE(first_panel != nullptr);

        SaoPanelDescriptor descriptor{};
        REQUIRE(sao_ui_panel_get_descriptor(first_panel, &descriptor) == SAO_STATUS_OK);
        CHECK(std::string_view(descriptor.title_utf8) == "Process Selector");
        CHECK(descriptor.movable);
        CHECK(descriptor.resizable);
        CHECK(descriptor.show_close_button);
        REQUIRE(descriptor.theme_override_json_utf8 != nullptr);
        const std::string theme = descriptor.theme_override_json_utf8;
        CHECK(theme.find("fisheye") == std::string::npos);
        CHECK(Json::parse(theme)["colors"]["APP_ACCENT"] == "#25d7f2");

        Snapshot snapshot{};
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        REQUIRE(snapshot.visible);
        const Json spec = Json::parse(snapshot.rendered_spec_json);
        const auto action =
            find_action_node(spec, sao::launcher::process_selector_panel::kAttachAction);
        REQUIRE(action.has_value());
        REQUIRE(action->contains("payload"));
        const Json& payload = (*action)["payload"];
        CHECK(payload["pid"] == target.pid);
        CHECK(payload["start_time_100ns"] == target.start_time_100ns);

        REQUIRE(owner.dispatch_action(sao::launcher::process_selector_panel::kAttachAction,
                                      payload.dump()) == SAO_STATUS_OK);
        CHECK(enumerate_calls == 1);
        CHECK(query_calls == 1);
        CHECK(attached_pids == std::vector<std::uint32_t>{target.pid});
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        REQUIRE(snapshot.attached_process.has_value());
        CHECK(snapshot.attached_process->identity() == target.identity());
        CHECK(snapshot.status_text.find("Attached PID 321") != std::string::npos);

        REQUIRE(owner.close() == SAO_STATUS_OK);
        REQUIRE(owner.open() == SAO_STATUS_OK);
        CHECK(enumerate_calls == 2);
        CHECK(compositor.layer_count() == 1);
        sao_ui_panel_handle_t reused_panel = nullptr;
        REQUIRE(sao_ui_panel_find_by_id(compositor.get(),
                                        sao::launcher::process_selector_panel::kPanelId,
                                        &reused_panel) == SAO_STATUS_OK);
        CHECK(reused_panel == first_panel);
    }
    CHECK(compositor.layer_count() == 0);
}

TEST_CASE("process selector renders empty and attach failure states",
          "[launcher][process_selector][empty][error][focused]") {
    HeadlessCompositor compositor;
    std::vector<ProcessRecord> current;
    Operations operations{};
    operations.current_process_id = 999;
    operations.enumerate_snapshot = [&](std::vector<ProcessRecord>& out) {
        out = current;
        return SAO_STATUS_OK;
    };
    operations.query_process = [&](std::uint32_t pid, ProcessRecord& out) {
        const auto found = std::ranges::find(current, pid, &ProcessRecord::pid);
        if (found == current.end())
            return SAO_STATUS_ERR_PROCESS_GONE;
        out = *found;
        return SAO_STATUS_OK;
    };
    operations.attach = [](std::uint32_t) { return SAO_STATUS_ERR_ACCESS_DENIED; };

    {
        Owner owner(compositor.get(), std::move(operations));
        REQUIRE(owner.open() == SAO_STATUS_OK);
        Snapshot snapshot{};
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        CHECK(snapshot.visible_processes.empty());
        CHECK(snapshot.rendered_spec_json.find("No queryable processes") != std::string::npos);

        current = {process(55, 5500, R"(D:\Games\Denied\DeniedGame.exe)")};
        REQUIRE(owner.refresh() == SAO_STATUS_OK);
        CHECK(owner.attach({55, 5500}) == SAO_STATUS_ERR_ACCESS_DENIED);
        REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
        CHECK(snapshot.last_status == SAO_STATUS_ERR_ACCESS_DENIED);
        CHECK(snapshot.status_text.find("Attach failed") != std::string::npos);
        CHECK_FALSE(snapshot.attached_process.has_value());
    }
}
