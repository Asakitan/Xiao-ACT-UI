"""Regression tests for unified overlay mode fallback behavior."""

from __future__ import annotations

import time


def test_tk_mirror_keeps_input_until_proxy_ready() -> None:
    import inspect
    from render.tk_mirror import TkMirrorLayer

    src = inspect.getsource(TkMirrorLayer.attach)
    proxy_idx = src.index("layer.create_input_proxy(root)")
    transparent_idx = src.index("_user32.SetWindowLongPtrW")
    alpha_idx = src.index("win.attributes('-alpha', 0.01)")

    assert "_get_unified_overlay(root)" in src
    assert "uo.wait_ready(timeout=0.15)" in src
    assert "if layer._input_proxy is None" in src
    assert proxy_idx < transparent_idx
    assert proxy_idx < alpha_idx


def test_tk_mirror_syncs_input_proxy_on_visibility_changes() -> None:
    import inspect
    from render.tk_mirror import TkMirrorLayer

    assert "self._layer.sync_input_proxy()" in inspect.getsource(
        TkMirrorLayer.show)
    assert "self._layer.sync_input_proxy()" in inspect.getsource(
        TkMirrorLayer.hide)
    assert "self._layer.sync_input_proxy()" in inspect.getsource(
        TkMirrorLayer.set_position)
    assert "self._layer.sync_input_proxy()" in inspect.getsource(
        TkMirrorLayer.set_geometry)


def test_tk_mirror_forwards_to_tk_widget_before_win32_fallback() -> None:
    import inspect
    from render.tk_mirror import TkMirrorLayer

    src = inspect.getsource(TkMirrorLayer._on_mouse_button)
    assert "self._widget_at(self._tk_win, x, y)" in src
    assert "self._forward_tk_mouse_event(" in src
    assert "if forwarded:" in src
    assert "return" in src[src.index("if forwarded:"):]
    assert src.index("self._forward_tk_mouse_event(") < src.index(
        "_user32.PostMessageW")


def test_prestart_failure_disables_unified_mode() -> None:
    from render import gpu_overlay_window as gow
    from render import overlay_compositor as oc

    class FakeOverlay:
        def __init__(self) -> None:
            self.stop_count = 0

        def wait_ready(self, timeout: float = 0.0) -> bool:
            return False

        def stop(self) -> None:
            self.stop_count += 1

    fake = FakeOverlay()
    calls = {"get": 0, "reset": 0}
    old_get = gow._get_unified_overlay
    old_reset = getattr(oc, "reset_unified_overlay", None)
    old_sleep = time.sleep
    old_mode = gow.get_unified_overlay_mode()
    old_instance = getattr(gow, "_unified_overlay_instance", None)
    try:
        def fake_get(root=None):
            calls["get"] += 1
            gow._unified_overlay_instance = fake
            return fake

        def fake_reset():
            calls["reset"] += 1

        gow._get_unified_overlay = fake_get
        oc.reset_unified_overlay = fake_reset
        time.sleep = lambda _seconds: None
        gow.set_unified_overlay_mode(True)
        gow.prestart_unified_overlay(None)

        deadline = time.monotonic() + 2.0
        while gow.get_unified_overlay_mode():
            if time.monotonic() > deadline:
                raise AssertionError("unified overlay mode did not fall back")
            old_sleep(0.01)

        assert calls["get"] == 3, calls
        assert calls["reset"] == 1, calls
        assert fake.stop_count >= 1
        assert gow._unified_overlay_instance is None
    finally:
        gow._get_unified_overlay = old_get
        if old_reset is not None:
            oc.reset_unified_overlay = old_reset
        time.sleep = old_sleep
        gow._unified_overlay_instance = old_instance
        gow.set_unified_overlay_mode(old_mode)


def main() -> int:
    test_tk_mirror_keeps_input_until_proxy_ready()
    test_tk_mirror_syncs_input_proxy_on_visibility_changes()
    test_tk_mirror_forwards_to_tk_widget_before_win32_fallback()
    test_prestart_failure_disables_unified_mode()
    print("overlay mode selftest passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
