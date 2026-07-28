# -*- coding: utf-8 -*-
# update_host — 与 C++ 客户端 (sao_updater + sao_workshop_client) 对齐的 HTTP 服务
#
# ── Update wire (对齐 sao_auto/C/server/freetier/updater/updater.h) ──
#   GET  /update/{channel}/{target}/latest.json
#           → { "version", "url", "sha256", "size", "notes" }
#   GET  /update/{channel}/{target}/artifacts/{filename}
#           → binary
#   POST /update/{channel}/{target}/publish?version=&notes=&force_update=&minimum_version=
#           header: X-API-Key: <publish_api_key>
#           body:   application/octet-stream = 更新包 zip
#           → 保存到 artifacts/, 计算 sha256, 更新 latest.json
#
# ── Workshop wire (对齐 sao_auto/C/server/freetier/workshop_client/workshop_client.h) ──
#   GET  /api/v1/workshop/plugins?page=N&size=M&tag=T
#           → { "items": [...summary], "total": N }
#   GET  /api/v1/workshop/plugins/{id}
#           → { ...summary, description, sha256, signature_alg, size_bytes, min_major/minor/patch }
#   GET  /api/v1/workshop/plugins/{id}/download
#           → binary (.sao-plugin zip)
#   POST /api/v1/workshop/plugins/{id}/publish?version=&name=&tag=&author=&signature_alg=&min_major=&min_minor=&min_patch=
#           header: X-API-Key + 可选 X-SaoAuto-Description
#           body:   application/octet-stream = .sao-plugin zip
#
# 端口 15018 对外不变（uvicorn 绑 9973 时依赖反代）。存储 layout:
#   releases/update/{channel}/{target}/latest.json + artifacts/*.zip
#   releases/workshop/{plugin_id}/meta.json + versions/*.sao-plugin
#   releases/workshop/_catalog.json （全 plugin 列表缓存）

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import secrets
import sys
import time
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from fastapi import FastAPI, HTTPException, Header, Query, Request
from fastapi.responses import FileResponse, JSONResponse, Response


# ── 路径 ────────────────────────────────────────────────────────────────

if getattr(sys, "frozen", False):
    HERE = os.path.dirname(sys.executable)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))

RELEASE_DIR = os.environ.get("UPDATE_HOST_RELEASE_DIR", os.path.join(HERE, "releases"))
HOST_CONFIG_PATH = os.path.join(HERE, "update_host_config.json")

UPDATE_ROOT = os.path.join(RELEASE_DIR, "update")
WORKSHOP_ROOT = os.path.join(RELEASE_DIR, "workshop")
WORKSHOP_CATALOG_PATH = os.path.join(WORKSHOP_ROOT, "_catalog.json")

os.makedirs(UPDATE_ROOT, exist_ok=True)
os.makedirs(WORKSHOP_ROOT, exist_ok=True)


# ── config ─────────────────────────────────────────────────────────────

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
    os.makedirs(os.path.dirname(HOST_CONFIG_PATH) or ".", exist_ok=True)
    tmp = HOST_CONFIG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, HOST_CONFIG_PATH)


def _get_publish_api_key() -> str:
    env_key = os.environ.get("SAO_UPDATE_API_KEY", "").strip()
    if env_key:
        return env_key
    return (_load_host_config().get("publish_api_key", "") or "").strip()


def _bind_publish_api_key(api_key: str) -> str:
    value = (api_key or "").strip()
    if not value:
        return ""
    config = _load_host_config()
    config["publish_api_key"] = value
    _save_host_config(config)
    return value


def _get_base_url_from_request(request: Request) -> str:
    fwd_proto = request.headers.get("x-forwarded-proto", "")
    fwd_host = request.headers.get("x-forwarded-host") or request.headers.get("host", "")
    scheme = fwd_proto or request.url.scheme
    if fwd_host:
        return f"{scheme}://{fwd_host}"
    return f"{scheme}://{request.url.netloc}"


# ── 校验辅助 ────────────────────────────────────────────────────────────

