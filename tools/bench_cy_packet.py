# -*- coding: utf-8 -*-
"""Bench and parity-check mandatory Cython packet helpers."""
from __future__ import annotations

import argparse
import os
import random
import statistics
import struct
import sys
import time
from typing import Callable, Dict, Iterable, List, Tuple

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_THIS_DIR)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

import _sao_cy_packet as cy_packet  # type: ignore[import-not-found]


def _encode_varint(value: int) -> bytes:
    value = int(value)
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def _key(field_num: int, wire_type: int) -> bytes:
    return _encode_varint((int(field_num) << 3) | int(wire_type))


def _ref_read_varint(data: bytes, pos: int) -> Tuple[int, int]:
    result = 0
    shift = 0
    while pos < len(data):
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            return result, pos
        shift += 7
    return result, pos


def _ref_decode_fields(data: bytes) -> Dict[int, list]:
    fields: Dict[int, list] = {}
    pos = 0
    length = len(data)
    while pos < length:
        tag, pos = _ref_read_varint(data, pos)
        field_num = tag >> 3
        wire_type = tag & 0x07
        if wire_type == 0:
            val, pos = _ref_read_varint(data, pos)
            fields.setdefault(field_num, []).append(val)
        elif wire_type == 1:
            if pos + 8 > length:
                break
            val = struct.unpack_from("<q", data, pos)[0]
            pos += 8
            fields.setdefault(field_num, []).append(val)
        elif wire_type == 2:
            vlen, pos = _ref_read_varint(data, pos)
            if pos + vlen > length:
                break
            val = data[pos:pos + vlen]
            pos += vlen
            fields.setdefault(field_num, []).append(val)
        elif wire_type == 5:
            if pos + 4 > length:
                break
            val = struct.unpack_from("<f", data, pos)[0]
            pos += 4
            fields.setdefault(field_num, []).append(val)
        else:
            break
    return fields


def _ref_decode_int32_from_raw(raw: bytes) -> int:
    if not raw:
        return 0
    val, _ = _ref_read_varint(raw, 0)
    if val > 0x7FFFFFFF:
        val -= 0x100000000
    return val


def _ref_decode_float32_from_raw(raw: bytes):
    if not raw or len(raw) < 4:
        return None
    return struct.unpack_from("<f", raw, 0)[0]


def _ref_scan_c3sb_nested(data: bytes) -> bool:
    sig = b"\x00\x63\x33\x53\x42\x00"
    offset = 0
    while offset + 4 < len(data):
        try:
            plen = struct.unpack_from(">I", data, offset)[0]
        except struct.error:
            break
        if plen < 6 or plen > 0xFFFFF:
            break
        end = offset + plen
        if end > len(data):
            break
        payload = data[offset + 4:end]
        if len(payload) > 11 and payload[5:5 + 6] == sig:
            return True
        offset = end
    return False


def _ref_find_frame_realign(data: bytes, max_scan: int = 65536) -> int:
    scan_end = min(len(data) - 5, max_scan)
    for i in range(1, scan_end):
        sz = struct.unpack_from(">I", data, i)[0]
        if 6 <= sz <= 0x0FFFFF:
            tp = struct.unpack_from(">H", data, i + 4)[0]
            msg = tp & 0x7FFF
            if msg in (2, 3, 4, 5, 6):
                return i
    return -1


def _field_samples() -> List[bytes]:
    rng = random.Random(0x53414F)
    samples: List[bytes] = []
    for idx in range(512):
        blob = bytearray()
        blob += _key(1, 0) + _encode_varint(rng.randrange(0, 2**32))
        text = f"player-{idx}-{rng.randrange(9999)}".encode("utf-8")
        blob += _key(2, 2) + _encode_varint(len(text)) + text
        blob += _key(3, 5) + struct.pack("<f", rng.random() * 1000.0)
        signed = rng.randrange(-(2**40), 2**40)
        blob += _key(4, 1) + struct.pack("<q", signed)
        if idx % 3 == 0:
            blob += _key(1, 0) + _encode_varint(idx)
        if idx % 17 == 0:
            blob += _key(9, 7)
        samples.append(bytes(blob))
    samples.append(_key(7, 2) + _encode_varint(32) + b"short")
    samples.append(_key(8, 1) + b"\x01\x02")
    return samples


