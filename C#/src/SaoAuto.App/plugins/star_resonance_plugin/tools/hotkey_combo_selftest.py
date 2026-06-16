# -*- coding: utf-8 -*-
"""Regression coverage for hotkey combo parsing / matching / dispatch.

Covers ``config.parse_hotkey`` / ``normalize_hotkey`` / ``hotkey_matches``
(子集匹配) / ``select_hotkey_match`` (最特异优先) — the shared logic behind
SAOHotkeyManager, the sao_webview dual-path listener and the headless
automation listener — plus SAOHotkeyManager dispatch behaviour (built-in vs
plugin, plain F-key vs CTRL combo coexistence, modifier retention after fire,
stale-modifier immunity, partial settings dict not killing built-ins) and the
shipped default keymap being conflict-free.

Run from the repo root: ``python -m tools.hotkey_combo_selftest``
"""

from __future__ import annotations

import _bootstrap  # noqa: F401

import re
import os
import unittest

import config as _config
from config import (
    DEFAULT_HOTKEYS,
    HOTKEY_FKEY_VK,
    hotkey_matches,
    normalize_hotkey,
    parse_hotkey,
    select_hotkey_match,
)

VK_F5 = HOTKEY_FKEY_VK["F5"]
VK_F8 = HOTKEY_FKEY_VK["F8"]
VK_F10 = HOTKEY_FKEY_VK["F10"]
VK_F12 = HOTKEY_FKEY_VK["F12"]
VK_CTRL_L = 162
VK_CTRL_GENERIC = 17
VK_SHIFT_L = 160


class _NoLiveModsMixin(unittest.TestCase):
    """强制 hotkey_mods_down 走 pressed 集合推断 (测试机的真实键盘状态
    不能影响断言)。"""

    def setUp(self):
        self._gaks = _config._HOTKEY_GAKS
        _config._HOTKEY_GAKS = None

    def tearDown(self):
        _config._HOTKEY_GAKS = self._gaks


class ParseHotkeyTests(unittest.TestCase):
    def test_plain_fkeys(self) -> None:
        self.assertEqual(parse_hotkey("F5"), {"vk": VK_F5, "mods": frozenset()})
        self.assertEqual(parse_hotkey("f12"), {"vk": VK_F12, "mods": frozenset()})

    def test_combos(self) -> None:
        self.assertEqual(parse_hotkey("CTRL+F8"),
                         {"vk": VK_F8, "mods": frozenset({"CTRL"})})
        self.assertEqual(parse_hotkey("Ctrl+Alt+F12"),
                         {"vk": VK_F12, "mods": frozenset({"CTRL", "ALT"})})
        # 别名 + 空白容忍
        self.assertEqual(parse_hotkey(" control + F5 "),
                         {"vk": VK_F5, "mods": frozenset({"CTRL"})})

    def test_dict_forms(self) -> None:
        # 旧式自定义 VK dict (不限 F1-F12)
        self.assertEqual(parse_hotkey({"vk": 65}), {"vk": 65, "mods": frozenset()})
        self.assertEqual(parse_hotkey({"vk": 65, "mods": ["ctrl"]}),
                         {"vk": 65, "mods": frozenset({"CTRL"})})
        # 插件映射的 {'key': ...} 形式
        self.assertEqual(parse_hotkey({"key": "CTRL+F8"}),
                         {"vk": VK_F8, "mods": frozenset({"CTRL"})})
        self.assertEqual(parse_hotkey({"name": "F6"}),
                         {"vk": HOTKEY_FKEY_VK["F6"], "mods": frozenset()})

    def test_invalid(self) -> None:
        for bad in ("", None, "CTRL", "CTRL+A", "F13", "F5+F6", "CTRL+",
                    {"vk": "x"}, {"key": ""}, {"vk": 65, "mods": ["hyper"]}, 5):
            self.assertIsNone(parse_hotkey(bad), repr(bad))


class NormalizeHotkeyTests(unittest.TestCase):
    def test_canonical_spelling(self) -> None:
        self.assertEqual(normalize_hotkey("F5"), "F5")
        self.assertEqual(normalize_hotkey(" control + f8 "), "CTRL+F8")
        self.assertEqual(normalize_hotkey("MENU+F1"), "ALT+F1")
        # 修饰键固定 CTRL,ALT,SHIFT 序 (与拼写顺序无关)
        self.assertEqual(normalize_hotkey("shift+ctrl+F5"), "CTRL+SHIFT+F5")

    def test_non_canonicalizable(self) -> None:
        self.assertIsNone(normalize_hotkey("CTRL+A"))
        self.assertIsNone(normalize_hotkey(""))
        # 自定义 VK (非 F1-F12) 没有规范名
        self.assertIsNone(normalize_hotkey({"vk": 65}))


