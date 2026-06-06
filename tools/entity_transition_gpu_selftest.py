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
        self._entry_overlay = None
        self._exit_overlay = None

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
        self.assertEqual(gpu.target, (440, 448))
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
        self.assertEqual(gpu.center, (260, 888))
        self.assertEqual(gpu.target, (260, 888))
        self.assertEqual(gpu.progress_values, [0.73])
        self.assertEqual(gpu.destroy_count, 1)


if __name__ == "__main__":
    unittest.main()
