from __future__ import annotations

import os
import sys
import types
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON = os.path.join(ROOT, 'python')
if PYTHON not in sys.path:
    sys.path.insert(0, PYTHON)

from render import overlay_compositor as oc


class _Callable:
    def __init__(self, fn):
        self._fn = fn
        self.restype = None
        self.argtypes = None

    def __call__(self, *args):
        return self._fn(*args)


class OverlayRegionOwnershipTests(unittest.TestCase):
    def test_failed_apply_frees_new_handle_but_success_transfers_ownership(self):
        deleted = []
        created = []
        outcomes = iter((False, True))

        def create_region(*_args):
            handle = 90 + len(created)
            created.append(handle)
            return handle

        fake_gdi = types.SimpleNamespace(
            CreateRectRgn=_Callable(create_region),
            GetObjectType=_Callable(
                lambda _handle: (_ for _ in ()).throw(
                    AssertionError("owned HRGN must never be queried"))),
            DeleteObject=_Callable(
                lambda handle: deleted.append(int(handle.value)) or True),
        )
        fake_user = types.SimpleNamespace(
            SetWindowRgn=_Callable(lambda *_args: next(outcomes)),
        )
        fake_windll = types.SimpleNamespace(gdi32=fake_gdi, user32=fake_user)

        subject = oc.UnifiedOverlay.__new__(oc.UnifiedOverlay)
        subject._host = types.SimpleNamespace(hwnd=123)
        subject._host_rgn_key = 'previous'

        old_windll = oc._ct.windll
        old_gdi = oc._gdi32
        oc._ct.windll = fake_windll
        oc._gdi32 = fake_gdi
        try:
            subject._sync_host_rgn(False)
            self.assertEqual(subject._host_rgn_key, 'previous')
            self.assertEqual(deleted, [90])

            subject._sync_host_rgn(False)
            self.assertEqual(subject._host_rgn_key, 'empty')
            self.assertEqual(deleted, [90])
            self.assertEqual(created, [90, 91])
        finally:
            oc._ct.windll = old_windll
            oc._gdi32 = old_gdi

    def test_empty_region_creation_failure_does_not_apply_or_advance_key(self):
        set_calls = []
        fake_gdi = types.SimpleNamespace(
            CreateRectRgn=_Callable(lambda *_args: 0),
            DeleteObject=_Callable(lambda *_args: True),
        )
        fake_user = types.SimpleNamespace(
            SetWindowRgn=_Callable(
                lambda *_args: set_calls.append(_args) or True),
        )
        subject = oc.UnifiedOverlay.__new__(oc.UnifiedOverlay)
        subject._host = types.SimpleNamespace(hwnd=123)
        subject._host_rgn_key = 'previous'
        old_windll = oc._ct.windll
        old_gdi = oc._gdi32
        oc._ct.windll = types.SimpleNamespace(
            gdi32=fake_gdi, user32=fake_user)
        oc._gdi32 = fake_gdi
        try:
            subject._sync_host_rgn(False)
        finally:
            oc._ct.windll = old_windll
            oc._gdi32 = old_gdi
        self.assertEqual(set_calls, [])
        self.assertEqual(subject._host_rgn_key, 'previous')

    def test_ext_create_failure_is_not_replaced_with_false_empty_success(self):
        fake_gdi = types.SimpleNamespace(
            ExtCreateRegion=_Callable(lambda *_args: 0),
            CreateRectRgn=_Callable(
                lambda *_args: (_ for _ in ()).throw(
                    AssertionError('non-empty build must not become empty'))),
        )
        old_gdi = oc._gdi32
        oc._gdi32 = fake_gdi
        try:
            self.assertIsNone(oc._build_region_from_rects([(0, 0, 2, 2)]))
        finally:
            oc._gdi32 = old_gdi


if __name__ == '__main__':
    unittest.main(verbosity=2)