class HotkeyMatchesTests(_NoLiveModsMixin):
    def test_subset_semantics(self) -> None:
        plain = parse_hotkey("F5")
        combo = parse_hotkey("CTRL+F5")
        # 只按 F5: plain 命中, combo 不命中 (缺 CTRL)
        self.assertTrue(hotkey_matches(plain, {VK_F5}))
        self.assertFalse(hotkey_matches(combo, {VK_F5}))
        # Ctrl+F5: 两个都"命中" (子集), 调度层由 select 取最特异
        self.assertTrue(hotkey_matches(plain, {VK_F5, VK_CTRL_L}))
        self.assertTrue(hotkey_matches(combo, {VK_F5, VK_CTRL_L}))
        # 通用 VK_CONTROL (17) 也认
        self.assertTrue(hotkey_matches(combo, {VK_F5, VK_CTRL_GENERIC}))
        # 多余修饰键不挡: Shift+Ctrl+F5 仍命中 CTRL+F5
        self.assertTrue(hotkey_matches(combo, {VK_F5, VK_CTRL_L, VK_SHIFT_L}))

    def test_mods_down_override(self) -> None:
        # 轮询路径: pressed 只含主键, 修饰键状态由 mods_down 提供
        combo = parse_hotkey("CTRL+F8")
        self.assertTrue(hotkey_matches(combo, {VK_F8}, mods_down={"CTRL"}))
        self.assertFalse(hotkey_matches(combo, {VK_F8}, mods_down=set()))

    def test_main_key_required(self) -> None:
        self.assertFalse(hotkey_matches(parse_hotkey("F5"), set()))
        self.assertFalse(hotkey_matches(None, {VK_F5}))


class SelectHotkeyMatchTests(_NoLiveModsMixin):
    def _cands(self):
        return [
            (parse_hotkey("F5"), "plain"),
            (parse_hotkey("CTRL+F5"), "combo"),
        ]

    def test_most_specific_wins(self) -> None:
        self.assertEqual(select_hotkey_match(self._cands(), {VK_F5}), "plain")
        self.assertEqual(
            select_hotkey_match(self._cands(), {VK_F5, VK_CTRL_L}), "combo")

    def test_irrelevant_modifier_falls_back_to_plain(self) -> None:
        # 急停场景: 自动化注入 SHIFT 时, 纯 F 键绑定必须照常触发
        self.assertEqual(
            select_hotkey_match(self._cands(), {VK_F5, VK_SHIFT_L}), "plain")

    def test_tie_takes_first(self) -> None:
        cands = [(parse_hotkey("F5"), "builtin"), (parse_hotkey("F5"), "plugin")]
        self.assertEqual(select_hotkey_match(cands, {VK_F5}), "builtin")

    def test_no_match(self) -> None:
        self.assertIsNone(select_hotkey_match(self._cands(), {VK_F8}))


class _Settings:
    def __init__(self, hotkeys):
        self._hotkeys = hotkeys

    def get(self, key, default=None):
        return self._hotkeys if key == "hotkeys" else default


