// Catch2 tests for ai_editor_main_panel (SDK panel API path, Phase A
// wire-up).  Uses a headless compositor + NULL launcher: the panel
// still renders through the SDK layer, only the Send Ping/Hello actions
// short-circuit with an "[error] backend not attached" line — that is
// exercised as a real code path in test_backend_not_attached_error.

#include <catch2/catch_test_macros.hpp>

#include "sao/ai_editor/ai_editor_main_panel.h"
#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ui/compositor.h"

#include <array>
#include <cstring>
#include <string>

namespace {

sao_ui_compositor_handle_t make_headless_compositor() {
    sao_ui_compositor_handle_t compositor = nullptr;
    const sao_status_t status =
        sao_ui_compositor_create(nullptr, nullptr, &compositor);
    if (status != SAO_STATUS_OK)
        return nullptr;
    return compositor;
}

std::string snapshot_output(sao_ai_editor_main_panel_t panel) {
    size_t needed = 0;
    (void)sao_ai_editor_main_panel_snapshot_output_for_testing(
        panel, nullptr, 0, &needed);
    if (needed == 0)
        return {};
    std::string buffer(needed, '\0');
    (void)sao_ai_editor_main_panel_snapshot_output_for_testing(
        panel, buffer.data(), buffer.size(), &needed);
    return buffer;
}

}  // namespace

TEST_CASE("ai_editor_main_panel create rejects null args",
          "[ai_editor][main_panel]") {
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(nullptr, nullptr, &panel) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(panel == nullptr);
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, nullptr) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel create + show + destroy round trip",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_ai_editor_main_panel_show(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_hide(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_tick(panel) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel destroy rejects unknown handle",
          "[ai_editor][main_panel]") {
    sao_ai_editor_main_panel_t bogus =
        reinterpret_cast<sao_ai_editor_main_panel_t>(
            static_cast<uintptr_t>(0xDEADBEEFu));
    REQUIRE(sao_ai_editor_main_panel_try_destroy(bogus) ==
            SAO_AI_EDITOR_ERR_HANDLE_INVALID);
}

TEST_CASE("ai_editor_main_panel output starts empty",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(snapshot_output(panel).empty());
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel dispatches ai.ping with backend not attached",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "ai.ping", nullptr, 0) == SAO_AI_EDITOR_OK);
    const std::string output = snapshot_output(panel);
    REQUIRE(output.find("[error] backend not attached") != std::string::npos);
    REQUIRE(output.find("ping") != std::string::npos);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel dispatches output.clear",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "ai.hello", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(snapshot_output(panel).empty());
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "output.clear", nullptr, 0) == SAO_AI_EDITOR_OK);
    REQUIRE(snapshot_output(panel).empty());
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("ai_editor_main_panel unknown action is recorded as warning",
          "[ai_editor][main_panel]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    REQUIRE(compositor != nullptr);
    sao_ai_editor_main_panel_t panel = nullptr;
    REQUIRE(sao_ai_editor_main_panel_create(compositor, nullptr, &panel) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_main_panel_dispatch_action_for_testing(
                panel, "unknown.thing", nullptr, 0) == SAO_AI_EDITOR_OK);
    const std::string output = snapshot_output(panel);
    REQUIRE(output.find("unknown action id: unknown.thing") !=
            std::string::npos);
    REQUIRE(sao_ai_editor_main_panel_try_destroy(panel) == SAO_AI_EDITOR_OK);
    sao_ui_compositor_destroy(compositor);
}
