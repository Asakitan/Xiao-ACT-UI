"""pack_drivers_sao2.py — pre-distribution driver packer.

Reads plaintext driver PE files from the source driver store (for
R1/R3/R5) and from the VT-Splitview build output (for VT), and writes
SAO2-encrypted bundles to the output directory (typically
`e:/VC/SAO-UI/sao_auto/C/platform/rt_io/assets/drivers`).

Shipped bundles carry innocuous locale-style aliases (e.g. `locale.dat`)
instead of `*.sys` / `*.sao2` names; the alias→canonical-runtime-name
mapping is maintained in `driver_asset_metadata.cmake` and
`driver_asset.cpp`.  The output blobs are what ship with the project.
On first launch, the helper's `driver_local_install_ensure` decodes each
SAO2 bundle and re-encrypts it with per-machine SAO3 under the canonical
runtime name (e.g. `%LOCALAPPDATA%\\...\\R1.sys.sao3`).

Usage:
    python pack_drivers_sao2.py                       # dry-run
    python pack_drivers_sao2.py --apply               # write bundles
    python pack_drivers_sao2.py --apply --src-dir X --out-dir Y

Refuses to run without --apply so a stray invocation from CI never
mutates the shipping assets.  Refuses to overwrite if the target
bundle decodes to identical bytes (idempotent no-op).
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import sys
from pathlib import Path

# ── SAO2 primitives (mirrored from rt_io.mcp.r1_engine) ──────────

_EK_SEED = b"\x4a\x91\xc3\x7f\x28\xe5\xd6\x0b\x73\xfa\x14\x9d\x55\xa2\x68\xbe"
_EK_FIXED = hashlib.sha256(_EK_SEED).digest()

# SAO2 envelope layout:
#   +0..+3    b"SAO2"
#   +4..+7    version u32 LE (currently 1)
#   +8..+19   12-byte nonce
#   +20..+35  16-byte GCM tag
#   +36..+39  plaintext size u32 LE
#   +40..     ciphertext
_SAO2_MAGIC = b"SAO2"
_SAO2_HEADER_SIZE = 40


def _bcrypt_gcm_encrypt(key: bytes, nonce: bytes, plaintext: bytes) -> tuple[bytes, bytes]:
    """AES-256-GCM encrypt via `cryptography` (portable across platforms)."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # type: ignore
    except ImportError:
        print("[FATAL] pip install cryptography  — required for packing")
        sys.exit(1)
    enc = Cipher(algorithms.AES(key), modes.GCM(nonce)).encryptor()
    ct = enc.update(plaintext) + enc.finalize()
    return enc.tag, ct


def encrypt_sao2_fixed(plaintext: bytes) -> bytes:
    if not plaintext.startswith(b"MZ"):
        raise ValueError("SAO2 payload must start with a PE MZ header")
    nonce = os.urandom(12)
    tag, ct = _bcrypt_gcm_encrypt(_EK_FIXED, nonce, plaintext)
    header = (
        _SAO2_MAGIC
        + (1).to_bytes(4, "little")
        + nonce
        + tag
        + len(plaintext).to_bytes(4, "little")
    )
    return header + ct


