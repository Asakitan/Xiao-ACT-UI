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
#   GET  /api/v1/workshop/admin
#           → 浏览器管理页（API key 仅由页面通过 X-API-Key header 提交）
#   GET  /api/v1/workshop/admin/plugins、PATCH/DELETE /api/v1/workshop/plugins/{id}
#           → 管理目录、元数据、旧版本及整插件
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
from urllib.parse import urlsplit

from fastapi import FastAPI, HTTPException, Header, Query, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, Response


# ── 路径 ────────────────────────────────────────────────────────────────

if getattr(sys, "frozen", False):
    HERE = os.path.dirname(sys.executable)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))

RELEASE_DIR = os.environ.get("UPDATE_HOST_RELEASE_DIR", os.path.join(HERE, "releases"))
HOST_CONFIG_PATH = os.path.join(HERE, "update_host_config.json")


def _validated_public_base_url(value: str) -> str:
    normalized = (value or "").strip().rstrip("/")
    try:
        parsed = urlsplit(normalized)
        port = parsed.port
    except ValueError as exc:
        raise RuntimeError("invalid UPDATE_HOST_PUBLIC_BASE_URL") from exc
    if (
        parsed.scheme not in ("http", "https")
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
        or port is None
        or not (1 <= port <= 65535)
    ):
        raise RuntimeError("invalid UPDATE_HOST_PUBLIC_BASE_URL")
    return normalized


PUBLIC_BASE_URL = _validated_public_base_url(
    os.environ.get("UPDATE_HOST_PUBLIC_BASE_URL", "http://x2.sjcmc.cn:15018")
)

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


def _get_publish_api_key() -> str:
    env_key = os.environ.get("SAO_UPDATE_API_KEY", "").strip()
    if env_key:
        return env_key
    return (_load_host_config().get("publish_api_key", "") or "").strip()


def _get_base_url_from_request(request: Request) -> str:
    return PUBLIC_BASE_URL


# ── 校验辅助 ────────────────────────────────────────────────────────────

_SEG_RE = re.compile(r"^[A-Za-z0-9._\-]{1,64}$")
_PLUGIN_ID_RE = re.compile(r"^[A-Za-z0-9._\-]{1,63}$")
_VERSION_RE = re.compile(r"^[0-9]+(\.[0-9]+){1,3}([A-Za-z0-9\-.+]*)?$")
_COMMIT_RE = re.compile(r"^[0-9a-fA-F]{7,64}$")


def _safe_seg(v: str) -> str:
    if not v or not _SEG_RE.match(v) or ".." in v:
        raise HTTPException(400, f"invalid path segment: {v!r}")
    return v


def _safe_plugin_id(v: str) -> str:
    if not v or not _PLUGIN_ID_RE.match(v) or v == "." or ".." in v:
        raise HTTPException(400, f"invalid plugin id: {v!r}")
    return v


def _safe_version(v: str) -> str:
    if not v or not _VERSION_RE.match(v):
        raise HTTPException(400, f"invalid version: {v!r}")
    return v


def _safe_commit(v: str) -> str:
    if not v or not _COMMIT_RE.match(v):
        raise HTTPException(400, "commit must be 7..64 hexadecimal characters")
    return v.lower()


def _require_api_key(x_api_key: Optional[str], request: Request):
    expected = _get_publish_api_key()
    if not expected:
        raise HTTPException(503, "publish_api_key is not configured")
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


def _update_anchor_path(channel: str, target: str) -> str:
    return os.path.join(_update_dir(channel, target), "anchor.json")


