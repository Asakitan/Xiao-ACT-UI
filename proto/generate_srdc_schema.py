#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Extract a machine-readable field index from SRDC's BlueProtobuf_pb.js."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'StarResonanceDamageCounter' / 'algo' / 'BlueProtobuf_pb.js'
OUT = Path(__file__).resolve().with_name('srdc_blueprotobuf_schema.json')

FIELD_BLOCK_RE = re.compile(
    r'/\*\*(?P<comment>.*?)\*/\s*proto\.(?P<message>[A-Za-z0-9_]+)\.prototype\.get(?P<getter>[A-Za-z0-9_]+)(?P<suffix>List|Map)?\s*=\s*function',
    re.S,
)
COMMENT_FIELD_RE = re.compile(
    r'\*\s+(?:(?P<label>optional|required|repeated)\s+)?(?P<type>map<[^>]+>|[A-Za-z0-9_.<>]+)\s+(?P<field>[A-Za-z0-9_]+)\s*=\s*(?P<number>\d+);'
)


def main() -> int:
    if not SRC.exists():
        raise SystemExit(f'Source file not found: {SRC}')

    text = SRC.read_text(encoding='utf-8')
    messages = {}

    for match in FIELD_BLOCK_RE.finditer(text):
        comment = match.group('comment')
        comment_match = COMMENT_FIELD_RE.search(comment)
        if not comment_match:
            continue
        message_name = match.group('message')
        suffix = match.group('suffix') or ''
        field_info = {
            'number': int(comment_match.group('number')),
            'name': comment_match.group('field'),
            'getter': match.group('getter'),
            'type': comment_match.group('type'),
            'label': comment_match.group('label') or ('map' if suffix == 'Map' else 'singular'),
            'accessor_suffix': suffix,
        }
        messages.setdefault(message_name, []).append(field_info)

    for fields in messages.values():
        fields.sort(key=lambda item: item['number'])

    payload = {
        'source': str(SRC.relative_to(ROOT)),
        'message_count': len(messages),
        'messages': messages,
    }
    OUT.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding='utf-8')
    print(f'Wrote {OUT} ({len(messages)} messages)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
