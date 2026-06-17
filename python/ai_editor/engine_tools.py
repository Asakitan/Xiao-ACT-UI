"""LLM-callable tools — aligned with VSCode's built-in tool set.

Core tools mirror VSCode Copilot's tool design:
  editFile, readFile, listFiles, searchFiles, runTerminal,
  askQuestion, taskComplete, getConfirmation

Plus one aggregate ``engine`` tool for platform and plugin runtime queries,
keeping the tool list clean for the LLM.
"""

from __future__ import annotations

import glob
import json
import os
import subprocess
import time
from typing import Any, Dict, List, Optional

from ai_editor.tool_registry import ToolRegistry


def register_engine_tools(registry: ToolRegistry, gui_ref: Any) -> None:
    """Register VSCode-aligned tools + engine aggregate."""

    # ==================================================================
    # VSCode Core: File Operations
    # ==================================================================

    registry.register(
        name="readFile",
        description="Read a file's content. Returns the text content of the specified file.",
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Absolute or relative file path"},
                "startLine": {"type": "integer", "description": "Start line (1-based, optional)"},
                "endLine": {"type": "integer", "description": "End line (inclusive, optional)"},
            },
            "required": ["path"],
        },
        handler=lambda path, startLine=0, endLine=0: _read_file(path, int(startLine), int(endLine)),
        category="file",
    )

    registry.register(
        name="editFile",
        description="Create or edit a file. Can write full content or replace a specific range.",
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path to create or edit"},
                "content": {"type": "string", "description": "New file content (full or partial)"},
                "startLine": {"type": "integer", "description": "Start line to replace (1-based, 0=full rewrite)"},
                "endLine": {"type": "integer", "description": "End line to replace (inclusive)"},
            },
            "required": ["path", "content"],
        },
        handler=lambda path, content, startLine=0, endLine=0: _edit_file(path, content, int(startLine), int(endLine)),
        category="file",
        requires_confirm=True,
    )

    registry.register(
        name="listFiles",
        description="List files and directories at a path. Returns names with type indicators.",
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Directory path (default: current directory)", "default": "."},
                "pattern": {"type": "string", "description": "Glob pattern to filter (e.g. '*.py')", "default": ""},
                "recursive": {"type": "boolean", "description": "Recurse into subdirectories", "default": False},
                "limit": {"type": "integer", "description": "Max entries to return", "default": 100},
            },
        },
        handler=lambda path=".", pattern="", recursive=False, limit=100: _list_files(path, pattern, bool(recursive), int(limit)),
        category="file",
    )

    registry.register(
        name="searchFiles",
        description="Search for text in files using grep/regex. Returns matching lines with context.",
        parameters={
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Search text or regex pattern"},
                "path": {"type": "string", "description": "Directory to search in", "default": "."},
                "pattern": {"type": "string", "description": "File glob filter (e.g. '*.py')", "default": ""},
                "caseSensitive": {"type": "boolean", "description": "Case-sensitive search", "default": False},
                "regex": {"type": "boolean", "description": "Treat query as regex", "default": False},
                "limit": {"type": "integer", "description": "Max results", "default": 50},
            },
            "required": ["query"],
        },
        handler=lambda query, path=".", pattern="", caseSensitive=False, regex=False, limit=50: _search_files(query, path, pattern, bool(caseSensitive), bool(regex), int(limit)),
        category="file",
    )

    # ==================================================================
    # VSCode Core: Terminal / Shell
    # ==================================================================

    registry.register(
        name="runTerminal",
        description="Execute a shell command and return stdout/stderr. Timeout 30s.",
        parameters={
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to execute"},
                "cwd": {"type": "string", "description": "Working directory (optional)", "default": ""},
            },
            "required": ["command"],
        },
        handler=lambda command, cwd="": _run_terminal(command, cwd),
        category="terminal",
        requires_confirm=True,
    )

    # ==================================================================
    # VSCode Core: Interaction
    # ==================================================================

    registry.register(
        name="askQuestion",
        description="Ask the user a question and wait for their response. Use for clarification.",
        parameters={
            "type": "object",
            "properties": {
                "question": {"type": "string", "description": "Question to ask the user"},
            },
            "required": ["question"],
        },
        handler=lambda question: {"type": "question", "question": question, "note": "Displayed to user in chat"},
        category="interaction",
    )

    registry.register(
        name="taskComplete",
        description="Signal that the current task is complete. Include a summary.",
        parameters={
            "type": "object",
            "properties": {
                "summary": {"type": "string", "description": "Summary of what was accomplished"},
            },
            "required": ["summary"],
        },
        handler=lambda summary: {"status": "complete", "summary": summary},
        category="interaction",
    )

    registry.register(
        name="getConfirmation",
        description="Ask the user to confirm a potentially dangerous action before proceeding.",
        parameters={
            "type": "object",
            "properties": {
                "action": {"type": "string", "description": "Description of the action to confirm"},
                "risk": {"type": "string", "description": "Risk level: low/medium/high", "default": "medium"},
            },
            "required": ["action"],
        },
        handler=lambda action, risk="medium": {"type": "confirmation", "action": action, "risk": risk},
        category="interaction",
        requires_confirm=True,
    )

    # ==================================================================
    # VSCode Core: Editor
    # ==================================================================

    registry.register(
        name="editor_getContent",
        description="Get the current content of the active editor tab.",
        parameters={"type": "object", "properties": {}},
        handler=lambda: {"note": "Resolved via JS bridge — returns editor text + language"},
        category="editor",
    )

    registry.register(
        name="editor_setContent",
        description="Set the content of the active editor tab.",
        parameters={
            "type": "object",
            "properties": {
                "content": {"type": "string", "description": "Content to set"},
                "language": {"type": "string", "description": "Language mode", "default": ""},
            },
            "required": ["content"],
        },
        handler=lambda content, language="": {"note": "Dispatched via JS bridge", "length": len(content)},
        category="editor",
    )

    registry.register(
        name="editor_getSelection",
        description="Get the currently selected text in the editor.",
        parameters={"type": "object", "properties": {}},
        handler=lambda: {"note": "Resolved via JS bridge"},
        category="editor",
    )

    # ==================================================================
    # Engine aggregate — single entry point for platform/plugin queries
    # ==================================================================

    registry.register(
        name="engine",
        description=(
            "Query the SAO ACT engine. Accepts an 'action' parameter.\n"
            "\n"
            "Platform actions (always available):\n"
            "  system_info — ACT version, uptime, data source\n"
            "  plugins — installed plugin list\n"
            "  settings_get — read a setting (pass 'key')\n"
            "  settings_set — write a setting (pass 'key' and 'value')\n"
            "  memory_status — memory data source health\n"
            "  eval — evaluate a Python expression (pass 'expression')\n"
            "  exec — execute Python code block (pass 'code')\n"
            "\n"
            "Plugin actions are registered dynamically by loaded plugins.\n"
        ),
        parameters={
            "type": "object",
            "properties": {
                "action": {"type": "string", "description": "Which engine query to run"},
                "key": {"type": "string", "description": "Parameter for settings_get/set", "default": ""},
                "value": {"description": "Value for settings_set", "default": None},
                "expression": {"type": "string", "description": "Python expression for eval", "default": ""},
                "code": {"type": "string", "description": "Python code for exec", "default": ""},
                "target": {"type": "string", "description": "Optional plugin-specific target", "default": ""},
            },
            "required": ["action"],
        },
        handler=lambda **kw: _engine_dispatch(gui_ref, **kw),
        category="engine",
    )


