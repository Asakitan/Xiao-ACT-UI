"""Right-sidebar chat provider registry.

Providers are tabs in the right sidebar (Assistant / Copilot / Claude Code / Codex / plugin).
Each provider wraps a conversation with its own model config and system prompt.

Built-in providers auto-detect availability from supported settings.
Plugins call ``register_chat_provider()`` to add custom tabs.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Dict, List, Optional

from ai_editor.llm_engine import ProviderConfig


BUILTIN_PROVIDER_ORDER = ("chat",)
COPILOT_UNAVAILABLE_REASON = (
    "GitHub Copilot Chat uses VS Code's native ChatWidget/chat participant surface, "
    "not an extension WebviewView."
)
_KEY_REQUIRED_PROVIDERS = {"openai", "anthropic", "deepseek"}
_CLI_PROVIDER_SECTION_NAMES = {
    "claude-code": "claude_code",
    "codex": "codex",
}
_CLI_PROVIDER_DEFAULT_COMMANDS = {
    "claude-code": ("claude",),
    "codex": ("codex",),
}


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
    webview_id: str = ""
    webview_ids: List[str] = field(default_factory=list)
    extension_ids: List[str] = field(default_factory=list)
    metadata: Dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def _metadata_values(value: Any) -> List[Any]:
        if isinstance(value, str):
            return [value]
        if isinstance(value, list):
            return value
        return []

    def candidate_webview_ids(self) -> List[str]:
        ids: List[str] = []
        for value in [
            self.webview_id,
            *self.webview_ids,
            *self._metadata_values(self.metadata.get("webview_ids")),
            *self._metadata_values(self.metadata.get("candidate_view_ids")),
        ]:
            candidate = str(value or "").strip()
            if candidate and candidate not in ids:
                ids.append(candidate)
        return ids

    def candidate_extension_ids(self) -> List[str]:
        ids: List[str] = []
        for value in [
            *self.extension_ids,
            *self._metadata_values(self.metadata.get("extension_ids")),
        ]:
            candidate = str(value or "").strip()
            if candidate and candidate not in ids:
                ids.append(candidate)
        return ids

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
        id="chat", name="Assistant", icon="\U0001f4ac",
        builtin=True, auto_agent=False,
    ),
    # Copilot / Claude Code / Codex are NOT built-in.
    # They appear only when their extension is installed and registers
    # its own webview view via registerWebviewViewProvider().
    # We do not write shim/mock UI for them.
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
        """Return providers with availability and runtime surface metadata."""
        result = []
        for p in self.list_all():
            status = describe_provider_status(p, settings_getter)
            d = p.to_dict()
            d.update(status)
            d["builtin"] = p.builtin
            result.append(d)
        return result

    @staticmethod
    def _check_available(p: ChatProviderDef,
                         settings_getter: Optional[Callable]) -> bool:
        return bool(describe_provider_status(p, settings_getter).get("available"))


def _unavailable(status: Dict[str, Any], reason: str) -> Dict[str, Any]:
    updated = dict(status)
    updated["available"] = False
    updated["status"] = "unavailable"
    updated["unavailable_reason"] = reason
    return updated


def _base_config_from_settings(
        settings_getter: Optional[Callable],
        base_config: Optional[ProviderConfig] = None) -> ProviderConfig:
    if isinstance(base_config, ProviderConfig):
        return base_config
    ai = _get_ai_editor_settings(settings_getter)
    return ProviderConfig(
        provider=str(ai.get("provider") or ProviderConfig().provider),
        api_key=str(ai.get("api_key") or ""),
        base_url=str(ai.get("base_url") or ""),
        model=str(ai.get("model") or ""),
        transport=_normalize_transport(
            ai.get("transport"), "chat_completions", {"chat_completions", "responses"}),
    )


def _compose_runtime_config(
        provider_type: str,
        settings_getter: Optional[Callable],
        base_config: Optional[ProviderConfig] = None,
        *,
        explicit_model: str = "",
        explicit_base_url: str = "",
        explicit_api_key: str = "",
        explicit_transport: str = "",
        system_prompt: str = "",
) -> ProviderConfig:
    base = _base_config_from_settings(settings_getter, base_config)
    defaults = ProviderConfig(provider=provider_type)
    api_key = str(explicit_api_key or "")
    if not api_key:
        api_key = _get_provider_key(provider_type, settings_getter)
    if not api_key and base.provider == provider_type:
        api_key = base.api_key
    base_url = str(explicit_base_url or "")
    if not base_url and base.provider == provider_type and base.base_url:
        base_url = base.base_url
    if not base_url:
        base_url = defaults.effective_base_url
    model = str(explicit_model or "")
    if not model and base.provider == provider_type and base.model:
        model = base.model
    if not model:
        model = defaults.effective_model
    transport = str(explicit_transport or "")
    if not transport:
        transport = base.transport if base.provider == provider_type else "chat_completions"
    transport = _normalize_transport(
        transport, "chat_completions", {"chat_completions", "responses"})
    return ProviderConfig(
        provider=provider_type,
        api_key=api_key,
        base_url=base_url,
        model=model,
        temperature=base.temperature,
        max_tokens=base.max_tokens,
        system_prompt=system_prompt,
        transport=transport,
        top_p=base.top_p,
        frequency_penalty=base.frequency_penalty,
        presence_penalty=base.presence_penalty,
        stop=list(base.stop),
        max_input_tokens=base.max_input_tokens,
        max_output_tokens=base.max_output_tokens,
        timeout=base.timeout,
        extra_headers=dict(base.extra_headers),
        extra_body=dict(base.extra_body),
    )


def _config_is_runnable(cfg: Optional[ProviderConfig]) -> bool:
    if cfg is None:
        return False
    if not cfg.effective_base_url:
        return False
    if cfg.provider in _KEY_REQUIRED_PROVIDERS and not cfg.api_key:
        return False
    return True


def build_provider_runtime_config(
        provider: ChatProviderDef,
        settings_getter: Optional[Callable],
        base_config: Optional[ProviderConfig] = None) -> Optional[ProviderConfig]:
    if provider.id == "chat":
        return _base_config_from_settings(settings_getter, base_config)
    if provider.id == "copilot":
        return None
    if provider.id == "claude-code":
        return _compose_runtime_config(
            "anthropic",
            settings_getter,
            base_config,
            explicit_model=_get_provider_option("claude_code", "model", settings_getter),
            system_prompt=provider.system_prompt,
        )
    if provider.id == "codex":
        requested = _normalize_transport(
            _get_provider_option("codex", "transport", settings_getter),
            "chat_completions",
            {"chat_completions", "responses", "cli"},
        )
        resolved = "responses" if requested == "cli" else requested
        return _compose_runtime_config(
            "openai",
            settings_getter,
            base_config,
            explicit_model=_get_provider_option("codex", "model", settings_getter) or provider.model,
            explicit_transport=resolved,
            system_prompt=provider.system_prompt,
        )
    return _compose_runtime_config(
        provider.provider_type,
        settings_getter,
        base_config,
        explicit_model=provider.model,
        explicit_base_url=provider.base_url,
        explicit_api_key=provider.api_key,
        system_prompt=provider.system_prompt,
    )


def describe_provider_status(
        provider: ChatProviderDef,
        settings_getter: Optional[Callable],
        base_config: Optional[ProviderConfig] = None) -> Dict[str, Any]:
    status: Dict[str, Any] = {
        "available": True,
        "status": "available",
        "unavailable_reason": "",
        "api_key_available": True,
        "requested_transport": "",
        "resolved_transport": "",
        "runtime_mode": "direct",
        "backend_provider": "",
        "transport": "",
        "model": provider.model,
    }
    if provider.id == "chat":
        return status

    if provider.id == "copilot":
        has_auth = _has_copilot_auth(settings_getter)
        copilot_available = has_auth
        status.update({
            "requested_transport": "chatParticipant",
            "resolved_transport": "native-chat",
            "transport": "native-chat",
            "runtime_mode": "native-chat",
            "backend_provider": "chatParticipant",
            "api_key_available": has_auth,
            "capability": "github-copilot-chat",
            "model": provider.model,
            "native_chat": True,
        })
        if not copilot_available:
            return _unavailable(
                status,
                "GitHub Copilot is not configured. "
                "Set a Copilot auth token in Settings to enable.",
            )
        return status

    if provider.id == "claude-code":
        cli_info = provider_runtime_cli_status("claude-code", settings_getter)
        status.update(cli_info)
        status.update({
            "requested_transport": "webviewView",
            "resolved_transport": "webviewView",
            "transport": "extension-webview",
            "runtime_mode": "extension-webview",
            "backend_provider": "extension",
            "model": provider.model or "Claude Code",
            "api_key_available": True,
        })
        return status

    if provider.id == "codex":
        cli_info = provider_runtime_cli_status("codex", settings_getter)
        status.update(cli_info)
        status.update({
            "requested_transport": "webviewView",
            "resolved_transport": "webviewView",
            "transport": "extension-webview",
            "runtime_mode": "extension-webview",
            "backend_provider": "extension",
            "model": provider.model or "Codex",
            "api_key_available": True,
        })
        return status

    if provider.candidate_webview_ids():
        status.update({
            "requested_transport": "webviewView",
            "resolved_transport": "webviewView",
            "transport": "extension-webview",
            "runtime_mode": "extension-webview",
            "backend_provider": "extension",
            "api_key_available": True,
        })
        return status

    cfg = build_provider_runtime_config(provider, settings_getter, base_config)
    status.update({
        "backend_provider": provider.provider_type,
        "resolved_transport": cfg.transport if cfg else "chat_completions",
        "transport": cfg.transport if cfg else "chat_completions",
        "model": cfg.effective_model if cfg else provider.model,
        "api_key_available": bool(provider.api_key or _get_provider_key(provider.provider_type, settings_getter)),
    })
    if not _config_is_runnable(cfg):
        if provider.provider_type in _KEY_REQUIRED_PROVIDERS:
            return _unavailable(
                status,
                f"API key is not configured for provider type {provider.provider_type}.",
            )
        return _unavailable(status, "Configure this provider in Settings.")
    return status


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


def provider_runtime_cli_status(provider_id: str,
                                settings_getter: Optional[Callable]) -> Dict[str, Any]:
    """Return CLI availability for a native provider controller."""
    section = _CLI_PROVIDER_SECTION_NAMES.get(provider_id, provider_id)
    configured_path = _get_provider_cli_path(section, settings_getter)
    return {
        "cli_configured": bool(configured_path),
        "cli_available": bool(provider_runtime_cli_path(provider_id, settings_getter)),
        "cli_args_count": _get_provider_args_count(section, settings_getter),
    }


def provider_runtime_cli_path(provider_id: str,
                              settings_getter: Optional[Callable]) -> str:
    """Resolve the CLI executable that backs a native provider controller.

    GitHub Copilot Chat intentionally has no CLI mapping here: VS Code hosts it
    through native chat participants, not a provider CLI or WebviewView.
    """
    normalized = str(provider_id or "").strip()
    if normalized == "copilot":
        return ""
    section = _CLI_PROVIDER_SECTION_NAMES.get(normalized, normalized)
    configured_path = _get_provider_cli_path(section, settings_getter)
    if configured_path:
        resolved = _resolve_cli_path(configured_path)
        if resolved:
            return resolved

    for command in _CLI_PROVIDER_DEFAULT_COMMANDS.get(normalized, ()):  # trusted command names
        resolved = _resolve_cli_path(command)
        if resolved:
            return resolved
    return ""


def _resolve_cli_path(cli_path: str) -> str:
    candidate = str(cli_path or "").strip().strip('"').strip("'")
    if not candidate:
        return ""
    expanded = os.path.expandvars(os.path.expanduser(candidate))
    if os.path.isabs(expanded) or os.path.dirname(expanded):
        return expanded if os.path.isfile(expanded) else ""
    return shutil.which(expanded) or ""

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
