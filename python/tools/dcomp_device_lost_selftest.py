# -*- coding: utf-8 -*-
# Selftest: DCompBridge._note_present_hr device-loss handling.
#
# Verifies the Present-HRESULT classifier that keeps a monitor power-off /
# GPU reset from crash-exiting the app:
# - DXGI_ERROR_DEVICE_REMOVED / RESET / HUNG  → device lost: bridge is
# marked dead (so every present() short-circuits to SwapBuffers) and
# the GL interop state is torn down.
# - DXGI_STATUS_OCCLUDED (monitor off, device fine) and S_OK           → NOT a loss: bridge stays alive, keeps presenting.
#
# Pure logic — constructs the bridge via object.__new__ to skip the real
# D3D11/DComp device creation, so it needs no GPU.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from render import dcomp_bridge as db  # noqa: E402


def _make_bridge():
    # A DCompBridge with just the fields _note_present_hr touches, no
    # real device (object.__new__ bypasses __init__'s D3D creation).
    b = object.__new__(db.DCompBridge)
    b._alive = True
    b._teardown_calls = 0

    def _fake_teardown():
        b._teardown_calls += 1
    b._teardown_gl_interop_state = _fake_teardown
    return b


def main() -> int:
    S_OK = 0x00000000
    cases_lost = [
        ('DEVICE_REMOVED', db._DXGI_ERROR_DEVICE_REMOVED),
        ('DEVICE_RESET', db._DXGI_ERROR_DEVICE_RESET),
        ('DEVICE_HUNG', db._DXGI_ERROR_DEVICE_HUNG),
    ]
    cases_alive = [
        ('S_OK', S_OK),
        ('OCCLUDED', db._DXGI_STATUS_OCCLUDED),
    ]

    failures = []

    for name, code in cases_lost:
        b = _make_bridge()
        # HRESULTs come back from _vc as a signed c_long; feed the signed
        # form to prove the & 0xFFFFFFFF normalisation inside works.
        signed = code - 0x100000000 if code & 0x80000000 else code
        lost = b._note_present_hr(signed)
        if not lost:
            failures.append(f'{name}: expected lost=True, got {lost}')
        if b._alive:
            failures.append(f'{name}: expected _alive=False after loss')
        if b._teardown_calls != 1:
            failures.append(
                f'{name}: expected 1 interop teardown, got {b._teardown_calls}')
        print(f'  {name:16} signed=0x{signed & 0xFFFFFFFF:08X} '
              f'lost={lost} alive={b._alive} teardown={b._teardown_calls}')

    for name, code in cases_alive:
        b = _make_bridge()
        lost = b._note_present_hr(code)
        if lost:
            failures.append(f'{name}: expected lost=False, got {lost}')
        if not b._alive:
            failures.append(f'{name}: expected _alive=True (not a loss)')
        if b._teardown_calls != 0:
            failures.append(
                f'{name}: expected 0 teardown, got {b._teardown_calls}')
        print(f'  {name:16} code=0x{code & 0xFFFFFFFF:08X} '
              f'lost={lost} alive={b._alive} teardown={b._teardown_calls}')

    # A second loss notification must stay idempotent (already dead).
    b = _make_bridge()
    b._note_present_hr(db._DXGI_ERROR_DEVICE_REMOVED - 0x100000000)
    b._note_present_hr(db._DXGI_ERROR_DEVICE_REMOVED - 0x100000000)
    if b._alive:
        failures.append('idempotent: expected _alive=False')
    print(f'  idempotent-2x    alive={b._alive} teardown={b._teardown_calls}')

    # ── device_removed(): proactive GetDeviceRemovedReason probe used
    # before the un-timeout'd interop lock (wglDXLockObjectsNV has no
    # timeout parameter and is driver-defined against an already-lost
    # device — see render_to_gpu_texture_begin's docstring). Mocks
    # db._vc so no real D3D11 device/COM pointer is needed.
    orig_vc = db._vc
    try:
        b = _make_bridge()
        b._d3d_dev = object()  # any non-None sentinel; _vc is mocked
        db._vc = lambda *a, **k: (
            db._DXGI_ERROR_DEVICE_REMOVED - 0x100000000)
        removed = b.device_removed()
        print(f'  device_removed() on a lost device: {removed} '
              f'(expect True), alive={b._alive} '
              f'teardown={b._teardown_calls}', flush=True)
        if not removed or b._alive or b._teardown_calls != 1:
            failures.append('device_removed(): did not classify a lost '
                            'device correctly')

        b2 = _make_bridge()
        b2._d3d_dev = object()
        db._vc = lambda *a, **k: 0  # S_OK
        removed2 = b2.device_removed()
        print(f'  device_removed() on a healthy device: {removed2} '
              f'(expect False), alive={b2._alive}', flush=True)
        if removed2 or not b2._alive or b2._teardown_calls != 0:
            failures.append('device_removed(): a healthy device was '
                            'misclassified as lost')
    finally:
        db._vc = orig_vc

    # render_to_gpu_texture_begin() must check device_removed() BEFORE
    # ever calling self._interop.lock() — a poisoned _interop (raises if
    # touched) proves the short-circuit actually prevents reaching the
    # risky call, not just that the method happens to return False.
    class _PoisonInterop:
        def lock(self, *_a, **_k):
            raise AssertionError(
                'interop.lock() was called despite a lost device')

    b3 = _make_bridge()
    b3._gl_interop_active = True
    b3._interop = _PoisonInterop()
    b3._gpu_tex_dx_handle = object()
    b3.device_removed = lambda: True  # simulate a confirmed-lost device
    began = b3.render_to_gpu_texture_begin()
    print(f'  render_to_gpu_texture_begin() with a lost device: '
          f'began={began} (expect False, no AssertionError above)',
          flush=True)
    if began:
        failures.append('render_to_gpu_texture_begin(): returned True '
                        'despite device_removed() reporting loss')

    if failures:
        print('\nFAIL:')
        for f in failures:
            print('  -', f)
        return 1
    print('\nAll device-loss classification checks passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