_SEG_RE = re.compile(r"^[A-Za-z0-9._\-]{1,64}$")
_PLUGIN_ID_RE = re.compile(r"^[A-Za-z0-9._\-]{1,63}$")
_VERSION_RE = re.compile(r"^[0-9]+(\.[0-9]+){1,3}([A-Za-z0-9\-.+]*)?$")


def _safe_seg(v: str) -> str:
    if not v or not _SEG_RE.match(v) or ".." in v:
        raise HTTPException(400, f"invalid path segment: {v!r}")
    return v


def _safe_plugin_id(v: str) -> str:
    if not v or not _PLUGIN_ID_RE.match(v) or ".." in v:
        raise HTTPException(400, f"invalid plugin id: {v!r}")
    return v


def _safe_version(v: str) -> str:
    if not v or not _VERSION_RE.match(v):
        raise HTTPException(400, f"invalid version: {v!r}")
    return v


def _require_api_key(x_api_key: Optional[str], request: Request):
    expected = _get_publish_api_key()
    if not expected:
        # 首次调用时接受任何 key 并绑定（首启自 bootstrap）
        first = (x_api_key or "").strip()
        if not first:
            raise HTTPException(401, "publish_api_key not set; provide X-API-Key to bind")
        _bind_publish_api_key(first)
        return
    got = (x_api_key or "").strip()
    if not got or not hmac.compare_digest(got, expected):
        raise HTTPException(403, "invalid X-API-Key")


def _sha256_hex_of_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _now_utc_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _now_ms() -> int:
    return int(time.time() * 1000)


# ── FastAPI ────────────────────────────────────────────────────────────

app = FastAPI(title="SAO Update+Workshop Host", version="3.0.0-cpp")


@app.get("/health")
async def health():
    return {
        "status": "ok",
        "server": "sao-update-host",
        "version": "3.0.0-cpp",
        "time": _now_utc_iso(),
    }


# ══ Update Path ═════════════════════════════════════════════════════════

def _update_dir(channel: str, target: str) -> str:
    return os.path.join(UPDATE_ROOT, _safe_seg(channel), _safe_seg(target))


def _update_latest_path(channel: str, target: str) -> str:
    return os.path.join(_update_dir(channel, target), "latest.json")


def _update_artifacts_dir(channel: str, target: str) -> str:
    return os.path.join(_update_dir(channel, target), "artifacts")


