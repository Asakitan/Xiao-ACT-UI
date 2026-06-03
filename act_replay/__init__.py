# -*- coding: utf-8 -*-
"""Replay helpers for TCP-first ACT development.

The package intentionally works with normalized event dictionaries rather than
live Npcap input so analytics, triggers, and UI render specs can be validated
offline before touching packet capture or Cython hot paths.
"""

from .harness import ActReplayHarness
from .timeline import replay_timeline_status

__all__ = ["ActReplayHarness", "replay_timeline_status"]
