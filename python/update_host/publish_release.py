# -*- coding: utf-8 -*-
# 发布脚本: 把一个目录或 zip 包注册为某 channel/target 的最新版本.
#
# 用法:
# python publish_release.py --version 2.1.0       --package path/to/update-2.1.0.zip       --type runtime-delta       [--minimum 2.0.1] [--force] [--notes "修复..."]       [--channel stable] [--target windows-x64-native]       [--release-dir releases]
#
# 会:
# 1. 把 zip 复制到 <release-dir>/update/<channel>/<target>/artifacts/update-<version>.zip
# 2. 计算 SHA256
# 3. 写入 <release-dir>/update/<channel>/<target>/latest.json

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from pathlib import Path


_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
_SEMVER_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$")
_LOCKS = {}
_LOCKS_GUARD = threading.Lock()
_PUBLICATION_FORMAT_VERSION = 1

def _segment(value, label):
    if not value or not _SEGMENT_RE.fullmatch(value) or ".." in value:
        raise ValueError(f"invalid {label}: {value!r}")
    return value

def _semver(value, label):
    if not _SEMVER_RE.fullmatch(value or ""):
        raise ValueError(f"invalid {label}: {value!r}")
    for part in value.split("+", 1)[0].split("-", 1)[-1].split("."):
        if part.isdigit() and len(part) > 1 and part.startswith("0"):
            raise ValueError(f"invalid {label}: {value!r}")
    return value


def _version_core(value):
    return tuple(int(part) for part in re.split(r"[-+]", value, maxsplit=1)[0].split("."))

def _within(root, child):
    root = Path(root).resolve()
    child = Path(child).resolve()
    child.relative_to(root)
    return child


def _valid_sha256(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value) is not None


def _quarantine_journal(path):
    for attempt in range(32):
        target = Path(f"{path}.corrupt.{os.getpid()}.{time.time_ns()}.{attempt}")
        try:
            os.replace(path, target)
            return
        except FileNotFoundError:
            return
        except OSError:
            continue

@contextmanager
def _lock(path):
    key = str(Path(path).resolve())
    with _LOCKS_GUARD:
        local = _LOCKS.setdefault(key, threading.RLock())
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with local, open(path, "a+b") as handle:
        handle.seek(0, os.SEEK_END)
        if handle.tell() == 0:
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == "nt":
                import msvcrt
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)

def _atomic_copy(src, dst):
    fd, tmp = tempfile.mkstemp(prefix=f".{dst.name}.", suffix=".tmp", dir=str(dst.parent))
    os.close(fd)
    try:
        with open(src, "rb") as inp, open(tmp, "wb") as out:
            while True:
                chunk = inp.read(1024 * 1024)
                if not chunk: break
                out.write(chunk)
            out.flush(); os.fsync(out.fileno())
        os.replace(tmp, dst)
    finally:
        try: os.remove(tmp)
        except OSError: pass

def _atomic_json(path, data):
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            json.dump(data, out, ensure_ascii=False, indent=2)
            out.flush(); os.fsync(out.fileno())
        os.replace(tmp, path)
    finally:
        try: os.remove(tmp)
        except OSError: pass

def _recover_release_unlocked(target_dir: Path, channel: str, target: str):
    journal_path = target_dir / ".publish-journal.json"
    if not journal_path.is_file():
        return
    try:
        journal = json.loads(journal_path.read_text(encoding="utf-8"))
        version = journal["version"]
        filename = f"update-{version}.zip"
        url = f"/update/{channel}/{target}/artifacts/{filename}"
        expected = journal["manifest_data"]
        if (
            journal.get("format_version") != _PUBLICATION_FORMAT_VERSION
            or journal.get("channel") != channel
            or journal.get("target") != target
            or not isinstance(version, str)
            or not _SEMVER_RE.fullmatch(version)
            or journal.get("url") != url
            or journal.get("filename") != filename
            or not _valid_sha256(journal.get("sha256"))
            or not isinstance(journal.get("size"), int)
            or journal.get("size", 0) <= 0
            or not isinstance(expected, dict)
            or expected.get("version") != version
            or expected.get("url") != url
            or expected.get("sha256") != journal.get("sha256")
            or expected.get("size") != journal.get("size")
            or not isinstance(expected.get("notes"), str)
        ):
            _quarantine_journal(journal_path)
            return
        artifact = _within(target_dir / "artifacts", target_dir / "artifacts" / filename)
        manifest_path = _within(target_dir, target_dir / "latest.json")
        if (_within(target_dir / "artifacts", journal["artifact"]) != artifact or
                _within(target_dir, journal["manifest"]) != manifest_path or
                journal.get("staging") != ""):
            _quarantine_journal(journal_path)
            return
        digest, size = sha256_file(str(artifact))
        if artifact.is_file() and digest == expected["sha256"] and size == expected["size"]:
            _atomic_json(manifest_path, expected)
        else:
            pass
    except (OSError, KeyError, ValueError, json.JSONDecodeError, TypeError):
        _quarantine_journal(journal_path)
    else:
        try: journal_path.unlink()
        except OSError: pass


