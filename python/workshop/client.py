# -*- coding: utf-8 -*-
# HTTP client for the workshop API — zero external dependencies.

from __future__ import annotations

import hashlib
import http.client
import hmac
import json
import os
import ssl
import urllib.parse
from urllib.error import HTTPError
from typing import Any, Callable, Optional

from tls_pinning import validate_peer_certificate

_UA = "SAO-Workshop/1.0"
_TIMEOUT = 30
_DL_TIMEOUT = 300
_WORKSHOP_ORIGIN = "https://x2.sjcmc.cn:15018"


class _PinnedHTTPSConnection(http.client.HTTPSConnection):
    def connect(self):
        super().connect()
        try:
            certificate = self.sock.getpeercert(binary_form=True) if self.sock else b""
            validate_peer_certificate(certificate)
        except Exception:
            self.close()
            raise


_TLS_CONTEXT = ssl.create_default_context()
_TLS_CONTEXT.check_hostname = False
_TLS_CONTEXT.verify_mode = ssl.CERT_NONE


def _validate_url(url: str) -> urllib.parse.SplitResult:
    parsed = urllib.parse.urlsplit(url)
    try:
        port = parsed.port
    except ValueError as exc:
        raise ValueError("Workshop requires the fixed HTTPS origin") from exc
    if (
        parsed.scheme.casefold() != "https"
        or parsed.hostname is None
        or parsed.hostname.casefold() != "x2.sjcmc.cn"
        or port != 15018
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
    ):
        raise ValueError("Workshop requires the fixed HTTPS origin")
    return parsed


def _request(method: str, url: str, headers: dict[str, str], body: bytes = b"", timeout: float = _TIMEOUT):
    parsed = _validate_url(url)
    target = parsed.path or "/"
    if parsed.query:
        target += "?" + parsed.query
    connection = _PinnedHTTPSConnection(
        parsed.hostname,
        parsed.port,
        timeout=timeout,
        context=_TLS_CONTEXT,
    )
    try:
        connection.request(method, target, body=body, headers=headers)
        response = connection.getresponse()
        if not 200 <= response.status < 300:
            response_body = response.read(4096)
            raise HTTPError(url, response.status, response.reason, response.headers, response_body)
        return connection, response
    except Exception:
        connection.close()
        raise


def _get_json(url: str, timeout: float = _TIMEOUT) -> dict:
    connection, response = _request(
        "GET",
        url,
        {"User-Agent": _UA, "Accept": "application/json"},
        timeout=timeout,
    )
    try:
        return json.loads(response.read().decode("utf-8"))
    finally:
        response.close()
        connection.close()


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
    connection, response = _request("POST", url, headers, body=body, timeout=timeout)
    try:
        return json.loads(response.read().decode("utf-8"))
    finally:
        response.close()
        connection.close()


class WorkshopClient:
    def __init__(self, base_url: str, api_key: str = "", is_paid: bool = False, workshop_token: str = ""):
        if (base_url or "").rstrip("/") != _WORKSHOP_ORIGIN:
            raise ValueError("Workshop requires the fixed HTTPS origin")
        self.base = _WORKSHOP_ORIGIN
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
        connection, response = _request("GET", url, headers, timeout=_DL_TIMEOUT)
        sha = hashlib.sha256()
        total = 0
        try:
            content_length = int(response.headers.get("Content-Length", 0) or 0)
            with open(tmp, "wb") as f:
                while True:
                    chunk = response.read(65536)
                    if not chunk:
                        break
                    f.write(chunk)
                    sha.update(chunk)
                    total += len(chunk)
                    if progress_cb and content_length:
                        progress_cb(total, content_length)
        finally:
            response.close()
            connection.close()

        digest = sha.hexdigest()
        if expected_sha256 and not hmac.compare_digest(digest, expected_sha256.lower()):
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
            "signature_alg": metadata.get("signature_alg", "none"),
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
        # build. Requires a valid workshop token every call; no local caching.
        raise RuntimeError("protected Workshop content keys are unavailable on the v1 host")