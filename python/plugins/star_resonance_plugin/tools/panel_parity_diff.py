# -*- coding: utf-8 -*-
"""Pixel-diff supplied ACT WebView reference PNGs against Entity/Tk renders.

Run from repository root:

    python sao_auto/tools/panel_parity_diff.py

The tool treats root-level ``_webref_{panel}.png`` files as immutable targets,
compares them with root-level ``_entity_{panel}.png`` renders, writes
``_diff_{panel}.png`` images, and emits ``_panel_parity_report.json``.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from PIL import Image, ImageChops, ImageEnhance, ImageStat

PANEL_KEYS = [
    'aggregate',
    'action_log',
    'mem_scope',
    'combatant',
    'data_source_health',
    'death_recap',
    'graph',
    'offline_import',
    'plugin_manager',
    'report_export',
    'skill_drilldown',
    'timeline_vcr',
    'trigger_timer',
]


def _repo_root(path: str | None) -> Path:
    if path:
        return Path(path).resolve()
    here = Path(__file__).resolve()
    return here.parents[2]


def _open_rgba(path: Path) -> Image.Image:
    return Image.open(path).convert('RGBA')


def _fit_canvas(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    if img.size == size:
        return img
    canvas = Image.new('RGBA', size, (0, 0, 0, 0))
    canvas.paste(img, (0, 0))
    return canvas


def _diff_stats(reference: Image.Image, candidate: Image.Image) -> dict[str, Any]:
    width = max(reference.width, candidate.width)
    height = max(reference.height, candidate.height)
    ref = _fit_canvas(reference, (width, height))
    ent = _fit_canvas(candidate, (width, height))
    diff = ImageChops.difference(ref, ent)
    stat = ImageStat.Stat(diff)
    extrema = diff.getextrema()
    max_delta = max(channel[1] for channel in extrema)
    hist = diff.convert('L').histogram()
    equal_pixels = hist[0]
    pixels = width * height
    mismatch_pixels = max(0, pixels - equal_pixels)
    mae = sum(stat.mean[:3]) / 3.0
    rms = sum(stat.rms[:3]) / 3.0
    return {
        'canvas_size': [width, height],
        'mismatch_pixels': mismatch_pixels,
        'mismatch_pct': (mismatch_pixels * 100.0 / pixels) if pixels else 0.0,
        'mae': mae,
        'rms': rms,
        'max_delta': int(max_delta),
        'diff_image': diff,
    }


def _write_diff(diff: Image.Image, out_path: Path) -> None:
    visible = ImageEnhance.Brightness(diff.convert('RGB')).enhance(4.0)
    visible.save(out_path)


def compare_panel(root: Path, key: str) -> dict[str, Any]:
    ref_path = root / f'_webref_{key}.png'
    ent_path = root / f'_entity_{key}.png'
    diff_path = root / f'_diff_{key}.png'
    row: dict[str, Any] = {
        'panel': key,
        'reference': str(ref_path),
        'entity': str(ent_path),
        'diff': str(diff_path),
        'ok': False,
        'missing': [],
    }
    if not ref_path.exists():
        row['missing'].append(str(ref_path))
    if not ent_path.exists():
        row['missing'].append(str(ent_path))
    if row['missing']:
        row['error'] = 'missing image'
        return row
    reference = _open_rgba(ref_path)
    candidate = _open_rgba(ent_path)
    stats = _diff_stats(reference, candidate)
    _write_diff(stats.pop('diff_image'), diff_path)
    row.update(stats)
    row['reference_size'] = list(reference.size)
    row['entity_size'] = list(candidate.size)
    row['size_match'] = reference.size == candidate.size
    row['ok'] = True
    return row


def _print_table(rows: list[dict[str, Any]], fail_over_pct: float | None) -> None:
    print(f"{'panel':22} {'web':11} {'entity':11} {'mismatch%':>10} {'rms':>9} {'max':>5} status")
    for row in rows:
        if not row.get('ok'):
            print(f"{row['panel']:22} {'-':11} {'-':11} {'-':>10} {'-':>9} {'-':>5} MISSING")
            continue
        mismatch = float(row.get('mismatch_pct') or 0.0)
        status = 'OK'
        if fail_over_pct is not None and mismatch > fail_over_pct:
            status = 'FAIL'
        if not row.get('size_match'):
            status += '+SIZE'
        print(
            f"{row['panel']:22} "
            f"{row['reference_size'][0]}x{row['reference_size'][1]:<6} "
            f"{row['entity_size'][0]}x{row['entity_size'][1]:<6} "
            f"{mismatch:10.3f} "
            f"{float(row.get('rms') or 0.0):9.3f} "
            f"{int(row.get('max_delta') or 0):5d} {status}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Diff ACT WebView reference PNGs against Entity/Tk renders.')
    parser.add_argument('--root', help='Repository root containing _webref_*.png and _entity_*.png')
    parser.add_argument('--fail-over-pct', type=float, default=None, help='Exit non-zero if any panel mismatch percentage exceeds this value')
    parser.add_argument('--json-out', default='_panel_parity_report.json', help='JSON report path relative to root')
    args = parser.parse_args(argv)

    root = _repo_root(args.root)
    rows = [compare_panel(root, key) for key in PANEL_KEYS]
    report = {
        'root': str(root),
        'panels': rows,
        'summary': {
            'count': len(rows),
            'ok': sum(1 for row in rows if row.get('ok')),
            'missing': sum(1 for row in rows if not row.get('ok')),
            'max_mismatch_pct': max((float(row.get('mismatch_pct') or 0.0) for row in rows), default=0.0),
            'avg_mismatch_pct': sum(float(row.get('mismatch_pct') or 0.0) for row in rows) / len(rows),
        },
    }
    out_path = root / args.json_out
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    _print_table(rows, args.fail_over_pct)
    print(f'json {out_path}')
    if any(not row.get('ok') for row in rows):
        return 1
    if args.fail_over_pct is not None:
        for row in rows:
            if float(row.get('mismatch_pct') or 0.0) > args.fail_over_pct:
                return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
