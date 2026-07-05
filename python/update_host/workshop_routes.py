# -*- coding: utf-8 -*-
# Creative Workshop API routes — plugin marketplace on update_host.

from __future__ import annotations

import hashlib
import hmac
import json
import os
import secrets
import shutil
import time
import zipfile
from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse

router = APIRouter(prefix="/api/workshop", tags=["workshop"])

_WORKSHOP_DIR: str = ""
_AUTH_FN = None
_ACTIVE_UPLOADS: dict = {}
_UPLOAD_TTL = 600
_CHUNK_DIR: str = ""
#: Sibling of ``release_dir`` (NOT a subdirectory) — the ``/downloads`` static
#: mount only serves ``release_dir`` and below, so this stays unreachable over
#: HTTP no matter what ends up in it. Holds: raw source zips for closed-source
#: plugins + their AES content keys. Never referenced by any download route.
_WORKSHOP_PRIVATE_DIR: str = ""


def init_workshop(release_dir: str, auth_fn):
    global _WORKSHOP_DIR, _AUTH_FN, _CHUNK_DIR, _WORKSHOP_PRIVATE_DIR
    _WORKSHOP_DIR = os.path.join(release_dir, "workshop")
    _CHUNK_DIR = os.path.join(release_dir, "_workshop_chunks")
    _WORKSHOP_PRIVATE_DIR = os.path.join(
        os.path.dirname(os.path.abspath(release_dir.rstrip("/\\"))), "_workshop_private")
    _AUTH_FN = auth_fn


def _private_dir(plugin_id: str) -> str:
    d = os.path.join(_WORKSHOP_PRIVATE_DIR, _safe_id(plugin_id))
    os.makedirs(d, exist_ok=True)
    return d


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


def _sha256_and_size(path: str) -> tuple[str, int]:
    hasher = hashlib.sha256()
    size = 0
    with open(path, "rb") as fp:
        while True:
            chunk = fp.read(1024 * 1024)
            if not chunk:
                break
            hasher.update(chunk)
            size += len(chunk)
    return hasher.hexdigest(), size


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
        "access_level": meta.get("access_level", "free"),
        "open_source": bool(meta.get("open_source", True)),
        "protected": bool(meta.get("protected", False)),
        "published_at": meta.get("published_at", ""),
        "updated_at": meta.get("updated_at", ""),
        # 内部字段, 只用来在 catalog() 里按调用方 token 算 is_mine, 从不
        # 直接出现在对外的 JSON 响应里(见 catalog() 的裁剪逻辑)。
        "_uploader_token": meta.get("uploader_token", ""),
    }


def _upsert_catalog(plugin_id: str, meta: dict):
    catalog = _load_catalog()
    entry = _rebuild_catalog_entry(plugin_id, meta)
    catalog = [e for e in catalog if e.get("plugin_id") != plugin_id]
    catalog.append(entry)
    catalog.sort(key=lambda e: e.get("updated_at", ""), reverse=True)
    _save_catalog(catalog)


def _protect_uploaded_plugin(plugin_id: str, version: str, meta: dict, public_zip_path: str) -> None:
    # Move the just-uploaded raw zip out of the public tree, compile it into a
    # native+encrypted artifact, and put *that* back at ``public_zip_path``.
    #
    # Raises :class:`HTTPException` on any failure — a closed-source publish
    # must never silently fall back to shipping the plaintext source, and must
    # never leave the public slot pointing at a file that no longer exists
    # without the caller finding out.
    private_dir = _private_dir(plugin_id)
    raw_private_path = os.path.join(private_dir, f"plugin-{version}.source.zip")
    try:
        shutil.move(public_zip_path, raw_private_path)
    except Exception as exc:
        raise HTTPException(500, f"failed to isolate source archive: {exc}") from exc

    from update_host import build_service
    build_log: list[str] = []
    result = build_service.build_protected_plugin(
        raw_private_path, private_dir, log=build_log.append)

    if not result.get("ok"):
        raise HTTPException(
            400,
            f"closed-source build failed for {plugin_id} v{version}: {result.get('message')} "
            f"— log: {'; '.join(build_log[-10:])}",
        )

    try:
        shutil.move(result["artifact_path"], public_zip_path)
    except Exception as exc:
        raise HTTPException(500, f"failed to publish protected artifact: {exc}") from exc

    key_path = os.path.join(private_dir, f"{version}.key")
    with open(key_path, "wb") as fp:
        fp.write(result["content_key"])

    meta["protected"] = True
    meta["native_abi"] = result.get("native_abi", "")


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


