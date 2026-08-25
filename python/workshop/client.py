# -*- coding: utf-8 -*-
# HTTP client for the workshop API — zero external dependencies.

from __future__ import annotations

import hashlib
import json
import os
import urllib.parse
import urllib.request
from typing import Any, Callable, Optional

_UA = "SAO-Workshop/1.0"
_TIMEOUT = 30
_DL_TIMEOUT = 300


def _get_json(url: str, timeout: float = _TIMEOUT) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": _UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _post_json(url: str, api_key: str, body: bytes = b"",
               content_type: str = "application/octet-stream",
               timeout: float = _TIMEOUT,
               extra_headers: Optional[dict[str, str]] = None) -> dict:
    headers = {
        "User-Agent": _UA,
        "Accept": "application/json",
        "Content-Type": content_type,
        "X-API-Key": api_key,
    }
    if extra_headers:
        headers.update(extra_headers)
    req = urllib.request.Request(url, data=body, method="POST", headers=headers)
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
        params = {"page": str(page), "size": str(per_page), "sort": sort}
        if tag:
            params["tag"] = tag
        if game_id:
            params["game_id"] = game_id
        if search:
            params["search"] = search
        qs = urllib.parse.urlencode(params)
        data = _get_json(f"{self.base}/api/v1/workshop/plugins?{qs}")
        items = data.get("items", []) if isinstance(data, dict) else []
        return {
            "ok": True,
            "plugins": items,
            "items": items,
            "total": int(data.get("total", len(items))),
            "page": int(data.get("page", page)),
            "per_page": int(data.get("size", per_page)),
            "game_ids": data.get("game_ids", {}),
        }

    def detail(self, plugin_id: str) -> dict:
        plugin = _get_json(
            f"{self.base}/api/v1/workshop/plugins/{urllib.parse.quote(plugin_id)}"
        )
        return {"ok": True, "plugin": plugin, **plugin}

    def download(
        self,
        plugin_id: str,
        dest_dir: str,
        version: str = "",
        expected_sha256: str = "",
        progress_cb: Optional[Callable[[int, int], None]] = None,
    ) -> str:
        url = (
            f"{self.base}/api/v1/workshop/plugins/"
            f"{urllib.parse.quote(plugin_id)}/download"
        )
        if version:
            url += "?" + urllib.parse.urlencode({"version": version})

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
        plugin_id = str(metadata.get("plugin_id", ""))
        if not plugin_id:
            raise ValueError("plugin_id is required")
        tags = metadata.get("tags", [])
        tag = str(metadata.get("tag", "") or (tags[0] if tags else ""))
        minimum = str(metadata.get("minimum_app_version", ""))
        version_parts = minimum.split(".") if minimum else []
        minimum_parts = []
        for part in version_parts[:3]:
            try:
                minimum_parts.append(max(0, int(part)))
            except ValueError:
                minimum_parts.append(0)
        while len(minimum_parts) < 3:
            minimum_parts.append(0)

        params = {
            "version": metadata.get("version", "0.0.0"),
            "name": metadata.get("name", ""),
            "author": metadata.get("author", ""),
            "tag": tag,
            "game_ids": json.dumps(metadata.get("game_ids", [])),
            "signature_alg": metadata.get("signature_alg", "ed25519"),
            "min_major": minimum_parts[0],
            "min_minor": minimum_parts[1],
            "min_patch": minimum_parts[2],
        }
        token = self.api_key or self.workshop_token
        if not token:
            raise RuntimeError("workshop publish token is unavailable")
        with open(zip_path, "rb") as stream:
            body = stream.read()
        if progress_cb:
            progress_cb(file_size, file_size)
        qs = urllib.parse.urlencode(params)
        description = str(
            metadata.get("description", "") or metadata.get("long_description", "")
        )
        return _post_json(
            f"{self.base}/api/v1/workshop/plugins/"
            f"{urllib.parse.quote(plugin_id)}/publish?{qs}",
            token,
            body=body,
            timeout=_DL_TIMEOUT,
            extra_headers={"X-SaoAuto-Description": description},
        )

    def delete(self, plugin_id: str, version: str = "") -> dict:
        raise RuntimeError("workshop delete is unavailable on the v1 host")

    def fetch_content_key(self, plugin_id: str, version: str = "") -> bytes:
        # Fetch the AES-256 content key for a closed-source (protected) plugin
        # build. Requires network + a valid workshop token every call — no local
        # caching by design: a protected plugin is only ever decryptable while
        # the app is live and talking to the server.
        raise RuntimeError("protected Workshop content keys are unavailable on the v1 host")
