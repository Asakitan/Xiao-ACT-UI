#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Compare Python zstd output with the optional Node22 SRDC parity helper."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

import zstandard


ROOT = Path(__file__).resolve().parents[2]
NODE_HELPER = Path(__file__).resolve().with_name('node22_srdc_parity.js')


def py_decompress(payload: bytes) -> bytes:
    dctx = zstandard.ZstdDecompressor(max_window_size=2**25)
    with dctx.stream_reader(payload) as reader:
        chunks = []
        while True:
            chunk = reader.read(65536)
            if not chunk:
                break
            chunks.append(chunk)
    return b''.join(chunks)


def sha256(buffer: bytes) -> str:
    return hashlib.sha256(buffer).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('payload', help='Path to a zstd-compressed payload file')
    args = parser.parse_args()

    payload_path = Path(args.payload).resolve()
    payload = payload_path.read_bytes()
    py_output = py_decompress(payload)

    result = {
        'python': {
            'input_size': len(payload),
            'decompressed_size': len(py_output),
            'sha256': sha256(py_output),
        },
        'node22': None,
        'node_available': False,
    }

    node_bin = shutil.which('node')
    if node_bin and NODE_HELPER.exists():
        proc = subprocess.run(
            [node_bin, str(NODE_HELPER), '--base64', base64.b64encode(payload).decode('ascii')],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            result['node22'] = json.loads(proc.stdout.strip())
            result['node_available'] = True
            result['match'] = (
                result['python']['decompressed_size'] == result['node22']['decompressed_size'] and
                result['python']['sha256'] == result['node22']['sha256']
            )
        else:
            result['node_available'] = True
            result['node22_error'] = proc.stderr.strip() or proc.stdout.strip() or f'node exited {proc.returncode}'

    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