# ======================================================================
# File operation handlers
# ======================================================================

def _read_file(path: str, start: int = 0, end: int = 0) -> Dict[str, Any]:
    try:
        path = os.path.abspath(path)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        if start > 0:
            s = max(0, start - 1)
            e = end if end > 0 else len(lines)
            selected = lines[s:e]
            return {"path": path, "lines": len(selected), "startLine": start,
                    "content": "".join(selected)}
        return {"path": path, "lines": len(lines), "content": "".join(lines[:5000]),
                "truncated": len(lines) > 5000}
    except Exception as exc:
        return {"error": str(exc)}


def _edit_file(path: str, content: str, start: int = 0, end: int = 0) -> Dict[str, Any]:
    try:
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if start > 0:
            with open(path, "r", encoding="utf-8") as f:
                lines = f.readlines()
            s = max(0, start - 1)
            e = end if end > 0 else start
            new_lines = content.split("\n")
            lines[s:e] = [l + "\n" for l in new_lines]
            with open(path, "w", encoding="utf-8") as f:
                f.writelines(lines)
            return {"ok": True, "path": path, "linesModified": len(new_lines)}
        else:
            with open(path, "w", encoding="utf-8") as f:
                f.write(content)
            return {"ok": True, "path": path, "bytesWritten": len(content.encode("utf-8"))}
    except Exception as exc:
        return {"error": str(exc)}


