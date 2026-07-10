from __future__ import annotations

import os
import queue
import sys
import threading
import types
import unittest
from unittest import mock


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON = os.path.join(ROOT, 'python')
if PYTHON not in sys.path:
    sys.path.insert(0, PYTHON)

from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin
from render import overlay_compositor as oc
from render import overlay_host as oh
from render import webview_proxy as wvp
from render.overlay_compositor import UnifiedOverlay
from render.overlay_host import OverlayHost
from render.tk_mirror import TkMirrorLayer
from mem_probe._vf import _VFence


class OverlayInputLifecycleTests(unittest.TestCase):
    def _z_order_case(self, game_hwnd: int, game_exstyle: int):
        calls = []
        mutations = []
        fake_dc = types.ModuleType('mem_probe._dc')
        fake_dc.syscall_set_window_pos = (
            lambda hwnd, after, *_args: calls.append((hwnd, after)) or True)
        old_dc = sys.modules.get('mem_probe._dc')
        sys.modules['mem_probe._dc'] = fake_dc

        class User32:
            @staticmethod
            def IsWindow(hwnd):
                return bool(hwnd)

            @staticmethod
            def GetWindowLongPtrW(hwnd, _index):
                return game_exstyle if int(hwnd) == int(game_hwnd) else 0

            @staticmethod
            def SetWindowPos(*_args):
                raise AssertionError('syscall path unexpectedly failed')

        old_windll = oc._ct.windll
        oc._ct.windll = types.SimpleNamespace(user32=User32())
        subject = UnifiedOverlay.__new__(UnifiedOverlay)
        subject._host = types.SimpleNamespace(hwnd=100)
        subject._game_hwnd = game_hwnd
        subject._dc_mutations = types.SimpleNamespace(
            submit_dc=lambda hwnd, op, method, *args: (
                mutations.append((hwnd, op, method, args)) or True))
        subject._z_sorted = []
        subject._thread = object()
        try:
            subject._enforce_z_order()
        finally:
            oc._ct.windll = old_windll
            if old_dc is None:
                sys.modules.pop('mem_probe._dc', None)
            else:
                sys.modules['mem_probe._dc'] = old_dc
        return calls, mutations

    def test_game_normal_z_order_sequence_is_preserved(self):
        calls, mutations = self._z_order_case(200, 0)
        self.assertEqual(calls, [(100, 200)])
        self.assertEqual(
            [item[2] for item in mutations],
            ['hide_window_rect', 'hide_exstyle', 'hide_z_order'])

    def test_game_topmost_z_order_sequence_is_preserved(self):
        calls, mutations = self._z_order_case(200, 0x8)
        self.assertEqual(calls, [(100, 200)])
        self.assertEqual(
            [item[2] for item in mutations],
            ['set_exstyle_bit', 'hide_window_rect',
             'hide_exstyle', 'hide_z_order'])

    def test_no_game_z_order_sequence_is_preserved(self):
        calls, mutations = self._z_order_case(0, 0)
        self.assertEqual(calls, [(100, -1)])
        self.assertEqual(
            [item[2] for item in mutations],
            ['hide_window_rect', 'hide_exstyle', 'hide_z_order'])

    def test_capture_affinity_is_applied_and_verified_only_on_hrender(self):
        # hControl 是 1x1 decoy (无 GL/DComp/WM_PAINT/hbrBackground=NULL),
        # 客户区永远没像素可截; 给它上 WDA 反而把 SetWindowDisplayAffinity=
        # 0x11 (反作弊高优先级指纹) 焊到"唯一暴露在 EnumWindows 里的窗口"
        # 上, 抹掉 hRender hide_z_order unlink 换来的隐蔽性. set_capture_mode
        # 必须只碰 self.hwnd (hRender), 完全不 touch control_hwnd.
        calls = []
        fake_dc = types.ModuleType('mem_probe._dc')
        fake_dc.apply = lambda hwnd: calls.append(('apply', hwnd)) or True
        fake_dc.remove = lambda hwnd: calls.append(('remove', hwnd)) or True
        fake_dc.verify = lambda hwnd: calls.append(('verify', hwnd)) or True
        old_dc = sys.modules.get('mem_probe._dc')
        sys.modules['mem_probe._dc'] = fake_dc
        host = OverlayHost.__new__(OverlayHost)
        host.hwnd = 10
        host.control_hwnd = 20
        host._capture_excluded = False
        try:
            host.set_capture_mode(True)
            self.assertTrue(host._capture_excluded)
            self.assertEqual(calls, [
                ('apply', 10), ('verify', 10)])
            self.assertNotIn(('apply', 20), calls)
            self.assertNotIn(('verify', 20), calls)
            calls.clear()
            host.set_capture_mode(False)
            self.assertFalse(host._capture_excluded)
            self.assertEqual(calls, [('remove', 10)])
            self.assertNotIn(('remove', 20), calls)
        finally:
            if old_dc is None:
                sys.modules.pop('mem_probe._dc', None)
            else:
                sys.modules['mem_probe._dc'] = old_dc

    def test_local_proxy_keeps_fullscreen_host_passthrough(self):
        passthrough = []
        subject = UnifiedOverlay.__new__(UnifiedOverlay)
        subject._lock = threading.RLock()
        subject._z_sorted = [types.SimpleNamespace(
            visible=True, click_through=False, _input_proxy=object())]
        subject._host = types.SimpleNamespace(
            set_input_passthrough=lambda value: passthrough.append(value))
        subject._cmd_q = queue.Queue()

        subject.sync_host_input_mode()
        subject._cmd_q.get_nowait()()
        self.assertEqual(passthrough, [True])

    def test_proxyless_interactive_layer_cannot_disable_host_passthrough(self):
        passthrough = []
        subject = UnifiedOverlay.__new__(UnifiedOverlay)
        subject._lock = threading.RLock()
        subject._z_sorted = [types.SimpleNamespace(
            visible=True, click_through=False, _input_proxy=None)]
        subject._host = types.SimpleNamespace(
            set_input_passthrough=lambda value: passthrough.append(value))
        subject._cmd_q = queue.Queue()
        subject.sync_host_input_mode()
        subject._cmd_q.get_nowait()()
        self.assertEqual(passthrough, [True])

    def test_interactive_layer_creation_requests_local_proxy(self):
        requested = []
        subject = UnifiedOverlay.__new__(UnifiedOverlay)
        subject._layers = {}
        subject._z_sorted = []
        subject._lock = threading.RLock()
        subject._dc_mutations = None
        subject._cmd_q = queue.Queue()
        subject._dcomp = None
        subject.destroy_layer = lambda _name: None
        subject._recalc_fps = lambda: None
        subject.attach_layer_input_proxy = (
            lambda name: requested.append(name) or True)
        layer = subject.create_layer(
            'interactive-test', 10, 10, click_through=False)
        self.assertFalse(layer.click_through)
        self.assertEqual(requested, ['interactive-test'])

    def test_old_fisheye_delayed_close_cannot_close_new_overlay(self):
        callbacks = []

        class Root:
            @staticmethod
            def after(_delay, callback):
                callbacks.append(callback)

        class Subject(SAOPlayerGUIFisheyeMixin):
            def _any_panel_open(self):
                return False

            def _stop_fisheye_overlay(self, *args, **kwargs):
                raise AssertionError('immediate stop path was not expected')

        old_calls = []
        new_calls = []
        old = types.SimpleNamespace(
            _request_fadeout=lambda: old_calls.append('fade'))
        new = types.SimpleNamespace(
            _request_fadeout=lambda: new_calls.append('fade'))
        subject = Subject()
        subject.root = Root()
        subject._sao_menu = None
        subject._fisheye_ov = old
        subject._sao_panel_transition_until = 0.0
        subject._play_motion_blur = lambda **_kwargs: None

        subject._maybe_stop_fisheye()
        self.assertEqual(len(callbacks), 1)
        subject._fisheye_ov = new
        callbacks.pop(0)()
        self.assertEqual(old_calls, [])
        self.assertEqual(new_calls, [])

    def test_fisheye_uses_canonical_z_order_decision(self):
        calls = []
        overlay = types.SimpleNamespace(
            hwnd=123,
            enforce_z_order_now=lambda **kwargs: calls.append(kwargs),
        )
        subject = SAOPlayerGUIFisheyeMixin()
        subject._fisheye_hit_layer = object()
        with mock.patch.object(oc, 'get_unified_overlay', return_value=overlay):
            subject._raise_compositor_above_fisheye_hit_layer()
        self.assertEqual(calls, [{'force_topmost': True}])

    def test_input_proxy_destroy_waits_asynchronously_for_generation_drain(self):
        callbacks = []

        class Proxy:
            withdraw_calls = 0
            destroy_calls = 0

            def withdraw(self):
                self.withdraw_calls += 1

            @staticmethod
            def winfo_id():
                return 77

            @staticmethod
            def after(_delay, callback):
                callbacks.append(callback)

            def destroy(self):
                self.destroy_calls += 1

        barrier = types.SimpleNamespace(done=False, confirmed=False)
        coordinator = types.SimpleNamespace(
            begin_invalidate=lambda hwnd, timeout=0: barrier)
        proxy = Proxy()
        layer = oc.CompositorLayer.__new__(oc.CompositorLayer)
        layer._input_proxy = proxy
        layer._dc_mutations = coordinator
        fake_user = types.SimpleNamespace(GetAncestor=lambda *_args: 0x1234)
        old_windll = oc._ct.windll
        oc._ct.windll = types.SimpleNamespace(user32=fake_user)
        try:
            layer.destroy_input_proxy()
            self.assertEqual(proxy.withdraw_calls, 1)
            self.assertEqual(proxy.destroy_calls, 0)
            self.assertIsNone(layer._input_proxy)
            callbacks.pop(0)()
            self.assertEqual(proxy.destroy_calls, 0)
            barrier.done = True
            barrier.confirmed = True
            callbacks.pop(0)()
            self.assertEqual(proxy.destroy_calls, 1)
        finally:
            oc._ct.windll = old_windll

    def test_tk_mirror_join_timeout_preserves_live_owner_state(self):
        class LiveThread:
            join_calls = 0

            def is_alive(self):
                return True

            def join(self, timeout=None):
                self.join_calls += 1

        mirror = TkMirrorLayer.__new__(TkMirrorLayer)
        mirror._attached = True
        mirror._running = True
        mirror._stop_evt = threading.Event()
        mirror._thread = LiveThread()
        mirror._layer = object()
        self.assertFalse(mirror.detach())
        self.assertIsNotNone(mirror._thread)
        self.assertIsNotNone(mirror._layer)
        self.assertTrue(mirror._attached)

    def test_host_destroy_refuses_to_destroy_hwnd_before_mutation_drain(self):
        destroyed = []
        host = OverlayHost.__new__(OverlayHost)
        host._destroyed = False
        host._dc_mutations = types.SimpleNamespace(
            invalidate=lambda *_args, **_kwargs: False)
        host.hwnd = 10
        host.control_hwnd = 20
        host._owner_hwnd = 30
        host.hglrc = 0
        host.hdc = 0
        host.ctx = object()
        fake_user = types.SimpleNamespace(
            DestroyWindow=lambda hwnd: destroyed.append(hwnd) or True,
            IsWindow=lambda _hwnd: True,
        )
        old_user = oh._user32
        oh._user32 = fake_user
        try:
            self.assertFalse(host.destroy())
        finally:
            oh._user32 = old_user
        self.assertEqual(destroyed, [])
        self.assertFalse(host._destroyed)
        self.assertEqual(host.hwnd, 10)

    def test_runtime_exception_quiesces_mutations_and_runs_finally_cleanup(self):
        events = []

        class Coordinator:
            accepting = True

            def quiesce(self):
                self.accepting = False
                events.append('quiesce')

            def submit(self):
                return self.accepting

        subject = UnifiedOverlay.__new__(UnifiedOverlay)
        subject._running = True
        subject._stop_evt = threading.Event()
        subject._ready = threading.Event()
        subject._ready_ok = True
        subject._init_error = None
        subject._teardown_confirmed = False
        subject._dc_mutations = Coordinator()
        subject._run_body = lambda: (_ for _ in ()).throw(RuntimeError('boom'))
        outcomes = iter((False, True))

        def teardown():
            events.append(('submit_after_quiesce', subject._dc_mutations.submit()))
            return next(outcomes)

        subject._teardown_render_thread_once = teardown
        with mock.patch.object(oc.time, 'sleep', return_value=None):
            subject._run()
        self.assertTrue(subject._teardown_confirmed)
        self.assertTrue(subject._ready.is_set())
        self.assertEqual(events[0], 'quiesce')
        self.assertEqual(events[1:], [
            ('submit_after_quiesce', False),
            ('submit_after_quiesce', False),
        ])

    def test_vfence_and_webview_keep_live_thread_owner_on_timeout(self):
        class LiveThread:
            def is_alive(self):
                return True

            def join(self, timeout=None):
                return None

        live = LiveThread()
        vf = _VFence.__new__(_VFence)
        vf._stop = threading.Event()
        vf._evt = threading.Event()
        vf._evt.set()
        vf._active = True
        vf._th = live
        vf._lock = threading.RLock()
        self.assertFalse(vf.stop(timeout=0.0))
        self.assertIs(vf._th, live)

        proxy = wvp.WebViewProxy.__new__(wvp.WebViewProxy)
        proxy._running = True
        proxy._stop_evt = threading.Event()
        proxy._thread = live
        proxy._layer = object()
        proxy._name = 'test'
        self.assertFalse(proxy.stop())
        self.assertIs(proxy._thread, live)
        self.assertIsNotNone(proxy._layer)

    def test_streaming_publish_and_start_are_atomic_with_rundown(self):
        start_entered = threading.Event()
        allow_start = threading.Event()
        errors = []

        class ControlledThread:
            def __init__(self, *args, **kwargs):
                self._alive = False

            def start(self):
                start_entered.set()
                allow_start.wait(1.0)
                self._alive = False

            def is_alive(self):
                return self._alive

            def join(self, timeout=None):
                if not start_entered.is_set():
                    raise RuntimeError('join before start')

        subject = UnifiedOverlay.__new__(UnifiedOverlay)
        subject._streaming_lock = threading.Lock()
        subject._streaming_threads_lock = threading.RLock()
        subject._streaming_threads = set()
        subject._streaming_accepting = True
        subject._stop_evt = threading.Event()
        subject._host = None
        real_thread = threading.Thread
        with mock.patch.object(oc.threading, 'Thread', ControlledThread):
            setter = real_thread(target=lambda: subject.set_streaming_mode(True))
            setter.start()
            self.assertTrue(start_entered.wait(1.0))
            stopper = real_thread(target=lambda: (
                errors.append(subject._stop_streaming_workers(timeout=1.0))))
            stopper.start()
            allow_start.set()
            setter.join(timeout=1.0)
            stopper.join(timeout=1.0)
        self.assertEqual(errors, [True])


if __name__ == '__main__':
    unittest.main(verbosity=2)
