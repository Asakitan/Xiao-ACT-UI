# -*- coding: utf-8 -*-
"""SAO Auto - 远程更新服务 (独立 FastAPI)

部署:
  uvicorn sao_auto.update_host.app:app --host 0.0.0.0 --port 9330

配置 (环境变量或 update_host_config.json):
  UPDATE_HOST_RELEASE_DIR  : 发布包根目录, 内含 channel/<channel>/manifest.json + 包文件
  UPDATE_HOST_DOWNLOADS    : 下载根目录 (默认同 RELEASE_DIR)

manifest 文件: <RELEASE_DIR>/<channel>/<target>/manifest.json
{
  "version": "2.1.0",
  "minimum_version": "2.0.1",
  "force_update": false,
  "package_type": "runtime-delta",
  "target": "windows-x64",
  "channel": "stable",
  "download_url": "/downloads/stable/windows-x64/update-2.1.0.zip",
  "sha256": "...",
  "size": 12345,
  "notes": "...",
  "published_at": "2026-04-19T12:00:00Z"
}

发布脚本: 见 publish_release.py
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import zipfile
from datetime import datetime, timezone
from typing import Optional

try:
    from fastapi import FastAPI, HTTPException, Request
    from fastapi.responses import FileResponse, JSONResponse
    from fastapi.staticfiles import StaticFiles
except Exception as e:  # pragma: no cover
    raise SystemExit(f"FastAPI 未安装: pip install fastapi uvicorn  ({e})")


if getattr(sys, "frozen", False):
    HERE = os.path.dirname(sys.executable)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_RELEASE_DIR = os.environ.get(
    "UPDATE_HOST_RELEASE_DIR", os.path.join(HERE, "releases")
)
DOWNLOADS_DIR = os.environ.get("UPDATE_HOST_DOWNLOADS", DEFAULT_RELEASE_DIR)
HOST_CONFIG_PATH = os.path.join(HERE, "update_host_config.json")


def _load_host_config() -> dict:
    if not os.path.exists(HOST_CONFIG_PATH):
        return {}
    try:
        with open(HOST_CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save_host_config(data: dict) -> None:
    os.makedirs(os.path.dirname(HOST_CONFIG_PATH), exist_ok=True)
    with open(HOST_CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


def _get_local_publish_api_key() -> str:
    config = _load_host_config()
    value = config.get("publish_api_key", "")
    return value.strip() if isinstance(value, str) else ""


def _bind_publish_api_key(api_key: str) -> str:
    value = (api_key or "").strip()
    if not value:
        return ""
    config = _load_host_config()
    config["publish_api_key"] = value
    _save_host_config(config)
    return value


def _safe_channel_target(channel: str, target: str) -> tuple[str, str]:
    safe_channel = "".join(c for c in channel if c.isalnum() or c in "-_") or "stable"
    safe_target = "".join(c for c in target if c.isalnum() or c in "-_") or "windows-x64"
    return safe_channel, safe_target


def _anchor_key(channel: str, target: str) -> str:
    safe_channel, safe_target = _safe_channel_target(channel, target)
    return f"{safe_channel}/{safe_target}"


def _get_anchor(channel: str, target: str) -> dict:
    safe_channel, safe_target = _safe_channel_target(channel, target)
    config = _load_host_config()
    anchors = config.get("anchors") if isinstance(config.get("anchors"), dict) else {}
    item = anchors.get(_anchor_key(safe_channel, safe_target), {}) if isinstance(anchors, dict) else {}
    if isinstance(item, dict) and (item.get("commit") or item.get("version")):
        return {
            "channel": safe_channel,
            "target": safe_target,
            "commit": str(item.get("commit") or ""),
            "commit_short": str(item.get("commit_short") or ""),
            "version": str(item.get("version") or ""),
            "synced_at": str(item.get("synced_at") or ""),
            "source": str(item.get("source") or "manual-sync"),
        }

    manifest = _load_manifest(safe_channel, safe_target)
    if isinstance(manifest, dict) and (manifest.get("commit") or manifest.get("version")):
        return {
            "channel": safe_channel,
            "target": safe_target,
            "commit": str(manifest.get("commit") or ""),
            "commit_short": str(manifest.get("commit_short") or ""),
            "version": str(manifest.get("version") or ""),
            "synced_at": str(manifest.get("published_at") or ""),
            "source": "manifest",
        }

    return {
        "channel": safe_channel,
        "target": safe_target,
        "commit": "",
        "commit_short": "",
        "version": "",
        "synced_at": "",
        "source": "",
    }


def _set_anchor(
    channel: str,
    target: str,
    commit: str,
    commit_short: str = "",
    version: str = "",
    source: str = "manual-sync",
) -> dict:
    safe_channel, safe_target = _safe_channel_target(channel, target)
    commit = (commit or "").strip()
    if not commit:
        raise ValueError("missing commit")

    config = _load_host_config()
    anchors = config.get("anchors") if isinstance(config.get("anchors"), dict) else {}
    payload = {
        "commit": commit,
        "commit_short": (commit_short or "").strip() or commit[:8],
        "version": (version or "").strip(),
        "synced_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": (source or "manual-sync").strip(),
    }
    anchors[_anchor_key(safe_channel, safe_target)] = payload
    config["anchors"] = anchors
    _save_host_config(config)
    return {
        "channel": safe_channel,
        "target": safe_target,
        **payload,
    }


def _normalize_url(url: str, base_url: str) -> str:
    if not url:
        return ""
    if url.startswith(("http://", "https://")):
        return url
    if url.startswith("/"):
        return base_url.rstrip("/") + url
    return base_url.rstrip("/") + "/" + url


# ── Version comparison (mirrors sao_updater._parse_version) ──────────
def _parse_version(v: str) -> tuple:
    raw = (v or "").strip().lstrip("vV")
    if not raw:
        return (0, 0, 0, 0), ()

    core_text = ""
    suffix_text = raw
    for idx, ch in enumerate(raw):
        if not (ch.isdigit() or ch == "."):
            core_text = raw[:idx]
            suffix_text = raw[idx:].lstrip("-+_.")
            break
    else:
        core_text = raw
        suffix_text = ""

    parts = []
    for chunk in core_text.split(".") if core_text else []:
        try:
            parts.append(int(chunk))
        except Exception:
            parts.append(0)
    while len(parts) < 4:
        parts.append(0)

    suffix_tokens = []
    token = []
    for ch in suffix_text:
        if ch.isalnum():
            token.append(ch)
            continue
        if token:
            text = "".join(token)
            if text.isdigit():
                suffix_tokens.append((1, int(text)))
            else:
                suffix_tokens.append((0, text.lower()))
            token = []
    if token:
        text = "".join(token)
        if text.isdigit():
            suffix_tokens.append((1, int(text)))
        else:
            suffix_tokens.append((0, text.lower()))

    return tuple(parts[:4]), tuple(suffix_tokens)


def compare_versions(a: str, b: str) -> int:
    pa, pb = _parse_version(a), _parse_version(b)
    core_a, suffix_a = pa
    core_b, suffix_b = pb
    if core_a < core_b:
        return -1
    if core_a > core_b:
        return 1

    if not suffix_a and not suffix_b:
        return 0
    if suffix_a and not suffix_b:
        return 1
    if suffix_b and not suffix_a:
        return -1

    for token_a, token_b in zip(suffix_a, suffix_b):
        if token_a == token_b:
            continue
        if token_a[0] != token_b[0]:
            return -1 if token_a[0] < token_b[0] else 1
        if token_a[1] < token_b[1]:
            return -1
        if token_a[1] > token_b[1]:
            return 1

    if len(suffix_a) < len(suffix_b):
        return -1
    if len(suffix_a) > len(suffix_b):
        return 1
    return 0


# ── Manifest / version-chain helpers ─────────────────────────────────
def _load_manifest(channel: str, target: str) -> Optional[dict]:
    safe_channel, safe_target = _safe_channel_target(channel, target)
    path = os.path.join(DEFAULT_RELEASE_DIR, safe_channel, safe_target, "manifest.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _load_versioned_manifest(channel: str, target: str, version: str) -> Optional[dict]:
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    safe_ver = "".join(c for c in version if c.isalnum() or c in ".-")
    path = os.path.join(DEFAULT_RELEASE_DIR, safe_ch, safe_tg, f"manifest-{safe_ver}.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _load_versions_index(channel: str, target: str) -> list:
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    path = os.path.join(DEFAULT_RELEASE_DIR, safe_ch, safe_tg, "versions.json")
    if not os.path.exists(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list):
            return sorted(data, key=lambda v: _parse_version(v))
        return []
    except Exception:
        return []


def _save_versions_index(channel: str, target: str, versions: list):
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    d = os.path.join(DEFAULT_RELEASE_DIR, safe_ch, safe_tg)
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "versions.json")
    sorted_versions = sorted(set(versions), key=lambda v: _parse_version(v))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(sorted_versions, f, ensure_ascii=False, indent=2)


def _find_next_version(versions: list, current: str) -> str:
    """Return the first version in the sorted list that is > current."""
    for v in versions:
        if compare_versions(v, current) > 0:
            return v
    return ""


def _safe_version_tag(version: str) -> str:
    return "".join(c for c in (version or "") if c.isalnum() or c in ".-") or "0.0.0"


def _release_target_dir(channel: str, target: str) -> str:
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    return os.path.join(DEFAULT_RELEASE_DIR, safe_ch, safe_tg)


def _manifest_zip_path(channel: str, target: str, manifest: dict) -> str:
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    target_dir = _release_target_dir(safe_ch, safe_tg)
    download_url = str(manifest.get("download_url") or "").strip()
    prefix = f"/downloads/{safe_ch}/{safe_tg}/"
    if download_url.startswith(prefix):
        rel = download_url[len(prefix):].lstrip("/")
        rel = rel.replace("/", os.sep)
        path = os.path.normpath(os.path.join(target_dir, rel))
        target_root = os.path.normpath(target_dir)
        if path == target_root or path.startswith(target_root + os.sep):
            return path
    safe_ver = _safe_version_tag(str(manifest.get("version") or ""))
    safe_type = str(manifest.get("package_type") or "runtime-delta")
    return os.path.join(target_dir, f"update-{safe_ver}-{safe_type}.zip")


def _sha256_file(path: str) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(64 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def _build_cumulative_delta_manifest(
    channel: str,
    target: str,
    current: str,
    latest_manifest: dict,
) -> Optional[dict]:
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    latest_ver = str(latest_manifest.get("version") or "").strip()
    if not current or not latest_ver:
        return None

    versions = _load_versions_index(safe_ch, safe_tg)
    if latest_ver not in versions:
        versions.append(latest_ver)
        versions = sorted(set(versions), key=_parse_version)

    chain_versions = [
        v for v in versions
        if compare_versions(v, current) > 0 and compare_versions(v, latest_ver) <= 0
    ]
    if len(chain_versions) <= 1:
        return None

    chain_manifests = []
    merged_min_version = ""
    merged_force_update = bool(latest_manifest.get("force_update"))
    for ver in chain_versions:
        manifest = latest_manifest if ver == latest_ver else _load_versioned_manifest(safe_ch, safe_tg, ver)
        if not isinstance(manifest, dict):
            return None
        if str(manifest.get("package_type") or "") != "runtime-delta":
            return None
        min_ver = str(manifest.get("minimum_version") or "").strip()
        if min_ver:
            if compare_versions(current, min_ver) < 0:
                return None
            if (not merged_min_version) or compare_versions(min_ver, merged_min_version) > 0:
                merged_min_version = min_ver
        if bool(manifest.get("force_update")):
            merged_force_update = True
        zip_path = _manifest_zip_path(safe_ch, safe_tg, manifest)
        if not os.path.isfile(zip_path):
            return None
        chain_manifests.append(manifest)

    chain_sig = hashlib.sha256(
        json.dumps(
            [
                {
                    "version": str(item.get("version") or ""),
                    "sha256": str(item.get("sha256") or ""),
                    "size": int(item.get("size") or 0),
                    "package_type": str(item.get("package_type") or ""),
                }
                for item in chain_manifests
            ],
            ensure_ascii=False,
            sort_keys=True,
        ).encode("utf-8")
    ).hexdigest()[:12]

    merged_dir = os.path.join(_release_target_dir(safe_ch, safe_tg), "_merged")
    os.makedirs(merged_dir, exist_ok=True)
    merged_name = (
        f"update-{_safe_version_tag(current)}-to-{_safe_version_tag(latest_ver)}"
        f"-runtime-delta-{chain_sig}.zip"
    )
    merged_path = os.path.join(merged_dir, merged_name)

    if not os.path.isfile(merged_path):
        tmp_path = merged_path + ".tmp"
        entries: dict[str, bytes] = {}
        try:
            for manifest in chain_manifests:
                zip_path = _manifest_zip_path(safe_ch, safe_tg, manifest)
                with zipfile.ZipFile(zip_path, "r") as src:
                    for info in src.infolist():
                        if info.is_dir():
                            continue
                        entries[info.filename] = src.read(info.filename)

            with zipfile.ZipFile(tmp_path, "w", zipfile.ZIP_DEFLATED) as dst:
                for arcname in sorted(entries):
                    dst.writestr(arcname, entries[arcname])
            os.replace(tmp_path, merged_path)
        except Exception as exc:
            try:
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except Exception:
                pass
            try:
                print(f"[update_host] cumulative delta build failed: {exc}")
            except Exception:
                pass
            return None

    digest, size = _sha256_file(merged_path)
    merged_manifest = dict(latest_manifest)
    merged_manifest.update({
        "version": latest_ver,
        "minimum_version": merged_min_version,
        "force_update": merged_force_update,
        "package_type": "runtime-delta",
        "target": safe_tg,
        "channel": safe_ch,
        "download_url": f"/downloads/{safe_ch}/{safe_tg}/_merged/{merged_name}",
        "sha256": digest,
        "size": size,
        "merged_from": current,
        "merged_versions": chain_versions,
    })
    return merged_manifest


def _empty_manifest_response(channel: str, target: str, current: Optional[str] = None) -> dict:
    safe_channel, safe_target = _safe_channel_target(channel, target)
    return {
        "available": False,
        "detail": "manifest not found",
        "version": (current or "").strip(),
        "minimum_version": "",
        "force_update": False,
        "package_type": "",
        "target": safe_target,
        "channel": safe_channel,
        "download_url": "",
        "sha256": "",
        "size": 0,
        "notes": "",
        "published_at": "",
    }


app = FastAPI(title="SAO Auto Update Host", version="1.0.0")

# 始终挂载 /downloads。否则如果服务启动时 releases 目录还不存在,
# /api/update/latest 之后即使能读到后续写入的 manifest, /downloads/*
# 仍然因为路由未挂载而持续返回 404, 表现为“能检测到更新但点击下载失败”。
app.mount("/downloads", StaticFiles(directory=DOWNLOADS_DIR, check_dir=False), name="downloads")


@app.get("/api/health")
def health():
    return {
        "ok": True,
        "release_dir": DEFAULT_RELEASE_DIR,
        "downloads_dir": DOWNLOADS_DIR,
        "downloads_dir_exists": os.path.isdir(DOWNLOADS_DIR),
    }


@app.get("/api/update/latest")
def latest(
    request: Request,
    channel: str = "stable",
    target: str = "windows-x64",
    current: Optional[str] = None,
):
    """按版本链顺序下发更新.

    规则:
      1. current 未提供 → 返回最新 manifest (向后兼容)
      2. current >= latest.version → available=false (已是最新)
      3. latest.minimum_version 存在且 current < minimum_version → 返回最新 (强制跳版本)
      4. 否则 → 从 versions.json 找到 current 的下一个版本, 返回对应 manifest
      5. 找不到 / 文件缺失 → 回退返回最新 manifest
    """
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    latest_manifest = _load_manifest(safe_ch, safe_tg)
    if not latest_manifest:
        return JSONResponse(_empty_manifest_response(channel, target, current))

    base_url = str(request.base_url).rstrip("/")
    current_ver = (current or "").strip()
    latest_ver = latest_manifest.get("version", "")

    def _finalize(m: dict) -> JSONResponse:
        m = dict(m)
        m.setdefault("available", True)
        m["download_url"] = _normalize_url(m.get("download_url", ""), base_url)
        return JSONResponse(m)

    # (1) No current → return latest
    if not current_ver:
        return _finalize(latest_manifest)

    # (2) Client >= latest → up to date
    if compare_versions(current_ver, latest_ver) >= 0:
        resp = _empty_manifest_response(channel, target, current)
        resp["version"] = latest_ver
        return JSONResponse(resp)

    # (3) minimum_version force jump
    min_ver = (latest_manifest.get("minimum_version") or "").strip()
    if min_ver and compare_versions(current_ver, min_ver) < 0:
        return _finalize(latest_manifest)

    # If every version between current -> latest is a runtime-delta and none
    # of them requires a newer minimum_version than the client already has,
    # collapse the whole chain into one cumulative delta zip.
    cumulative_manifest = _build_cumulative_delta_manifest(
        safe_ch, safe_tg, current_ver, latest_manifest,
    )
    if cumulative_manifest:
        return _finalize(cumulative_manifest)

    # (4) Sequential: find next version after current
    versions = _load_versions_index(safe_ch, safe_tg)
    next_ver = _find_next_version(versions, current_ver)
    if next_ver and next_ver != latest_ver:
        versioned = _load_versioned_manifest(safe_ch, safe_tg, next_ver)
        if versioned:
            return _finalize(versioned)

    # (5) Fallback to latest
    return _finalize(latest_manifest)


@app.get("/api/update/summary")
def summary():
    """列出所有 channel/target 的最新版本."""
    out = []
    if not os.path.isdir(DEFAULT_RELEASE_DIR):
        return {"channels": []}
    for channel in sorted(os.listdir(DEFAULT_RELEASE_DIR)):
        chan_dir = os.path.join(DEFAULT_RELEASE_DIR, channel)
        if not os.path.isdir(chan_dir):
            continue
        for target in sorted(os.listdir(chan_dir)):
            t_dir = os.path.join(chan_dir, target)
            mf = os.path.join(t_dir, "manifest.json")
            if not os.path.exists(mf):
                continue
            try:
                with open(mf, "r", encoding="utf-8") as f:
                    data = json.load(f)
                out.append({
                    "channel": channel,
                    "target": target,
                    "version": data.get("version"),
                    "package_type": data.get("package_type"),
                    "published_at": data.get("published_at"),
                    "force_update": bool(data.get("force_update")),
                })
            except Exception:
                continue
    return {"channels": out}


@app.get("/api/update/anchor")
def get_anchor(channel: str = "stable", target: str = "windows-x64"):
    return JSONResponse(_get_anchor(channel, target))


@app.get("/")
def root():
    return {
        "name": "SAO Auto Update Host",
        "endpoints": [
            "/api/health",
            "/api/update/latest",
            "/api/update/summary",
            "/api/update/anchor",
            "POST /api/update/anchor",
            "POST /api/update/publish",
            "/downloads/*",
        ],
    }


# ── 发布上传 (dev_publish.py --upload 调用) ──────────────────────────
_PUBLISH_API_KEY = _get_local_publish_api_key()


def _authorize_publish_request(request: Request) -> None:
    api_key = request.headers.get("X-API-Key", "")
    global _PUBLISH_API_KEY
    local_api_key = _PUBLISH_API_KEY or _get_local_publish_api_key()
    if not local_api_key:
        local_api_key = _bind_publish_api_key(api_key)
        _PUBLISH_API_KEY = local_api_key
    if not local_api_key or api_key != local_api_key:
        raise HTTPException(status_code=401, detail="invalid or missing API key")


@app.post("/api/update/anchor")
def sync_anchor(
    request: Request,
    channel: str = "stable",
    target: str = "windows-x64",
    commit: str = "",
    commit_short: str = "",
    version: str = "",
    source: str = "manual-sync",
):
    _authorize_publish_request(request)
    try:
        return JSONResponse(_set_anchor(channel, target, commit, commit_short, version, source))
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e)) from e


@app.post("/api/update/publish")
async def publish(
    request: Request,
    version: str,
    package_type: str = "runtime-delta",
    force_update: str = "false",
    notes: str = "",
    minimum_version: str = "",
    channel: str = "stable",
    target: str = "windows-x64",
    commit: str = "",
    commit_short: str = "",
    anchor_commit: str = "",
    anchor_commit_short: str = "",
    anchor_version: str = "",
):
    """接收 dev_publish.py 上传的 zip 包并写入 releases/."""
    _authorize_publish_request(request)

    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="empty body")

    # ── Sanitize ──
    safe_ch, safe_tg = _safe_channel_target(channel, target)
    safe_ver = "".join(c for c in version if c.isalnum() or c in ".-") or "0.0.0"
    safe_type = package_type if package_type in ("runtime-delta", "full-package") else "runtime-delta"

    target_dir = os.path.join(DEFAULT_RELEASE_DIR, safe_ch, safe_tg)
    os.makedirs(target_dir, exist_ok=True)

    fname = f"update-{safe_ver}-{safe_type}.zip"
    dst = os.path.join(target_dir, fname)

    with open(dst, "wb") as f:
        f.write(body)

    digest = hashlib.sha256(body).hexdigest()

    manifest = {
        "version": safe_ver,
        "minimum_version": minimum_version,
        "force_update": force_update.lower() in ("true", "1", "yes"),
        "package_type": safe_type,
        "target": safe_tg,
        "channel": safe_ch,
        "download_url": f"/downloads/{safe_ch}/{safe_tg}/{fname}",
        "sha256": digest,
        "size": len(body),
        "notes": notes,
        "commit": commit,
        "commit_short": commit_short,
        "anchor_commit": anchor_commit,
        "anchor_commit_short": anchor_commit_short,
        "anchor_version": anchor_version,
        "published_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }

    manifest_path = os.path.join(target_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    # Per-version manifest + versions index (sequential delivery)
    versioned_path = os.path.join(target_dir, f"manifest-{safe_ver}.json")
    with open(versioned_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    versions = _load_versions_index(safe_ch, safe_tg)
    if safe_ver not in versions:
        versions.append(safe_ver)
    _save_versions_index(safe_ch, safe_tg, versions)

    if (commit or "").strip():
        _set_anchor(
            channel=safe_ch,
            target=safe_tg,
            commit=commit,
            commit_short=commit_short,
            version=safe_ver,
            source="publish",
        )

    return JSONResponse(manifest)
