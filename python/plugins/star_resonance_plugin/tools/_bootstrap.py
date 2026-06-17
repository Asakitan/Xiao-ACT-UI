# -*- coding: utf-8 -*-
"""Selftest bootstrap for direct `python tools/<script>.py` runs."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PY_ROOT = ROOT.parent.parent
PROTOCOL_ROOT = ROOT / "protocol"
CYTHON_ROOT = ROOT / "cython"

for path in (PY_ROOT, ROOT, PROTOCOL_ROOT, CYTHON_ROOT):
    text = str(path)
    if text not in sys.path:
        sys.path.insert(0, text)
