# -*- coding: utf-8 -*-
"""Creative Workshop API routes — plugin marketplace on update_host."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import shutil
import time
import zipfile
from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse

router = APIRouter(prefix="/api/workshop", tags=["workshop"])

_WORKSHOP_DIR: str = ""
_AUTH_FN = None
_ACTIVE_UPLOADS: dict = {}
_UPLOAD_TTL = 600
_CHUNK_DIR: str = ""


def init_workshop(release_dir: str, auth_fn):
    global _WORKSHOP_DIR, _AUTH_FN, _CHUNK_DIR
    _WORKSHOP_DIR = os.path.join(release_dir, "workshop")
    _CHUNK_DIR = os.path.join(release_dir, "_workshop_chunks")
    _AUTH_FN = auth_fn


def _ws_dir() -> str:
    os.makedirs(_WORKSHOP_DIR, exist_ok=True)
    return _WORKSHOP_DIR


def _plugin_dir(plugin_id: str) -> str:
    safe = _safe_id(plugin_id)
    d = os.path.join(_ws_dir(), safe)
    os.makedirs(d, exist_ok=True)
    return d


def _safe_id(raw: str) -> str:
    return "".join(c for c in (raw or "") if c.isalnum() or c in "-_").strip("-_") or "unknown"


def _safe_ver(raw: str) -> str:
    return "".join(c for c in (raw or "") if c.isalnum() or c in ".-").strip(".-") or "0.0.0"


def _load_json(path: str) -> dict | list | None:
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _save_json(path: str, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


def _catalog_path() -> str:
    return os.path.join(_ws_dir(), "catalog.json")


def _load_catalog() -> list[dict]:
    data = _load_json(_catalog_path())
    return data if isinstance(data, list) else []


def _save_catalog(entries: list[dict]):
    _save_json(_catalog_path(), entries)


def _rebuild_catalog_entry(plugin_id: str, meta: dict) -> dict:
    return {
        "plugin_id": plugin_id,
        "name": meta.get("name", plugin_id),
        "version": meta.get("version", "0.0.0"),
        "author": meta.get("author", ""),
        "description": meta.get("description", ""),
        "game_ids": meta.get("game_ids", []),
        "tags": meta.get("tags", []),
        "language": meta.get("language", "python"),
        "icon_url": meta.get("icon_url", ""),
        "download_count": meta.get("download_count", 0),
        "size": meta.get("size", 0),
        "published_at": meta.get("published_at", ""),
        "updated_at": meta.get("updated_at", ""),
    }


def _upsert_catalog(plugin_id: str, meta: dict):
    catalog = _load_catalog()
    entry = _rebuild_catalog_entry(plugin_id, meta)
    catalog = [e for e in catalog if e.get("plugin_id") != plugin_id]
    catalog.append(entry)
    catalog.sort(key=lambda e: e.get("updated_at", ""), reverse=True)
    _save_catalog(catalog)


def _cleanup_stale_uploads():
    now = time.monotonic()
    expired = [uid for uid, info in _ACTIVE_UPLOADS.items() if now - info["created"] > _UPLOAD_TTL]
    for uid in expired:
        info = _ACTIVE_UPLOADS.pop(uid, None)
        if info:
            d = info.get("chunk_dir", "")
            if d and os.path.isdir(d):
                shutil.rmtree(d, ignore_errors=True)


# ── Read endpoints (no auth) ──────────────────────────────────────


@router.get("/catalog")
def catalog(
    game_id: str = "",
    tag: str = "",
    search: str = "",
    sort: str = "updated_at",
    order: str = "desc",
    page: int = 1,
    per_page: int = 40,
):
    entries = _load_catalog()

    if game_id:
        entries = [e for e in entries if game_id in e.get("game_ids", [])]
    if tag:
        entries = [e for e in entries if tag in e.get("tags", [])]
    if search:
        q = search.lower()
        entries = [
            e for e in entries
            if q in (e.get("name") or "").lower()
            or q in (e.get("description") or "").lower()
            or q in (e.get("author") or "").lower()
            or q in (e.get("plugin_id") or "").lower()
        ]

    sort_key = sort if sort in ("updated_at", "published_at", "download_count", "name") else "updated_at"
    reverse = order != "asc"
    entries.sort(key=lambda e: e.get(sort_key, ""), reverse=reverse)

    total = len(entries)
    page = max(1, page)
    per_page = max(1, min(per_page, 100))
    start = (page - 1) * per_page
    page_entries = entries[start:start + per_page]

    all_game_ids: dict[str, int] = {}
    all_tags: dict[str, int] = {}
    for e in _load_catalog():
        for g in e.get("game_ids", []):
            all_game_ids[g] = all_game_ids.get(g, 0) + 1
        for t in e.get("tags", []):
            all_tags[t] = all_tags.get(t, 0) + 1

    return JSONResponse({
        "ok": True,
        "total": total,
        "page": page,
        "per_page": per_page,
        "plugins": page_entries,
        "game_ids": all_game_ids,
        "tags": all_tags,
    })


@router.get("/detail/{plugin_id}")
def detail(plugin_id: str):
    safe = _safe_id(plugin_id)
    meta_path = os.path.join(_plugin_dir(safe), "meta.json")
    meta = _load_json(meta_path)
    if not meta:
        raise HTTPException(404, f"plugin {safe} not found")
    return JSONResponse({"ok": True, "plugin": meta})


@router.get("/download/{plugin_id}")
def download(plugin_id: str, version: str = ""):
    safe = _safe_id(plugin_id)
    pdir = os.path.join(_ws_dir(), safe)
    if not os.path.isdir(pdir):
        raise HTTPException(404, f"plugin {safe} not found")

    if not version:
        manifest = _load_json(os.path.join(pdir, "manifest.json"))
        if manifest:
            version = manifest.get("version", "")
    version = _safe_ver(version)

    zip_path = os.path.join(pdir, f"plugin-{version}.zip")
    if not os.path.isfile(zip_path):
        raise HTTPException(404, f"version {version} not found for {safe}")

    meta_path = os.path.join(pdir, "meta.json")
    meta = _load_json(meta_path)
    if isinstance(meta, dict):
        meta["download_count"] = meta.get("download_count", 0) + 1
        _save_json(meta_path, meta)
        _upsert_catalog(safe, meta)

    return FileResponse(zip_path, media_type="application/zip",
                        filename=f"{safe}-{version}.zip")


# ── Write endpoints (auth required) ──────────────────────────────


@router.post("/publish/init")
async def publish_init(
    request: Request,
    plugin_id: str,
    version: str,
    file_size: int,
    chunk_size: int = 4 * 1024 * 1024,
    name: str = "",
    author: str = "",
    description: str = "",
    long_description: str = "",
    game_ids: str = "[]",
    tags: str = "[]",
    language: str = "python",
    minimum_app_version: str = "",
    requires: str = "[]",
    permissions: str = "[]",
):
    if _AUTH_FN:
        _AUTH_FN(request)
    _cleanup_stale_uploads()

    if file_size <= 0:
        raise HTTPException(400, "file_size must be > 0")
    chunk_size = max(512 * 1024, min(chunk_size, 32 * 1024 * 1024))
    total_chunks = (file_size + chunk_size - 1) // chunk_size

    uid = secrets.token_hex(12)
    os.makedirs(_CHUNK_DIR, exist_ok=True)
    chunk_dir = os.path.join(_CHUNK_DIR, uid)
    os.makedirs(chunk_dir, exist_ok=True)

    def _parse_json_list(raw: str) -> list:
        try:
            v = json.loads(raw)
            return v if isinstance(v, list) else []
        except Exception:
            return []

    _ACTIVE_UPLOADS[uid] = {
        "created": time.monotonic(),
        "chunk_dir": chunk_dir,
        "chunk_size": chunk_size,
        "file_size": file_size,
        "total_chunks": total_chunks,
        "received": set(),
        "meta": {
            "plugin_id": _safe_id(plugin_id),
            "version": _safe_ver(version),
            "name": name or plugin_id,
            "author": author,
            "description": description,
            "long_description": long_description,
            "game_ids": _parse_json_list(game_ids),
            "tags": _parse_json_list(tags),
            "language": language,
            "minimum_app_version": minimum_app_version,
            "requires": _parse_json_list(requires),
            "permissions": _parse_json_list(permissions),
        },
    }

    return JSONResponse({
        "upload_id": uid,
        "chunk_size": chunk_size,
        "total_chunks": total_chunks,
    })


@router.post("/publish/chunk")
async def publish_chunk(request: Request, upload_id: str, index: int):
    if _AUTH_FN:
        _AUTH_FN(request)

    info = _ACTIVE_UPLOADS.get(upload_id)
    if not info:
        raise HTTPException(404, "upload session not found or expired")
    if index < 0 or index >= info["total_chunks"]:
        raise HTTPException(400, f"chunk index out of range [0, {info['total_chunks']})")

    chunk_path = os.path.join(info["chunk_dir"], f"{index:06d}")
    body = await request.body()
    if not body:
        raise HTTPException(400, "empty chunk")

    with open(chunk_path, "wb") as f:
        f.write(body)
    info["received"].add(index)

    return JSONResponse({
        "index": index,
        "size": len(body),
        "received": len(info["received"]),
        "total": info["total_chunks"],
    })


@router.post("/publish/complete")
async def publish_complete(request: Request, upload_id: str):
    if _AUTH_FN:
        _AUTH_FN(request)

    info = _ACTIVE_UPLOADS.get(upload_id)
    if not info:
        raise HTTPException(404, "upload session not found or expired")

    missing = set(range(info["total_chunks"])) - info["received"]
    if missing:
        raise HTTPException(400, f"missing {len(missing)} chunks: {sorted(missing)[:20]}")

    meta = info["meta"]
    safe_id = meta["plugin_id"]
    safe_ver = meta["version"]

    pdir = _plugin_dir(safe_id)
    fname = f"plugin-{safe_ver}.zip"
    dst = os.path.join(pdir, fname)

    hasher = hashlib.sha256()
    size = 0
    tmp = dst + ".assembling"
    try:
        with open(tmp, "wb") as out:
            for i in range(info["total_chunks"]):
                chunk_path = os.path.join(info["chunk_dir"], f"{i:06d}")
                with open(chunk_path, "rb") as cf:
                    data = cf.read()
                out.write(data)
                hasher.update(data)
                size += len(data)
        os.replace(tmp, dst)
    except Exception as exc:
        try:
            os.remove(tmp)
        except Exception:
            pass
        raise HTTPException(500, f"assembly failed: {exc}") from exc
    finally:
        shutil.rmtree(info["chunk_dir"], ignore_errors=True)
        _ACTIVE_UPLOADS.pop(upload_id, None)

    digest = hasher.hexdigest()
    now_str = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    try:
        with zipfile.ZipFile(dst, "r") as zf:
            names = zf.namelist()
            pjson_name = ""
            icon_name = ""
            for n in names:
                base = os.path.basename(n)
                if base == "plugin.json":
                    pjson_name = n
                if base in ("icon.png", "icon.jpg", "icon.svg"):
                    icon_name = n
            if pjson_name:
                pjson = json.loads(zf.read(pjson_name).decode("utf-8"))
                if isinstance(pjson, dict):
                    meta["name"] = meta["name"] or pjson.get("name", safe_id)
                    if not meta["description"]:
                        meta["description"] = pjson.get("description", "")
                    if not meta["game_ids"]:
                        meta["game_ids"] = pjson.get("game_ids", [])
            if icon_name:
                ext = os.path.splitext(icon_name)[1]
                icon_dst = os.path.join(pdir, f"icon{ext}")
                with open(icon_dst, "wb") as f:
                    f.write(zf.read(icon_name))
                meta["icon_url"] = f"/downloads/workshop/{safe_id}/icon{ext}"
    except Exception:
        pass

    existing_meta = _load_json(os.path.join(pdir, "meta.json"))
    prev_count = 0
    if isinstance(existing_meta, dict):
        prev_count = existing_meta.get("download_count", 0)

    meta.update({
        "download_url": f"/downloads/workshop/{safe_id}/{fname}",
        "sha256": digest,
        "size": size,
        "download_count": prev_count,
        "published_at": meta.get("published_at") or now_str,
        "updated_at": now_str,
    })

    _save_json(os.path.join(pdir, "meta.json"), meta)
    _save_json(os.path.join(pdir, "manifest.json"), {
        "plugin_id": safe_id,
        "version": safe_ver,
        "download_url": meta["download_url"],
        "sha256": digest,
        "size": size,
    })
    _save_json(os.path.join(pdir, f"manifest-{safe_ver}.json"), meta)

    versions_path = os.path.join(pdir, "versions.json")
    versions = _load_json(versions_path)
    if not isinstance(versions, list):
        versions = []
    if safe_ver not in versions:
        versions.append(safe_ver)
    versions.sort()
    _save_json(versions_path, versions)

    _upsert_catalog(safe_id, meta)

    return JSONResponse({"ok": True, "plugin": meta})
