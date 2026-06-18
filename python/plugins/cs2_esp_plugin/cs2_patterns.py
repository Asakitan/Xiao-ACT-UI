# -*- coding: utf-8 -*-
"""CS2 ESP plugin — client.dll pattern scanner.

Resolves RVAs by scanning ``client.dll`` memory for byte patterns with
wildcards and a RIP-relative capture marker. This lets the plugin survive
CS2 Steam updates without manually refreshing ``offsets.json``: the
patterns (instruction opcodes around a ``mov/lea [rip+disp], reg``) are
far more stable across updates than the resolved RVAs themselves.

Pattern syntax (whitespace-separated tokens):
  ``XX``    literal byte (2 hex digits, case-insensitive)
  ``??``    single wildcard byte
  ``{rip}`` RIP-relative capture: read int32 LE at this position, the
            captured RVA = (position + 4) + disp32. The first ``{rip}``
            in a pattern is the value returned for that name.

Only the first match per pattern is used. Scanning reads the module in
chunks (with pattern-length overlap at chunk boundaries) so a 50 MB
``client.dll`` stays tractable for a one-time startup scan through the
read-only Engine A channel.

Patterns are translated from cs2-dumper's pelite pattern DSL
(``src/analysis/offsets.rs``): pelite ``${'}`` (RIP-relative reference)
→ ``{rip}``; pelite ``u4`` / ``?`` → ``?? ?? ?? ??`` / ``??``.
"""

from __future__ import annotations

from typing import List, Optional, Sequence, Tuple

# Scan in 4 MB chunks. Patterns are short (<32 bytes), so a 64-byte
# overlap between consecutive chunks covers any cross-boundary match.
CHUNK_SIZE = 4 * 1024 * 1024
CHUNK_OVERLAP = 64


class Pattern:
    """A compiled pattern: list of (byte_or_None) literals plus capture index."""

    __slots__ = ("name", "literals", "capture_offset", "length")

    def __init__(self, name: str, tokens: Sequence[str]) -> None:
        self.name = name
        literals: List[Optional[int]] = []
        capture_offset = -1
        for tok in tokens:
            t = tok.strip()
            if not t:
                continue
            if t == "{rip}":
                if capture_offset >= 0:
                    raise ValueError(f"pattern {name!r}: multiple {{rip}} captures not supported")
                capture_offset = len(literals)
                # The displacement is 4 bytes; represent as 4 wildcard bytes
                # (the capture math reads them directly).
                literals.extend([None, None, None, None])
            elif t in ("??", "?"):
                literals.append(None)
            else:
                try:
                    literals.append(int(t, 16))
                except ValueError as exc:
                    raise ValueError(f"pattern {name!r}: bad token {tok!r}") from exc
        if not literals:
            raise ValueError(f"pattern {name!r}: empty pattern")
        self.literals = literals
        self.capture_offset = capture_offset
        self.length = len(literals)

    @property
    def has_capture(self) -> bool:
        return self.capture_offset >= 0


def compile_pattern(name: str, pattern_str: str) -> Pattern:
    return Pattern(name, pattern_str.split())


def _match_at(pat: Pattern, buf: bytes, offset: int) -> bool:
    lit = pat.literals
    n = pat.length
    if offset + n > len(buf):
        return False
    for i in range(n):
        b = lit[i]
        if b is not None and buf[offset + i] != b:
            return False
    return True


def _capture_rva(pat: Pattern, buf: bytes, match_offset: int, chunk_base: int) -> int:
    """Return the RVA the {rip} capture points at, or the match RVA if none.

    ``chunk_base`` is the RVA of byte 0 of ``buf``. The RIP-relative
    target = (chunk_base + match_offset + capture_offset + 4) + disp32,
    i.e. the instruction byte *after* the 4-byte displacement plus the
    signed displacement — the standard x86-64 RIP-relative addressing.
    """
    if not pat.has_capture:
        return chunk_base + match_offset
    co = pat.capture_offset
    import struct
    disp = struct.unpack_from("<i", buf, match_offset + co)[0]
    next_ip = chunk_base + match_offset + co + 4
    return next_ip + disp


def scan_chunk(pat: Pattern, buf: bytes, chunk_base: int) -> Optional[int]:
    """Scan one buffer; return the captured RVA for the first match or None."""
    n = pat.length
    limit = len(buf) - n
    if limit < 0:
        return None
    lit0 = pat.literals[0]
    # Linear scan; patterns are short and this runs once at startup.
    for i in range(limit + 1):
        if lit0 is not None and buf[i] != lit0:
            continue
        if _match_at(pat, buf, i):
            return _capture_rva(pat, buf, i, chunk_base)
    return None


def scan_module(read_fn, module_base: int, module_size: int,
                patterns: Sequence[Pattern]) -> dict:
    """Scan a module in chunks and resolve every pattern's RVA.

    ``read_fn(addr, size) -> Optional[bytes]`` is the reader's batch read
    callback (typically ``EngineAReader.read``). Returns ``{name: rva}``;
    patterns that did not match are omitted from the result.
    """
    resolved: dict = {}
    pending = list(patterns)
    if not pending or module_size <= 0:
        return resolved
    max_pat = max(p.length for p in pending)
    overlap = max(CHUNK_OVERLAP, max_pat)
    offset = 0
    while offset < module_size and pending:
        chunk_len = min(CHUNK_SIZE, module_size - offset)
        # Read a bit past chunk_len so cross-boundary matches land inside
        # the buffer; the next iteration re-scans the overlap region.
        read_len = min(chunk_len + overlap, module_size - offset)
        buf = read_fn(module_base + offset, read_len)
        if not buf:
            break
        chunk_base = offset
        still_pending = []
        for pat in pending:
            rva = scan_chunk(pat, buf, chunk_base)
            if rva is not None and 0 < rva < module_size:
                resolved[pat.name] = rva
            else:
                still_pending.append(pat)
        pending = still_pending
        offset += chunk_len
    return resolved


# ── CS2 pattern definitions (from a2x/cs2-dumper src/analysis/offsets.rs) ──
# These are the opcode sequences immediately around the RIP-relative
# reference that stores/loads each global. They survive most CS2 updates
# because the surrounding instructions rarely change even when the
# resolved RVA shifts.

PATTERNS = {
    # dwEntityList: mov [rip+disp], rcx ; jmp rel32 ; int3
    "dwEntityList": "48 89 0D {rip} E9 ?? ?? ?? ?? CC",
    # dwViewMatrix: lea rcx, [rip+disp] ; shl rax, 6
    "dwViewMatrix": "48 8D 0D {rip} 48 C1 E0 06",
    # dwLocalPlayerController: mov rax, [rip+disp] ; mov [r14+...], edi
    "dwLocalPlayerController": "48 8B 05 {rip} 41 89 BE",
    # dwPrediction: lea rax, [rip+disp] ; ret ; padding ; push rbx ; push rsi ; push r12
    # (cs2-dumper derives dwLocalPlayerPawn from this via a secondary scan;
    #  we keep it available for future use but do not wire the derivation.)
    "dwPrediction": "48 8D 05 {rip} C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 40 53 56 41 54",
}


def default_patterns() -> List[Pattern]:
    """Compiled patterns for the RVAs the scanner knows how to resolve."""
    return [compile_pattern(name, spec) for name, spec in PATTERNS.items()]
