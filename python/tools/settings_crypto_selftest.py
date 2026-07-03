# -*- coding: utf-8 -*-
"""Focused selftest for settings_crypto (AES-256-GCM + DPAPI envelope)."""

from __future__ import annotations

import base64
import json
import os
import sys
import tempfile
import traceback

# runs from tools/ — the importable package root is python/ (parent)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import settings_crypto as sc

_FAILURE_DETAIL_LINE_LIMIT = 80
_FINAL_FAILURE_POINT_HEADING = "FAILED CHECK POINTS (final):"


def _failure_detail_tail(detail: str) -> list[str]:
    lines = str(detail or "").splitlines()
    if len(lines) <= _FAILURE_DETAIL_LINE_LIMIT:
        return lines
    omitted = len(lines) - _FAILURE_DETAIL_LINE_LIMIT
    return [f"... omitted {omitted} earlier detail lines ...", *lines[-_FAILURE_DETAIL_LINE_LIMIT:]]


def _failure_point_reason(exc: BaseException) -> str:
    message = str(exc).strip()
    if message:
        first_line = message.splitlines()[0].strip()
        if first_line:
            return first_line
    return f"{type(exc).__name__} without detail"


def _print_final_failed_check_points(exc: BaseException, detail: str) -> None:
    print()
    print("=" * 50)
    print(_FINAL_FAILURE_POINT_HEADING)
    print(f"  1. {type(exc).__name__}: {_failure_point_reason(exc)}")
    detail_lines = _failure_detail_tail(detail)
    if detail_lines:
        print("     detail:")
        for line in detail_lines:
            print(f"       {line}")