def _scan_samples() -> Tuple[List[bytes], List[Tuple[bytes, int]]]:
    payload = b"abcde" + b"\x00\x63\x33\x53\x42\x00" + b"zzzzzz"
    nested_hit = struct.pack(">I", len(payload) + 4) + payload
    nested_miss = struct.pack(">I", 14) + b"no-signature"
    nested_bad = b"\xff\xff\xff\xffgarbage"

    valid = struct.pack(">IH", 9, 6) + b"abc"
    realign = b"\x99bad!" + valid + b"tail"
    no_realign = b"\x99bad!" + struct.pack(">IH", 2, 99) + b"tail"
    return [nested_hit, nested_miss, nested_bad], [
        (realign, 5),
        (valid, -1),
        (no_realign, -1),
    ]


def _time_call(fn: Callable[[], object], loops: int) -> List[float]:
    samples: List[float] = []
    for _ in range(loops):
        t0 = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - t0)
    return samples


def _stats(samples: List[float]) -> Tuple[float, float, float]:
    ordered = sorted(samples)
    avg = statistics.fmean(ordered) * 1000.0
    p50 = ordered[int(round((len(ordered) - 1) * 0.50))] * 1000.0
    p95 = ordered[int(round((len(ordered) - 1) * 0.95))] * 1000.0
    return avg, p50, p95


def _print_result(label: str, py_samples: List[float],
                  cy_samples: List[float]) -> None:
    py_avg, py_p50, py_p95 = _stats(py_samples)
    cy_avg, cy_p50, cy_p95 = _stats(cy_samples)
    speed = py_avg / cy_avg if cy_avg > 0 else 0.0
    print(
        f"{label:30s} "
        f"py avg={py_avg:7.3f} p50={py_p50:7.3f} p95={py_p95:7.3f} ms  "
        f"cy avg={cy_avg:7.3f} p50={cy_p50:7.3f} p95={cy_p95:7.3f} ms  "
        f"{speed:5.2f}x"
    )


def _assert_parity() -> None:
    for sample in _field_samples():
        assert cy_packet.decode_fields(sample) == _ref_decode_fields(sample)
    for value in (0, 1, 127, 128, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF):
        raw = _encode_varint(value)
        assert cy_packet.decode_int32_from_raw(raw) == _ref_decode_int32_from_raw(raw)
    for value in (0.0, 1.5, -3.25, 12345.125):
        raw = struct.pack("<f", value)
        assert cy_packet.decode_float32_from_raw(raw) == _ref_decode_float32_from_raw(raw)
    nested_samples, realign_samples = _scan_samples()
    for sample in nested_samples:
        assert bool(cy_packet.scan_c3sb_nested(sample)) == _ref_scan_c3sb_nested(sample)
    for sample, expected in realign_samples:
        assert cy_packet.find_frame_realign(sample, 65536) == expected
        assert _ref_find_frame_realign(sample, 65536) == expected


def _loop_decode_fields(samples: Iterable[bytes], fn: Callable[[bytes], object]) -> None:
    for sample in samples:
        fn(sample)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assert-parity", action="store_true")
    args = parser.parse_args()

    _assert_parity()
    if args.assert_parity:
        print("Parity OK")

    field_samples = _field_samples()
    nested_samples, realign_samples = _scan_samples()
    print("Cython packet helper benchmark")
    _print_result(
        "decode_fields batch",
        _time_call(lambda: _loop_decode_fields(field_samples, _ref_decode_fields), 1000),
        _time_call(lambda: _loop_decode_fields(field_samples, cy_packet.decode_fields), 1000),
    )
    raw_values = [_encode_varint(v) for v in (0, 1, 127, 128, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)] * 128
    _print_result(
        "decode_int32 raw batch",
        _time_call(lambda: [_ref_decode_int32_from_raw(v) for v in raw_values], 3000),
        _time_call(lambda: [cy_packet.decode_int32_from_raw(v) for v in raw_values], 3000),
    )
    scan_buf = b"x" * 2048 + realign_samples[0][0]
    _print_result(
        "frame realign scan",
        _time_call(lambda: _ref_find_frame_realign(scan_buf, 65536), 2000),
        _time_call(lambda: cy_packet.find_frame_realign(scan_buf, 65536), 2000),
    )
    _print_result(
        "nested c3SB scan",
        _time_call(lambda: [_ref_scan_c3sb_nested(v) for v in nested_samples], 5000),
        _time_call(lambda: [cy_packet.scan_c3sb_nested(v) for v in nested_samples], 5000),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
