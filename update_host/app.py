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


def _normalize_url(url: str, base_url: str) -> str:
    if not url:
        return ""
    if url.startswith(("http://", "https://")):
        return url
    if url.startswith("/"):
        return base_url.rstrip("/") + url
    return base_url.rstrip("/") + "/" + url


def _load_manifest(channel: str, target: str) -> Optional[dict]:
    safe_channel = "".join(c for c in channel if c.isalnum() or c in "-_") or "stable"
    safe_target = "".join(c for c in target if c.isalnum() or c in "-_") or "windows-x64"
    path = os.path.join(DEFAULT_RELEASE_DIR, safe_channel, safe_target, "manifest.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


app = FastAPI(title="SAO Auto Update Host", version="1.0.0")

if os.path.isdir(DOWNLOADS_DIR):
    app.mount("/downloads", StaticFiles(directory=DOWNLOADS_DIR), name="downloads")


@app.get("/api/health")
def health():
    return {"ok": True, "release_dir": DEFAULT_RELEASE_DIR}


@app.get("/api/update/latest")
def latest(
    request: Request,
    channel: str = "stable",
    target: str = "windows-x64",
    current: Optional[str] = None,
):
    manifest = _load_manifest(channel, target)
    if not manifest:
        raise HTTPException(status_code=404, detail="manifest not found")
    base_url = str(request.base_url).rstrip("/")
    manifest = dict(manifest)
    manifest["download_url"] = _normalize_url(manifest.get("download_url", ""), base_url)
    return JSONResponse(manifest)


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


@app.get("/")
def root():
    return {
        "name": "SAO Auto Update Host",
        "endpoints": [
            "/api/health",
            "/api/update/latest",
            "/api/update/summary",
            "POST /api/update/publish",
            "/downloads/*",
        ],
    }


# ── 发布上传 (dev_publish.py --upload 调用) ──────────────────────────
_PUBLISH_API_KEY = _get_local_publish_api_key()


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
):
    """接收 dev_publish.py 上传的 zip 包并写入 releases/."""
    # ── Auth ──
    api_key = request.headers.get("X-API-Key", "")
    global _PUBLISH_API_KEY
    local_api_key = _PUBLISH_API_KEY or _get_local_publish_api_key()
    if not local_api_key:
        local_api_key = _bind_publish_api_key(api_key)
        _PUBLISH_API_KEY = local_api_key
    if not local_api_key or api_key != local_api_key:
        raise HTTPException(status_code=401, detail="invalid or missing API key")

    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="empty body")

    # ── Sanitize ──
    safe_ch = "".join(c for c in channel if c.isalnum() or c in "-_") or "stable"
    safe_tg = "".join(c for c in target if c.isalnum() or c in "-_") or "windows-x64"
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
        "published_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }

    manifest_path = os.path.join(target_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    return JSONResponse(manifest)
