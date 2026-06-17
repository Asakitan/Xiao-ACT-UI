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
import os
import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from ai_editor.extension_host import EventEmitter, Disposable


@dataclass
class AuthSession:
    id: str = ""
    access_token: str = ""
    account_id: str = ""
    account_label: str = ""
    scopes: List[str] = field(default_factory=list)
    provider_id: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.id, "accessToken": self.access_token,
            "account": {"id": self.account_id, "label": self.account_label},
            "scopes": self.scopes,
        }


class AuthService:
    """Central authentication manager."""

    def __init__(self, storage_dir: str = "") -> None:
        self._storage = storage_dir or os.path.join(
            os.path.expanduser("~"), ".sao", "auth")
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
                    session = AuthSession(
                        id=raw.id if hasattr(raw, "id") else str(id(raw)),
                        access_token=getattr(raw, "access_token", ""),
                        account_id=getattr(raw, "account_id", ""),
                        account_label=getattr(raw, "account_label", ""),
                        scopes=list(scopes or []),
                        provider_id=provider_id,
                    )
                    self._add_session(session)
                    return session
            except Exception:
                pass
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
        with self._lock:
            sessions = self._sessions.get(provider_id, [])
            before = len(sessions)
            self._sessions[provider_id] = [
                s for s in sessions if s.id != session_id]
            removed = len(self._sessions[provider_id]) < before
        if removed:
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

    def _load(self) -> None:
        try:
            with open(self._storage_file(), "r", encoding="utf-8") as f:
                data = json.load(f)
            for pid, sessions_raw in data.items():
                for sr in sessions_raw:
                    s = AuthSession(
                        id=sr.get("id", ""),
                        access_token=sr.get("accessToken", ""),
                        account_id=sr.get("account", {}).get("id", ""),
                        account_label=sr.get("account", {}).get("label", ""),
                        scopes=sr.get("scopes", []),
                        provider_id=pid,
                    )
                    self._sessions.setdefault(pid, []).append(s)
        except Exception:
            pass

    def _save(self) -> None:
        try:
            data = {}
            for pid, sessions in self._sessions.items():
                data[pid] = [s.to_dict() for s in sessions]
            with open(self._storage_file(), "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=1)
        except Exception:
            pass


_singleton: Optional[AuthService] = None


def get_auth_service() -> AuthService:
    global _singleton
    if _singleton is None:
        _singleton = AuthService()
    return _singleton
