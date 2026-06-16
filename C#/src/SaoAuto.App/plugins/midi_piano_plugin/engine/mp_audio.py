# -*- coding: utf-8 -*-
"""mp_audio — MIDI 试听（在本机扬声器播放，听演奏效果，不驱动游戏）。

MIDI 试听后端链 FluidSynth → WinMCI → pygame 中，FluidSynth 需额外 DLL + 30MB
SoundFont，这里**只取无需额外资源的两条后端**：

  1. WinMCI —— 调用 Windows 自带 MIDI 合成器（winmm.mciSendStringW），零依赖、零音色库；
  2. pygame —— SAO-UI 已自带的依赖，作为 WinMCI 不可用时的后备。

试听播放的是**原始 MIDI 文件**（曲子本来的样子）。一切操作都防御性包裹，绝不让试听
异常波及插件主流程。
"""

from __future__ import annotations

import os
import threading


class MidiAudition:
    _ALIAS = "mp_piano_audition"

    def __init__(self):
        self._backend = None          # 'winmci' | 'pygame' | None
        self._winmm = None
        self._opened = False
        self._pg_ready = False
        self._lock = threading.RLock()
        self._cur_path = ""
        self._detect()

    # ── 后端探测 ──────────────────────────────────────────────────────────
    def _detect(self):
        try:
            import ctypes
            self._winmm = ctypes.windll.winmm
            self._backend = "winmci"
            return
        except Exception:
            self._winmm = None
        try:
            import pygame  # noqa: F401
            self._backend = "pygame"
        except Exception:
            self._backend = None

    def backend_name(self) -> str:
        return {"winmci": "Windows 合成器", "pygame": "pygame"}.get(self._backend, "不可用")

    def available(self) -> bool:
        return self._backend is not None

    # ── WinMCI ────────────────────────────────────────────────────────────
    def _mci(self, command: str):
        import ctypes
        buf = ctypes.create_unicode_buffer(256)
        rc = self._winmm.mciSendStringW(command, buf, 256, 0)
        return rc, buf.value

    def _winmci_play(self, path: str) -> bool:
        self._winmci_close()
        # 用引号包裹路径以兼容空格/中文
        rc, _ = self._mci(f'open "{path}" type sequencer alias {self._ALIAS}')
        if rc != 0:
            return False
        self._opened = True
        rc, _ = self._mci(f"play {self._ALIAS}")
        return rc == 0

    def _winmci_close(self):
        if self._opened:
            try:
                self._mci(f"close {self._ALIAS}")
            except Exception:
                pass
            self._opened = False

    def _winmci_is_playing(self) -> bool:
        if not self._opened:
            return False
        try:
            _rc, val = self._mci(f"status {self._ALIAS} mode")
            return val.strip().lower() == "playing"
        except Exception:
            return False

    # ── pygame ────────────────────────────────────────────────────────────
    def _pygame_play(self, path: str) -> bool:
        try:
            import pygame
            if not self._pg_ready:
                pygame.mixer.init()
                self._pg_ready = True
            pygame.mixer.music.load(path)
            pygame.mixer.music.play()
            return True
        except Exception:
            return False

    def _pygame_close(self):
        if self._pg_ready:
            try:
                import pygame
                pygame.mixer.music.stop()
            except Exception:
                pass

    def _pygame_is_playing(self) -> bool:
        if not self._pg_ready:
            return False
        try:
            import pygame
            return bool(pygame.mixer.music.get_busy())
        except Exception:
            return False

    # ── 对外 API ──────────────────────────────────────────────────────────
    def play(self, path: str) -> bool:
        if not path or not os.path.isfile(path) or not self._backend:
            return False
        with self._lock:
            self._cur_path = path
            if self._backend == "winmci":
                if self._winmci_play(path):
                    return True
                # WinMCI 失败 → 尝试 pygame
                try:
                    import pygame  # noqa
                    self._backend = "pygame"
                except Exception:
                    return False
            return self._pygame_play(path)

    def stop(self):
        with self._lock:
            self._winmci_close()
            self._pygame_close()

    def is_playing(self) -> bool:
        with self._lock:
            if self._backend == "winmci":
                return self._winmci_is_playing()
            return self._pygame_is_playing()


__all__ = ["MidiAudition"]
