# -*- coding: utf-8 -*-
"""Multi-language scripting runtime registry.

Discovers and manages language runtimes (Lua, C#, AngelScript, Emma)
so that plugins can be written in any supported language while calling
the full platform SDK through a unified bridge.

Usage from PluginManager::

    from act_platform.scripting import get_runtime, LANGUAGE_PYTHON

    lang = manifest.get("language", LANGUAGE_PYTHON)
    if lang != LANGUAGE_PYTHON:
        runtime = get_runtime(lang)
        module  = runtime.load_script(entry_path, record, ctx)
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from .base import ScriptRuntime

LANGUAGE_PYTHON = "python"
LANGUAGE_LUA = "lua"
LANGUAGE_CSHARP = "csharp"
LANGUAGE_ANGELSCRIPT = "angelscript"
LANGUAGE_EMMA = "emma"

SUPPORTED_LANGUAGES = (
    LANGUAGE_PYTHON,
    LANGUAGE_LUA,
    LANGUAGE_CSHARP,
    LANGUAGE_ANGELSCRIPT,
    LANGUAGE_EMMA,
)

ENTRY_EXTENSIONS: dict[str, str] = {
    ".py": LANGUAGE_PYTHON,
    ".lua": LANGUAGE_LUA,
    ".cs": LANGUAGE_CSHARP,
    ".as": LANGUAGE_ANGELSCRIPT,
    ".emma": LANGUAGE_EMMA,
}

_runtimes: dict[str, "ScriptRuntime"] = {}


def get_runtime(language: str) -> "ScriptRuntime":
    """Return (and lazily create) the runtime for *language*.

    Raises ``ValueError`` for unknown languages, ``RuntimeError`` if
    the runtime's native dependency is missing.
    """
    lang = _normalize(language)
    if lang == LANGUAGE_PYTHON:
        raise ValueError("Python plugins use the native loader, not a ScriptRuntime")
    if lang not in SUPPORTED_LANGUAGES:
        raise ValueError(f"unsupported plugin language: {language!r}")

    cached = _runtimes.get(lang)
    if cached is not None:
        return cached

    rt = _create_runtime(lang)
    _runtimes[lang] = rt
    return rt


def runtime_available(language: str) -> bool:
    """Check whether *language*'s runtime can be instantiated."""
    lang = _normalize(language)
    if lang == LANGUAGE_PYTHON:
        return True
    if lang not in SUPPORTED_LANGUAGES:
        return False
    try:
        get_runtime(lang)
        return True
    except (RuntimeError, ImportError):
        return False


def detect_language(entry: str, manifest_language: str = "") -> str:
    """Infer plugin language from manifest ``language`` field or entry extension."""
    if manifest_language:
        lang = _normalize(manifest_language)
        if lang in SUPPORTED_LANGUAGES:
            return lang
    import os
    _, ext = os.path.splitext(entry)
    return ENTRY_EXTENSIONS.get(ext.lower(), LANGUAGE_PYTHON)


def list_runtimes() -> dict[str, dict]:
    """Return status of all language runtimes."""
    out = {}
    for lang in SUPPORTED_LANGUAGES:
        if lang == LANGUAGE_PYTHON:
            out[lang] = {"available": True, "loaded": True, "error": ""}
            continue
        try:
            rt = get_runtime(lang)
            out[lang] = {"available": True, "loaded": True, "error": "",
                         "engine": rt.engine_name}
        except (RuntimeError, ImportError, ValueError) as exc:
            out[lang] = {"available": False, "loaded": False, "error": str(exc)}
    return out


def _normalize(language: str) -> str:
    text = str(language or "").strip().lower().replace("-", "").replace("_", "")
    aliases = {
        "python": LANGUAGE_PYTHON, "py": LANGUAGE_PYTHON,
        "lua": LANGUAGE_LUA,
        "csharp": LANGUAGE_CSHARP, "cs": LANGUAGE_CSHARP, "c#": LANGUAGE_CSHARP,
        "dotnet": LANGUAGE_CSHARP, ".net": LANGUAGE_CSHARP,
        "angelscript": LANGUAGE_ANGELSCRIPT, "as": LANGUAGE_ANGELSCRIPT,
        "angel": LANGUAGE_ANGELSCRIPT,
        "emma": LANGUAGE_EMMA,
    }
    return aliases.get(text, text)


def _create_runtime(lang: str) -> "ScriptRuntime":
    if lang == LANGUAGE_LUA:
        from .lua_runtime import LuaRuntime
        return LuaRuntime()
    if lang == LANGUAGE_CSHARP:
        from .csharp_runtime import CSharpRuntime
        return CSharpRuntime()
    if lang == LANGUAGE_ANGELSCRIPT:
        from .angel_runtime import AngelScriptRuntime
        return AngelScriptRuntime()
    if lang == LANGUAGE_EMMA:
        from .emma_runtime import EmmaRuntime
        return EmmaRuntime()
    raise ValueError(f"no runtime implementation for language: {lang!r}")
