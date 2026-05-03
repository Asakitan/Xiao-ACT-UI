"""Popup animation state."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class PopupState:
    # Lifecycle
    is_open: bool = False
    fade_alpha: float = 0.0       # 0..1 master alpha for the GPU layer
    open_t0: float = 0.0          # time.monotonic() baseline for animations

    # Menu bar (fisheye column)
    menu_items: List[Dict] = field(default_factory=list)   # [{icon, name, can_active}, ...]
    btn_size: List[float] = field(default_factory=list)    # current rendered size per button
    btn_hover_t: List[float] = field(default_factory=list) # 0..1 hover lerp per button
    hover_btn_idx: Optional[int] = None
    active_menu_idx: Optional[int] = None

    # Child bar
    child_menus: Dict[str, List[Dict]] = field(default_factory=dict)
    child_rows: List[Dict] = field(default_factory=list)   # current active menu items
    pending_child_rows: List[Dict] = field(default_factory=list)
    row_hover_t: List[float] = field(default_factory=list)
    row_anim_w: List[int] = field(default_factory=list)    # slide-in width per row
    row_anim_t0: float = 0.0
    child_fade_t: float = 1.0                              # 1=hidden, 0=fully visible
    child_phase: str = 'idle'                              # idle|fadeout|fadein
    hover_row_idx: Optional[int] = None
    line_h: int = 0                                        # connecting vertical line height

    # HUD
    hud_phase: float = 0.0                                 # accumulated phase for scan/dot anim
    hud_dx: int = 0
    hud_dy: int = 0