def _publish_release(target_dir, artifacts_dir, dst, manifest_path, pkg_path, manifest, channel, target):
    with _lock(target_dir / "_publish.lock"):
        _recover_release_unlocked(target_dir, channel, target)
        journal = target_dir / ".publish-journal.json"
        source_digest, source_size = sha256_file(str(pkg_path))
        journal_manifest = dict(manifest, sha256=source_digest, size=source_size)
        _atomic_json(journal, {
            "format_version": _PUBLICATION_FORMAT_VERSION,
            "channel": channel,
            "target": target,
            "version": manifest["version"],
            "url": manifest["url"],
            "filename": dst.name,
            "sha256": source_digest,
            "size": source_size,
            "staging": "",
            "artifact": str(dst),
            "manifest": str(manifest_path),
            "manifest_data": journal_manifest,
        })
        _atomic_copy(pkg_path, dst)
        digest, size = sha256_file(str(dst))
        manifest = dict(manifest, sha256=digest, size=size)
        _atomic_json(manifest_path, manifest)
        try: journal.unlink()
        except OSError: pass
    return digest, size

def sha256_file(path: str) -> tuple:
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(64 * 1024)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--package", required=True, help="本地 zip 包路径")
    parser.add_argument("--type", default="runtime-delta", choices=["runtime-delta", "full-package"])
    parser.add_argument("--minimum", default="", help="minimum_version (低于则强制升级)")
    parser.add_argument("--force", action="store_true", help="设置 force_update=true")
    parser.add_argument("--notes", default="")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--target", default="windows-x64-native")
    parser.add_argument("--release-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "releases"))
    args = parser.parse_args()

    try:
        version = _semver(args.version, "version")
        minimum = _semver(args.minimum, "minimum") if args.minimum else ""
        if minimum and _version_core(minimum) > _version_core(version):
            raise ValueError("minimum must not exceed version")
        channel = _segment(args.channel, "channel")
        target = _segment(args.target, "target")
        if target != "windows-x64-native":
            raise ValueError("target must be windows-x64-native")
        release_root = Path(args.release_dir).resolve()
        pkg_path = Path(args.package).resolve()
        if pkg_path.is_symlink() or not pkg_path.is_file():
            raise ValueError(f"package is not a regular file: {pkg_path}")
    except ValueError as exc:
        print(f"[publish] {exc}", file=sys.stderr)
        return 2
    pkg = str(pkg_path)
    if not os.path.exists(pkg):
        print(f"[publish] 包不存在: {pkg}", file=sys.stderr)
        return 1

    target_dir = _within(release_root, release_root / "update" / channel / target)
    artifacts_dir = _within(target_dir, target_dir / "artifacts")
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    fname = f"update-{version}.zip"
    dst = _within(artifacts_dir, artifacts_dir / fname)
    manifest_path = _within(target_dir, target_dir / "latest.json")
    manifest = {
        "version": version,
        "url": f"/update/{channel}/{target}/artifacts/{fname}",
        "sha256": "",
        "size": 0,
        "notes": args.notes,
        "channel": channel,
        "target": target,
        "force_update": bool(args.force),
        "minimum_version": minimum,
        "package_type": args.type,
        "published_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    digest, size = _publish_release(target_dir, artifacts_dir, dst, manifest_path, pkg_path, manifest, channel, target)
    print(f"[publish] 已发布 v{args.version} -> {dst}")
    print(f"[publish] manifest: {manifest_path}")
    print(f"[publish] sha256:   {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
