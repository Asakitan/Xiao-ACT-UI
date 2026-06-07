# -*- coding: utf-8 -*-
"""mp_api — 引擎装配 facade + 自动识别增强 + UI 辅助。

在引擎三件套（mp_parser / mp_mapper / mp_player）之上做两件事：
  1) autodetect_mode_system(): 按覆盖率自动在 60 键(classic) / 88 键(extended) 间择优
     （60/88 仍保留手动切换按钮）。
  2) 一些纯展示辅助（曲库扫描 / 时间格式化 / 状态摘要），供 plugin.py 渲染面板。
"""

from __future__ import annotations

import os
from typing import List, Tuple

_MIDI_EXTS = (".mid", ".midi")


# ── 曲库扫描 ────────────────────────────────────────────────────────────────
def list_midi_files(dirs) -> List[Tuple[str, str]]:
    """扫描给定目录（不递归子目录的子目录，仅一层 + 直接子文件夹）里的 MIDI 文件。

    返回 [(显示名, 绝对路径), ...]，按显示名排序、去重。
    """
    seen = set()
    out: List[Tuple[str, str]] = []
    for d in dirs or []:
        if not d or not os.path.isdir(d):
            continue
        for root, _subdirs, files in os.walk(d):
            for fn in files:
                if fn.lower().endswith(_MIDI_EXTS):
                    full = os.path.abspath(os.path.join(root, fn))
                    if full in seen:
                        continue
                    seen.add(full)
                    out.append((fn, full))
    out.sort(key=lambda x: x[0].lower())
    return out


def fmt_time(sec) -> str:
    try:
        sec = max(0, int(round(float(sec))))
    except Exception:
        return "00:00"
    return f"{sec // 60:02d}:{sec % 60:02d}"


# ── 自动识别：60/88 + 移调 ─────────────────────────────────────────────────
def _coverage_for(mapper, notes, system) -> Tuple[float, int]:
    """在不改变 mapper 现状的前提下，试算某模式系统的最佳移调与覆盖率。"""
    old_sys, old_t = mapper.mode_system, mapper.transpose
    try:
        mapper.set_mode_system(system)
        t = mapper.suggest_transpose(notes)
        mapper.set_transpose(t)
        cov = mapper.analyze_coverage(notes)["coverage"]
    finally:
        mapper.set_mode_system(old_sys)
        mapper.set_transpose(old_t)
    return cov, t


def autodetect_mode_system(player, apply: bool = True, margin: float = 0.01) -> dict:
    """按覆盖率自动在 classic(60) / extended(88) 间择优，并刷新移调。

    选择策略：仅当 extended 覆盖率明显高于 classic（> margin）才用 88 键，
    否则用 60 键（更少模式切换、更贴近常见的 60 键琴）。并列偏 classic。

    apply=True 时把结果写回 player（set_mode_system + 重建自动映射 + 清零手动移调）。
    返回 {'system', 'classic_cov', 'ext_cov', 'transpose'}。
    """
    notes = [n.note for n in getattr(player.parser, "notes", []) or []]
    if not notes:
        return {"system": player.get_mode_system(), "classic_cov": 0.0,
                "ext_cov": 0.0, "transpose": 0}

    classic_cov, _ct = _coverage_for(player.mapper, notes, "classic")
    ext_cov, _et = _coverage_for(player.mapper, notes, "extended")

    chosen = "extended" if ext_cov > classic_cov + margin else "classic"

    if apply:
        player.set_mode_system(chosen)
        player._user_transpose = 0
        # 重建自动八度映射（_note_remap）
        try:
            player._analyze_and_setup_mapping()
        except Exception:
            pass

    return {
        "system": chosen,
        "classic_cov": round(classic_cov, 4),
        "ext_cov": round(ext_cov, 4),
        "transpose": getattr(player, "_octave_offset", 0) * 12,
    }


def set_mode_system(player, system: str):
    """手动切 60/88：切系统并重建自动映射、清零手动移调（保持与自动识别一致的状态）。"""
    player.set_mode_system(system)
    player._user_transpose = 0
    if getattr(player.parser, "notes", None):
        try:
            player._analyze_and_setup_mapping()
        except Exception:
            pass


# ── 状态摘要（供 UI）────────────────────────────────────────────────────────
def coverage_pct(player) -> float:
    notes = [n.note for n in getattr(player.parser, "notes", []) or []]
    if not notes:
        return 0.0
    try:
        return player.mapper.analyze_coverage(notes)["coverage"] * 100.0
    except Exception:
        return 0.0


def build_status(player) -> dict:
    """汇总播放/曲目/模式信息，plugin.py 据此渲染面板。"""
    st = player.state
    parser = player.parser
    total = getattr(parser, "total_time", 0.0) or 0.0
    cur = getattr(st, "current_time", 0.0) or 0.0
    playing = bool(getattr(st, "is_playing", False))
    paused = bool(getattr(st, "is_paused", False))
    if not playing:
        phase = "stopped"
    elif paused:
        phase = "paused"
    else:
        phase = "playing"
    return {
        "phase": phase,
        "current": cur,
        "total": total,
        "pct": (cur / total) if total > 0 else 0.0,
        "bpm": getattr(parser, "bpm", 0) or 0,
        "notes": len(getattr(parser, "notes", []) or []),
        "coverage": coverage_pct(player),
        "mode_system": player.get_mode_system(),
        "octave_offset": getattr(player, "_octave_offset", 0) * 12,
        "user_transpose": getattr(player, "_user_transpose", 0),
        "speed": getattr(st, "speed", 1.0),
        "legato": player.get_legato_overlap() if hasattr(player, "get_legato_overlap") else False,
    }