@app.get("/update/{channel}/{target}/latest.json")
async def get_update_latest(channel: str, target: str, request: Request):
    p = _update_latest_path(channel, target)
    if not os.path.isfile(p):
        # 无内容时返回 empty manifest（C++ 客户端会比较版本 → has_update=0）
        return JSONResponse({
            "version": "0.0.0",
            "url": "",
            "sha256": "",
            "size": 0,
            "notes": "no release published yet",
        })
    with open(p, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    # 若 url 是相对路径，改成绝对 URL 供 C++ updater 直接 GET
    url = manifest.get("url", "")
    if url and not url.startswith(("http://", "https://")):
        base = _get_base_url_from_request(request)
        url = base.rstrip("/") + "/" + url.lstrip("/")
        manifest = dict(manifest, url=url)
    # 严格 5 字段返回
    return {
        "version": str(manifest.get("version", "")),
        "url": str(manifest.get("url", "")),
        "sha256": str(manifest.get("sha256", "")),
        "size": int(manifest.get("size", 0)),
        "notes": str(manifest.get("notes", "")),
    }


@app.get("/update/{channel}/{target}/artifacts/{filename}")
async def get_update_artifact(channel: str, target: str, filename: str):
    if "/" in filename or "\\" in filename or ".." in filename:
        raise HTTPException(400, "invalid filename")
    p = os.path.join(_update_artifacts_dir(channel, target), filename)
    if not os.path.isfile(p):
        raise HTTPException(404, "artifact not found")
    return FileResponse(p, media_type="application/zip", filename=filename)


@app.post("/update/{channel}/{target}/publish")
async def publish_update(
    channel: str,
    target: str,
    request: Request,
    version: str = Query(...),
    notes: str = Query(""),
    force_update: bool = Query(False),
    minimum_version: str = Query(""),
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    _safe_seg(channel)
    _safe_seg(target)
    _safe_version(version)

    art_dir = _update_artifacts_dir(channel, target)
    os.makedirs(art_dir, exist_ok=True)

    filename = f"update-{version}.zip"
    dest_path = os.path.join(art_dir, filename)

    # 流式接收 body → 落盘 → SHA-256
    tmp_path = dest_path + ".uploading"
    total = 0
    h = hashlib.sha256()
    try:
        with open(tmp_path, "wb") as out:
            async for chunk in request.stream():
                if not chunk:
                    continue
                out.write(chunk)
                h.update(chunk)
                total += len(chunk)
        if total == 0:
            raise HTTPException(400, "empty request body")
        os.replace(tmp_path, dest_path)
    except HTTPException:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise
    except Exception as e:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise HTTPException(500, f"write failed: {e}")

    sha256_hex = h.hexdigest()

    # C++ updater 用 max_body=256KB 拉 latest.json（updater.cpp:165）；这里 truncate notes
    # 保 200KB 上限，留 56KB 余量给其他字段 + JSON overhead。
    NOTES_MAX_BYTES = 200 * 1024
    notes_safe = notes
    if notes_safe:
        enc = notes_safe.encode("utf-8", errors="replace")
        if len(enc) > NOTES_MAX_BYTES:
            enc = enc[:NOTES_MAX_BYTES]
            notes_safe = enc.decode("utf-8", errors="ignore") + "\n[truncated]"

    manifest = {
        "version": version,
        "url": f"/update/{channel}/{target}/artifacts/{filename}",
        "sha256": sha256_hex,
        "size": total,
        "notes": notes_safe,
        "channel": channel,
        "target": target,
        "force_update": bool(force_update),
        "minimum_version": minimum_version or "",
        "published_at": _now_utc_iso(),
    }

    latest_path = _update_latest_path(channel, target)
    tmp = latest_path + ".tmp"
    os.makedirs(os.path.dirname(latest_path) or ".", exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    os.replace(tmp, latest_path)

    return {
        "ok": True,
        "version": version,
        "sha256": sha256_hex,
        "size": total,
        "latest_url": manifest["url"],
    }


@app.get("/update/{channel}/{target}/history")
async def list_update_history(channel: str, target: str):
    art_dir = _update_artifacts_dir(channel, target)
    if not os.path.isdir(art_dir):
        return {"items": []}
    items = []
    for name in sorted(os.listdir(art_dir), reverse=True):
        if not name.endswith(".zip"):
            continue
        p = os.path.join(art_dir, name)
        try:
            st = os.stat(p)
        except OSError:
            continue
        items.append({
            "filename": name,
            "size": st.st_size,
            "mtime": int(st.st_mtime),
        })
    return {"items": items}


# ══ Workshop Path ══════════════════════════════════════════════════════

def _workshop_plugin_dir(plugin_id: str) -> str:
    return os.path.join(WORKSHOP_ROOT, _safe_plugin_id(plugin_id))


def _workshop_meta_path(plugin_id: str) -> str:
    return os.path.join(_workshop_plugin_dir(plugin_id), "meta.json")


def _workshop_versions_dir(plugin_id: str) -> str:
    return os.path.join(_workshop_plugin_dir(plugin_id), "versions")


def _load_meta(plugin_id: str) -> Optional[Dict[str, Any]]:
    p = _workshop_meta_path(plugin_id)
    if not os.path.isfile(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _save_meta(plugin_id: str, meta: Dict[str, Any]) -> None:
    p = _workshop_meta_path(plugin_id)
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    os.replace(tmp, p)


_UINT32_MAX = 0xFFFFFFFF


def _clamp_u32(v: Any) -> int:
    # C++ workshop_client 用 uint32_t 接收 rating/downloads，超 4.29B 会 wrap；此处 clamp。
    n = int(v or 0)
    if n < 0:
        return 0
    if n > _UINT32_MAX:
        return _UINT32_MAX
    return n


def _summary_from_meta(meta: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "id": str(meta.get("id", "")),
        "name": str(meta.get("name", "")),
        "version": str(meta.get("version", "")),
        "tag": str(meta.get("tag", "")),
        "author": str(meta.get("author", "")),
        "updated_ms": int(meta.get("updated_ms", 0)),
        "rating": _clamp_u32(meta.get("rating", 0)),
        "downloads": _clamp_u32(meta.get("downloads", 0)),
    }


def _rebuild_catalog() -> List[Dict[str, Any]]:
    items = []
    if os.path.isdir(WORKSHOP_ROOT):
        for name in sorted(os.listdir(WORKSHOP_ROOT)):
            if name.startswith("_"):
                continue
            meta = _load_meta(name)
            if meta:
                items.append(_summary_from_meta(meta))
    catalog = {"items": items, "updated_at": _now_utc_iso()}
    tmp = WORKSHOP_CATALOG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(catalog, f, ensure_ascii=False, indent=2)
    os.replace(tmp, WORKSHOP_CATALOG_PATH)
    return items


def _load_catalog_items() -> List[Dict[str, Any]]:
    if os.path.isfile(WORKSHOP_CATALOG_PATH):
        try:
            with open(WORKSHOP_CATALOG_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
            items = data.get("items", [])
            if isinstance(items, list):
                return items
        except Exception:
            pass
    return _rebuild_catalog()


@app.get("/api/v1/workshop/plugins")
async def list_workshop_plugins(
    page: int = Query(1, ge=1),
    size: int = Query(20, ge=1, le=100),
    tag: str = Query(""),
):
    items = _load_catalog_items()
    if tag:
        items = [it for it in items if str(it.get("tag", "")) == tag]
    total = len(items)
    start = (page - 1) * size
    end = start + size
    slice_ = items[start:end]
    # 每个 item 必须含 C++ summary 全部字段
    result = [
        {
            "id": str(it.get("id", "")),
            "name": str(it.get("name", "")),
            "version": str(it.get("version", "")),
            "tag": str(it.get("tag", "")),
            "author": str(it.get("author", "")),
            "updated_ms": int(it.get("updated_ms", 0)),
            "rating": _clamp_u32(it.get("rating", 0)),
            "downloads": _clamp_u32(it.get("downloads", 0)),
        }
        for it in slice_
    ]
    return {"items": result, "total": total, "page": page, "size": size}


@app.get("/api/v1/workshop/plugins/{plugin_id}")
async def get_workshop_plugin_detail(plugin_id: str):
    meta = _load_meta(_safe_plugin_id(plugin_id))
    if meta is None:
        raise HTTPException(404, "plugin not found")
    return {
        # summary
        "id": str(meta.get("id", "")),
        "name": str(meta.get("name", "")),
        "version": str(meta.get("version", "")),
        "tag": str(meta.get("tag", "")),
        "author": str(meta.get("author", "")),
        "updated_ms": int(meta.get("updated_ms", 0)),
        "rating": _clamp_u32(meta.get("rating", 0)),
        "downloads": _clamp_u32(meta.get("downloads", 0)),
        # detail
        "description": str(meta.get("description", "")),
        "sha256": str(meta.get("sha256", "")),
        "signature_alg": str(meta.get("signature_alg", "ed25519")),
        "size_bytes": int(meta.get("size_bytes", 0)),
        "min_major": _clamp_u32(meta.get("min_major", 0)),
        "min_minor": _clamp_u32(meta.get("min_minor", 0)),
        "min_patch": _clamp_u32(meta.get("min_patch", 0)),
    }


@app.get("/api/v1/workshop/plugins/{plugin_id}/download")
async def download_workshop_plugin(plugin_id: str):
    meta = _load_meta(_safe_plugin_id(plugin_id))
    if meta is None:
        raise HTTPException(404, "plugin not found")
    version = str(meta.get("version", ""))
    versions_dir = _workshop_versions_dir(plugin_id)
    filename = f"{plugin_id}-{version}.sao-plugin"
    p = os.path.join(versions_dir, filename)
    if not os.path.isfile(p):
        raise HTTPException(404, "artifact not found")

    # 记一次下载 (uint32 clamp — C++ 端存 uint32_t，超 4.29B 直接不再递增)
    try:
        cur = _clamp_u32(meta.get("downloads", 0))
        if cur < _UINT32_MAX:
            meta["downloads"] = cur + 1
            _save_meta(plugin_id, meta)
            _rebuild_catalog()
    except Exception:
        pass

    return FileResponse(p, media_type="application/octet-stream", filename=filename)


@app.post("/api/v1/workshop/plugins/{plugin_id}/publish")
async def publish_workshop_plugin(
    plugin_id: str,
    request: Request,
    version: str = Query(...),
    name: str = Query(""),
    tag: str = Query(""),
    author: str = Query(""),
    signature_alg: str = Query("ed25519"),
    min_major: int = Query(0),
    min_minor: int = Query(0),
    min_patch: int = Query(0),
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
    x_description: Optional[str] = Header(None, alias="X-SaoAuto-Description"),
):
    _require_api_key(x_api_key, request)
    _safe_plugin_id(plugin_id)
    _safe_version(version)
    if signature_alg not in ("ed25519", "ecdsa-p256"):
        raise HTTPException(400, "signature_alg must be 'ed25519' or 'ecdsa-p256'")

    versions_dir = _workshop_versions_dir(plugin_id)
    os.makedirs(versions_dir, exist_ok=True)
    filename = f"{plugin_id}-{version}.sao-plugin"
    dest_path = os.path.join(versions_dir, filename)

    tmp_path = dest_path + ".uploading"
    total = 0
    h = hashlib.sha256()
    try:
        with open(tmp_path, "wb") as out:
            async for chunk in request.stream():
                if not chunk:
                    continue
                out.write(chunk)
                h.update(chunk)
                total += len(chunk)
        if total == 0:
            raise HTTPException(400, "empty request body")
        os.replace(tmp_path, dest_path)
    except HTTPException:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise
    except Exception as e:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise HTTPException(500, f"write failed: {e}")

    prev = _load_meta(plugin_id) or {}
    meta = {
        "id": plugin_id,
        "name": name or prev.get("name", plugin_id),
        "version": version,
        "tag": tag or prev.get("tag", ""),
        "author": author or prev.get("author", ""),
        "updated_ms": _now_ms(),
        "rating": _clamp_u32(prev.get("rating", 0)),
        "downloads": _clamp_u32(prev.get("downloads", 0)),
        "description": x_description or prev.get("description", ""),
        "sha256": h.hexdigest(),
        "signature_alg": signature_alg,
        "size_bytes": total,
        "min_major": _clamp_u32(min_major),
        "min_minor": _clamp_u32(min_minor),
        "min_patch": _clamp_u32(min_patch),
        "published_at": _now_utc_iso(),
    }
    _save_meta(plugin_id, meta)
    _rebuild_catalog()

    return {
        "ok": True,
        "id": plugin_id,
        "version": version,
        "sha256": meta["sha256"],
        "size": total,
    }


@app.delete("/api/v1/workshop/plugins/{plugin_id}")
async def delete_workshop_plugin(
    plugin_id: str,
    request: Request,
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    plugin_id = _safe_plugin_id(plugin_id)
    plugin_dir = _workshop_plugin_dir(plugin_id)
    if not os.path.isdir(plugin_dir):
        raise HTTPException(404, "plugin not found")
    import shutil
    shutil.rmtree(plugin_dir, ignore_errors=True)
    _rebuild_catalog()
    return {"ok": True, "id": plugin_id}


# ══ Admin/Debug ═════════════════════════════════════════════════════════

@app.get("/api/v1/workshop/refresh_catalog")
async def refresh_catalog(x_api_key: Optional[str] = Header(None, alias="X-API-Key"), request: Request = None):
    _require_api_key(x_api_key, request)
    items = _rebuild_catalog()
    return {"ok": True, "items": len(items)}


if __name__ == "__main__":
    import uvicorn
    port = int(os.environ.get("UPDATE_HOST_PORT", "9973"))
    uvicorn.run(app, host="0.0.0.0", port=port, timeout_keep_alive=120)
