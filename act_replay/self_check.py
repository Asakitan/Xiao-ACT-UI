# -*- coding: utf-8 -*-
"""Compatibility entry point for the ACT replay smoke test."""

from .selftest import build_demo_events, main

__all__ = ["build_demo_events", "main"]


if __name__ == "__main__":
    raise SystemExit(main())