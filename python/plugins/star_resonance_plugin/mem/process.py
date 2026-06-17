# -*- coding: utf-8 -*-
"""Star Resonance process wrapper."""

from __future__ import annotations

from typing import Optional

from mem_probe.process import (
    GameProcess,
    GameProcessError,
    MemoryRegion,
    ModuleInfo,
    find_pid_by_name,
    is_admin,
)
from plugins.star_resonance_plugin.sr_config import GAME_PROCESS_NAMES


class StarProcess(GameProcess):
    """Game-named wrapper owned by the Star Resonance plugin."""

    def __init__(self, process_name: Optional[str] = None) -> None:
        super().__init__(
            process_name=process_name,
            process_names=None if process_name else GAME_PROCESS_NAMES,
        )


StarProcessError = GameProcessError

__all__ = [
    "GameProcess",
    "GameProcessError",
    "MemoryRegion",
    "ModuleInfo",
    "StarProcess",
    "StarProcessError",
    "find_pid_by_name",
    "is_admin",
]
