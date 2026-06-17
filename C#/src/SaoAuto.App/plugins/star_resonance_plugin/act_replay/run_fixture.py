# -*- coding: utf-8 -*-
"""Run a normalized ACT replay JSON/JSONL fixture and print a summary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict

from plugins.star_resonance_plugin.engines.act_trigger_engine import ActTriggerEngine

from .fixture_io import load_fixture_events
from .harness import ActReplayHarness
from .importer import load_normalized_import


def _summary(snapshot: Dict[str, Any]) -> Dict[str, Any]:
    render_spec = snapshot.get("render_spec") or {}
    live = snapshot.get("live") or {}
    triggers = snapshot.get("triggers") or {}
    sources = snapshot.get("sources") or {}
    return {
        "ok": True,
        "render_mode": render_spec.get("mode"),
        "data_source": (sources.get("summary") or sources.get("packet") or {}).get("data_source"),
        "total_damage": live.get("total_damage"),
        "entities": len(live.get("entities") or []),
        "render_rows": len(render_spec.get("rows") or []),
        "trigger_events": len(triggers.get("emitted") or triggers.get("recent") or []),
        "target_hp_pct": (render_spec.get("target") or render_spec.get("boss") or {}).get("hp_pct"),
        "dungeon_id": (render_spec.get("context") or {}).get("dungeon_id"),
        "dungeon_scene_id": (render_spec.get("context") or {}).get("dungeon_scene_id"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Replay a normalized ACT JSON/JSONL fixture")
    parser.add_argument("fixture", nargs="?", default="demo_events", help="fixture name or JSONL path")
    parser.add_argument("--rules", help="optional JSON file containing an act_trigger_rules array or a rule list")
    parser.add_argument("--self-uid", type=int, default=0, help="override self UID")
    parser.add_argument("--full", action="store_true", help="print the full ACT snapshot instead of a summary")
    args = parser.parse_args(argv)

    candidate = Path(args.fixture)
    if candidate.exists():
        self_uid, events = load_normalized_import(candidate)
    else:
        self_uid, events = load_fixture_events(args.fixture)
    if args.self_uid > 0:
        self_uid = args.self_uid

    rules = []
    if args.rules:
        raw_rules = json.loads(Path(args.rules).read_text(encoding="utf-8"))
        if isinstance(raw_rules, dict):
            raw_rules = raw_rules.get("act_trigger_rules") or raw_rules.get("rules") or []
        rules = raw_rules if isinstance(raw_rules, list) else []

    trigger_engine = ActTriggerEngine(rules)
    harness = ActReplayHarness(
        trigger_engine=trigger_engine,
        source_probe={"data_source": "fixture", "running": True, "error_msg": ""},
    )
    if self_uid > 0:
        harness.set_self_uid(self_uid)
    snapshot = harness.replay(events)
    output = snapshot if args.full else _summary(snapshot)
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
