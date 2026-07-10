from __future__ import annotations

import os
import sys
import threading
import types
import unittest
from unittest import mock


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON = os.path.join(ROOT, 'python')
if PYTHON not in sys.path:
    sys.path.insert(0, PYTHON)

from gui_modules.sao_gui_lifecycle_mixin import SAOPlayerGUILifecycleMixin
from act_platform.plugins import PluginContext


class _ExitIntercept(BaseException):
    pass


class LifecycleShutdownTests(unittest.TestCase):
    def _run_hard_exit(self, compositor_ok: bool, *, leases_ok: bool = True,
                       cleanup_ok: bool = True, plugins_ok: bool = True):
        events = []

        class Subject(SAOPlayerGUILifecycleMixin):
            def _finalize_close(self, *, quit_root=True):
                events.append(('finalize', quit_root))
                return compositor_ok

            def _await_helper_shutdown_ui(self):
                events.append('helper')
                return cleanup_ok

        runtime = types.ModuleType('act_platform.runtime')
        runtime.shutdown_act_plugin_manager = lambda _owner: (
            events.append('plugin-rundown') or plugins_ok)
        old_runtime = sys.modules.get('act_platform.runtime')
        old_exit = os._exit
        from mem_probe import process
        old_wait = process._wait_for_target_leases_closed
        sys.modules['act_platform.runtime'] = runtime
        process._wait_for_target_leases_closed = lambda timeout=0: (
            events.append('lease-rundown') or leases_ok)

        def fake_exit(code):
            events.append(('exit', code))
            raise _ExitIntercept()

        os._exit = fake_exit
        try:
            if compositor_ok and leases_ok and cleanup_ok and plugins_ok:
                with self.assertRaises(_ExitIntercept):
                    Subject()._hard_exit_process()
            else:
                Subject()._hard_exit_process()
        finally:
            os._exit = old_exit
            process._wait_for_target_leases_closed = old_wait
            if old_runtime is None:
                sys.modules.pop('act_platform.runtime', None)
            else:
                sys.modules['act_platform.runtime'] = old_runtime
        return events

    def test_compositor_teardown_precedes_helper_and_process_exit(self):
        events = self._run_hard_exit(True)
        self.assertLess(events.index('plugin-rundown'),
                        events.index(('finalize', False)))
        self.assertLess(events.index(('finalize', False)),
                        events.index('lease-rundown'))
        self.assertLess(events.index('lease-rundown'), events.index('helper'))
        self.assertLess(events.index('helper'), events.index(('exit', 0)))

    def test_unproven_compositor_stop_blocks_helper_and_hard_exit(self):
        events = self._run_hard_exit(False)
        self.assertEqual(events, ['plugin-rundown', ('finalize', False)])

    def test_live_plugin_worker_blocks_all_later_shutdown_stages(self):
        events = self._run_hard_exit(True, plugins_ok=False)
        self.assertEqual(events, ['plugin-rundown'])

    def test_live_consumer_lease_blocks_helper_and_hard_exit(self):
        events = self._run_hard_exit(True, leases_ok=False)
        self.assertEqual(events, [
            'plugin-rundown', ('finalize', False), 'lease-rundown'])

    def test_unconfirmed_helper_cleanup_blocks_hard_exit(self):
        events = self._run_hard_exit(True, cleanup_ok=False)
        self.assertEqual(events, [
            'plugin-rundown', ('finalize', False),
            'lease-rundown', 'helper'])

    def test_plugin_context_keeps_live_thread_registered_until_rundown(self):
        release = threading.Event()
        worker = threading.Thread(target=release.wait, daemon=True)
        worker.start()
        context = PluginContext.__new__(PluginContext)
        context._stop_event = threading.Event()
        context._registered_threads = [worker]
        context._registered_threads_lock = threading.RLock()
        self.assertFalse(context._signal_stop_and_join(timeout=0.01))
        self.assertEqual(context._registered_threads, [worker])
        release.set()
        worker.join(timeout=1)
        self.assertTrue(context._signal_stop_and_join(timeout=0.1))
        self.assertEqual(context._registered_threads, [])

    def test_dead_helper_without_cleanup_confirmation_still_blocks_exit(self):
        from mem_probe import rt_io_proxy

        subject = SAOPlayerGUILifecycleMixin()
        with (
            mock.patch.object(rt_io_proxy, 'helper_alive', return_value=False),
            mock.patch.object(
                rt_io_proxy, '_stop_helper',
                return_value={'confirmed': False}) as stop,
        ):
            self.assertFalse(subject._await_helper_shutdown_ui())
        stop.assert_called_once_with(wait_seconds=15.0)

    def test_local_no_session_cleanup_allows_exit_without_fake_ack(self):
        from mem_probe import rt_io_proxy

        subject = SAOPlayerGUILifecycleMixin()
        with (
            mock.patch.object(rt_io_proxy, 'helper_alive', return_value=False),
            mock.patch.object(
                rt_io_proxy, '_stop_helper',
                return_value={
                    'confirmed': True,
                    'ack_received': False,
                    'error': 'no_session_created',
                }),
        ):
            self.assertTrue(subject._await_helper_shutdown_ui())

    def test_panel_rundown_failure_is_retried_before_compositor(self):
        events = []

        class Root:
            @staticmethod
            def after_cancel(_token):
                return None

            @staticmethod
            def quit():
                events.append('quit')

        class Panel:
            calls = 0

            def destroy(self):
                self.calls += 1
                events.append(('panel', self.calls))
                return self.calls > 1

        class Subject(SAOPlayerGUILifecycleMixin):
            def _stop_fisheye_overlay(self, **_kwargs):
                return None

            def _stop_overlay_runtime(self):
                events.append('compositor')
                return True

        subject = Subject()
        subject.root = Root()
        subject._close_finalized = False
        subject._destroyed = False
        subject._breath_active = True
        subject._lift_loop_active = True
        subject._cache_loop_stop = threading.Event()
        subject._process_selector_panel = Panel()
        subject._entry_overlay = None
        subject._exit_overlay = None
        subject._hotkey_mgr = None
        subject._state_mgr = None
        subject._cfg_settings_ref = None
        subject._sao_menu = None
        runtime = types.ModuleType('act_platform.runtime')
        runtime.shutdown_act_plugin_manager = lambda _owner: True
        old_runtime = sys.modules.get('act_platform.runtime')
        sys.modules['act_platform.runtime'] = runtime
        try:
            self.assertFalse(subject._finalize_close(quit_root=False))
            self.assertFalse(subject._close_finalized)
            self.assertIsNotNone(subject._process_selector_panel)
            self.assertNotIn('compositor', events)
            self.assertTrue(subject._finalize_close(quit_root=False))
        finally:
            if old_runtime is None:
                sys.modules.pop('act_platform.runtime', None)
            else:
                sys.modules['act_platform.runtime'] = old_runtime
        self.assertEqual(events, [('panel', 1), ('panel', 2), 'compositor'])
        self.assertTrue(subject._close_finalized)


if __name__ == '__main__':
    unittest.main(verbosity=2)