def _list_files(path: str, pattern: str, recursive: bool, limit: int) -> Dict[str, Any]:
    try:
        path = os.path.abspath(path)
        if pattern:
            if recursive:
                entries = glob.glob(os.path.join(path, "**", pattern), recursive=True)
            else:
                entries = glob.glob(os.path.join(path, pattern))
        else:
            if recursive:
                entries = []
                for root, dirs, files in os.walk(path):
                    for f in files:
                        entries.append(os.path.join(root, f))
                        if len(entries) >= limit:
                            break
                    if len(entries) >= limit:
                        break
            else:
                entries = [os.path.join(path, e) for e in os.listdir(path)]
        result = []
        for e in entries[:limit]:
            is_dir = os.path.isdir(e)
            result.append({
                "name": os.path.relpath(e, path),
                "type": "directory" if is_dir else "file",
                "size": os.path.getsize(e) if not is_dir else 0,
            })
        return {"path": path, "entries": result, "total": len(result),
                "truncated": len(entries) > limit}
    except Exception as exc:
        return {"error": str(exc)}


def _search_files(query: str, path: str, pattern: str, case_sensitive: bool,
                  regex: bool, limit: int) -> Dict[str, Any]:
    import re
    try:
        path = os.path.abspath(path)
        flags = 0 if case_sensitive else re.IGNORECASE
        if regex:
            pat = re.compile(query, flags)
        else:
            pat = re.compile(re.escape(query), flags)
        results = []
        file_pattern = pattern or "*"
        for fpath in glob.glob(os.path.join(path, "**", file_pattern), recursive=True):
            if os.path.isdir(fpath):
                continue
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    for i, line in enumerate(f, 1):
                        if pat.search(line):
                            results.append({
                                "file": os.path.relpath(fpath, path),
                                "line": i,
                                "text": line.rstrip()[:200],
                            })
                            if len(results) >= limit:
                                break
            except (OSError, UnicodeDecodeError):
                continue
            if len(results) >= limit:
                break
        return {"query": query, "results": results, "total": len(results)}
    except Exception as exc:
        return {"error": str(exc)}


# ======================================================================
# Terminal handler
# ======================================================================

def _run_terminal(command: str, cwd: str = "") -> Dict[str, Any]:
    try:
        kwargs: Dict[str, Any] = {
            "shell": True, "capture_output": True, "text": True, "timeout": 30,
        }
        if cwd:
            kwargs["cwd"] = os.path.abspath(cwd)
        result = subprocess.run(command, **kwargs)
        return {
            "exitCode": result.returncode,
            "stdout": result.stdout[:8000] if result.stdout else "",
            "stderr": result.stderr[:4000] if result.stderr else "",
        }
    except subprocess.TimeoutExpired:
        return {"error": "Command timed out (30s)", "exitCode": -1}
    except Exception as exc:
        return {"error": str(exc)}


