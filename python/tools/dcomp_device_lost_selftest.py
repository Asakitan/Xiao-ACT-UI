# -*- coding: utf-8 -*-
"""Selftest: DCompBridge._note_present_hr device-loss handling.

Verifies the Present-HRESULT classifier that keeps a monitor power-off /
GPU reset from crash-exiting the app:
  - DXGI_ERROR_DEVICE_REMOVED / RESET / HUNG  → device lost: bridge is
    marked dead (so every present() short-circuits to SwapBuffers) and
    the GL interop state is torn down.
  - DXGI_STATUS_OCCLUDED (monitor off, device fine) and S_OK           → NOT a loss: bridge stays alive, keeps presenting.

Pure logic — constructs the bridge via object.__new__ to skip the real
D3D11/DComp device creation, so it needs no GPU.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from render import dcomp_bridge as db  # noqa: E402


def _make_bridge():
    """A DCompBridge with just the fields _note_present_hr touches, no
    real device (object.__new__ bypasses __init__'s D3D creation)."""
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

    if failures:
        print('\nFAIL:')
        for f in failures:
            print('  -', f)
        return 1
    print('\nAll device-loss classification checks passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
