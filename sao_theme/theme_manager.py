# -*- coding: utf-8 -*-
"""Panel Theme Registry (split from sao_theme.py — verbatim)."""

# ============================================================
# Panel Theme Registry (ULW overlay themes — RGBA tuples for PIL)
# ============================================================

_PANEL_THEME_REGISTRY: dict[str, dict[str, dict[str, tuple]]] = {}


def register_panel_theme(panel_key: str, theme_name: str, colors: dict[str, tuple]):
    """注册一个面板主题配色 (RGBA tuple dict)。

    面板模块在 import 时调用此函数自注册，例如::

        register_panel_theme('dps', 'light', DPS_THEME_LIGHT)
        register_panel_theme('dps', 'dark', DPS_THEME_DARK)
    """
    bucket = _PANEL_THEME_REGISTRY.setdefault(panel_key, {})
    bucket[theme_name] = colors


def get_panel_theme(panel_key: str, theme_name: str) -> dict[str, tuple]:
    """获取面板主题配色，fallback 到 light。

    若 panel_key 或 theme_name 不存在则返回空 dict（不会报错）。
    """
    bucket = _PANEL_THEME_REGISTRY.get(panel_key, {})
    return dict(bucket.get(theme_name) or bucket.get('light', {}))


def list_panel_theme_names(panel_key: str) -> list[str]:
    """列出某面板所有已注册的主题名称。"""
    return list(_PANEL_THEME_REGISTRY.get(panel_key, {}).keys())