def decrypt_sao2_fixed(envelope: bytes) -> bytes:
    """Reverse; used to check idempotency."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # type: ignore
    except ImportError:
        return b""
    if len(envelope) < _SAO2_HEADER_SIZE or envelope[:4] != _SAO2_MAGIC:
        return b""
    nonce = envelope[8:20]
    tag = envelope[20:36]
    ct = envelope[_SAO2_HEADER_SIZE:]
    try:
        dec = Cipher(algorithms.AES(_EK_FIXED), modes.GCM(nonce, tag)).decryptor()
        pt = dec.update(ct) + dec.finalize()
        return pt
    except Exception:
        return b""


# ── Packer ───────────────────────────────────────────────────────

# Drivers we ship, as (source .sys name, shipped alias) pairs.  Shipped
# bundles deliberately carry innocuous locale-style filenames (no `.sys`,
# no `.sao2` suffix).  This shipped-alias namespace is DISTINCT from the
# legacy install-time placeholder names in Python rt_io.py `_DAT_PAIRS` /
# C++ install_key.cpp `kDatPairs` (locale_data.bin, theme_default.dat, ...);
# the two namespaces intentionally never collide (pinned by
# test_driver_asset_resolution.cpp).  The canonical runtime name mapping
# lives in `C/platform/rt_io/cmake/driver_asset_metadata.cmake` and
# `driver_asset.cpp` (RoleSpec bundle filenames).
#
# `source` is resolved against the plaintext driver store except for the
# VT entries, which are resolved against the VT-Splitview build output.
_DEFAULT_SHIP_MAP: tuple[tuple[str, str, str], ...] = (
    # (kind, source .sys name, shipped alias)
    ("store", "R1.sys", "locale.dat"),
    ("store", "R3.sys", "theme.dat"),
    ("store", "R5.sys", "display.dat"),
    ("vt-release", "VT-Splitview.sys", "fontmetrics.dat"),
    ("vt-debug", "VT-Splitview.sys", "iconcache.dat"),
)

# Legacy compatibility for `--drivers` overrides.
_LEGACY_STORE_NAMES: dict[str, str] = {
    "R1.sys": "locale.dat",
    "R2.sys": "R2.sys.sao2",
    "R3.sys": "theme.dat",
    "R5.sys": "display.dat",
}


def _default_src_dir() -> Path:
    # Repo root / driver store — resolved relative to this file so
    # the packer works no matter where it's invoked from.
    here = Path(__file__).resolve()
    # sao_auto/python/tools/pack_drivers_sao2.py → repo root is 3 up.
    for up in (here.parent.parent.parent.parent, here.parent.parent.parent):
        candidate = up / "driver store"
        if candidate.is_dir():
            return candidate
    return here.parent.parent.parent.parent / "driver store"


def _default_vt_dir() -> Path:
    here = Path(__file__).resolve()
    # → <repo>/VT-Splitview
    for up in (here.parent.parent.parent.parent, here.parent.parent.parent):
        candidate = up / "VT-Splitview"
        if candidate.is_dir():
            return candidate
    return here.parent.parent.parent.parent / "VT-Splitview"


def _default_out_dir() -> Path:
    here = Path(__file__).resolve()
    # → sao_auto/C/platform/rt_io/assets/drivers
    return (
        here.parent.parent.parent
        / "C" / "platform" / "rt_io" / "assets" / "drivers"
    )


def pack_one(src: Path, out: Path, apply: bool) -> str:
    if not src.is_file():
        return f"  SKIP {src.name}: not found at {src}"
    pt = src.read_bytes()
    if not pt.startswith(b"MZ"):
        return f"  SKIP {src.name}: not a PE (no MZ)"
    # Idempotency: if out already exists and decodes to the same
    # plaintext, do nothing.
    if out.is_file():
        existing = out.read_bytes()
        existing_pt = decrypt_sao2_fixed(existing)
        if existing_pt == pt:
            return f"  ok   {src.name}  →  {out.name}  (unchanged)"
    envelope = encrypt_sao2_fixed(pt)
    action = "WRITE" if apply else "PLAN "
    if apply:
        out.parent.mkdir(parents=True, exist_ok=True)
        # Atomic: temp + rename.
        tmp = out.with_suffix(out.suffix + ".tmp")
        tmp.write_bytes(envelope)
        tmp.replace(out)
    return (
        f"  {action} {src.name}  →  {out.name}  "
        f"({len(pt):,} B pt → {len(envelope):,} B env)"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack drivers as SAO2 for shipping")
    ap.add_argument("--src-dir", type=Path, default=None,
                    help="source directory (default: <repo>/driver store)")
    ap.add_argument("--vt-dir", type=Path, default=None,
                    help="VT-Splitview build root (default: <repo>/VT-Splitview)")
    ap.add_argument("--out-dir", type=Path, default=None,
                    help="output directory (default: assets/drivers)")
    ap.add_argument("--apply", action="store_true",
                    help="actually write; without it does a dry-run")
    ap.add_argument("--drivers", nargs="+", default=None,
                    help="legacy: store driver .sys names to pack "
                         "(uses the alias map for output names)")
    args = ap.parse_args()

    src_dir = args.src_dir or _default_src_dir()
    vt_dir = args.vt_dir or _default_vt_dir()
    out_dir = args.out_dir or _default_out_dir()
    print(f"src : {src_dir}")
    print(f"vt  : {vt_dir}")
    print(f"out : {out_dir}")
    print(f"mode: {'APPLY' if args.apply else 'DRY-RUN'}")

    if not src_dir.is_dir():
        print(f"[FATAL] source directory does not exist: {src_dir}")
        return 1

    # Build the effective ship list.
    if args.drivers:
        ship: list[tuple[str, str, str]] = []
        for name in args.drivers:
            alias = _LEGACY_STORE_NAMES.get(name)
            if alias is None:
                print(f"[FATAL] unknown legacy driver name: {name}")
                return 1
            ship.append(("store", name, alias))
    else:
        ship = list(_DEFAULT_SHIP_MAP)

    print()
    for kind, name, alias in ship:
        if kind == "store":
            src = src_dir / name
        elif kind == "vt-release":
            src = vt_dir / "x64" / "Release" / name
        elif kind == "vt-debug":
            src = vt_dir / "x64" / "Debug" / name
        else:
            print(f"[FATAL] unknown ship kind: {kind}")
            return 1
        out = out_dir / alias
        print(pack_one(src, out, args.apply))

    if not args.apply:
        print()
        print("Dry-run only.  Add --apply to actually write.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
