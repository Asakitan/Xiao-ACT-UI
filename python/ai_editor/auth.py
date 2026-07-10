"""Authentication service for the SAO AI Editor.

Provides session management for extension authentication:
  - GitHub OAuth device flow (for Copilot / CC)
  - API key-based auth (for OpenAI / Anthropic / DeepSeek)
  - Keychain-like secure storage (file-based, per-provider)

Extensions call ``vscode.authentication.getSession(providerId, scopes)``
which routes through this service.
"""

from __future__ import annotations

import json
import logging
import os
import tempfile
import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from ai_editor.extension_host import EventEmitter, Disposable
from ai_editor.secret_store import (
    block_legacy_migration,
    get_with_legacy_migration,
    secret_ref_part,
)


logger = logging.getLogger(__name__)


@dataclass
class AuthSession:
    id: str = ""
    access_token: str = ""
    account_id: str = ""
    account_label: str = ""
    scopes: List[str] = field(default_factory=list)
    provider_id: str = ""

    def to_dict(self, include_token: bool = True) -> Dict[str, Any]:
        result = {
            "id": self.id,
            "account": {"id": self.account_id, "label": self.account_label},
            "scopes": self.scopes,
        }
        if include_token:
            result["accessToken"] = self.access_token
        return result

    def to_public_dict(self) -> Dict[str, Any]:
        """Return metadata safe for the broad pywebview settings bridge."""
        result = self.to_dict(include_token=False)
        result["hasToken"] = bool(self.access_token)
        return result


