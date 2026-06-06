# -*- coding: utf-8 -*-
"""Regression tests for direct-GPU Entity entry/exit transition routing."""

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import gui_modules.sao_gui_lifecycle_mixin as lifecycle_mod
import gui_modules.sao_gui_link_animation_mixin as link_mod
from gui_modules.sao_gpu_entity_transition import EntityTransitionGpuOverlay
from gui_modules.sao_gui_lifecycle_mixin import SAOPlayerGUILifecycleMixin
from gui_modules.sao_gui_link_animation_mixin import SAOPlayerGUILinkAnimationMixin


class _Root:
    def winfo_screenwidth(self) -> int:
        return 1920

    def winfo_screenheight(self) -> int:
        return 1080


class _AfterRoot(_Root):
    def __init__(self) -> None:
        self.after_calls = []

    def after(self, delay, callback=None):
        self.after_calls.append((delay, callback))
        return f"after-{len(self.after_calls)}"


class _Float:
    def winfo_rootx(self) -> int:
        return 120

    def winfo_rooty(self) -> int:
        return 840


class _HpOverlay:
    _x = 50
    _y = 900
    WIDTH = 500
    HEIGHT = 120


class _FakeGpu:
    instances = []

    def __init__(self, root, *, kind, center, target=None, title=''):
        self.root = root
        self.kind = kind
        self.center = center
        self.target = target
        self.title = title
        self.progress_values = []
        self.destroy_count = 0
        _FakeGpu.instances.append(self)

    def start(self) -> bool:
        return True

    def set_progress(self, progress: float) -> None:
        self.progress_values.append(progress)

    def destroy(self) -> None:
        self.destroy_count += 1


class _Owner(SAOPlayerGUILinkAnimationMixin, SAOPlayerGUILifecycleMixin):
    def __init__(self) -> None:
        self.root = _Root()
        self._fw = 280
        self._fh = 96
        self._float = _Float()
        self.settings = None
        self._entry_overlay = None
        self._exit_overlay = None
        self._hp_overlay = None

    def _finalize_close(self) -> None:
        raise AssertionError('_finalize_close should not run in GPU route tests')


class EntityTransitionGpuRouteTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_gpu = link_mod.EntityTransitionGpuOverlay
        link_mod.EntityTransitionGpuOverlay = _FakeGpu
        _FakeGpu.instances = []

    def tearDown(self) -> None:
        link_mod.EntityTransitionGpuOverlay = self._orig_gpu

    def test_entry_overlay_uses_direct_gpu_transition(self) -> None:
        owner = _Owner()

        overlay = owner._create_entry_overlay(100, 200, 300, 400)
        owner._draw_entry_overlay(0.42)
        owner._cleanup_entry_overlay()

        gpu = _FakeGpu.instances[0]
        self.assertIs(overlay['gpu_transition'], gpu)
        self.assertEqual(gpu.kind, 'entry')
        self.assertEqual(gpu.center, (240, 248))
        self.assertEqual(gpu.target, (692.5, 1044.0))
        self.assertEqual(gpu.progress_values, [0.42])
        self.assertEqual(gpu.destroy_count, 1)

    def test_exit_overlay_uses_direct_gpu_transition(self) -> None:
        owner = _Owner()

        overlay = owner._create_exit_overlay(mode='switch', target_label='WebView')
        owner._draw_exit_overlay(0.73)
        owner._cleanup_exit_overlay()

        gpu = _FakeGpu.instances[0]
        self.assertIs(overlay['gpu_transition'], gpu)
        self.assertEqual(gpu.kind, 'exit')
        self.assertEqual(gpu.center, (692.5, 1044.0))
        self.assertEqual(gpu.target, (692.5, 1044.0))
        self.assertEqual(gpu.progress_values, [0.73])
        self.assertEqual(gpu.destroy_count, 1)

    def test_focus_center_prefers_existing_hp_overlay(self) -> None:
        owner = _Owner()
        owner._hp_overlay = _HpOverlay()

        self.assertEqual(owner._entity_transition_focus_center(1, 2), (300.0, 960.0))


class _FakeWin:
    def __init__(self) -> None:
        self.destroy_count = 0

    def destroy(self) -> None:
        self.destroy_count += 1


class _FakeGlObject:
    def __init__(self) -> None:
        self.release_count = 0

    def release(self) -> None:
        self.release_count += 1


class _FadeableOverlay:
    def __init__(self) -> None:
        self.fade_count = 0
        self.hide_count = 0

    def fade_out(self) -> None:
        self.fade_count += 1

    def hide(self) -> None:
        self.hide_count += 1


class _DestroyableOverlay:
    def __init__(self, order, name: str) -> None:
        self.order = order
        self.name = name

    def destroy(self) -> None:
        self.order.append(f"{self.name}.destroy")


class _QuitRoot:
    def __init__(self, order) -> None:
        self.order = order

    def after_cancel(self, _aid) -> None:
        self.order.append("root.after_cancel")

    def quit(self) -> None:
        self.order.append("root.quit")


class _StopFlag:
    def __init__(self, order) -> None:
        self.order = order

    def set(self) -> None:
        self.order.append("cache.stop")