def _present_catalog_entry(entry: dict, caller_token: str) -> dict:
    # 外部可见版本: 剥掉内部 _uploader_token, 换成一个不泄露原值的 is_mine 布尔。
    out = {k: v for k, v in entry.items() if k != "_uploader_token"}
    owner = entry.get("_uploader_token", "")
    out["is_mine"] = bool(caller_token) and bool(owner) and hmac.compare_digest(str(owner), str(caller_token))
    return out


@router.get("/catalog")
def catalog(
    request: Request,
    game_id: str = "",
    tag: str = "",
    search: str = "",
    sort: str = "updated_at",
    order: str = "desc",
    page: int = 1,
    per_page: int = 40,
):
    caller_token = (request.headers.get("X-API-Key") or "").strip()
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
        "plugins": [_present_catalog_entry(e, caller_token) for e in page_entries],
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
def download(request: Request, plugin_id: str, version: str = ""):
    safe = _safe_id(plugin_id)
    pdir = os.path.join(_ws_dir(), safe)
    if not os.path.isdir(pdir):
        raise HTTPException(404, f"plugin {safe} not found")

    meta_path = os.path.join(pdir, "meta.json")
    meta = _load_json(meta_path)

    if isinstance(meta, dict) and meta.get("access_level") == "paid":
        client_paid = request.headers.get("X-Paid-User", "").lower() in ("true", "1")
        if not client_paid:
            raise HTTPException(403, "This plugin requires a paid license")

    if not version:
        manifest = _load_json(os.path.join(pdir, "manifest.json"))
        if manifest:
            version = manifest.get("version", "")
    version = _safe_ver(version)

    zip_path = os.path.join(pdir, f"plugin-{version}.zip")
    if not os.path.isfile(zip_path):
        raise HTTPException(404, f"version {version} not found for {safe}")

    if isinstance(meta, dict):
        meta["download_count"] = meta.get("download_count", 0) + 1
        _save_json(meta_path, meta)
        _upsert_catalog(safe, meta)

    return FileResponse(zip_path, media_type="application/zip",
                        filename=f"{safe}-{version}.zip")


@router.get("/key/{plugin_id}")
def get_content_key(request: Request, plugin_id: str, version: str = ""):
    # Issue the AES-256 content key for a closed-source (protected) plugin
    # build. Requires a workshop token (``X-API-Key``, device-bound — see
    # ``workshop.app.get_workshop_token``) on every call; no caching anywhere
    # server-side per-caller, so this is a live network dependency by design,
    # not a one-time unlock.
    token = _get_workshop_token(request)
    safe = _safe_id(plugin_id)
    pdir = os.path.join(_ws_dir(), safe)
    meta = _load_json(os.path.join(pdir, "meta.json"))
    if not isinstance(meta, dict):
        raise HTTPException(404, f"plugin {safe} not found")
    if not meta.get("protected"):
        raise HTTPException(400, f"plugin {safe} is not a protected build")

    if meta.get("access_level") == "paid":
        client_paid = request.headers.get("X-Paid-User", "").lower() in ("true", "1")
        if not client_paid:
            raise HTTPException(403, "This plugin requires a paid license")

    if not version:
        manifest = _load_json(os.path.join(pdir, "manifest.json"))
        version = manifest.get("version", "") if manifest else ""
    safe_ver = _safe_ver(version)

    key_path = os.path.join(_private_dir(safe), f"{safe_ver}.key")
    if not os.path.isfile(key_path):
        raise HTTPException(404, f"no content key for {safe} v{safe_ver}")

    import base64
    with open(key_path, "rb") as fp:
        key_bytes = fp.read()
    # 服务器不落任何"谁在什么时候取过这把 key"的日志之外的东西 —— 密钥本身
    # 从不缓存到磁盘之外的地方，每次请求都是一次活的握手。token 只用来判断
    # "有没有资格拿"，不代表这把 key 之后就能离线复用。
    return JSONResponse({"ok": True, "key_b64": base64.b64encode(key_bytes).decode("ascii")})