class AuthService:
    """Central authentication manager."""

    def __init__(self, storage_dir: str = "", secret_store: Any = None) -> None:
        self._storage = storage_dir or os.path.join(
            os.path.expanduser("~"), ".sao", "auth")
        if secret_store is None:
            from ai_editor.secret_store import ProtectedSecretStore
            secret_store = ProtectedSecretStore(
                os.path.join(self._storage, "sessions.vault.json"))
        self._secret_store = secret_store
        self._providers: Dict[str, Any] = {}
        self._sessions: Dict[str, List[AuthSession]] = {}
        self._change_emitter = EventEmitter()
        self._lock = threading.Lock()
        self._load()

    @property
    def on_did_change_sessions(self):
        return self._change_emitter.event

    def register_provider(self, provider_id: str, label: str,
                          provider: Any = None) -> Disposable:
        self._providers[provider_id] = {
            "label": label, "provider": provider}
        return Disposable(lambda: self._providers.pop(provider_id, None))

    def get_session(self, provider_id: str,
                    scopes: Optional[List[str]] = None,
                    options: Optional[Dict] = None) -> Optional[AuthSession]:
        with self._lock:
            sessions = self._sessions.get(provider_id, [])
        if scopes:
            for s in sessions:
                if set(scopes).issubset(set(s.scopes)):
                    return s
        elif sessions:
            return sessions[0]

        if options and options.get("createIfNone"):
            return self.create_session(provider_id, scopes or [])
        return None

    def create_session(self, provider_id: str,
                       scopes: List[str] = None) -> Optional[AuthSession]:
        prov = self._providers.get(provider_id, {}).get("provider")
        if prov and hasattr(prov, "create_session"):
            try:
                raw = prov.create_session(scopes or [], {})
                if raw:
                    session = self._normalize_session(
                        provider_id, raw, scopes or [])
                    self._add_session(session)
                    return session
            except Exception as exc:
                logger.warning("Failed to create auth session for %s: %s",
                               provider_id, exc)
        return None

    def create_session_from_token(self, provider_id: str, token: str,
                                   label: str = "",
                                   scopes: List[str] = None) -> AuthSession:
        import uuid
        session = AuthSession(
            id=uuid.uuid4().hex[:12],
            access_token=token,
            account_id=provider_id,
            account_label=label or provider_id,
            scopes=scopes or [],
            provider_id=provider_id,
        )
        self._add_session(session)
        return session

    def remove_session(self, provider_id: str, session_id: str) -> bool:
        provider_key = str(provider_id or "")
        session_key = str(session_id or "")
        with self._lock:
            legacy_ref = self._legacy_session_secret_ref(
                provider_key, session_key)
            legacy_owners = {
                (str(pid), str(session.id or ""))
                for pid, provider_sessions in self._sessions.items()
                for session in provider_sessions
                if self._legacy_session_secret_ref(
                    str(pid), str(session.id or "")) == legacy_ref
            }
            legacy_is_unique = legacy_owners == {(provider_key, session_key)}
            sessions = self._sessions.get(provider_id, [])
            before = len(sessions)
            self._sessions[provider_id] = [
                s for s in sessions if s.id != session_id]
            removed = len(self._sessions[provider_id]) < before
        if removed:
            try:
                self._secret_store.delete(
                    self._session_secret_ref(provider_id, session_id))
            except Exception as exc:
                logger.warning("Failed to remove protected auth token: %s", exc)
            try:
                if legacy_is_unique:
                    self._secret_store.delete(legacy_ref)
                else:
                    block_legacy_migration(self._secret_store, legacy_ref)
            except Exception as exc:
                logger.warning(
                    "Failed to retire legacy protected auth token: %s", exc)
            self._save()
            self._change_emitter.fire({"provider": provider_id,
                                        "removed": [session_id]})
        return removed

    def list_sessions(self, provider_id: str = "") -> List[AuthSession]:
        if provider_id:
            return list(self._sessions.get(provider_id, []))
        all_sessions = []
        for sessions in self._sessions.values():
            all_sessions.extend(sessions)
        return all_sessions

    def _add_session(self, session: AuthSession) -> None:
        with self._lock:
            self._sessions.setdefault(session.provider_id, []).append(session)
        self._save()
        self._change_emitter.fire({
            "provider": session.provider_id, "added": [session.id]})

    def _storage_file(self) -> str:
        os.makedirs(self._storage, exist_ok=True)
        return os.path.join(self._storage, "sessions.json")

    @staticmethod
    def _session_secret_ref(provider_id: str, session_id: str) -> str:
        return (
            f"auth-session/{secret_ref_part(provider_id)}"
            f"/{secret_ref_part(session_id)}"
        )

    @staticmethod
    def _legacy_session_secret_ref(provider_id: str, session_id: str) -> str:
        """Reproduce the pre-v2 lossy auth-session vault reference."""
        safe_provider = str(provider_id or "unknown").replace("/", "_")
        safe_session = str(session_id or "unknown").replace("/", "_")
        return f"auth-session/{safe_provider}/{safe_session}"

    @staticmethod
    def _normalize_session(provider_id: str, raw: Any,
                           scopes: List[str]) -> AuthSession:
        if isinstance(raw, AuthSession):
            raw.provider_id = raw.provider_id or provider_id
            raw.scopes = list(raw.scopes or scopes)
            return raw
        if isinstance(raw, dict):
            account = raw.get("account", {})
            return AuthSession(
                id=str(raw.get("id") or raw.get("session_id") or ""),
                access_token=str(raw.get("accessToken") or raw.get("access_token") or raw.get("token") or ""),
                account_id=str(raw.get("account_id") or account.get("id") or provider_id),
                account_label=str(raw.get("account_label") or account.get("label") or raw.get("label") or provider_id),
                scopes=list(raw.get("scopes") or scopes),
                provider_id=str(raw.get("provider_id") or provider_id),
            )
        return AuthSession(
            id=getattr(raw, "id", "") or str(id(raw)),
            access_token=(getattr(raw, "access_token", "")
                          or getattr(raw, "accessToken", "")
                          or getattr(raw, "token", "")),
            account_id=(getattr(raw, "account_id", "")
                        or getattr(raw, "accountId", "")
                        or provider_id),
            account_label=(getattr(raw, "account_label", "")
                           or getattr(raw, "accountLabel", "")
                           or getattr(raw, "label", "")
                           or provider_id),
            scopes=list(getattr(raw, "scopes", None) or scopes),
            provider_id=getattr(raw, "provider_id", "") or provider_id,
        )

    def _load(self) -> None:
        path = self._storage_file()
        migrated = False
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if not isinstance(data, dict):
                raise ValueError("Auth session metadata root must be an object")

            # Decide legacy ownership before reading any token.  The old
            # slash-to-underscore reference was lossy, so a colliding legacy
            # entry must never be assigned to either candidate session.
            legacy_owners: Dict[str, set[tuple[str, str]]] = {}
            for raw_pid, sessions_raw in data.items():
                if not isinstance(sessions_raw, list):
                    continue
                pid = str(raw_pid)
                for sr in sessions_raw:
                    if not isinstance(sr, dict):
                        continue
                    session_id = str(sr.get("id") or "")
                    legacy_ref = self._legacy_session_secret_ref(pid, session_id)
                    legacy_owners.setdefault(legacy_ref, set()).add(
                        (pid, session_id))

            for pid, sessions_raw in data.items():
                if not isinstance(sessions_raw, list):
                    continue
                pid = str(pid)
                for sr in sessions_raw:
                    if not isinstance(sr, dict):
                        continue
                    session_id = str(sr.get("id") or "")
                    secret_ref = self._session_secret_ref(pid, session_id)
                    legacy_ref = self._legacy_session_secret_ref(pid, session_id)
                    stored_ref = str(sr.get("secretRef") or legacy_ref)
                    legacy_is_unique = legacy_owners.get(legacy_ref, set()) == {
                        (pid, session_id)
                    }
                    access_token = ""
                    if sr.get("accessToken"):
                        # One-time migration: only remove plaintext after the
                        # protected write succeeds.
                        access_token = str(sr.get("accessToken") or "")
                        self._secret_store.set(secret_ref, access_token)
                        migrated = True
                    elif stored_ref == secret_ref:
                        access_token = self._secret_store.get(secret_ref, "")
                    elif stored_ref == legacy_ref:
                        access_token = get_with_legacy_migration(
                            self._secret_store,
                            secret_ref,
                            legacy_ref,
                            allow_legacy=legacy_is_unique,
                        )
                        if self._secret_store.has(secret_ref):
                            migrated = True
                    else:
                        logger.warning(
                            "Ignoring unexpected auth token reference for %s/%s",
                            pid, session_id)
                        access_token = self._secret_store.get(secret_ref, "")
                    s = AuthSession(
                        id=session_id,
                        access_token=access_token,
                        account_id=sr.get("account", {}).get("id", ""),
                        account_label=sr.get("account", {}).get("label", ""),
                        scopes=sr.get("scopes", []),
                        provider_id=pid,
                    )
                    self._sessions.setdefault(pid, []).append(s)
            if migrated:
                self._save()
        except FileNotFoundError:
            return
        except Exception as exc:
            logger.warning("Failed to load auth sessions from %s: %s",
                           path, exc)

    def _save(self) -> bool:
        path = self._storage_file()
        try:
            data = {}
            for pid, sessions in self._sessions.items():
                entries = []
                for session in sessions:
                    secret_ref = self._session_secret_ref(pid, session.id)
                    self._secret_store.set(secret_ref, session.access_token)
                    metadata = session.to_dict(include_token=False)
                    metadata["secretRef"] = secret_ref
                    entries.append(metadata)
                data[pid] = entries
            os.makedirs(os.path.dirname(path), exist_ok=True)
            fd, tmp_path = tempfile.mkstemp(
                prefix=".sessions.", suffix=".tmp", dir=os.path.dirname(path))
            try:
                with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
                    json.dump(data, f, ensure_ascii=False, indent=1)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(tmp_path, path)
            except Exception:
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass
                raise
            return True
        except Exception as exc:
            logger.warning("Failed to save auth sessions to %s: %s",
                           path, exc)
            return False


_singleton: Optional[AuthService] = None


def get_auth_service() -> AuthService:
    global _singleton
    if _singleton is None:
        _singleton = AuthService()
    return _singleton