def _write_update_anchor(channel: str, target: str, anchor: Dict[str, Any]) -> None:
    path = _update_anchor_path(channel, target)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as stream:
        json.dump(anchor, stream, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def _update_artifacts_dir(channel: str, target: str) -> str:
    return os.path.join(_update_dir(channel, target), "artifacts")


@app.get("/update/{channel}/{target}/anchor")
async def get_update_anchor(channel: str, target: str):
    _safe_seg(channel)
    _safe_seg(target)
    path = _update_anchor_path(channel, target)
    data: Dict[str, Any] = {}
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as stream:
                loaded = json.load(stream)
            if isinstance(loaded, dict):
                data = loaded
        except Exception as exc:
            raise HTTPException(500, "update anchor is unreadable") from exc

    latest_path = _update_latest_path(channel, target)
    if os.path.isfile(latest_path):
        try:
            with open(latest_path, "r", encoding="utf-8") as stream:
                latest = json.load(stream)
        except Exception as exc:
            raise HTTPException(500, "latest update manifest is unreadable") from exc
        latest_commit = str(latest.get("commit", "")) if isinstance(latest, dict) else ""
        if latest_commit and data.get("commit") != latest_commit:
            latest_commit = _safe_commit(latest_commit)
            data = {
                "commit": latest_commit,
                "commit_short": str(latest.get("commit_short", "")) or latest_commit[:8],
                "version": str(latest.get("version", "")),
                "source": "publish-recovery",
                "updated_at": _now_utc_iso(),
            }
            _write_update_anchor(channel, target, data)
    return data


@app.post("/update/{channel}/{target}/anchor")
async def set_update_anchor(
    channel: str,
    target: str,
    request: Request,
    commit: str = Query(...),
    commit_short: str = Query(""),
    version: str = Query(""),
    source: str = Query("manual-sync"),
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    _safe_seg(channel)
    _safe_seg(target)
    commit = _safe_commit(commit)
    commit_short = _safe_commit(commit_short) if commit_short else commit[:8]
    if version:
        _safe_version(version)
    anchor = {
        "commit": commit,
        "commit_short": commit_short,
        "version": version,
        "source": source[:64],
        "updated_at": _now_utc_iso(),
    }
    _write_update_anchor(channel, target, anchor)
    return anchor


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
    commit: str = Query(""),
    commit_short: str = Query(""),
    anchor_commit: str = Query(""),
    anchor_commit_short: str = Query(""),
    anchor_version: str = Query(""),
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    _safe_seg(channel)
    _safe_seg(target)
    _safe_version(version)
    if commit:
        commit = _safe_commit(commit)
        commit_short = _safe_commit(commit_short) if commit_short else commit[:8]
    if anchor_commit:
        anchor_commit = _safe_commit(anchor_commit)
    if anchor_commit_short:
        anchor_commit_short = _safe_commit(anchor_commit_short)
    if anchor_version:
        _safe_version(anchor_version)

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
        "commit": commit,
        "commit_short": commit_short,
        "anchor_commit": anchor_commit,
        "anchor_commit_short": anchor_commit_short,
        "anchor_version": anchor_version,
        "published_at": _now_utc_iso(),
    }

    latest_path = _update_latest_path(channel, target)
    tmp = latest_path + ".tmp"
    os.makedirs(os.path.dirname(latest_path) or ".", exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    os.replace(tmp, latest_path)

    if commit:
        _write_update_anchor(
            channel,
            target,
            {
                "commit": commit,
                "commit_short": commit_short,
                "version": version,
                "source": "publish",
                "updated_at": _now_utc_iso(),
            },
        )

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


def _normalize_game_ids(value: Any) -> List[str]:
    if not isinstance(value, list):
        return []
    result: List[str] = []
    for item in value[:32]:
        game_id = str(item)
        if not _SEG_RE.match(game_id) or ".." in game_id:
            raise HTTPException(400, "invalid Workshop game_id")
        if game_id not in result:
            result.append(game_id)
    return result


def _parse_game_ids(value: str) -> List[str]:
    if not value:
        return []
    try:
        decoded = json.loads(value)
    except Exception as exc:
        raise HTTPException(400, "game_ids must be a JSON array") from exc
    return _normalize_game_ids(decoded)


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
        "game_ids": _normalize_game_ids(meta.get("game_ids", [])),
    }


def _rebuild_catalog() -> List[Dict[str, Any]]:
    items = []
    if os.path.isdir(WORKSHOP_ROOT):
        for name in sorted(os.listdir(WORKSHOP_ROOT)):
            if name.startswith("_"):
                continue
            try:
                safe_name = _safe_plugin_id(name)
            except HTTPException:
                continue
            if safe_name != name:
                continue
            meta = _load_meta(safe_name)
            if isinstance(meta, dict):
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



def _scan_plugin_versions(plugin_id: str) -> List[str]:
    safe_id = _safe_plugin_id(plugin_id)
    versions_dir = _workshop_versions_dir(safe_id)
    if not os.path.isdir(versions_dir):
        return []
    prefix = f"{safe_id}-"
    suffix = ".sao-plugin"
    versions = set()
    for filename in os.listdir(versions_dir):
        if not filename.startswith(prefix) or not filename.endswith(suffix):
            continue
        version = filename[len(prefix):-len(suffix)]
        try:
            safe_version = _safe_version(version)
        except HTTPException:
            continue
        if safe_version == version and os.path.isfile(os.path.join(versions_dir, filename)):
            versions.add(version)
    return sorted(versions)


_ADMIN_META_FIELDS = (
    "id",
    "name",
    "version",
    "tag",
    "author",
    "updated_ms",
    "rating",
    "downloads",
    "game_ids",
    "description",
    "sha256",
    "signature_alg",
    "size_bytes",
    "min_major",
    "min_minor",
    "min_patch",
    "published_at",
)


def _admin_plugin_record(meta: Dict[str, Any], versions: Optional[List[str]] = None) -> Dict[str, Any]:
    record = {key: meta.get(key) for key in _ADMIN_META_FIELDS}
    record["versions"] = list(versions if versions is not None else _scan_plugin_versions(str(meta.get("id", ""))))
    return record


def _admin_text(value: Any, field: str, max_bytes: int) -> str:
    if not isinstance(value, str):
        raise HTTPException(400, f"{field} must be a string")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise HTTPException(400, f"{field} must be valid UTF-8") from exc
    if len(encoded) > max_bytes:
        raise HTTPException(400, f"{field} exceeds {max_bytes} UTF-8 bytes")
    if "\x00" in value:
        raise HTTPException(400, f"{field} must not contain NUL")
    allowed_controls = "\t\r\n" if field == "description" else ""
    if any(
        (ord(character) < 0x20 and character not in allowed_controls)
        or ord(character) == 0x7f
        for character in value
    ):
        raise HTTPException(400, f"{field} must not contain control characters")
    if field == "name" and not value.strip():
        raise HTTPException(400, "name must not be empty")
    return value


def _admin_u32(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise HTTPException(400, f"{field} must be an integer")
    if value < 0 or value > _UINT32_MAX:
        raise HTTPException(400, f"{field} must be between 0 and {_UINT32_MAX}")
    return value


def _load_admin_meta(plugin_id: str) -> Dict[str, Any]:
    meta = _load_meta(plugin_id)
    if not isinstance(meta, dict):
        raise HTTPException(404, "plugin not found")
    try:
        meta_id = _safe_plugin_id(str(meta.get("id", "")))
    except HTTPException as exc:
        raise HTTPException(409, "plugin metadata identity mismatch") from exc
    if meta_id != plugin_id:
        raise HTTPException(409, "plugin metadata identity mismatch")
    return meta

@app.get("/api/v1/workshop/plugins")
async def list_workshop_plugins(
    page: int = Query(1, ge=1),
    size: int = Query(20, ge=1, le=100),
    tag: str = Query(""),
    game_id: str = Query(""),
    search: str = Query(""),
    sort: str = Query("updated_at"),
):
    items = _load_catalog_items()
    game_counts: Dict[str, int] = {}
    for item in items:
        for item_game_id in _normalize_game_ids(item.get("game_ids", [])):
            game_counts[item_game_id] = game_counts.get(item_game_id, 0) + 1
    if tag:
        items = [it for it in items if str(it.get("tag", "")) == tag]
    if game_id:
        _safe_seg(game_id)
        items = [it for it in items if game_id in _normalize_game_ids(it.get("game_ids", []))]
    if search:
        needle = search.casefold()
        items = [
            it for it in items
            if needle in str(it.get("id", "")).casefold()
            or needle in str(it.get("name", "")).casefold()
            or needle in str(it.get("author", "")).casefold()
        ]
    sort_keys = {
        "updated_at": lambda item: int(item.get("updated_ms", 0)),
        "rating": lambda item: _clamp_u32(item.get("rating", 0)),
        "downloads": lambda item: _clamp_u32(item.get("downloads", 0)),
        "name": lambda item: str(item.get("name", "")).casefold(),
    }
    if sort not in sort_keys:
        raise HTTPException(400, "sort must be updated_at|rating|downloads|name")
    items = sorted(items, key=sort_keys[sort], reverse=sort != "name")
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
            "game_ids": _normalize_game_ids(it.get("game_ids", [])),
        }
        for it in slice_
    ]
    return {
        "items": result,
        "total": total,
        "page": page,
        "size": size,
        "game_ids": game_counts,
    }


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
        "game_ids": _normalize_game_ids(meta.get("game_ids", [])),
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
async def download_workshop_plugin(plugin_id: str, version: str = Query("")):
    meta = _load_meta(_safe_plugin_id(plugin_id))
    if meta is None:
        raise HTTPException(404, "plugin not found")
    if version:
        _safe_version(version)
    else:
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
    game_ids: str = Query(""),
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
    parsed_game_ids = _parse_game_ids(game_ids)
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
        "game_ids": parsed_game_ids or _normalize_game_ids(prev.get("game_ids", [])),
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


