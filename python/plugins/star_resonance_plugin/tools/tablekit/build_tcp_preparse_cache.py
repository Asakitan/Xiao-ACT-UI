# -*- coding: utf-8 -*-
# Build compact TCP pre-parse name cache from live probe matched rows.
#
# This keeps the full live_probe_act_matched_rows.json as provenance and writes a
# small runtime cache consumed by tools.tablekit.name_tables and PacketBridge.
from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from plugins.star_resonance_plugin.net.tcp_name_cache import build_index_from_live_rows, sanitize_shared_cache, shared_cache_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        default=os.path.join(ROOT, "assets", "name_tables", "live_probe_act_matched_rows.json"),
        help="Path to live_probe_act_matched_rows.json",
    )
    parser.add_argument(
        "--output",
        default=shared_cache_path(),
        help="Path to write compact tcp_preparse_name_cache.json",
    )
    args = parser.parse_args(argv)

    if not os.path.isfile(args.input):
        print(f"input not found: {args.input}", file=sys.stderr)
        return 2
    with open(args.input, "r", encoding="utf-8") as f:
        rows_obj = json.load(f)
    index = sanitize_shared_cache(build_index_from_live_rows(rows_obj))
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    by_kind = index.get("names", {}).get("by_kind", {})
    summary = {kind: len(rows or {}) for kind, rows in by_kind.items()}
    print(json.dumps({"output": args.output, "summary": summary}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
