# -*- coding: utf-8 -*-
"""build_break_time_cache — dump ALL MonsterTable BreakingContinueTime to cache.

Requires game process attached. Reads every row from MonsterTable and saves
template_id → BreakingContinueTime (seconds) to assets/break_time_cache.json.

Usage:
  python -m tools.build_break_time_cache
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    from engines.break_time_lookup import build_full_cache, _cache, _CACHE_PATH

    print("[build_break_time_cache] Connecting to game process...")
    count = build_full_cache()
    if count > 0:
        print(f"[OK] {count} new entries added, {len(_cache)} total → {_CACHE_PATH}")
    elif _cache:
        print(f"[OK] No new entries (cache already has {len(_cache)} entries)")
    else:
        print("[FAIL] Could not read MonsterTable — is the game running?")
        sys.exit(1)

    # Print summary
    vals = sorted(set(_cache.values()))
    print(f"\nBreakingContinueTime distribution:")
    for v in vals:
        ids = [k for k, val in _cache.items() if val == v]
        print(f"  {v:.1f}s: {len(ids)} monsters")


if __name__ == "__main__":
    main()