# ======================================================================
# Engine aggregate dispatcher
# ======================================================================

def _engine_dispatch(gui_ref: Any, action: str = "", **kw) -> Any:
    handlers = {
        "memory_status": lambda: _memory_status(gui_ref),
        "system_info": lambda: _system_info(gui_ref),
        "plugins": lambda: _plugins(gui_ref),
        "settings_get": lambda: _settings_get(gui_ref, kw.get("key", "")),
        "settings_set": lambda: _settings_set(gui_ref, kw.get("key", ""), kw.get("value")),
        "eval": lambda: _eval(gui_ref, kw.get("expression", "")),
        "exec": lambda: _exec(gui_ref, kw.get("code", "")),
    }
    platform_action = action in handlers
    fn = handlers.get(action)
    plugin_handlers = getattr(gui_ref, '_ai_engine_actions', None)
    if not fn and isinstance(plugin_handlers, dict):
        fn = plugin_handlers.get(action)
    if not fn:
        available = list(handlers.keys())
        if isinstance(plugin_handlers, dict):
            available.extend(sorted(str(k) for k in plugin_handlers.keys()))
        return {"error": f"Unknown engine action: {action}", "available": available}
    try:
        if platform_action:
            return fn()
        return fn(**kw)
    except Exception as exc:
        return {"error": str(exc)}


# ── Platform engine sub-handlers ──

def _memory_status(g: Any) -> Dict:
    bridge = getattr(g, '_mem_bridge', None)
    if not bridge:
        pb = getattr(g, '_packet_bridge', None)
        if pb: bridge = getattr(pb, '_unified_data_source', None)
    if not bridge: return {"connected": False}
    fn = getattr(bridge, 'status', None)
    if callable(fn):
        try: return fn()
        except: pass
    return {"connected": True, "type": type(bridge).__name__}

def _system_info(g: Any) -> Dict:
    try:
        from config import APP_VERSION
    except ImportError:
        APP_VERSION = "?"
    s = getattr(g, 'settings', None)
    return {
        "version": APP_VERSION,
        "ui_mode": s.get("ui_mode", "?") if s else "?",
        "data_source": s.get("data_source", "?") if s else "?",
        "uptime_s": round(time.monotonic() - getattr(g, '_start_time', time.monotonic()), 1),
    }

def _plugins(g: Any) -> Dict:
    pm = getattr(g, '_plugin_manager', None)
    if not pm: return {"plugins": []}
    manifests = getattr(pm, '_manifests', None) or {}
    enabled = getattr(pm, '_enabled', None) or {}
    return {"plugins": [{"id": pid, "name": m.get("name", pid), "enabled": enabled.get(pid, False)} for pid, m in manifests.items()]}

def _settings_get(g: Any, key: str) -> Any:
    s = getattr(g, 'settings', None)
    return {"key": key, "value": s.get(key) if s else None}

def _settings_set(g: Any, key: str, value: Any) -> Dict:
    s = getattr(g, 'settings', None)
    if not s: return {"error": "Settings not available"}
    s.set(key, value)
    try: s.save()
    except: pass
    return {"ok": True, "key": key}

def _eval(g: Any, expression: str) -> Dict:
    try:
        return {"result": eval(expression, {"__builtins__": __builtins__}, {"gui": g, "json": json, "time": time})}
    except Exception as exc:
        return {"error": str(exc)}

def _exec(g: Any, code: str) -> Dict:
    ns = {"gui": g, "json": json, "time": time, "_output": []}
    try:
        exec(code + "\n", {"__builtins__": __builtins__}, ns)
        return {"ok": True, "output": ns.get("_output", [])}
    except Exception as exc:
        return {"error": str(exc)}
