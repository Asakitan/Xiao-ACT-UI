"""Right-sidebar chat provider registry.

Providers are tabs in the right sidebar (Chat / Copilot / Claude Code / Codex / plugin).
Each provider wraps a conversation with its own model config and system prompt.

Built-in providers auto-detect availability from supported settings.
Plugins call ``register_chat_provider()`` to add custom tabs.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Dict, List, Optional


BUILTIN_PROVIDER_ORDER = ("chat", "copilot", "claude-code", "codex")
COPILOT_UNAVAILABLE_REASON = (
    "Copilot is registered as an explicit backend capability surface, but "
    "direct GitHub Copilot chat transport is not available in this runtime yet."
)


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

_COPILOT_SYSTEM = (
    "You are GitHub Copilot Chat inside the SAO AI Editor. Use editor context "
    "and available tools to help with coding tasks."
)


BUILTIN_PROVIDERS: List[ChatProviderDef] = [
    ChatProviderDef(
        id="chat", name="Chat", icon="\U0001f4ac",
        builtin=True, auto_agent=False,
    ),
    ChatProviderDef(
        id="copilot", name="Copilot", icon="◉",
        provider_type="github-copilot", model="GitHub Copilot Chat",
        system_prompt=_COPILOT_SYSTEM, auto_agent=False, builtin=True,
    ),
    ChatProviderDef(
        id="claude-code", name="Claude Code", icon="✦",
        provider_type="anthropic", model="",
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
        existing = self._providers.get(provider.id)
        if existing and existing.builtin and not provider.builtin:
            raise ValueError(f"Cannot override built-in provider: {provider.id}")
        self._providers[provider.id] = provider

    def unregister(self, provider_id: str) -> bool:
        p = self._providers.get(provider_id)
        if p and p.builtin:
            return False
        return self._providers.pop(provider_id, None) is not None

    def get(self, provider_id: str) -> Optional[ChatProviderDef]:
        return self._providers.get(provider_id)

    def list_all(self) -> List[ChatProviderDef]:
        return sorted(self._providers.values(), key=_provider_sort_key)

    def list_available(self,
                       settings_getter: Optional[Callable] = None,
                       ) -> List[Dict[str, Any]]:
        """Return providers with availability flag based on supported configuration."""
        result = []
        for p in self.list_all():
            status = self._provider_status(p, settings_getter)
            d = p.to_dict()
            d.update(status)
            d["builtin"] = p.builtin
            result.append(d)
        return result

    @staticmethod
    def _check_available(p: ChatProviderDef,
                         settings_getter: Optional[Callable]) -> bool:
        return bool(ChatProviderRegistry._provider_status(
            p, settings_getter).get("available"))

    @staticmethod
    def _provider_status(p: ChatProviderDef,
                         settings_getter: Optional[Callable]) -> Dict[str, Any]:
        status: Dict[str, Any] = {
            "available": True,
            "status": "available",
            "unavailable_reason": "",
            "api_key_available": True,
        }
        if p.id == "chat":
            return status
        if p.id == "copilot":
            status["api_key_available"] = _has_copilot_auth(settings_getter)
            status["transport"] = "copilot-chat"
            status["model"] = p.model
            status["direct_transport_available"] = False
            status["capability"] = "github-copilot-chat"
            return _unavailable(
                status,
                COPILOT_UNAVAILABLE_REASON,
            )
        if p.id == "claude-code":
            section = _get_provider_section("claude_code", settings_getter)
            model = _get_provider_option("claude_code", "model", settings_getter)
            status["model"] = model or p.model
            status.update(_cli_status("claude_code", "claude", settings_getter))
            status["api_key_available"] = bool(_get_provider_key("anthropic", settings_getter))
            prefer_cli = _get_provider_bool("claude_code", "prefer_cli", settings_getter)
            if prefer_cli:
                return _unavailable(
                    status,
                    "Claude Code CLI preference is configured, but this backend "
                    "does not execute the Claude CLI transport yet. Turn off "
                    "Prefer CLI to use the Anthropic API/proxy path.",
                )
            if not status["api_key_available"]:
                return _unavailable(
                    status,
                    "Anthropic API key is not configured for Claude Code.",
                )
            status["prefer_cli"] = bool(section.get("prefer_cli"))
            return status
        if p.id == "codex":
            transport = _normalize_transport(
                _get_provider_option("codex", "transport", settings_getter),
                "chat_completions",
                {"chat_completions", "responses", "cli"},
            )
            model = _get_provider_option("codex", "model", settings_getter)
            status["transport"] = transport
            status["model"] = model or p.model
            status.update(_cli_status("codex", "codex", settings_getter))
            status["api_key_available"] = bool(_get_provider_key("openai", settings_getter))
            if transport == "cli":
                return _unavailable(
                    status,
                    "Codex CLI transport is configured, but this backend does "
                    "not execute the Codex CLI transport yet. Select Chat "
                    "Completions or Responses to use the Codex tab.",
                )
            if not status["api_key_available"]:
                return _unavailable(status, "OpenAI API key is not configured for Codex.")
            return status
        if p.api_key:
            return status
        key = _get_provider_key(p.provider_type, settings_getter)
        status["api_key_available"] = bool(key)
        if not key:
            return _unavailable(
                status,
                f"API key is not configured for provider type {p.provider_type}.",
            )
        return status


def _unavailable(status: Dict[str, Any], reason: str) -> Dict[str, Any]:
    updated = dict(status)
    updated["available"] = False
    updated["status"] = "unavailable"
    updated["unavailable_reason"] = reason
    return updated


def _provider_sort_key(provider: ChatProviderDef) -> tuple[int, int, str]:
    try:
        builtin_index = BUILTIN_PROVIDER_ORDER.index(provider.id)
    except ValueError:
        builtin_index = len(BUILTIN_PROVIDER_ORDER)
    return (0 if provider.builtin else 1, builtin_index, provider.name.casefold())


def _normalize_transport(value: Any, default: str,
                         allowed: set[str]) -> str:
    raw = str(value or default).strip().lower().replace("-", "_")
    if raw in {"openai_responses", "response"}:
        raw = "responses"
    return raw if raw in allowed else default


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
        key = keys.get(provider_type, "")
        if key:
            return key
    legacy_keys = ai.get("_provider_keys", {})
    if isinstance(legacy_keys, dict):
        return legacy_keys.get(provider_type, "")
    return ""


def _has_copilot_auth(settings_getter: Optional[Callable]) -> bool:
    ai = _get_ai_editor_settings(settings_getter)
    auth = ai.get("auth", {})
    if isinstance(auth, dict):
        for key in ("github", "github-copilot", "copilot"):
            value = auth.get(key)
            if isinstance(value, str) and value.strip():
                return True
            if isinstance(value, dict) and any(
                    str(value.get(field, "")).strip()
                    for field in ("access_token", "accessToken", "token")):
                return True
    return bool(_get_provider_key("github-copilot", settings_getter)
                or _get_provider_key("copilot", settings_getter))


def _get_ai_editor_settings(settings_getter: Optional[Callable]) -> Dict[str, Any]:
    if not settings_getter:
        return {}
    ai = settings_getter("ai_editor", {}) or {}
    return ai if isinstance(ai, dict) else {}


def _get_provider_cli_path(section_name: str,
                           settings_getter: Optional[Callable]) -> str:
    """Read ai_editor.<section>.cli_path without leaking the configured path."""
    ai = _get_ai_editor_settings(settings_getter)
    section = ai.get(section_name, {})
    if isinstance(section, dict):
        cli_path = section.get("cli_path", "")
        if isinstance(cli_path, str) and cli_path.strip():
            return cli_path.strip()
    if settings_getter:
        dotted = settings_getter(f"ai_editor.{section_name}.cli_path", "") or ""
        if isinstance(dotted, str):
            return dotted.strip()
    return ""


def _get_provider_section(section_name: str,
                          settings_getter: Optional[Callable]) -> Dict[str, Any]:
    ai = _get_ai_editor_settings(settings_getter)
    section = ai.get(section_name, {})
    return section if isinstance(section, dict) else {}


def _get_provider_option(section_name: str, option: str,
                         settings_getter: Optional[Callable]) -> str:
    section = _get_provider_section(section_name, settings_getter)
    value = section.get(option, "")
    if isinstance(value, str):
        return value.strip()
    return ""


def _get_provider_bool(section_name: str, option: str,
                       settings_getter: Optional[Callable]) -> bool:
    section = _get_provider_section(section_name, settings_getter)
    return bool(section.get(option))


def _get_provider_args_count(section_name: str,
                             settings_getter: Optional[Callable]) -> int:
    section = _get_provider_section(section_name, settings_getter)
    args = section.get("cli_args", section.get("args", []))
    if isinstance(args, str):
        return len([part for part in args.splitlines() if part.strip()])
    if isinstance(args, list):
        return len([part for part in args if str(part).strip()])
    return 0


def _cli_status(section_name: str, default_command: str,
                settings_getter: Optional[Callable]) -> Dict[str, Any]:
    configured_path = _get_provider_cli_path(section_name, settings_getter)
    candidate = configured_path or default_command
    return {
        "cli_configured": bool(configured_path),
        "cli_available": _cli_available(candidate),
        "cli_args_count": _get_provider_args_count(section_name, settings_getter),
    }


def _cli_available(cli_path: str) -> bool:
    if not cli_path:
        return False
    candidate = cli_path.strip().strip('"').strip("'")
    if not candidate:
        return False
    expanded = os.path.expandvars(os.path.expanduser(candidate))
    if os.path.isabs(expanded) or os.path.dirname(expanded):
        return os.path.isfile(expanded)
    return shutil.which(expanded) is not None


_singleton: Optional[ChatProviderRegistry] = None


def get_provider_registry() -> ChatProviderRegistry:
    global _singleton
    if _singleton is None:
        _singleton = ChatProviderRegistry()
    return _singleton