# ── Write endpoints (workshop token auth) ────────────────────────


def _get_workshop_token(request: Request) -> str:
    token = (request.headers.get("X-API-Key") or "").strip()
    if not token:
        raise HTTPException(401, "missing workshop token (X-API-Key header)")
    return token


def _check_plugin_owner(plugin_id: str, token: str):
    safe = _safe_id(plugin_id)
    meta = _load_json(os.path.join(_ws_dir(), safe, "meta.json"))
    if isinstance(meta, dict):
        owner = meta.get("uploader_token", "")
        if owner and owner != token:
            try:
                if _AUTH_FN:
                    pass
            except Exception:
                pass
            raise HTTPException(403, "only the original uploader or admin can update this plugin")


def _authorize_delete(request: Request, plugin_id: str) -> None:
    # 原始上传者凭自己的 workshop token 能删自己的；管理员 key 能删任何插件。
    #
    # ★先查 ownership 再兜底查 _AUTH_FN，顺序不能反：_authorize_publish_request
    # 在管理员 key 还没绑定过的全新部署上会把"第一个打进来的请求"的 key 直接
    # 收编成管理员 key(参见 app.py _bind_publish_api_key) —— 如果先调它，
    # 随便一个陌生 token 删别人的插件也会先把自己'扶正'成 admin 再放行，
    # 实测过一次踩中这个坑(见 workshop delete owner 测试)。ownership 检查零
    # 副作用，永远排第一。
    token = (request.headers.get("X-API-Key") or "").strip()
    if not token:
        raise HTTPException(401, "missing workshop token (X-API-Key header)")

    safe = _safe_id(plugin_id)
    meta = _load_json(os.path.join(_ws_dir(), safe, "meta.json"))
    if not isinstance(meta, dict):
        raise HTTPException(404, f"plugin {safe} not found")
    owner = meta.get("uploader_token", "")
    if owner and hmac.compare_digest(str(owner), str(token)):
        return

    if _AUTH_FN:
        _AUTH_FN(request)
        return

    raise HTTPException(403, "only the original uploader or admin can delete this plugin")


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
    access_level: str = "free",
    minimum_app_version: str = "",
    requires: str = "[]",
    permissions: str = "[]",
    open_source: str = "true",
):
    token = _get_workshop_token(request)
    _check_plugin_owner(plugin_id, token)
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
            "access_level": access_level if access_level in ("free", "paid") else "free",
            "minimum_app_version": minimum_app_version,
            "requires": _parse_json_list(requires),
            "permissions": _parse_json_list(permissions),
            "uploader_token": token,
            "open_source": str(open_source).strip().lower() not in ("false", "0", "no"),
        },
    }

    return JSONResponse({
        "upload_id": uid,
        "chunk_size": chunk_size,
        "total_chunks": total_chunks,
    })


@router.post("/publish/chunk")
async def publish_chunk(request: Request, upload_id: str, index: int):
    _get_workshop_token(request)

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
    _get_workshop_token(request)

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

    if not meta.get("open_source", True):
        _protect_uploaded_plugin(safe_id, safe_ver, meta, dst)
        # dst 现在是编译+加密后的产物，跟原始上传字节完全不同 — sha256/size
        # 必须对最终真正会被下发的文件重新计算，否则客户端下载后校验必挂。
        digest, size = _sha256_and_size(dst)

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


# ── Management endpoints (auth required) ─────────────────────────


