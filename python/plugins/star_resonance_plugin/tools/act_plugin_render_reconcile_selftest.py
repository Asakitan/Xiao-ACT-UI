# -*- coding: utf-8 -*-
# Regression coverage for the reconciling Tk plugin renderer (SpecRenderer).
#
# The renderer must update widgets IN PLACE when the spec structure is unchanged
# (so animated panels don't flicker and interactive buttons aren't destroyed under
# the user's cursor during playback), and only rebuild subtrees that change shape.
# Needs a Tk display (works headless on Windows).

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

try:
    import tkinter as tk
    _root = tk.Tk()
    _root.withdraw()
    _HAS_TK = True
except Exception:
    _HAS_TK = False

from act_platform.ui_spec import UI, normalize_ui_spec

if _HAS_TK:
    from gui_modules.sao_plugin_ui_render import SpecRenderer


def _spec(pct, label, value, note_x):
    return normalize_ui_spec(UI.panel("MIDI 钢琴", [
        UI.section("状态", [
            UI.bar("进度", pct=pct, color="cyan", caption=f"{int(pct*100)}%"),
            UI.kv("曲目", value),
        ], accent="gold"),
        UI.section("控制", [
            UI.row([UI.button(label, "play_pause", style="primary"),
                    UI.button("停止", "stop", style="danger")]),
        ], accent="cyan"),
        UI.canvas(200, 60, [UI.rect(note_x, 0, 6, 60, fill="accent")], bg="body"),
    ]))


@unittest.skipUnless(_HAS_TK, "Tk display unavailable")
class ReconcileTests(unittest.TestCase):
    def setUp(self):
        self.frame = tk.Frame(_root)
        self.fired = []
        self.r = SpecRenderer(self.frame, lambda a, p: self.fired.append((a, p)))

    def tearDown(self):
        try:
            self.frame.destroy()
        except Exception:
            pass

    def _all_widgets(self):
        out = []
        def walk(w):
            for c in w.winfo_children():
                out.append(c)
                walk(c)
        walk(self.frame)
        return out

    def _find(self, cls, **attrs):
        hits = []
        for w in self._all_widgets():
            if w.winfo_class() == cls:
                ok = True
                for k, v in attrs.items():
                    try:
                        if str(w.cget(k)) != str(v):
                            ok = False
                            break
                    except Exception:
                        ok = False
                        break
                if ok:
                    hits.append(w)
        return hits

    def test_same_structure_updates_in_place(self):
        self.r.render(_spec(0.1, "▶ 播放", "song A", 10))
        stop_btns = self._find("Button", text="停止")
        self.assertEqual(len(stop_btns), 1)
        stop_id = str(stop_btns[0])
        play_btns = self._find("Button", text="▶ 播放")
        self.assertEqual(len(play_btns), 1)
        play_id = str(play_btns[0])
        canvases = [w for w in self._all_widgets() if w.winfo_class() == "Canvas"]
        self.assertEqual(len(canvases), 1)
        canvas_id = str(canvases[0])

        # Re-render with same STRUCTURE, different values (progress + label + kv + canvas).
        self.r.render(_spec(0.8, "⏸ 暂停", "song A", 150))

        # The very same widget objects must survive (not destroyed/recreated).
        self.assertEqual(str(self._find("Button", text="停止")[0]), stop_id)
        self.assertEqual([str(w) for w in self._all_widgets() if w.winfo_class() == "Canvas"][0], canvas_id)
        # The play/pause button is the SAME widget, with updated text.
        play_now = self._find("Button", text="⏸ 暂停")
        self.assertEqual(len(play_now), 1)
        self.assertEqual(str(play_now[0]), play_id)
        self.assertFalse(self._find("Button", text="▶ 播放"))  # old label gone (in-place)

    def test_button_fires_after_rerender(self):
        self.r.render(_spec(0.1, "▶ 播放", "x", 10))
        self.r.render(_spec(0.5, "⏸ 暂停", "x", 50))
        stop = self._find("Button", text="停止")[0]
        stop.invoke()
        self.assertIn(("stop", {}), self.fired)

    def test_structural_change_rebuilds(self):
        self.r.render(_spec(0.1, "▶ 播放", "x", 10))
        n_before = len(self._all_widgets())
        # Add an extra section → structure changes; must not raise + must render it.
        spec2 = normalize_ui_spec(UI.panel("MIDI 钢琴", [
            UI.section("状态", [UI.kv("曲目", "x")], accent="gold"),
            UI.section("额外", [UI.text("hello")], accent="cyan"),
        ]))
        self.r.render(spec2)
        self.assertTrue(self._find("Label", text="hello"))

    def test_canvas_redraws_in_place(self):
        self.r.render(_spec(0.1, "p", "x", 10))
        canvas = [w for w in self._all_widgets() if w.winfo_class() == "Canvas"][0]
        cid = str(canvas)
        self.r.render(_spec(0.1, "p", "x", 180))  # only canvas op moves
        canvas2 = [w for w in self._all_widgets() if w.winfo_class() == "Canvas"][0]
        self.assertEqual(str(canvas2), cid)  # same Canvas widget, just redrawn


if __name__ == "__main__":
    unittest.main(verbosity=2)
