# -*- coding: utf-8 -*-
"""prerender_bossraid_panels - 离屏构造 BossRaid 简单/详细面板, 切到机制页,
导入示例档案, 截图保存到 temp/prerender/。用来在不进游戏的前提下确认 notes
全文渲染、卡片布局、编辑表单不报错。

run: python -m tools.prerender_bossraid_panels
out: temp/prerender/bossraid_simple_mech.png / bossraid_detail_mech.png
"""
from __future__ import annotations

import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

import tkinter as tk

EXAMPLE = os.path.join(_ROOT, "assets", "boss_raids", "13023_噩梦P3_机制示例.json")
OUT_DIR = os.path.join(_ROOT, "temp", "prerender")


class _Settings:
    """In-memory settings double with the get/set/save the editors expect."""

    def __init__(self):
        self._d = {}

    def get(self, k, default=None):
        v = self._d.get(k, default)
        return default if v is None else v

    def set(self, k, v):
        self._d[k] = v

    def save(self):
        pass


def _seed_settings():
    from plugins.star_resonance_plugin.engines.boss_raid_engine import (
        load_boss_raid_config, save_boss_raid_config, normalize_profile)
    s = _Settings()
    payload = json.load(open(EXAMPLE, encoding="utf-8"))
    prof = normalize_profile(payload["profile"])
    cfg = load_boss_raid_config(s)
    cfg["profiles"] = [prof]
    cfg["active_profile_id"] = prof["id"]
    save_boss_raid_config(s, cfg)
    return s


def _mech_api(settings):
    from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms

    def _load(scene_key=None, boss_base_id=None):
        return bms.build_mechanics_state(settings, None, None,
                                         scene_key=scene_key,
                                         boss_base_id=boss_base_id)

    return {
        "load": _load,
        "save_mech": lambda m: bms.upsert_mechanic(settings, m),
        "delete_mech": lambda mid: bms.delete_mechanic(settings, mid),
        "test": lambda m, k: {"ok": True},
        "set_master": lambda f: bms.set_mechanics_master(settings, f),
        "create_from_skill": lambda sid, nm="", dur=None:
            bms.create_mechanic_from_skill(settings, sid, nm, dur),
        "bind": lambda mid, sid: bms.bind_skill_to_mechanic(settings, mid, sid),
        "unbind": lambda mid, sid: bms.unbind_skill_from_mechanic(settings, mid, sid),
        "search_catalog": bms.search_skill_catalog,
    }


def _grab(win, path):
    """Screenshot the toplevel by its on-screen bbox (PIL ImageGrab, all-screen)."""
    try:
        from PIL import ImageGrab
    except Exception as e:
        print(f"[prerender] PIL unavailable ({e}); skip screenshot {path}")
        return False
    win.update_idletasks()
    win.update()
    time.sleep(0.4)
    win.update()
    x, y = win.winfo_rootx(), win.winfo_rooty()
    w, h = win.winfo_width(), win.winfo_height()
    if w < 10 or h < 10:
        print(f"[prerender] window too small {w}x{h}; skip {path}")
        return False
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h))
    img.save(path)
    print(f"[prerender] saved {path}  ({w}x{h})")
    return True


def _audit_mechanics_state(settings):
    """Headless contract audit (no Tk): every card renders notes + chips?"""
    from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
    st = bms.build_mechanics_state(settings, None, None)
    problems = []
    mechs = st.get("mechanics") or []
    for m in mechs:
        det = m.get("detect") or {}
        names = m.get("skill_names") or {}
        ids = list(det.get("skill_ids") or []) + list(det.get("buff_ids") or [])
        if not ids:
            problems.append(f"{m.get('name')}: 无检测id")
        unnamed = [c for c in ids if not (names.get(str(c)) or "").strip()]
        if unnamed:
            problems.append(f"{m.get('name')}: 检测id无名 {unnamed}")
        notes = (m.get("notes") or "").strip()
        if not notes:
            problems.append(f"{m.get('name')}: notes 为空")
        if not (m.get("summary") or {}).get("bound_count"):
            problems.append(f"{m.get('name')}: summary.bound_count=0")
    print(f"[audit] {len(mechs)} 机制, master={st.get('master')}")
    if problems:
        for p in problems:
            print(f"[audit] ✗ {p}")
    else:
        print("[audit] ✓ 全部机制: 检测id有名 + notes非空 + bound_count>0")
    return not problems


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    settings = _seed_settings()
    ok_audit = _audit_mechanics_state(settings)

    root = tk.Tk()
    root.geometry("1600x1000+0+0")
    root.update_idletasks()

    from plugins.star_resonance_plugin.panels.sao_gui_bossraid import BossRaidPanel
    from plugins.star_resonance_plugin.panels.sao_gui_profile_editors import BossRaidDetailPanel

    api = _mech_api(settings)
    load_fn = lambda: __import__("engines.boss_raid_engine",
                                 fromlist=["load_boss_raid_config"]
                                 ).load_boss_raid_config(settings)
    save_fn = lambda cfg: __import__("engines.boss_raid_engine",
                                     fromlist=["save_boss_raid_config"]
                                     ).save_boss_raid_config(settings, cfg)

    results = {}

    # ── simple panel ──
    simple = BossRaidPanel(
        master=root, load_fn=load_fn, save_fn=save_fn,
        engine_ref=lambda: None,
        on_toggle=lambda *_: None, on_start=lambda *_: None,
        on_next=lambda *_: None, on_reset=lambda: None,
        mechanics_api=api)
    try:
        simple.show()
        simple._switch_tab("mechanics")
        # 展开第一张卡的编辑表单, 验证 notes Text / buff chips / 序列 UI 不报错
        mechs = (api["load"]().get("mechanics") or [])
        if mechs:
            simple._mech_edit(str(mechs[0]["id"]))
        root.update()
        time.sleep(0.3)
        root.update()
        results["simple"] = _grab(simple._win,
                                  os.path.join(OUT_DIR, "bossraid_simple_mech.png"))
    except Exception as e:
        import traceback
        traceback.print_exc()
        results["simple"] = False

    # ── detail panel ──
    detail = BossRaidDetailPanel(
        master=root, load_fn=load_fn, save_fn=save_fn,
        author_fn=None, mechanics_api=api)
    try:
        detail.show() if hasattr(detail, "show") else detail.toggle()
        root.update()
        time.sleep(0.3)
        root.update()
        win = getattr(detail, "_win", None) or getattr(detail, "_window", None)
        if win is not None:
            results["detail"] = _grab(win,
                                      os.path.join(OUT_DIR, "bossraid_detail_mech.png"))
        else:
            print("[prerender] detail panel has no _win attr")
            results["detail"] = False
    except Exception as e:
        import traceback
        traceback.print_exc()
        results["detail"] = False

    print(f"[prerender] audit_ok={ok_audit} screenshots={results}")
    root.after(200, root.destroy)
    try:
        root.mainloop()
    except Exception:
        pass
    return 0 if ok_audit else 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main())