@router.delete("/plugin/{plugin_id}")
def delete_plugin(request: Request, plugin_id: str, version: str = ""):
    # 删自己上传的插件：管理员 key 能删任何插件；原始上传者(自己的 workshop
    # token 匹配发布时记录的 uploader_token) 只能删自己的，见 _authorize_delete。
    _authorize_delete(request, plugin_id)

    safe = _safe_id(plugin_id)
    pdir = os.path.join(_ws_dir(), safe)
    if not os.path.isdir(pdir):
        raise HTTPException(404, f"plugin {safe} not found")

    if version:
        safe_ver = _safe_ver(version)
        zip_path = os.path.join(pdir, f"plugin-{safe_ver}.zip")
        manifest_path = os.path.join(pdir, f"manifest-{safe_ver}.json")
        for f in (zip_path, manifest_path):
            if os.path.isfile(f):
                os.remove(f)
        _delete_private_version_data(safe, safe_ver)
        versions_path = os.path.join(pdir, "versions.json")
        versions = _load_json(versions_path)
        if isinstance(versions, list) and safe_ver in versions:
            versions.remove(safe_ver)
            _save_json(versions_path, versions)
        if versions:
            latest = versions[-1]
            latest_manifest = _load_json(os.path.join(pdir, f"manifest-{latest}.json"))
            if latest_manifest:
                _save_json(os.path.join(pdir, "manifest.json"), latest_manifest)
                _upsert_catalog(safe, latest_manifest)
            return JSONResponse({"ok": True, "deleted_version": safe_ver, "remaining": versions})

    shutil.rmtree(pdir, ignore_errors=True)
    if os.path.isdir(_WORKSHOP_PRIVATE_DIR):
        shutil.rmtree(os.path.join(_WORKSHOP_PRIVATE_DIR, safe), ignore_errors=True)
    catalog = _load_catalog()
    catalog = [e for e in catalog if e.get("plugin_id") != safe]
    _save_catalog(catalog)
    return JSONResponse({"ok": True, "deleted": safe})


def _delete_private_version_data(plugin_id: str, version: str) -> None:
    # 删掉某个版本在私有目录下的明文源码 zip + AES 内容密钥(如果是受保护构建)。
    private_dir = os.path.join(_WORKSHOP_PRIVATE_DIR, plugin_id)
    for name in (f"plugin-{version}.source.zip", f"{version}.key"):
        path = os.path.join(private_dir, name)
        if os.path.isfile(path):
            os.remove(path)


@router.get("/manage")
def manage_list(request: Request):
    if _AUTH_FN:
        _AUTH_FN(request)

    result = []
    ws = _ws_dir()
    if not os.path.isdir(ws):
        return JSONResponse({"ok": True, "plugins": []})
    for name in sorted(os.listdir(ws)):
        pdir = os.path.join(ws, name)
        if not os.path.isdir(pdir) or name.startswith("_"):
            continue
        meta = _load_json(os.path.join(pdir, "meta.json"))
        if not isinstance(meta, dict):
            continue
        versions = _load_json(os.path.join(pdir, "versions.json"))
        meta["versions"] = versions if isinstance(versions, list) else []
        result.append(meta)
    return JSONResponse({"ok": True, "plugins": result})


@router.patch("/plugin/{plugin_id}")
async def update_plugin_meta(request: Request, plugin_id: str):
    if _AUTH_FN:
        _AUTH_FN(request)

    safe = _safe_id(plugin_id)
    pdir = os.path.join(_ws_dir(), safe)
    meta_path = os.path.join(pdir, "meta.json")
    meta = _load_json(meta_path)
    if not isinstance(meta, dict):
        raise HTTPException(404, f"plugin {safe} not found")

    body = await request.json()
    allowed = ("name", "author", "description", "long_description",
               "game_ids", "tags", "access_level", "language", "minimum_app_version")
    for key in allowed:
        if key in body:
            meta[key] = body[key]

    _save_json(meta_path, meta)
    _upsert_catalog(safe, meta)
    return JSONResponse({"ok": True, "plugin": meta})


# ── Admin web panel ──────────────────────────────────────────────

