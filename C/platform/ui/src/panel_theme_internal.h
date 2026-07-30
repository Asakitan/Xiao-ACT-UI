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

extern std::array<std::atomic<int32_t>, SAO_UI_METRIC_TOKEN_COUNT>
    g_panel_metric_test_overrides;

enum class PanelSemanticColorToken : int32_t {
    DialogNeutral = 0,
    Count,
};

int32_t resolve_panel_metric(SaoUiThemeId theme_id, SaoUiMetricToken metric) noexcept;
PanelResolvedTheme resolve_theme(SaoUiThemeId theme_id, uint64_t generation) noexcept;
PanelResolvedTheme resolve_process_theme() noexcept;

extern thread_local const PanelResolvedTheme* g_panel_paint_theme;

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

uint32_t panel_theme_color(SaoUiColorToken token) noexcept;
uint32_t panel_theme_color(PanelSemanticColorToken token) noexcept;
int32_t panel_theme_metric(SaoUiMetricToken metric) noexcept;

} // namespace sao::ui::detail