class HotkeyManagerDispatchTests(_NoLiveModsMixin):
    """SAOHotkeyManager._check_combos 行为 (无 pynput 依赖, 直接喂按键集)。"""

    def _mgr(self, saved, actions, provider=None):
        from gui_modules.sao_hotkey_manager import SAOHotkeyManager
        mgr = SAOHotkeyManager.__new__(SAOHotkeyManager)  # 不启动监听线程
        mgr.settings = _Settings(saved)
        mgr.actions = actions
        mgr.hotkey_provider = provider
        mgr._pressed_keys = set()
        return mgr

    def test_builtin_and_plugin_combo_coexist(self) -> None:
        fired = []
        mgr = self._mgr(
            {"toggle_auto_dodge": "F12"},
            {"toggle_auto_dodge": lambda: fired.append("dodge")},
            provider=lambda: {"plugin.hs.toggle": {
                "key": "CTRL+F12", "callback": lambda: fired.append("hs")}},
        )
        mgr._pressed_keys = {VK_F12}
        mgr._check_combos()
        self.assertEqual(fired, ["dodge"])
        mgr._pressed_keys = {VK_F12, VK_CTRL_L}
        mgr._check_combos()
        self.assertEqual(fired, ["dodge", "hs"])

    def test_panic_fires_while_shift_injected(self) -> None:
        # 躲避自动化按住 SHIFT 冲刺时, F12 急停必须照常触发。
        fired = []
        mgr = self._mgr({}, {"toggle_auto_dodge": lambda: fired.append("dodge")})
        mgr._pressed_keys = {VK_F12, VK_SHIFT_L}
        mgr._check_combos()
        self.assertEqual(fired, ["dodge"])

    def test_stale_modifier_does_not_hijack(self) -> None:
        # LL hook 丢 key-up 时 pressed 里残留脏 Ctrl; GetAsyncKeyState 实测
        # 说没按 → 纯 F12 触发, CTRL+F12 不被劫持。
        _config._HOTKEY_GAKS = lambda vk: 0
        fired = []
        mgr = self._mgr(
            {"toggle_auto_dodge": "F12"},
            {"toggle_auto_dodge": lambda: fired.append("dodge")},
            provider=lambda: {"plugin.hs.toggle": {
                "key": "CTRL+F12", "callback": lambda: fired.append("hs")}},
        )
        mgr._pressed_keys = {VK_F12, VK_CTRL_L}  # 脏 Ctrl 残留
        mgr._check_combos()
        self.assertEqual(fired, ["dodge"])

    def test_modifiers_survive_fire(self) -> None:
        # 触发后修饰键保留在 pressed 集合里 (pynput 不会重发按住的 Ctrl),
        # 紧接着的第二个 Ctrl+F 组合才能命中。
        fired = []
        mgr = self._mgr({}, {}, provider=lambda: {
            "plugin.m.play": {"key": "CTRL+F8", "callback": lambda: fired.append("play")},
            "plugin.m.stop": {"key": "CTRL+F10", "callback": lambda: fired.append("stop")},
        })
        mgr._pressed_keys = {VK_CTRL_L, VK_F8}
        mgr._check_combos()
        self.assertEqual(fired, ["play"])
        self.assertEqual(mgr._pressed_keys, {VK_CTRL_L})
        mgr._pressed_keys.add(VK_F10)
        mgr._check_combos()
        self.assertEqual(fired, ["play", "stop"])

    def test_partial_saved_dict_keeps_builtin_defaults(self) -> None:
        # set_hotkey 可能写出只含插件项的部分 dict — 内置默认必须 merge
        # 回来, 不能被部分 dict 杀掉。
        fired = []
        mgr = self._mgr(
            {"plugin.m.play": "CTRL+F1"},
            {"toggle_recognition": lambda: fired.append("recog")},
        )
        mgr._pressed_keys = {VK_F5}
        mgr._check_combos()
        self.assertEqual(fired, ["recog"])

    def test_shadowing_gone_for_distinct_combos(self) -> None:
        # 审计修复的核心: 插件键挪到 CTRL+ 组合后不再被同号内置 F 键遮蔽。
        fired = []
        mgr = self._mgr(
            dict(DEFAULT_HOTKEYS),
            {"boss_raid_next_phase": lambda: fired.append("phase")},
            provider=lambda: {"plugin.midi.play_pause": {
                "key": "CTRL+F8", "callback": lambda: fired.append("midi")}},
        )
        mgr._pressed_keys = {VK_F8, VK_CTRL_L}
        mgr._check_combos()
        self.assertEqual(fired, ["midi"])


class ShippedDefaultsConflictFreeTests(unittest.TestCase):
    def test_builtin_defaults_unique_and_parseable(self) -> None:
        seen = {}
        for action, key in DEFAULT_HOTKEYS.items():
            self.assertIsNotNone(parse_hotkey(key), f"{action}={key}")
            canon = normalize_hotkey(key)
            self.assertNotIn(canon, seen,
                             f"{action} duplicates {seen.get(canon)}")
            seen[canon] = action

    def test_shipped_plugin_defaults_avoid_builtins(self) -> None:
        # 静态扫描内置插件的 register_hotkey 默认键: 必须可规范化且不与
        # 内置 DEFAULT_HOTKEYS、也不互相撞键。
        plugins_dir = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "plugins")
        pattern = re.compile(
            r"register_hotkey\(\s*\"[^\"]+\"\s*,[^)]*?default_key=\"([^\"]+)\"")
        taken = {normalize_hotkey(v): f"builtin:{a}"
                 for a, v in DEFAULT_HOTKEYS.items()}
        found = 0
        for root, _dirs, files in os.walk(plugins_dir):
            for fn in files:
                if fn != "plugin.py":
                    continue
                path = os.path.join(root, fn)
                with open(path, "r", encoding="utf-8") as fp:
                    src = fp.read()
                for key in pattern.findall(src):
                    found += 1
                    canon = normalize_hotkey(key)
                    self.assertIsNotNone(canon, f"{path}: {key}")
                    self.assertNotIn(canon, taken,
                                     f"{path}: {key} clashes {taken.get(canon)}")
                    taken[canon] = path
        # midi(play/stop) + hide_seek(toggle) 至少 3 个注册点被扫到
        self.assertGreaterEqual(found, 3)


if __name__ == "__main__":
    unittest.main()
