"""Generate `dps_snapshot.json` from a recorded JSONL damage-event tape.

Drives the canonical Python `DpsTracker.add_event` loop, then dumps
`get_snapshot(include_skills=True)` through the canonical envelope.

The C# parity test (`SaoAuto.ParityTests/DpsSnapshotParityTests.cs` —
to be added in a later session) replays the same tape against the
ported `DpsTracker` and compares with `numericEpsilon ≈ 1.0`
(integer damage counters), allowing for harmless float rounding on
DPS averages.

Tape line schema (one JSON object per line):

    {"t": 1234.5, "src": 17, "dst": 42, "skill": 9876,
     "damage": 12345, "is_player": true, ...}

Unknown keys are forwarded verbatim to `DpsTracker.add_event`.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from _common import add_common_args, import_canonical_runtime, write_fixture


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=Path,
        required=True,
        help="JSONL damage-event tape (one event per line).",
    )
    add_common_args(parser, "dps_snapshot.json")
    args = parser.parse_args()

    if not args.input.exists():
        raise SystemExit(f"Tape not found: {args.input}")

    import_canonical_runtime()
    from sao_auto.dps_tracker import DpsTracker  # type: ignore

    tracker = DpsTracker()
    n_events = 0
    with args.input.open("r", encoding="utf-8") as fh:
        for line_no, raw in enumerate(fh, 1):
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            try:
                evt = json.loads(raw)
            except json.JSONDecodeError as e:
                raise SystemExit(f"{args.input}:{line_no}: invalid JSON ({e})")
            tracker.add_event(evt)
            n_events += 1

    snapshot = tracker.get_snapshot(include_skills=True)
    write_fixture(
        args.output,
        kind="dps_snapshot",
        source=f"{args.input.name} ({n_events} events)",
        data=snapshot,
        force=args.force,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
