"""User-scoped protected secret storage for the AI Editor.

The editor is a Windows desktop application.  Secrets are protected with the
current user's Windows DPAPI key and only opaque ciphertext is written to disk.
Callers must fail closed when the store is unavailable; this module deliberately
does not provide a plaintext fallback.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import tempfile
import threading
from pathlib import Path
from typing import Any, Dict, Iterable, Optional


_SECRET_KEY_RE = re.compile(r"^[A-Za-z0-9._/@:-]{1,512}$")
_DPAPI_FLAGS = 0x01  # CRYPTPROTECT_UI_FORBIDDEN
_DPAPI_ENTROPY = b"sao-ai-editor-secret-store-v1"
SECRET_PRESENT = "__SAO_SECRET_PRESENT__"


def secret_ref_part(value: Any) -> str:
    """Return a stable, collision-resistant vault-key component.

    Identifiers are hashed instead of lossy character replacement so values
    such as ``a/b`` and ``a?b`` cannot alias the same protected entry.
    """
    raw = str("" if value is None else value).encode("utf-8")
    return f"v2-{hashlib.sha256(raw).hexdigest()}"


def legacy_secret_ref_part(value: Any) -> str:
    """Reproduce the pre-v2 lossy key component for one-time migration."""
    normalized = re.sub(r"[^A-Za-z0-9._-]+", "_", str(value or "").strip())
    return normalized[:120] or "default"


def legacy_ref_is_unambiguous(value: Any, candidates: Iterable[Any]) -> bool:
    """Return whether a legacy key component identifies exactly one raw ID."""
    owner = str(value or "")
    legacy_part = legacy_secret_ref_part(owner)
    aliases = {
        str(candidate or "")
        for candidate in candidates
        if legacy_secret_ref_part(candidate) == legacy_part
    }
    return aliases == {owner}


def _legacy_migration_block_key(legacy_key: str) -> str:
    return (
        "migration-block/"
        + hashlib.sha256(str(legacy_key).encode("utf-8")).hexdigest()
    )


def block_legacy_migration(store: Any, legacy_key: str) -> None:
    """Permanently quarantine a legacy reference with ambiguous ownership."""
    block_key = _legacy_migration_block_key(legacy_key)
    if not store.has(block_key):
        store.set(block_key, "1")


def get_with_legacy_migration(
    store: Any,
    key: str,
    legacy_key: str,
    *,
    allow_legacy: bool,
    default: str = "",
) -> str:
    """Read a v2 entry, or safely move an unambiguous legacy entry to v2."""
    block_key = _legacy_migration_block_key(legacy_key)
    if not allow_legacy:
        block_legacy_migration(store, legacy_key)
        return store.get(key, default) if store.has(key) else default
    if store.has(key):
        return store.get(key, default)
    if store.has(block_key) or not store.has(legacy_key):
        return default
    value = store.get(legacy_key, default)
    store.set(key, value)
    store.delete(legacy_key)
    return value


def get_json_with_legacy_migration(
    store: Any,
    key: str,
    legacy_key: str,
    *,
    allow_legacy: bool,
    default: Any = None,
) -> Any:
    """JSON variant of :func:`get_with_legacy_migration`."""
    value = get_with_legacy_migration(
        store,
        key,
        legacy_key,
        allow_legacy=allow_legacy,
        default="",
    )
    if not value:
        return default
    try:
        return json.loads(value)
    except Exception as exc:
        raise SecretStoreError("Protected JSON secret is invalid") from exc


class SecretStoreError(RuntimeError):
    """Raised when protected storage is unavailable or cannot be updated."""


class ProtectedSecretStore:
    """Small atomic JSON vault whose values are encrypted with Windows DPAPI."""

    def __init__(self, path: str = "") -> None:
        default_path = os.path.join(
            os.path.expanduser("~"), ".sao", "secrets", "ai_editor.vault.json")
        self.path = os.path.abspath(path or default_path)
        self._lock = threading.RLock()

    @staticmethod
    def _validate_key(key: str) -> str:
        normalized = str(key or "").strip()
        if not _SECRET_KEY_RE.fullmatch(normalized):
            raise SecretStoreError("Invalid secret key")
        if any(part in {"", ".", ".."} for part in normalized.split("/")):
            raise SecretStoreError("Invalid secret key")
        return normalized

    @staticmethod
    def _win32crypt():
        if os.name != "nt":
            raise SecretStoreError(
                "Protected secret persistence requires Windows DPAPI")
        try:
            import win32crypt
        except Exception as exc:  # pragma: no cover - packaging/runtime guard
            raise SecretStoreError("Windows DPAPI support is unavailable") from exc
        return win32crypt

    @classmethod
    def _protect(cls, value: str) -> str:
        try:
            raw = cls._win32crypt().CryptProtectData(
                str(value).encode("utf-8"),
                "SAO AI Editor",
                _DPAPI_ENTROPY,
                None,
                None,
                _DPAPI_FLAGS,
            )
        except SecretStoreError:
            raise
        except Exception as exc:
            raise SecretStoreError("Failed to protect secret with Windows DPAPI") from exc
        return base64.b64encode(raw).decode("ascii")

    @classmethod
    def _unprotect(cls, value: str) -> str:
        try:
            encrypted = base64.b64decode(str(value), validate=True)
            _description, raw = cls._win32crypt().CryptUnprotectData(
                encrypted,
                _DPAPI_ENTROPY,
                None,
                None,
                _DPAPI_FLAGS,
            )
            return raw.decode("utf-8")
        except SecretStoreError:
            raise
        except Exception as exc:
            raise SecretStoreError("Failed to unprotect secret with Windows DPAPI") from exc

    def _read_unlocked(self) -> Dict[str, Any]:
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                raw = json.load(handle)
        except FileNotFoundError:
            return {"version": 1, "entries": {}}
        except Exception as exc:
            raise SecretStoreError("Protected secret vault is unreadable") from exc
        if not isinstance(raw, dict) or raw.get("version") != 1:
            raise SecretStoreError("Unsupported protected secret vault format")
        entries = raw.get("entries")
        if not isinstance(entries, dict):
            raise SecretStoreError("Protected secret vault entries are invalid")
        return {"version": 1, "entries": dict(entries)}

    def _write_unlocked(self, payload: Dict[str, Any]) -> None:
        target = Path(self.path)
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp_name = tempfile.mkstemp(
            prefix=f".{target.name}.", suffix=".tmp", dir=str(target.parent))
        try:
            try:
                os.chmod(tmp_name, 0o600)
            except OSError:
                pass
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
                json.dump(payload, handle, ensure_ascii=False, indent=2, sort_keys=True)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp_name, self.path)
        except Exception as exc:
            try:
                os.unlink(tmp_name)
            except OSError:
                pass
            raise SecretStoreError("Failed to atomically update secret vault") from exc

    def set(self, key: str, value: str) -> None:
        normalized = self._validate_key(key)
        if value is None:
            raise SecretStoreError("Secret value cannot be null")
        protected = self._protect(str(value))
        with self._lock:
            payload = self._read_unlocked()
            payload["entries"][normalized] = protected
            self._write_unlocked(payload)

    def set_json(self, key: str, value: Any) -> None:
        self.set(key, json.dumps(value, ensure_ascii=False, separators=(",", ":")))

    def get(self, key: str, default: str = "") -> str:
        normalized = self._validate_key(key)
        with self._lock:
            payload = self._read_unlocked()
            protected = payload["entries"].get(normalized)
        if protected is None:
            return default
        if not isinstance(protected, str):
            raise SecretStoreError("Protected secret entry is invalid")
        return self._unprotect(protected)

    def get_json(self, key: str, default: Any = None) -> Any:
        value = self.get(key, "")
        if not value:
            return default
        try:
            return json.loads(value)
        except Exception as exc:
            raise SecretStoreError("Protected JSON secret is invalid") from exc

    def has(self, key: str) -> bool:
        normalized = self._validate_key(key)
        with self._lock:
            return normalized in self._read_unlocked()["entries"]

    def delete(self, key: str) -> bool:
        normalized = self._validate_key(key)
        with self._lock:
            payload = self._read_unlocked()
            if normalized not in payload["entries"]:
                return False
            payload["entries"].pop(normalized, None)
            self._write_unlocked(payload)
        return True


class InMemorySecretStore:
    """Non-persistent store for ephemeral/in-memory settings adapters."""

    def __init__(self) -> None:
        self._entries: Dict[str, str] = {}
        self._lock = threading.RLock()

    def set(self, key: str, value: str) -> None:
        normalized = ProtectedSecretStore._validate_key(key)
        with self._lock:
            self._entries[normalized] = str(value)

    def get(self, key: str, default: str = "") -> str:
        normalized = ProtectedSecretStore._validate_key(key)
        with self._lock:
            return self._entries.get(normalized, default)

    def set_json(self, key: str, value: Any) -> None:
        self.set(key, json.dumps(value, ensure_ascii=False, separators=(",", ":")))

    def get_json(self, key: str, default: Any = None) -> Any:
        value = self.get(key, "")
        return json.loads(value) if value else default

    def has(self, key: str) -> bool:
        normalized = ProtectedSecretStore._validate_key(key)
        with self._lock:
            return normalized in self._entries

    def delete(self, key: str) -> bool:
        normalized = ProtectedSecretStore._validate_key(key)
        with self._lock:
            return self._entries.pop(normalized, None) is not None


_singleton: Optional[ProtectedSecretStore] = None
_singleton_lock = threading.Lock()


def get_secret_store() -> ProtectedSecretStore:
    global _singleton
    with _singleton_lock:
        if _singleton is None:
            _singleton = ProtectedSecretStore()
        return _singleton