class _FinalizeOwner(SAOPlayerGUILifecycleMixin):
    def __init__(self) -> None:
        self.order = []
        self.root = _QuitRoot(self.order)
        self._close_finalized = False
        self._destroyed = False
        self._breath_active = True
        self._lift_loop_active = True
        self._panel_float_after_id = None
        self._menu_refresh_after_id = None
        self._updater_mgr = None
        self._update_listener = None
        self._update_listener_installed = False
        self._hotkey_mgr = None
        self._state_mgr = None
        self._cfg_settings_ref = None
        self._sao_menu = None
        self._recognition_active = True
        self._cache_loop_stop = _StopFlag(self.order)
        self._status_panel = None
        self._update_panel = None
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._hp_overlay = _DestroyableOverlay(self.order, "hp")
        self._alert_overlay = None
        self._map_banner_overlay = None
        self._skillfx_overlay = None
        self._self_buff_overlay = None
        self._boss_buff_overlay = None
        for name in (
            '_autokey_panel', '_bossraid_panel',
            '_autokey_detail_panel', '_bossraid_detail_panel',
            '_commander_panel', '_act_plugin_manager_panel',
            '_act_trigger_timer_panel', '_act_data_source_health_panel',
            '_act_report_export_panel', '_act_offline_import_panel',
            '_act_timeline_vcr_panel', '_act_aggregate_panel',
            '_act_action_log_panel', '_act_death_recap_panel',
            '_act_graph_timeseries_panel', '_act_combatant_drilldown_panel',
            '_act_skill_drilldown_panel',
        ):
            setattr(self, name, None)
        self._float = None

    def _cleanup_entry_overlay(self) -> None:
        self.order.append("entry.cleanup")

    def _cleanup_exit_overlay(self) -> None:
        self.order.append("exit.cleanup")

    def _stop_fisheye_overlay(self, wait=False) -> None:
        self.order.append("fisheye.stop")

    def _stop_boss_hp_worker(self) -> None:
        self.order.append("boss_worker.stop")

    def _stop_recognition_engines(self) -> None:
        self.order.append("recognition.stop")

    def _destroy_hp_alpha_strip_windows(self) -> None:
        self.order.append("hp_alpha_strips.destroy")


class EntityTransitionGpuDestroyTests(unittest.TestCase):
    def test_shader_points_flip_top_left_screen_y_to_bottom_left_gl_y(self) -> None:
        overlay = EntityTransitionGpuOverlay(
            _Root(),
            kind='entry',
            center=(692.5, 1044.0),
            target=(692.5, 1044.0),
        )
        overlay._w = 1920
        overlay._h = 1080

        self.assertEqual(overlay._to_gl_point(overlay.center), (692.5, 36.0))

    def test_destroy_leaves_gl_objects_to_gpu_window_context_teardown(self) -> None:
        overlay = EntityTransitionGpuOverlay(
            _Root(),
            kind='entry',
            center=(1, 2),
        )
        win = _FakeWin()
        prog = _FakeGlObject()
        vao = _FakeGlObject()
        overlay._win = win
        overlay._prog = prog
        overlay._vao = vao

        overlay.destroy()

        self.assertEqual(win.destroy_count, 1)
        self.assertEqual(prog.release_count, 0)
        self.assertEqual(vao.release_count, 0)
        self.assertIsNone(overlay._prog)
        self.assertIsNone(overlay._vao)


class _LifecycleOwner(SAOPlayerGUILifecycleMixin):
    def __init__(self, *, gpu_exit: bool) -> None:
        self.root = _AfterRoot()
        self._close_finalized = False
        self._exit_animating = False
        self._destroyed = False
        self._breath_active = True
        self._lift_loop_active = True
        self._sao_menu = None
        self._hp_overlay = None
        self._boss_hp_overlay = None
        self._dps_overlay = None
        self._exit_overlay = None
        self._gpu_exit = gpu_exit
        self.draw_progress = []
        self.finalize_count = 0

    def _play_motion_blur(self, closing=False) -> None:
        self.motion_blur_closing = closing

    def _collect_exit_windows(self):
        return []

    def _create_exit_overlay(self, mode='exit', target_label=None):
        if self._gpu_exit:
            self._exit_overlay = {'gpu_transition': object()}
        else:
            self._exit_overlay = {'win': object()}
        return self._exit_overlay

    def _draw_exit_overlay(self, progress):
        self.draw_progress.append(progress)

    def _finalize_close(self):
        self._close_finalized = True
        self.finalize_count += 1


class EntityTransitionGpuExitLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_sound = lifecycle_mod.play_sound
        lifecycle_mod.play_sound = lambda _name: None

    def tearDown(self) -> None:
        lifecycle_mod.play_sound = self._orig_sound

    def test_gpu_exit_waits_one_present_window_before_finalize(self) -> None:
        owner = _LifecycleOwner(gpu_exit=True)
        after_shutdown_calls = []

        owner._run_exit_animation(after_shutdown=lambda: after_shutdown_calls.append(True))

        self.assertEqual(owner.draw_progress, [1.0])
        self.assertEqual(owner.finalize_count, 0)
        self.assertEqual(len(owner.root.after_calls), 1)
        delay, callback = owner.root.after_calls[0]
        self.assertEqual(delay, 80)
        self.assertTrue(callable(callback))

        callback()

        self.assertEqual(owner.finalize_count, 1)
        self.assertEqual(after_shutdown_calls, [True])

    def test_exit_prefers_overlay_fade_out_over_hide(self) -> None:
        owner = _LifecycleOwner(gpu_exit=True)
        hp = _FadeableOverlay()
        owner._hp_overlay = hp

        owner._run_exit_animation()

        self.assertEqual(hp.fade_count, 1)
        self.assertEqual(hp.hide_count, 0)

    def test_finalize_keeps_exit_overlay_until_gpu_children_are_destroyed(self) -> None:
        owner = _FinalizeOwner()

        owner._finalize_close()

        self.assertLess(
            owner.order.index("hp.destroy"),
            owner.order.index("exit.cleanup"),
        )
        self.assertLess(
            owner.order.index("exit.cleanup"),
            owner.order.index("root.quit"),
        )


if __name__ == "__main__":
    unittest.main()