@app.get("/api/v1/workshop/admin/plugins")
async def list_admin_workshop_plugins(
    request: Request,
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    records = []
    if os.path.isdir(WORKSHOP_ROOT):
        for name in sorted(os.listdir(WORKSHOP_ROOT)):
            plugin_dir = os.path.join(WORKSHOP_ROOT, name)
            if name.startswith("_") or not os.path.isdir(plugin_dir):
                continue
            try:
                safe_name = _safe_plugin_id(name)
            except HTTPException:
                continue
            if safe_name != name:
                continue
            meta = _load_meta(safe_name)
            if not isinstance(meta, dict):
                continue
            try:
                meta_id = _safe_plugin_id(str(meta.get("id", "")))
            except HTTPException:
                continue
            if meta_id != safe_name:
                continue
            records.append(_admin_plugin_record(meta, _scan_plugin_versions(safe_name)))
    return {"items": records, "total": len(records), "updated_at": _now_utc_iso()}


@app.patch("/api/v1/workshop/plugins/{plugin_id}")
async def update_workshop_plugin_meta(
    plugin_id: str,
    request: Request,
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    plugin_id = _safe_plugin_id(plugin_id)
    meta = _load_admin_meta(plugin_id)

    try:
        body = await request.json()
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise HTTPException(400, "request body must be valid JSON") from exc
    if not isinstance(body, dict):
        raise HTTPException(400, "request body must be a JSON object")

    allowed = {
        "name",
        "tag",
        "author",
        "description",
        "game_ids",
        "min_major",
        "min_minor",
        "min_patch",
    }
    unknown = sorted(str(key) for key in body if key not in allowed)
    if unknown:
        raise HTTPException(400, f"unknown fields: {', '.join(unknown)}")

    text_limits = {
        "name": 256,
        "tag": 128,
        "author": 256,
        "description": 64 * 1024,
    }
    for field, max_bytes in text_limits.items():
        if field in body:
            meta[field] = _admin_text(body[field], field, max_bytes)

    if "game_ids" in body:
        game_ids = body["game_ids"]
        if not isinstance(game_ids, list):
            raise HTTPException(400, "game_ids must be a list")
        if len(game_ids) > 32 or any(not isinstance(item, str) for item in game_ids):
            raise HTTPException(400, "game_ids must contain at most 32 strings")
        meta["game_ids"] = _normalize_game_ids(game_ids)

    for field in ("min_major", "min_minor", "min_patch"):
        if field in body:
            meta[field] = _admin_u32(body[field], field)

    meta["updated_ms"] = _now_ms()
    _save_meta(plugin_id, meta)
    _rebuild_catalog()
    return {"ok": True, "plugin": _admin_plugin_record(meta)}


@app.delete("/api/v1/workshop/plugins/{plugin_id}")
async def delete_workshop_plugin(
    plugin_id: str,
    request: Request,
    version: str = Query(""),
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
):
    _require_api_key(x_api_key, request)
    plugin_id = _safe_plugin_id(plugin_id)
    plugin_dir = _workshop_plugin_dir(plugin_id)
    if not os.path.isdir(plugin_dir):
        raise HTTPException(404, "plugin not found")

    meta = _load_admin_meta(plugin_id)

    if version:
        version = _safe_version(version)
        active_version = str(meta.get("version", ""))
        if version == active_version:
            raise HTTPException(
                409,
                "active version cannot be deleted; publish a newer version or delete the entire plugin",
            )
        filename = f"{plugin_id}-{version}.sao-plugin"
        artifact_path = os.path.join(_workshop_versions_dir(plugin_id), filename)
        if not os.path.isfile(artifact_path):
            raise HTTPException(404, "artifact not found")
        try:
            os.remove(artifact_path)
        except OSError as exc:
            raise HTTPException(500, "failed to delete plugin version") from exc
        remaining = _scan_plugin_versions(plugin_id)
        _rebuild_catalog()
        return {
            "ok": True,
            "id": plugin_id,
            "deleted_version": version,
            "remaining": remaining,
        }

    import shutil
    try:
        shutil.rmtree(plugin_dir)
    except OSError as exc:
        raise HTTPException(500, "failed to delete plugin") from exc
    _rebuild_catalog()
    return {"ok": True, "id": plugin_id, "remaining": []}

_ADMIN_HTML_TEMPLATE = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SAO Workshop Admin</title>
<style nonce="__NONCE__">
:root{color-scheme:dark;--bg:#11141a;--panel:#1b2029;--panel2:#222936;--line:#303949;--text:#edf2f7;--muted:#8c98aa;--gold:#e5b84b;--cyan:#5fe4ff;--red:#ff737d}
*{box-sizing:border-box}body{margin:0;padding:28px;background:radial-gradient(circle at 20% 0,#1e2b38 0,#11141a 42%);color:var(--text);font-family:"Segoe UI",system-ui,sans-serif;font-size:14px}
main{max-width:1280px;margin:0 auto}h1{margin:0;color:var(--gold);font-size:24px;letter-spacing:.04em}.sub{margin:5px 0 22px;color:var(--muted);font-size:12px}
.bar{display:flex;flex-wrap:wrap;gap:9px;align-items:center;margin:10px 0}input,textarea{border:1px solid var(--line);border-radius:7px;background:var(--panel);color:var(--text);font:inherit;padding:8px 10px;outline:none}input:focus,textarea:focus{border-color:var(--cyan);box-shadow:0 0 0 2px #5fe4ff22}#api-key{width:min(430px,100%)}#search{width:min(360px,100%)}
button{border:1px solid var(--line);border-radius:7px;background:var(--panel2);color:var(--text);padding:8px 13px;cursor:pointer;font:inherit}button:hover{border-color:var(--cyan);color:var(--cyan)}button.primary{border-color:var(--gold);background:#3b3018;color:var(--gold)}button.danger{color:var(--red)}button.small{padding:4px 8px;font-size:12px}
#message{min-height:22px;margin:12px 0;color:var(--muted)}#message.error{color:var(--red)}#message.ok{color:#6be69a}.table-wrap{overflow:auto;border:1px solid var(--line);border-radius:10px;background:#151a21}
table{width:100%;border-collapse:collapse;min-width:1040px}th{padding:11px 12px;text-align:left;color:var(--muted);font-size:11px;letter-spacing:.08em;text-transform:uppercase;border-bottom:1px solid var(--line);white-space:nowrap}td{padding:12px;border-bottom:1px solid #252d39;vertical-align:top}tr:last-child td{border-bottom:0}tr:hover td{background:#1b222c}.id{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--cyan);font-size:12px}
.versions{display:flex;flex-wrap:wrap;gap:5px;max-width:300px}.version{display:inline-flex;align-items:center;gap:5px;border:1px solid var(--line);border-radius:12px;padding:3px 7px;color:var(--text);font-size:12px}.version.active{border-color:var(--gold);color:var(--gold)}.version button{border:0;background:transparent;color:var(--red);padding:0;font-size:11px}.actions{display:flex;gap:6px;white-space:nowrap}.empty{text-align:center;color:var(--muted);padding:40px}
.modal{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;padding:18px;background:#000a;z-index:10}.modal[hidden]{display:none}.dialog{width:min(560px,100%);max-height:90vh;overflow:auto;border:1px solid var(--line);border-radius:12px;background:var(--panel);padding:20px;box-shadow:0 20px 80px #0008}.dialog h2{margin:0 0 14px;color:var(--gold);font-size:18px}.field{margin:10px 0}.field label{display:block;margin-bottom:5px;color:var(--muted);font-size:12px}.field input,.field textarea{width:100%}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:9px}.grid input{min-width:0}.dialog-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:16px}@media(max-width:700px){body{padding:18px}.grid{grid-template-columns:1fr}}
</style>
</head>
<body>
<main>
<h1>◇ SAO Workshop Admin</h1>
<div class="sub">Manage active v1 Workshop releases. The API key stays in this browser tab only.</div>
<div class="bar"><input id="api-key" type="password" autocomplete="off" placeholder="Workshop / Update X-API-Key"><button id="load">Load</button><button id="clear-key">Clear key</button></div>
<div class="bar"><input id="search" type="search" placeholder="Search by name or ID"><button id="refresh-list">Refresh list</button><button id="refresh-catalog">Refresh catalog</button></div>
<div id="message" role="status" aria-live="polite"></div>
<div class="table-wrap"><table><thead><tr><th>ID</th><th>Name</th><th>Author</th><th>Current version</th><th>All versions</th><th>Downloads</th><th>Rating</th><th>Updated</th><th>Actions</th></tr></thead><tbody id="plugin-rows"><tr><td colspan="9" class="empty">Enter an API key and load plugins.</td></tr></tbody></table></div>
</main>
<div id="edit-modal" class="modal" hidden><div class="dialog" role="dialog" aria-modal="true" aria-labelledby="edit-title"><h2 id="edit-title">Edit plugin metadata</h2>
<div class="field"><label for="edit-name">Name</label><input id="edit-name"></div><div class="field"><label for="edit-tag">Tag</label><input id="edit-tag"></div><div class="field"><label for="edit-author">Author</label><input id="edit-author"></div><div class="field"><label for="edit-description">Description</label><textarea id="edit-description" rows="5"></textarea></div><div class="field"><label for="edit-game-ids">Game IDs (comma separated)</label><input id="edit-game-ids"></div>
<div class="grid"><div class="field"><label for="edit-major">Min major</label><input id="edit-major" type="number" min="0" max="4294967295" step="1"></div><div class="field"><label for="edit-minor">Min minor</label><input id="edit-minor" type="number" min="0" max="4294967295" step="1"></div><div class="field"><label for="edit-patch">Min patch</label><input id="edit-patch" type="number" min="0" max="4294967295" step="1"></div></div>
<div class="dialog-actions"><button id="cancel-edit">Cancel</button><button id="save-edit" class="primary">Save</button></div></div></div>
<script nonce="__NONCE__">
(function(){
"use strict";
var STORAGE_KEY="sao-workshop-admin-api-key",keyInput=document.getElementById("api-key"),searchInput=document.getElementById("search"),rows=document.getElementById("plugin-rows"),message=document.getElementById("message"),modal=document.getElementById("edit-modal"),plugins=[],editingId="";
try{keyInput.value=sessionStorage.getItem(STORAGE_KEY)||"";}catch(e){}
function rememberKey(){var value=keyInput.value.trim();try{if(value){sessionStorage.setItem(STORAGE_KEY,value);}else{sessionStorage.removeItem(STORAGE_KEY);}}catch(e){}return value;}
function setMessage(text,kind){message.textContent=text||"";message.className=kind||"";}
function detailOf(data,status){if(data&&Array.isArray(data.detail)){return data.detail.map(function(item){return typeof item==="string"?item:JSON.stringify(item);}).join("; ");}if(data&&data.detail){return String(data.detail);}return "HTTP "+status;}
async function api(method,path,body){var key=rememberKey(),options={method:method,headers:{"X-API-Key":key,"Accept":"application/json"}};if(body!==undefined){options.headers["Content-Type"]="application/json";options.body=JSON.stringify(body);}var response=await fetch(path,options),raw=await response.text(),data={};if(raw){try{data=JSON.parse(raw);}catch(e){data={detail:raw};}}if(!response.ok){throw new Error(detailOf(data,response.status));}return data;}
function textCell(row,value,extra){var cell=document.createElement("td");cell.textContent=value==null?"":String(value);if(extra){cell.className=extra;}row.appendChild(cell);return cell;}
function updatedText(plugin){if(Number(plugin.updated_ms)>0){return new Date(Number(plugin.updated_ms)).toLocaleString();}return plugin.published_at||"";}
function visiblePlugins(){var needle=searchInput.value.trim().toLowerCase();if(!needle){return plugins;}return plugins.filter(function(plugin){return String(plugin.id||"").toLowerCase().indexOf(needle)>=0||String(plugin.name||"").toLowerCase().indexOf(needle)>=0;});}
function renderVersions(cell,plugin){var list=Array.isArray(plugin.versions)?plugin.versions:[];if(!list.length){cell.textContent="—";return;}var box=document.createElement("div");box.className="versions";list.forEach(function(version){var item=document.createElement("span");item.className="version"+(version===plugin.version?" active":"");var label=document.createElement("span");label.textContent=String(version);item.appendChild(label);if(version!==plugin.version){var remove=document.createElement("button");remove.type="button";remove.textContent="×";remove.title="Delete old version";remove.addEventListener("click",function(){deleteVersion(plugin.id,version);});item.appendChild(remove);}box.appendChild(item);});cell.appendChild(box);}
function render(){rows.replaceChildren();var list=visiblePlugins();if(!list.length){var empty=document.createElement("tr"),cell=textCell(empty,"No plugins found.","empty");cell.colSpan=9;rows.appendChild(empty);return;}list.forEach(function(plugin){var row=document.createElement("tr");textCell(row,plugin.id,"id");textCell(row,plugin.name||"");textCell(row,plugin.author||"");textCell(row,plugin.version||"");renderVersions(textCell(row,""),plugin);textCell(row,plugin.downloads==null?0:plugin.downloads);textCell(row,plugin.rating==null?0:plugin.rating);textCell(row,updatedText(plugin));var actions=textCell(row,"");actions.className="actions";var edit=document.createElement("button");edit.type="button";edit.className="small";edit.textContent="Edit";edit.addEventListener("click",function(){openEdit(plugin);});actions.appendChild(edit);var remove=document.createElement("button");remove.type="button";remove.className="small danger";remove.textContent="Delete";remove.addEventListener("click",function(){deletePlugin(plugin.id);});actions.appendChild(remove);rows.appendChild(row);});}
async function loadPlugins(){if(!rememberKey()){setMessage("Enter an X-API-Key first.","error");return;}try{var data=await api("GET","/api/v1/workshop/admin/plugins");plugins=Array.isArray(data.items)?data.items:[];render();setMessage("Loaded "+plugins.length+" plugin(s).","ok");}catch(error){setMessage(error.message,"error");}}
async function refreshCatalog(){if(!rememberKey()){setMessage("Enter an X-API-Key first.","error");return;}try{await api("GET","/api/v1/workshop/refresh_catalog");setMessage("Catalog refreshed.","ok");await loadPlugins();}catch(error){setMessage(error.message,"error");}}
async function deletePlugin(id){if(!window.confirm("Delete plugin "+id+" and all versions?")){return;}try{await api("DELETE","/api/v1/workshop/plugins/"+encodeURIComponent(id));setMessage("Plugin deleted.","ok");await loadPlugins();}catch(error){setMessage(error.message,"error");}}
async function deleteVersion(id,version){if(!window.confirm("Delete old version "+version+"?")){return;}try{await api("DELETE","/api/v1/workshop/plugins/"+encodeURIComponent(id)+"?version="+encodeURIComponent(version));setMessage("Version deleted.","ok");await loadPlugins();}catch(error){setMessage(error.message,"error");}}
function value(id){return document.getElementById(id).value;}
function openEdit(plugin){editingId=String(plugin.id||"");document.getElementById("edit-name").value=plugin.name||"";document.getElementById("edit-tag").value=plugin.tag||"";document.getElementById("edit-author").value=plugin.author||"";document.getElementById("edit-description").value=plugin.description||"";document.getElementById("edit-game-ids").value=Array.isArray(plugin.game_ids)?plugin.game_ids.join(", "):"";document.getElementById("edit-major").value=plugin.min_major==null?0:plugin.min_major;document.getElementById("edit-minor").value=plugin.min_minor==null?0:plugin.min_minor;document.getElementById("edit-patch").value=plugin.min_patch==null?0:plugin.min_patch;modal.hidden=false;}
function closeEdit(){modal.hidden=true;editingId="";}
function u32(id){var number=Number(value(id));if(!Number.isInteger(number)||number<0||number>4294967295){throw new Error(id+" must be an integer between 0 and 4294967295");}return number;}
async function saveEdit(){try{var ids=value("edit-game-ids").split(",").map(function(item){return item.trim();}).filter(Boolean),body={name:value("edit-name"),tag:value("edit-tag"),author:value("edit-author"),description:value("edit-description"),game_ids:ids,min_major:u32("edit-major"),min_minor:u32("edit-minor"),min_patch:u32("edit-patch")};await api("PATCH","/api/v1/workshop/plugins/"+encodeURIComponent(editingId),body);closeEdit();setMessage("Plugin metadata saved.","ok");await loadPlugins();}catch(error){setMessage(error.message,"error");}}
document.getElementById("load").addEventListener("click",loadPlugins);document.getElementById("refresh-list").addEventListener("click",loadPlugins);document.getElementById("refresh-catalog").addEventListener("click",refreshCatalog);document.getElementById("clear-key").addEventListener("click",function(){keyInput.value="";rememberKey();plugins=[];render();setMessage("API key cleared from this tab.","ok");});document.getElementById("cancel-edit").addEventListener("click",closeEdit);document.getElementById("save-edit").addEventListener("click",saveEdit);searchInput.addEventListener("input",render);render();
})();
</script>
</body>
</html>"""


@app.get("/api/v1/workshop/admin", response_class=HTMLResponse)
async def workshop_admin_page():
    nonce = secrets.token_urlsafe(24)
    html = _ADMIN_HTML_TEMPLATE.replace("__NONCE__", nonce)
    return HTMLResponse(
        html,
        headers={
            "Cache-Control": "no-store",
            "X-Frame-Options": "DENY",
            "X-Content-Type-Options": "nosniff",
            "Referrer-Policy": "no-referrer",
            "Content-Security-Policy": (
                f"default-src 'none'; style-src 'nonce-{nonce}'; script-src 'nonce-{nonce}'; "
                "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
            ),
        },
    )

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
