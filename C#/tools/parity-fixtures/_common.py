"""Shared helpers for parity-fixture generators.

Every fixture is a JSON object with the canonical envelope:

    {"kind": "<subsystem>", "source": "<input pointer>", "data": {...}}

`ParityFixture.Load(...)` on the C# side reads the whole object; tests
assert against `data`.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Mapping

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_FIXTURE_DIR = REPO_ROOT / "C#" / "tests" / "SaoAuto.ParityTests" / "Fixtures"


def write_fixture(
    out_path: Path,
    kind: str,
    source: str,
    data: Mapping[str, Any],
    *,
    force: bool = False,
) -> None:
    """Write `{kind, source, data}` to `out_path` (UTF-8, sorted keys, indent=2)."""
    out_path = Path(out_path)
    if out_path.exists() and not force:
        raise SystemExit(
            f"Refusing to overwrite existing fixture {out_path} (pass --force)."
        )
    envelope = {"kind": kind, "source": source, "data": dict(data)}
    out_path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(envelope, indent=2, sort_keys=True, ensure_ascii=False)
    tmp = out_path.with_suffix(out_path.suffix + ".tmp")
    tmp.write_text(text + "\n", encoding="utf-8")
    os.replace(tmp, out_path)
    print(f"[parity-fixture] wrote {out_path}  ({out_path.stat().st_size} bytes)")


def add_common_args(parser: argparse.ArgumentParser, default_name: str) -> None:
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_FIXTURE_DIR / default_name,
        help=f"Output fixture path (default: tests/SaoAuto.ParityTests/Fixtures/{default_name}).",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite the fixture if it already exists.",
    )


def import_canonical_runtime() -> None:
    """Make `import sao_auto....` work when generators are run from anywhere."""
    sao_auto_root = REPO_ROOT
    if str(sao_auto_root) not in sys.path:
        sys.path.insert(0, str(sao_auto_root))
