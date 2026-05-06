"""Generate `recognition_*.json` from a screenshot PNG of the game window.

Drives the canonical Python `recognition_engine.recognize_frame(...)`
on a single PNG and dumps its structured output through the canonical
envelope.

The C# parity test (`SaoAuto.ParityTests/RecognitionParityTests.cs`,
landing in a later session) feeds the same PNG into the ported
`RecognitionEngine` and compares with `numericEpsilon ≈ 0.01`
(template-match scores can drift by ULP-class amounts between
runtimes).
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

from _common import add_common_args, import_canonical_runtime, write_fixture


def _sha256_short(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=Path,
        required=True,
        help="Screenshot PNG of the game window.",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="Fixture suffix (default: derived from input filename). "
             "Final filename: recognition_<name>.json",
    )
    add_common_args(parser, "recognition_default.json")
    args = parser.parse_args()

    if not args.input.exists():
        raise SystemExit(f"PNG not found: {args.input}")

    if args.output.name == "recognition_default.json":
        suffix = args.name or args.input.stem
        args.output = args.output.with_name(f"recognition_{suffix}.json")

    import_canonical_runtime()
    try:
        import numpy as np  # type: ignore
        import cv2  # type: ignore
    except ImportError:
        raise SystemExit(
            "numpy + opencv-python are required (`pip install numpy opencv-python`)."
        )
    try:
        from sao_auto.recognition_engine import recognize_frame  # type: ignore
    except ImportError as e:
        raise SystemExit(f"canonical runtime missing: {e}")

    img_bgr = cv2.imread(str(args.input), cv2.IMREAD_COLOR)
    if img_bgr is None:
        raise SystemExit(f"cv2 could not decode {args.input}")

    result = recognize_frame(img_bgr)
    if hasattr(result, "to_dict"):
        data = result.to_dict()
    elif isinstance(result, dict):
        data = result
    else:
        # Fall back to vars() for dataclass-like objects.
        data = {k: v for k, v in vars(result).items() if not k.startswith("_")}

    write_fixture(
        args.output,
        kind="recognition_snapshot",
        source=f"{args.input.name} sha256={_sha256_short(args.input)} "
               f"shape={img_bgr.shape[1]}x{img_bgr.shape[0]}",
        data=data,
        force=args.force,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
