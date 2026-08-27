# -*- coding: utf-8 -*-
# Shared self-signed TLS certificate SPKI pinning primitives.

from __future__ import annotations

import base64
import hashlib
import re
from pathlib import Path

EXPECTED_SPKI_SHA256 = bytes.fromhex(
    "d3171ec5b86303233b6abda8e83142293d6910099aedd0cdac947d276006db0a"
)
_CERTIFICATE_RE = re.compile(
    br"-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----",
    re.DOTALL,
)


def _read_tlv(data: bytes, offset: int) -> tuple[int, int, int, int]:
    if offset >= len(data):
        raise ValueError("DER tag is truncated")
    tag = data[offset]
    cursor = offset + 1
    if cursor >= len(data):
        raise ValueError("DER length is truncated")
    first_length = data[cursor]
    cursor += 1
    if first_length & 0x80:
        length_size = first_length & 0x7F
        if length_size == 0 or length_size > 4 or cursor + length_size > len(data):
            raise ValueError("DER length is invalid")
        length = int.from_bytes(data[cursor:cursor + length_size], "big")
        cursor += length_size
    else:
        length = first_length
    end = cursor + length
    if end > len(data):
        raise ValueError("DER value is truncated")
    return tag, offset, cursor, end


def _children(data: bytes, value_start: int, value_end: int) -> list[tuple[int, int, int, int]]:
    result = []
    cursor = value_start
    while cursor < value_end:
        item = _read_tlv(data, cursor)
        if item[3] > value_end:
            raise ValueError("DER child exceeds parent")
        result.append(item)
        cursor = item[3]
    if cursor != value_end:
        raise ValueError("DER sequence is malformed")
    return result


def subject_public_key_info(cert_der: bytes) -> bytes:
    outer = _read_tlv(cert_der, 0)
    if outer[0] != 0x30 or outer[3] != len(cert_der):
        raise ValueError("certificate DER is not a complete sequence")
    certificate_parts = _children(cert_der, outer[2], outer[3])
    if len(certificate_parts) < 3 or certificate_parts[0][0] != 0x30:
        raise ValueError("certificate structure is invalid")
    tbs = _children(cert_der, certificate_parts[0][2], certificate_parts[0][3])
    spki_index = 6 if tbs and tbs[0][0] == 0xA0 else 5
    if len(tbs) <= spki_index or tbs[spki_index][0] != 0x30:
        raise ValueError("certificate SPKI is missing")
    spki = tbs[spki_index]
    _children(cert_der, spki[2], spki[3])
    return cert_der[spki[1]:spki[3]]


def spki_sha256(cert_der: bytes) -> bytes:
    return hashlib.sha256(subject_public_key_info(cert_der)).digest()


def certificate_der_from_pem(pem_bytes: bytes) -> bytes:
    match = _CERTIFICATE_RE.search(pem_bytes)
    if match is None:
        raise ValueError("TLS certificate PEM block is missing")
    try:
        return base64.b64decode(re.sub(br"\s+", b"", match.group(1)), validate=True)
    except (ValueError, TypeError) as exc:
        raise ValueError("TLS certificate PEM block is invalid") from exc


def certificate_file_spki_sha256(path: str | Path) -> bytes:
    return spki_sha256(certificate_der_from_pem(Path(path).read_bytes()))


def validate_certificate_file(path: str | Path) -> None:
    actual = certificate_file_spki_sha256(path)
    if actual != EXPECTED_SPKI_SHA256:
        raise RuntimeError(
            "TLS certificate SPKI pin mismatch: "
            f"expected {EXPECTED_SPKI_SHA256.hex()}, got {actual.hex()}"
        )


def validate_peer_certificate(cert_der: bytes) -> None:
    actual = spki_sha256(cert_der)
    if actual != EXPECTED_SPKI_SHA256:
        raise RuntimeError(
            "TLS peer SPKI pin mismatch: "
            f"expected {EXPECTED_SPKI_SHA256.hex()}, got {actual.hex()}"
        )