_ADMIN_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><title>Workshop Admin</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#1a1c22;color:#e8e8ec;padding:24px;max-width:1100px;margin:0 auto}
h1{font-size:20px;color:#dea620;margin-bottom:4px}
.sub{font-size:12px;color:#8a8d96;margin-bottom:20px}
.key-row{display:flex;gap:8px;margin-bottom:20px;align-items:center}
.key-row input{flex:1;max-width:400px;padding:6px 12px;border-radius:6px;border:1px solid #383c46;background:#282b33;color:#e8e8ec;font-size:13px}
.key-row button{padding:6px 16px;border-radius:6px;border:none;background:#dea620;color:#1a1c22;font-weight:600;cursor:pointer;font-size:13px}
.key-row button:hover{filter:brightness(1.1)}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;padding:8px 10px;color:#8a8d96;border-bottom:1px solid #383c46;font-size:11px;text-transform:uppercase;letter-spacing:1px}
td{padding:8px 10px;border-bottom:1px solid #282b33}
tr:hover td{background:#22252c}
.badge{display:inline-block;padding:1px 6px;border-radius:8px;font-size:10px;font-weight:600}
.badge-free{background:#1a3020;color:#4ae68a}
.badge-paid{background:#3a2010;color:#ffc040}
.actions button{padding:3px 10px;border-radius:6px;border:1px solid #383c46;background:#282b33;color:#e8e8ec;cursor:pointer;font-size:11px;margin-right:4px}
.actions button:hover{border-color:#68e4ff;color:#68e4ff}
.actions button.del{color:#ff707a}
.actions button.del:hover{border-color:#ff707a;background:#3a1a1a}
.empty{color:#8a8d96;text-align:center;padding:40px}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#282b33;color:#e8e8ec;padding:8px 24px;border-radius:16px;border:1px solid #68e4ff;font-size:12px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:999}
.toast.show{opacity:1}
.edit-modal{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.6);z-index:100;align-items:center;justify-content:center}
.edit-modal.active{display:flex}
.edit-box{background:#22252c;border:1px solid #383c46;border-radius:10px;padding:20px;width:500px;max-height:80vh;overflow-y:auto}
.edit-box h2{font-size:16px;color:#dea620;margin-bottom:12px}
.edit-box label{display:block;font-size:11px;color:#8a8d96;margin-bottom:2px;margin-top:8px}
.edit-box input,.edit-box textarea,.edit-box select{width:100%;padding:6px 10px;border-radius:6px;border:1px solid #383c46;background:#282b33;color:#e8e8ec;font-size:12px;font-family:inherit}
.edit-box textarea{min-height:60px;resize:vertical}
.edit-box .btn-row{margin-top:14px;display:flex;gap:8px;justify-content:flex-end}
.edit-box .btn-row button{padding:6px 18px;border-radius:8px;border:none;cursor:pointer;font-size:12px;font-weight:600}
.edit-box .btn-save{background:#dea620;color:#1a1c22}
.edit-box .btn-cancel{background:#383c46;color:#e8e8ec}
</style>
</head>
<body>
<h1>&#9670; Creative Workshop Admin</h1>
<div class="sub">Manage published plugins — edit metadata, delete versions, view stats.</div>

<div class="key-row">
  <input type="password" id="api-key" placeholder="API Key" />
  <button onclick="loadPlugins()">Load</button>
</div>

<table>
<thead><tr><th>Plugin</th><th>Author</th><th>Version</th><th>Access</th><th>Downloads</th><th>Updated</th><th>Actions</th></tr></thead>
<tbody id="tbody"><tr><td colspan="7" class="empty">Enter API key and click Load</td></tr></tbody>
</table>

<div class="edit-modal" id="edit-modal">
<div class="edit-box">
  <h2>Edit Plugin</h2>
  <input type="hidden" id="ed-id"/>
  <label>Name</label><input id="ed-name"/>
  <label>Author</label><input id="ed-author"/>
  <label>Description</label><input id="ed-desc"/>
  <label>Long Description</label><textarea id="ed-long"></textarea>
  <label>Games (comma separated)</label><input id="ed-games"/>
  <label>Tags (comma separated)</label><input id="ed-tags"/>
  <label>Access Level</label>
  <select id="ed-access"><option value="free">Free</option><option value="paid">Paid (Premium)</option></select>
  <div class="btn-row">
    <button class="btn-cancel" onclick="closeEdit()">Cancel</button>
    <button class="btn-save" onclick="saveEdit()">Save</button>
  </div>
</div>
</div>

<div class="toast" id="toast"></div>

<script>
var BASE = location.origin;
function key() { return document.getElementById('api-key').value.trim(); }

function api(method, path, body) {
  var opts = { method: method, headers: { 'X-API-Key': key(), 'Accept': 'application/json' } };
  if (body) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
  return fetch(BASE + path, opts).then(function(r) { return r.json(); });
}

function toast(msg) {
  var t = document.getElementById('toast'); t.textContent = msg;
  t.className = 'toast show'; setTimeout(function() { t.className = 'toast'; }, 3000);
}

function esc(s) { var d = document.createElement('div'); d.textContent = s || ''; return d.innerHTML; }

function loadPlugins() {
  if (!key()) { toast('Please enter API key'); return; }
  api('GET', '/api/workshop/manage').then(function(r) {
    if (!r.ok) { toast('Error: ' + (r.detail || 'unauthorized')); return; }
    renderTable(r.plugins || []);
  }).catch(function(e) { toast('Error: ' + e); });
}

function renderTable(plugins) {
  var tb = document.getElementById('tbody');
  if (!plugins.length) { tb.innerHTML = '<tr><td colspan="7" class="empty">No plugins published</td></tr>'; return; }
  var html = '';
  for (var i = 0; i < plugins.length; i++) {
    var p = plugins[i];
    var acc = p.access_level === 'paid'
      ? '<span class="badge badge-paid">PAID</span>'
      : '<span class="badge badge-free">FREE</span>';
    var vers = (p.versions || []).join(', ') || p.version || '?';
    html += '<tr>' +
      '<td><strong>' + esc(p.name || p.plugin_id) + '</strong><br><span style="color:#8a8d96;font-size:11px">' + esc(p.plugin_id) + '</span></td>' +
      '<td>' + esc(p.author) + '</td>' +
      '<td>' + esc(vers) + '</td>' +
      '<td>' + acc + '</td>' +
      '<td>' + (p.download_count || 0) + '</td>' +
      '<td style="font-size:11px">' + esc((p.updated_at || '').split('T')[0]) + '</td>' +
      '<td class="actions">' +
        '<button onclick="openEdit(\'' + esc(p.plugin_id) + '\')">Edit</button>' +
        '<button class="del" onclick="deletePlugin(\'' + esc(p.plugin_id) + '\')">Delete</button>' +
      '</td></tr>';
  }
  tb.innerHTML = html;
}

function deletePlugin(id) {
  if (!confirm('Delete plugin ' + id + '? This cannot be undone.')) return;
  api('DELETE', '/api/workshop/plugin/' + encodeURIComponent(id)).then(function(r) {
    toast(r.ok ? 'Deleted' : ('Error: ' + (r.detail || '')));
    loadPlugins();
  }).catch(function(e) { toast('Error: ' + e); });
}

var _editData = {};
function openEdit(id) {
  api('GET', '/api/workshop/detail/' + encodeURIComponent(id)).then(function(r) {
    if (!r.ok) { toast('Error loading plugin'); return; }
    var p = r.plugin;
    _editData = p;
    document.getElementById('ed-id').value = p.plugin_id;
    document.getElementById('ed-name').value = p.name || '';
    document.getElementById('ed-author').value = p.author || '';
    document.getElementById('ed-desc').value = p.description || '';
    document.getElementById('ed-long').value = p.long_description || '';
    document.getElementById('ed-games').value = (p.game_ids || []).join(', ');
    document.getElementById('ed-tags').value = (p.tags || []).join(', ');
    document.getElementById('ed-access').value = p.access_level || 'free';
    document.getElementById('edit-modal').className = 'edit-modal active';
  });
}

function closeEdit() { document.getElementById('edit-modal').className = 'edit-modal'; }

function saveEdit() {
  var id = document.getElementById('ed-id').value;
  var body = {
    name: document.getElementById('ed-name').value,
    author: document.getElementById('ed-author').value,
    description: document.getElementById('ed-desc').value,
    long_description: document.getElementById('ed-long').value,
    game_ids: document.getElementById('ed-games').value.split(',').map(function(s){return s.trim();}).filter(Boolean),
    tags: document.getElementById('ed-tags').value.split(',').map(function(s){return s.trim();}).filter(Boolean),
    access_level: document.getElementById('ed-access').value,
  };
  api('PATCH', '/api/workshop/plugin/' + encodeURIComponent(id), body).then(function(r) {
    if (r.ok) { toast('Saved'); closeEdit(); loadPlugins(); }
    else toast('Error: ' + (r.detail || ''));
  }).catch(function(e) { toast('Error: ' + e); });
}
</script>
</body>
</html>"""


@router.get("/admin", response_class=HTMLResponse)
def admin_page():
    return HTMLResponse(_ADMIN_HTML)
