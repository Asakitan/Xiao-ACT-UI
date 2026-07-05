# -*- coding: utf-8 -*-
# live_overlay_writer - verified local name-overlay writer.
#
# Runtime memory/TCP readers discover authoritative in-game Chinese names that may
# be missing or stale in the checked-in static tables. This helper writes ONLY to
# the local runtime TCP preparse cache (`tcp_preparse_name_cache.local.json` via
# `TcpNameCache.observe_name`) after validation.
#
# Safety rules:
# - never writes the shared `tcp_preparse_name_cache.json`;
# - validates text is printable and non-empty;
# - validates id/kind against TcpNameCache's normal observe_name path;
# - can require N consistent observations before persisting (default 1 to preserve
# existing immediate nameplate overlay behavior; callers can pass 3 for stricter
# confirmation without changing the storage contract).
from __future__ import annotations

import re
from collections import defaultdict
from typing import Any, DefaultDict, Tuple

_CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")


class LiveNameOverlayWriter:
    # Validated adapter around `TcpNameCache.observe_name`.
    #
    # The writer is intentionally tiny: it does not own paths or JSON format; it
    # delegates persistence to the existing TcpNameCache so load order, autosave,
    # sanitization, and `.local.json` path behavior stay unchanged.

    def __init__(self, cache, *, min_confirmations: int = 1,
                 source: str = "mem_nameplate", confidence: str = "mem") -> None:
        self.cache = cache
        self.min_confirmations = max(1, int(min_confirmations or 1))
        self.source = str(source or "mem_nameplate")
        self.confidence = str(confidence or "mem")
        self._seen: DefaultDict[Tuple[str, int, str], int] = defaultdict(int)

    @staticmethod
    def valid_text(text: Any) -> bool:
        s = str(text or "").strip()
        if not s or len(s) > 64:
            return False
        if _CONTROL_RE.search(s):
            return False
        return True

    @staticmethod
    def valid_kind(kind: Any) -> bool:
        return str(kind or "").strip() in {"monster", "boss", "npc", "dungeon", "scene"}

    @staticmethod
    def valid_id(id_: Any) -> bool:
        try:
            return int(id_) > 0
        except Exception:
            return False

    def observe(self, kind: Any, id_: Any, text: Any, *, context: dict | None = None) -> bool:
        # Validate and maybe persist one name.
        #
        # Returns True only when the observation was persisted (i.e. the consistency
        # threshold was reached and observe_name was called successfully). False means
        # validation failed or more confirmations are needed.
        if self.cache is None:
            return False
        if not (self.valid_kind(kind) and self.valid_id(id_) and self.valid_text(text)):
            return False
        k = str(kind).strip()
        iid = int(id_)
        s = str(text).strip()
        key = (k, iid, s)
        self._seen[key] += 1
        if self._seen[key] < self.min_confirmations:
            return False
        self.cache.observe_name(k, iid, s, source=self.source,
                                confidence=self.confidence,
                                context=context or {"verified_by": "live_overlay_writer"})
        return True


__all__ = ["LiveNameOverlayWriter"]
