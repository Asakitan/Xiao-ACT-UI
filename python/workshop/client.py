# -*- coding: utf-8 -*-
"""HTTP client for the workshop API — zero external dependencies."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Optional

_UA = "SAO-Workshop/1.0"
_TIMEOUT = 30
_DL_TIMEOUT = 300
_CHUNK_SIZE = 4 * 1024 * 1024


def _get_json(url: str, timeout: float = _TIMEOUT) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": _UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _get_json_auth(url: str, api_key: str, is_paid: bool = False, timeout: float = _TIMEOUT) -> dict:
    headers = {"User-Agent": _UA, "Accept": "application/json", "X-API-Key": api_key}
    if is_paid:
        headers["X-Paid-User"] = "true"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _post_json(url: str, api_key: str, body: bytes = b"",
               content_type: str = "application/octet-stream",
               timeout: float = _TIMEOUT) -> dict:
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "User-Agent": _UA,
        "Accept": "application/json",
        "Content-Type": content_type,
        "X-API-Key": api_key,
    })
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


class WorkshopClient:
    def __init__(self, base_url: str, api_key: str = "", is_paid: bool = False, workshop_token: str = ""):
        self.base = base_url.rstrip("/")
        self.api_key = api_key
        self.is_paid = is_paid
        self.workshop_token = workshop_token

    def catalog(self, game_id: str = "", search: str = "", tag: str = "",
                page: int = 1, per_page: int = 40, sort: str = "updated_at") -> dict:
        params = {"page": str(page), "per_page": str(per_page), "sort": sort}
        if game_id:
            params["game_id"] = game_id
        if search:
            params["search"] = search
        if tag:
            params["tag"] = tag
        qs = urllib.parse.urlencode(params)
        return _get_json(f"{self.base}/api/workshop/catalog?{qs}")

    def detail(self, plugin_id: str) -> dict:
        return _get_json(f"{self.base}/api/workshop/detail/{urllib.parse.quote(plugin_id)}")

    def download(
        self,
        plugin_id: str,
        dest_dir: str,
        version: str = "",
        expected_sha256: str = "",
        progress_cb: Optional[Callable[[int, int], None]] = None,
    ) -> str:
        params = {}
        if version:
            params["version"] = version
        qs = urllib.parse.urlencode(params) if params else ""
        url = f"{self.base}/api/workshop/download/{urllib.parse.quote(plugin_id)}"
        if qs:
            url += f"?{qs}"

        os.makedirs(dest_dir, exist_ok=True)
        safe_id = "".join(c for c in plugin_id if c.isalnum() or c in "-_") or "plugin"
        dst = os.path.join(dest_dir, f"{safe_id}.zip")
        tmp = dst + ".part"

        headers = {"User-Agent": _UA}
        if self.is_paid:
            headers["X-Paid-User"] = "true"
        req = urllib.request.Request(url, headers=headers)
        sha = hashlib.sha256()
        total = 0

        with urllib.request.urlopen(req, timeout=_DL_TIMEOUT) as resp:
            content_length = int(resp.headers.get("Content-Length", 0) or 0)
            with open(tmp, "wb") as f:
                while True:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    f.write(chunk)
                    sha.update(chunk)
                    total += len(chunk)
                    if progress_cb and content_length:
                        progress_cb(total, content_length)

        digest = sha.hexdigest()
        if expected_sha256 and digest != expected_sha256:
            os.remove(tmp)
            raise ValueError(f"SHA256 mismatch: expected {expected_sha256}, got {digest}")

        os.replace(tmp, dst)
        return dst

    def publish(
        self,
        zip_path: str,
        metadata: dict[str, Any],
        progress_cb: Optional[Callable[[int, int], None]] = None,
    ) -> dict:
        file_size = os.path.getsize(zip_path)
        init_params = {
            "plugin_id": metadata.get("plugin_id", ""),
            "version": metadata.get("version", "0.0.0"),
            "file_size": str(file_size),
            "chunk_size": str(_CHUNK_SIZE),
            "name": metadata.get("name", ""),
            "author": metadata.get("author", ""),
            "description": metadata.get("description", ""),
            "long_description": metadata.get("long_description", ""),
            "game_ids": json.dumps(metadata.get("game_ids", [])),
            "tags": json.dumps(metadata.get("tags", [])),
            "language": metadata.get("language", "python"),
            "access_level": metadata.get("access_level", "free"),
            "minimum_app_version": metadata.get("minimum_app_version", ""),
            "requires": json.dumps(metadata.get("requires", [])),
            "permissions": json.dumps(metadata.get("permissions", [])),
            "open_source": "true" if metadata.get("open_source", True) else "false",
        }

        token = self.workshop_token or self.api_key
        qs = urllib.parse.urlencode(init_params)
        init_resp = _post_json(f"{self.base}/api/workshop/publish/init?{qs}", token)
        upload_id = init_resp["upload_id"]
        chunk_size = int(init_resp["chunk_size"])
        total_chunks = int(init_resp["total_chunks"])

        sent = 0
        with open(zip_path, "rb") as f:
            for idx in range(total_chunks):
                data = f.read(chunk_size)
                chunk_qs = urllib.parse.urlencode({"upload_id": upload_id, "index": str(idx)})
                _post_json(
                    f"{self.base}/api/workshop/publish/chunk?{chunk_qs}",
                    token,
                    body=data,
                    timeout=_DL_TIMEOUT,
                )
                sent += len(data)
                if progress_cb:
                    progress_cb(sent, file_size)

        complete_qs = urllib.parse.urlencode({"upload_id": upload_id})
        return _post_json(f"{self.base}/api/workshop/publish/complete?{complete_qs}", token)

    def fetch_content_key(self, plugin_id: str, version: str = "") -> bytes:
        """Fetch the AES-256 content key for a closed-source (protected) plugin
        build. Requires network + a valid workshop token every call — no local
        caching by design: a protected plugin is only ever decryptable while
        the app is live and talking to the server.
        """
        import base64
        params = {}
        if version:
            params["version"] = version
        qs = urllib.parse.urlencode(params) if params else ""
        url = f"{self.base}/api/workshop/key/{urllib.parse.quote(plugin_id)}"
        if qs:
            url += f"?{qs}"
        token = self.workshop_token or self.api_key
        resp = _get_json_auth(url, token, is_paid=self.is_paid)
        if not resp.get("ok"):
            raise RuntimeError(resp.get("detail") or "key fetch failed")
        key = base64.b64decode(resp["key_b64"])
        if len(key) != 32:
            raise ValueError(f"unexpected content key length: {len(key)}")
        return key
