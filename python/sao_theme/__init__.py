# -*- coding: utf-8 -*-
"""SAO Utils 风格 UI 组件 (package).

在 tkinter 中重现 SAO 风格的:
  - PopUpMenu (全屏弹出菜单 + 半透明遮罩)
  - MenuBar (圆形图标按钮条, 下落动画, 滚轮切换)
  - LeftInfo (左侧用户信息面板, 展开动画)
  - ChildBar (右侧子菜单, 下拉动画)
    - Generic Alert (对话框, 宽度展开动画, 文字渐现)
  - HP Bar (血条进度条, 绿/黄/红渐变)
  - LinkStart (LINK START 粒子入场动画)

This is a pure physical split of the former monolithic ``sao_theme.py``
into sibling submodules. Re-exports below preserve 100% backward
compatibility: ``from sao_theme import SAOColors, SAOPopUpMenu, ...`` and
``import sao_theme; sao_theme.SAOColors`` keep working unchanged.

Submodules are imported in topological order (low-level first) so that
sibling absolute imports (``from sao_theme.colors import SAOColors`` etc.)
never hit a half-initialized package.
"""

# ── 底层: 配色 / 工具 / theme registry / 动画引擎 ──
from sao_theme.colors import SAOColors
from sao_theme.utils import (
    _aa_circle_icon,
    _make_aa_icon_button,
    _hex_to_rgba,
    ease_out,
    ease_in,
    ease_in_out,
    lerp,
    hex_to_rgb,
    rgb_to_hex,
    _strip_alpha,
    lerp_color,
)
from sao_theme.theme_manager import (
    _PANEL_THEME_REGISTRY,
    register_panel_theme,
    get_panel_theme,
    list_panel_theme_names,
)
from sao_theme.animator import Animator

# ── 中层: 圆按钮 / 菜单栏 / 左信息 / 子菜单 ──
from sao_theme.circle_button import SAOCircleButton
from sao_theme.menu_bar import SAOMenuBar
from sao_theme.left_info import SAOLeftInfo
from sao_theme.child_bar import SAOChildBar

# ── 高层: 弹出菜单 / 对话框 / HP 条 / LinkStart / 文件选择器 / 通用控件 ──
from sao_theme.popup_menu import SAOPopUpMenu
from sao_theme.dialogs import (
    SAODialog,
    SAOLeaderboardDialog,
    _clip_reveal,
    _close_alert,
)
from sao_theme.hp_bar import SAOHPBar
from sao_theme.link_start import SAOLinkStart
from sao_theme.file_picker import SAOFilePicker
from sao_theme.widgets import (
    SAOButton,
    SAOProgressBar,
    SAOStatusPill,
    SAOResizeGrip,
    SAOSeparator,
    SAOTitleBar,
)

# v2.3.x: GPU-native popup menu rewrite. Overrides the legacy
# `SAOPopUpMenu` class defined above with the new
# `ui_gpu.popup.SAOPopUpMenu` so all callers automatically pick up
# the new implementation via `from sao_theme import SAOPopUpMenu`.
# Legacy SAOMenuBar / SAOLeftInfo / SAOChildBar / SAOCircleButton
# classes remain defined above for binary backward-compat but are no
# longer instantiated by SAOPopUpMenu.
try:
    from ui_gpu import SAOPopUpMenu  # type: ignore[assignment]  # noqa: F811
except Exception as _e:
    import warnings as _warnings
    _warnings.warn(
        f'ui_gpu.SAOPopUpMenu unavailable, falling back to legacy: {_e}',
        RuntimeWarning,
        stacklevel=2,
    )


__all__ = [
    # colors
    'SAOColors',
    # utils
    '_aa_circle_icon', '_make_aa_icon_button', '_hex_to_rgba',
    'ease_out', 'ease_in', 'ease_in_out', 'lerp',
    'hex_to_rgb', 'rgb_to_hex', '_strip_alpha', 'lerp_color',
    # theme registry
    '_PANEL_THEME_REGISTRY', 'register_panel_theme',
    'get_panel_theme', 'list_panel_theme_names',
    # animator
    'Animator',
    # widgets / containers
    'SAOCircleButton', 'SAOMenuBar', 'SAOLeftInfo', 'SAOChildBar',
    'SAOPopUpMenu',
    'SAODialog', 'SAOLeaderboardDialog', '_clip_reveal', '_close_alert',
    'SAOHPBar', 'SAOLinkStart', 'SAOFilePicker',
    'SAOButton', 'SAOProgressBar', 'SAOStatusPill', 'SAOResizeGrip',
    'SAOSeparator', 'SAOTitleBar',
]
