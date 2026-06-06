# -*- coding: utf-8 -*-
"""Regression tests for direct-GPU Entity entry/exit transition routing."""

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import gui_modules.sao_gui_link_animation_mixin as link_mod
from gui_modules.sao_gpu_entity_transition import EntityTransitionGpuOverlay
from gui_modules.sao_gui_lifecycle_mixin import SAOPlayerGUILifecycleMixin
from gui_modules.sao_gui_link_animation_mixin import SAOPlayerGUILinkAnimationMixin


class _Root:
    def winfo_screenwidth(self) -> int:
        return 1920

    def winfo_screenheight(self) -> int:
        return 1080


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


class EntityTransitionGpuDestroyTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