def run_selftest() -> dict:
    sample = {
        "hotkeys": {"toggle_recognition": "F5"},
        "panel_themes": {"act": "dark"},
        "unicode_field": "你好世界",
        "nested": {"list": [1, 2, 3], "flag": True, "none": None},
    }

    # 1. round trip preserves data exactly.
    blob = sc.encode_settings(sample)
    decoded = sc.decode_settings(blob)
    assert decoded == sample, ("roundtrip mismatch", decoded, sample)

    # 2. envelope is recognizable and NOT plaintext.
    envelope = json.loads(blob.decode("utf-8"))
    assert envelope.get("_enc") == "v1", "encoded blob must carry the envelope marker"
    assert "key" in envelope and "data" in envelope, "envelope must carry key+data fields"
    assert not sc.is_legacy_plaintext(blob), "encoded blob must not be classified as legacy plaintext"

    # 3. every encode call rotates to a fresh random AES key (no key reuse).
    blob2 = sc.encode_settings(sample)
    envelope2 = json.loads(blob2.decode("utf-8"))
    assert envelope["key"] != envelope2["key"], "AES key must be randomized per save, not reused"
    assert envelope["data"] != envelope2["data"], "ciphertext must differ across saves (fresh nonce+key)"
    assert sc.decode_settings(blob2) == sample, "second envelope must independently decode correctly"

    # 4. legacy plaintext (or any plain JSON) passes through decode_settings unchanged.
    legacy_raw = json.dumps({"foo": "bar"}, ensure_ascii=False).encode("utf-8")
    assert sc.is_legacy_plaintext(legacy_raw), "plain JSON without _enc marker must be classified legacy"
    assert sc.decode_settings(legacy_raw) == {"foo": "bar"}, "legacy plaintext must decode as-is"

    # 5. tamper detection: flipping a byte in the ciphertext must be caught (AES-GCM auth tag).
    tampered_env = dict(envelope)
    tampered_cipher = bytearray(base64.b64decode(tampered_env["data"]))
    tampered_cipher[-1] ^= 0xFF
    tampered_env["data"] = base64.b64encode(bytes(tampered_cipher)).decode("ascii")
    tampered_raw = json.dumps(tampered_env).encode("utf-8")
    try:
        sc.decode_settings(tampered_raw)
        raise AssertionError("tampered ciphertext must raise SettingsCryptoError")
    except sc.SettingsCryptoError:
        pass

    # 6. corrupted / foreign DPAPI key blob must be rejected, not crash.
    bad_key_env = dict(envelope)
    bad_key_env["key"] = base64.b64encode(b"not-a-real-dpapi-blob-0000000000").decode("ascii")
    bad_key_raw = json.dumps(bad_key_env).encode("utf-8")
    try:
        sc.decode_settings(bad_key_raw)
        raise AssertionError("unrecognized DPAPI key blob must raise SettingsCryptoError")
    except sc.SettingsCryptoError:
        pass

    # 7. malformed envelope JSON (root not an object) must be rejected.
    try:
        sc.decode_settings(b"[1, 2, 3]")
        raise AssertionError("non-object root must raise SettingsCryptoError")
    except sc.SettingsCryptoError:
        pass

    with tempfile.TemporaryDirectory(prefix="sao_settings_crypto_selftest_") as root:
        # 8. read_settings_file / write_settings_file round trip through real disk I/O.
        path = os.path.join(root, "settings.json")
        assert sc.read_settings_file(path) == {}, "missing file must read back as {}"
        assert sc.write_settings_file(path, sample) is True, "write_settings_file must report success"
        assert sc.read_settings_file(path) == sample, "file round trip must preserve data"
        with open(path, "rb") as handle:
            on_disk = handle.read()
        assert not sc.is_legacy_plaintext(on_disk), "write_settings_file must write the encrypted envelope"

        # 9. write_settings_file rejects non-dict payloads instead of writing garbage.
        assert sc.write_settings_file(path, ["not", "a", "dict"]) is False

        # 10. migrate=True auto-rewrites a legacy plaintext file to the encrypted envelope
        #     using a single read, and the encrypted content decodes back to the original.
        legacy_path = os.path.join(root, "legacy_settings.json")
        with open(legacy_path, "w", encoding="utf-8") as handle:
            json.dump({"foo": "bar"}, handle)
        migrated = sc.read_settings_file(legacy_path, migrate=True)
        assert migrated == {"foo": "bar"}, "migrate=True must still return the original data"
        with open(legacy_path, "rb") as handle:
            migrated_raw = handle.read()
        assert not sc.is_legacy_plaintext(migrated_raw), (
            "migrate=True must rewrite the file as an encrypted envelope in place")
        assert sc.decode_settings(migrated_raw) == {"foo": "bar"}

        # 11. read_settings_file without migrate=True leaves a legacy file untouched.
        legacy_path2 = os.path.join(root, "legacy_settings2.json")
        with open(legacy_path2, "w", encoding="utf-8") as handle:
            json.dump({"foo": "bar"}, handle)
        assert sc.read_settings_file(legacy_path2) == {"foo": "bar"}
        with open(legacy_path2, "rb") as handle:
            untouched_raw = handle.read()
        assert sc.is_legacy_plaintext(untouched_raw), (
            "read_settings_file must not rewrite the file unless migrate=True")

        # 12. corrupted on-disk file reads back as {} rather than raising.
        corrupt_path = os.path.join(root, "corrupt_settings.json")
        with open(corrupt_path, "w", encoding="utf-8") as handle:
            handle.write("{not valid json")
        assert sc.read_settings_file(corrupt_path) == {}, "corrupt file must read back as {}"

    return {
        "ok": True,
        "roundtrip": True,
        "envelope_marker": True,
        "key_rotation_per_save": True,
        "legacy_plaintext_passthrough": True,
        "tamper_detection": True,
        "foreign_dpapi_key_rejected": True,
        "malformed_envelope_rejected": True,
        "file_io_roundtrip": True,
        "write_rejects_non_dict": True,
        "migrate_rewrites_in_place": True,
        "read_without_migrate_leaves_file_untouched": True,
        "corrupt_file_reads_as_empty": True,
    }


def main() -> int:
    try:
        print(json.dumps(run_selftest(), ensure_ascii=False, indent=2))
        return 0
    except Exception as exc:
        print(json.dumps({
            "ok": False,
            "failure_points_deferred": True,
        }, ensure_ascii=False, indent=2))
        _print_final_failed_check_points(exc, traceback.format_exc().rstrip())
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
