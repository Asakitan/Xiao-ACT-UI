"""Right-sidebar chat provider registry.

Providers are tabs in the right sidebar (CHAT / CLAUDE CODE / CODEX / plugin).
Each provider wraps a conversation with its own model config and system prompt.

Built-in providers auto-detect availability from settings (API keys).
Plugins call ``register_chat_provider()`` to add custom tabs.
"""

from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Dict, List, Optional


@dataclass
class ChatProviderDef:
    id: str
    name: str
    icon: str = "\U0001f4ac"
    provider_type: str = "openai"
    api_key: str = ""
    base_url: str = ""
    model: str = ""
    system_prompt: str = ""
    auto_agent: bool = False
    builtin: bool = False

    def to_dict(self) -> Dict[str, Any]:
        d = asdict(self)
        d.pop("builtin", None)
        d.pop("api_key", None)
        return d

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> ChatProviderDef:
        known = {f.name for f in cls.__dataclass_fields__.values()}
        return cls(**{k: v for k, v in d.items() if k in known})


_CC_SYSTEM = (
    "You are Claude Code, an AI coding agent. You have full access to editor "
    "tools: read/edit files, search, run terminal commands, and query the "
    "engine runtime. Work autonomously on tasks — plan, execute, verify. "
    "Use tools in parallel when independent. Ask only when genuinely blocked."
)

_CODEX_SYSTEM = (
    "You are Codex, an AI coding agent. You have full access to editor tools: "
    "read/edit files, search, run terminal commands, and query the engine "
    "runtime. Help the user with coding tasks efficiently and verify changes."
)


BUILTIN_PROVIDERS: List[ChatProviderDef] = [
    ChatProviderDef(
        id="chat", name="Chat", icon="\U0001f4ac",
        builtin=True, auto_agent=False,
    ),
    ChatProviderDef(
        id="claude-code", name="Claude Code", icon="✦",
        provider_type="anthropic", model="claude-sonnet-4-20250514",
        system_prompt=_CC_SYSTEM, auto_agent=True, builtin=True,
    ),
    ChatProviderDef(
        id="codex", name="Codex", icon="\U0001f52e",
        provider_type="openai", model="codex-mini-latest",
        system_prompt=_CODEX_SYSTEM, auto_agent=True, builtin=True,
    ),
]


class ChatProviderRegistry:

    def __init__(self) -> None:
        self._providers: Dict[str, ChatProviderDef] = {}
        for p in BUILTIN_PROVIDERS:
            self._providers[p.id] = p

    def register(self, provider: ChatProviderDef) -> None:
        self._providers[provider.id] = provider

    def unregister(self, provider_id: str) -> bool:
        p = self._providers.get(provider_id)
        if p and p.builtin:
            return False
        return self._providers.pop(provider_id, None) is not None

    def get(self, provider_id: str) -> Optional[ChatProviderDef]:
        return self._providers.get(provider_id)

    def list_all(self) -> List[ChatProviderDef]:
        return list(self._providers.values())

    def list_available(self,
                       settings_getter: Optional[Callable] = None,
                       ) -> List[Dict[str, Any]]:
        """Return providers with availability flag based on configured keys."""
        result = []
        for p in self._providers.values():
            avail = self._check_available(p, settings_getter)
            d = p.to_dict()
            d["available"] = avail
            d["builtin"] = p.builtin
            result.append(d)
        return result

    @staticmethod
    def _check_available(p: ChatProviderDef,
                         settings_getter: Optional[Callable]) -> bool:
        if p.id == "chat":
            return True
        if p.id == "claude-code":
            key = _get_provider_key("anthropic", settings_getter)
            if key:
                return True
            return shutil.which("claude") is not None
        if p.id == "codex":
            return bool(_get_provider_key("openai", settings_getter))
        if p.api_key:
            return True
        key = _get_provider_key(p.provider_type, settings_getter)
        return bool(key)


def _get_provider_key(provider_type: str,
                      settings_getter: Optional[Callable]) -> str:
    """Try to get an API key for a provider type from settings."""
    if not settings_getter:
        return ""
    ai = settings_getter("ai_editor", {}) or {}
    if not isinstance(ai, dict):
        return ""
    configured_provider = ai.get("provider", "")
    configured_key = ai.get("api_key", "")
    if configured_provider == provider_type and configured_key:
        return configured_key
    keys = ai.get("provider_keys", {})
    if isinstance(keys, dict):
        return keys.get(provider_type, "")
    return ""


_singleton: Optional[ChatProviderRegistry] = None


def get_provider_registry() -> ChatProviderRegistry:
    global _singleton
    if _singleton is None:
        _singleton = ChatProviderRegistry()
    return _singleton
