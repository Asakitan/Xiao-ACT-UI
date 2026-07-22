#pragma once

#include "sao/ui/theme.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace sao::ui::detail {

struct PanelResolvedTheme {
    SaoUiThemeId theme_id{SAO_UI_THEME_DARK};
    uint64_t generation{};
    std::array<uint32_t, SAO_UI_COLOR_TOKEN_COUNT> colors{};
    std::array<int32_t, SAO_UI_METRIC_TOKEN_COUNT> metrics{};
};

uint64_t process_theme_generation() noexcept;

inline std::array<std::atomic<int32_t>, SAO_UI_METRIC_TOKEN_COUNT>
    g_panel_metric_test_overrides{};

inline int32_t resolve_panel_metric(SaoUiThemeId theme_id, SaoUiMetricToken metric) noexcept {
    const int32_t test_override =
        g_panel_metric_test_overrides[static_cast<size_t>(metric)].load(std::memory_order_acquire);
    return test_override > 0 ? test_override : sao_ui_theme_resolve_metric(theme_id, metric);
}

inline PanelResolvedTheme resolve_theme(SaoUiThemeId theme_id, uint64_t generation) noexcept {
    PanelResolvedTheme resolved{};
    resolved.theme_id = theme_id;
    resolved.generation = generation;
    for (int32_t token = 0; token < SAO_UI_COLOR_TOKEN_COUNT; ++token) {
        resolved.colors[static_cast<size_t>(token)] = sao_ui_theme_resolve_color(
            theme_id, static_cast<SaoUiColorToken>(token));
    }
    for (int32_t metric = 0; metric < SAO_UI_METRIC_TOKEN_COUNT; ++metric) {
        resolved.metrics[static_cast<size_t>(metric)] = resolve_panel_metric(
            theme_id, static_cast<SaoUiMetricToken>(metric));
    }
    return resolved;
}

inline PanelResolvedTheme resolve_process_theme() noexcept {
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    uint64_t before = 0;
    uint64_t after = 0;
    do {
        before = process_theme_generation();
        (void)sao_ui_theme_get_active_id(&theme_id);
        after = process_theme_generation();
    } while (before != after);
    return resolve_theme(theme_id, after);
}

inline thread_local const PanelResolvedTheme* g_panel_paint_theme = nullptr;

class ScopedPanelPaintTheme final {
  public:
    explicit ScopedPanelPaintTheme(const PanelResolvedTheme& theme) noexcept
        : previous_(g_panel_paint_theme) {
        g_panel_paint_theme = &theme;
    }

    ScopedPanelPaintTheme(const ScopedPanelPaintTheme&) = delete;
    ScopedPanelPaintTheme& operator=(const ScopedPanelPaintTheme&) = delete;

    ~ScopedPanelPaintTheme() {
        g_panel_paint_theme = previous_;
    }

  private:
    const PanelResolvedTheme* previous_{};
};

inline uint32_t panel_theme_color(SaoUiColorToken token) noexcept {
    if (g_panel_paint_theme != nullptr) {
        return g_panel_paint_theme->colors[static_cast<size_t>(token)];
    }
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    (void)sao_ui_theme_get_active_id(&theme_id);
    return sao_ui_theme_resolve_color(theme_id, token);
}

inline int32_t panel_theme_metric(SaoUiMetricToken metric) noexcept {
    if (g_panel_paint_theme != nullptr) {
        return g_panel_paint_theme->metrics[static_cast<size_t>(metric)];
    }
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    (void)sao_ui_theme_get_active_id(&theme_id);
    return resolve_panel_metric(theme_id, metric);
}

} // namespace sao::ui::detail
