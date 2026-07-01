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
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
import uuid
from typing import Any, Dict, List, Optional, Tuple

from ai_editor.tool_registry import ToolRegistry

try:
    import winpty as _winpty  # type: ignore
    _PTY_AVAILABLE = os.name == "nt"
except Exception:
    _winpty = None
    _PTY_AVAILABLE = False


def _call_editor_api(api_ref: Any, method_name: str, *args: Any) -> Dict[str, Any]:
    if api_ref is None:
        return {"error": "No editor API available"}
    fn = getattr(api_ref, method_name, None)
    if not callable(fn):
        return {"error": f"Editor API method unavailable: {method_name}"}
    try:
        result = fn(*args)
    except Exception as exc:
        return {"error": str(exc)}
    if result is None:
        return {"error": f"Editor API method returned no result: {method_name}"}
    if isinstance(result, dict):
        return result
    return {"result": result}


def _call_live_editor_api(api_ref: Any, method_name: str, *args: Any) -> Dict[str, Any]:
    if api_ref is None:
        return {"error": "No editor API available"}
    if hasattr(api_ref, "_window") and getattr(api_ref, "_window") is None:
        return {"error": "Editor window is not available"}
    return _call_editor_api(api_ref, method_name, *args)


def register_engine_tools(registry: ToolRegistry, gui_ref: Any, api_ref: Any = None) -> None:
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
        tags={"readOnly": True},
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
        tags={"destructive": True},
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
        tags={"readOnly": True},
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
        tags={"readOnly": True},
    )

    # ==================================================================
    # VSCode Core: Terminal / Shell
    # ==================================================================

    registry.register(
        name="runTerminal",
        description="Execute a shell command and return stdout/stderr. Honors AI Editor terminal settings when configured.",
        parameters={
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to execute"},
                "cwd": {"type": "string", "description": "Working directory (optional)", "default": ""},
                "mode": {"type": "string", "description": "run (default), start, status, write, or stop", "default": "run"},
                "jobId": {"type": "string", "description": "Terminal job id for status/stop", "default": ""},
                "sinceSeq": {"type": "integer", "description": "Return job chunks after this sequence", "default": 0},
                "profile": {"type": "string", "description": "Terminal profile name to use for this command", "default": ""},
                "data": {"type": "string", "description": "Text to write to the running terminal job stdin", "default": ""},
                "closeStdin": {"type": "boolean", "description": "Close stdin after writing data", "default": False},
                "pty": {"type": "boolean", "description": "Use a real pseudo-terminal (ConPTY) for mode=start, needed for full-screen/raw-mode programs", "default": False},
                "cols": {"type": "integer", "description": "Terminal width in columns, for mode=start/resize", "default": 0},
                "rows": {"type": "integer", "description": "Terminal height in rows, for mode=start/resize", "default": 0},
            },
            "required": ["command"],
        },
        handler=lambda command="", cwd="", mode="run", jobId="", sinceSeq=0, profile="", data="", closeStdin=False, pty=False, cols=0, rows=0: _run_terminal(
            command, cwd, gui_ref, api_ref, mode, jobId, int(sinceSeq or 0), profile, data, bool(closeStdin),
            bool(pty), int(cols or 0), int(rows or 0)),
        category="terminal",
        requires_confirm=True,
        tags={"destructive": True},
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
        tags={"readOnly": True},
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
        tags={"readOnly": True},
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
        tags={"readOnly": True},
    )

    # ==================================================================
    # VSCode Core: Editor
    # ==================================================================

    registry.register(
        name="editor_getContent",
        description="Get the current content of the active editor tab.",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _call_live_editor_api(api_ref, "editor_get_content"),
        category="editor",
        tags={"readOnly": True},
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
        handler=lambda content, language="": _call_live_editor_api(api_ref, "editor_set_content", content, language),
        category="editor",
        tags={"destructive": True},
    )

    registry.register(
        name="editor_getSelection",
        description="Get the currently selected text in the editor.",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _call_live_editor_api(api_ref, "editor_get_selection"),
        category="editor",
        tags={"readOnly": True},
    )

    registry.register(
        name="editor_insertText",
        description="Insert text at the current cursor position in the active editor tab.",
        parameters={
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "Text to insert at the cursor"},
            },
            "required": ["text"],
        },
        handler=lambda text: _call_live_editor_api(api_ref, "editor_insert_text", text),
        category="editor",
        tags={"destructive": True},
    )

    registry.register(
        name="editor_goToLine",
        description="Move the editor cursor to a specific line number and scroll it into view.",
        parameters={
            "type": "object",
            "properties": {
                "line": {"type": "integer", "description": "1-based line number to navigate to"},
            },
            "required": ["line"],
        },
        handler=lambda line: _call_live_editor_api(api_ref, "editor_go_to_line", line),
        category="editor",
        tags={"readOnly": True},
    )

    # ==================================================================
    # Web Fetch
    # ==================================================================

    registry.register(
        name="webFetch",
        description="Fetch a URL and return its response. Supports GET/POST with optional headers and body.",
        parameters={
            "type": "object",
            "properties": {
                "url": {"type": "string", "description": "URL to fetch"},
                "method": {"type": "string", "description": "HTTP method (GET, POST, etc.)", "default": "GET"},
                "headers": {"type": "object", "description": "Request headers as key-value pairs", "default": {}},
                "body": {"type": "string", "description": "Request body (for POST/PUT)", "default": ""},
            },
            "required": ["url"],
        },
        handler=lambda url, method="GET", headers=None, body="": _web_fetch(url, method, headers or {}, body),
        category="network",
        tags={"readOnly": True},
    )

    # ==================================================================
    # Todo List Management
    # ==================================================================

    registry.register(
        name="manageTodoList",
        description=(
            "Manage a simple todo list. Actions:\n"
            "  add — add a new item (pass 'text')\n"
            "  remove — remove item by index (pass 'index', 0-based)\n"
            "  list — list all items\n"
            "  update — update item text at index (pass 'index' and 'text')\n"
        ),
        parameters={
            "type": "object",
            "properties": {
                "action": {"type": "string", "description": "Action: add, remove, list, update",
                           "enum": ["add", "remove", "list", "update"]},
                "text": {"type": "string", "description": "Todo item text (for add/update)", "default": ""},
                "index": {"type": "integer", "description": "Item index, 0-based (for remove/update)", "default": -1},
            },
            "required": ["action"],
        },
        handler=lambda action, text="", index=-1: _manage_todo(gui_ref, action, text, int(index)),
        category="interaction",
        tags={"readOnly": False},
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
            "  list_processes — list running processes\n"
            "  select_process — attach to a process by name or pid\n"
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
        tags={"readOnly": False},
    )

    # ==================================================================
    # SDK Dumper (only registered when the backend module is available)
    # ==================================================================

    try:
        from ai_editor import sdk_dumper as _sdk_mod  # noqa: F401
        _has_sdk_dumper = True
    except ImportError:
        _has_sdk_dumper = False

    if _has_sdk_dumper:
        registry.register(
            name="sdkDumper",
            description=(
                "Dump game engine SDK from a running process using memory reading.\n"
                "Supports: il2cpp (Unity IL2CPP), mono (Unity Mono), unreal (UE4/5), source (Valve Source).\n\n"
                "Actions:\n"
                "  detect — auto-detect game engine for a PID\n"
                "  dump — extract classes/fields/methods from process memory\n"
                "  list_engines — show supported engines\n"
                "  save — save dump result to file (json or header format)\n"
            ),
            parameters={
                "type": "object",
                "properties": {
                    "action": {"type": "string", "description": "detect | dump | list_engines | save",
                               "enum": ["detect", "dump", "list_engines", "save"]},
                    "pid": {"type": "integer", "description": "Target process ID"},
                    "engine": {"type": "string", "description": "Engine type (il2cpp/mono/unreal/source). Auto-detected if omitted."},
                    "output": {"type": "string", "description": "Output file path (for save action)"},
                    "format": {"type": "string", "description": "Output format: json or header", "default": "json"},
                },
                "required": ["action"],
            },
            handler=lambda **kw: _sdk_dumper_dispatch(**kw),
            category="engine",
            tags={"readOnly": True},
        )


# ======================================================================
# SDK Dumper dispatcher
# ======================================================================

_last_dump_result = None

def _sdk_dumper_dispatch(action: str = "", pid: int = 0, engine: str = "",
                          output: str = "", format: str = "json", **kw):
    global _last_dump_result
    try:
        from ai_editor.sdk_dumper import detect_engine, create_dumper, list_engines
    except ImportError as exc:
        return {"error": f"SDK Dumper not available: {exc}"}

    if action == "list_engines":
        return {"engines": list_engines()}

    if action == "detect":
        if not pid:
            return {"error": "pid is required"}
        eng = detect_engine(pid)
        return {"engine": eng, "pid": pid}

    if action == "dump":
        if not pid:
            return {"error": "pid is required"}
        if not engine:
            engine = detect_engine(pid)
            if engine == "unknown":
                return {"error": f"Could not auto-detect engine for PID {pid}. Specify engine manually."}
        dumper = None
        try:
            dumper = create_dumper(engine, pid)
            result = dumper.dump()
            _last_dump_result = result
            summary = {
                "engine": result.engine,
                "classes": len(result.classes),
                "enums": len(result.enums),
                "errors": result.errors[:5],
            }
            if result.classes:
                summary["sample_classes"] = [
                    {"name": c.full_name, "fields": len(c.fields), "methods": len(c.methods)}
                    for c in result.classes[:10]
                ]
            return summary
        except Exception as exc:
            return {"error": str(exc)}
        finally:
            try:
                reader = getattr(dumper, "reader", None)
                close = getattr(reader, "close", None)
                if callable(close):
                    close()
            except Exception as cleanup_exc:
                print(f"[SDKDumper] Reader cleanup failed: {cleanup_exc}")

    if action == "save":
        if not _last_dump_result:
            return {"error": "No dump result available. Run 'dump' first."}
        if not output:
            import time
            ext = "json" if format == "json" else "h"
            output = f"sdk_dump_{_last_dump_result.engine}_{int(time.time())}.{ext}"
        path = _last_dump_result.save(output, format)
        return {"ok": True, "path": path, "classes": len(_last_dump_result.classes)}

    return {"error": f"Unknown action: {action}. Use detect/dump/list_engines/save"}


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

_DEFAULT_TERMINAL_TIMEOUT = 30
_DEFAULT_STDOUT_LIMIT = 8000
_DEFAULT_STDERR_LIMIT = 4000
_TERMINAL_PROFILE_DEFINITIONS: Dict[str, Dict[str, Any]] = {
    "PowerShell 7 (No Profile)": {
        "shells": ["pwsh.exe", "pwsh"],
        "fallbackShells": ["powershell.exe", "powershell"],
        "args": ["-NoLogo", "-NoProfile"],
        "kind": "powershell",
    },
    "Windows PowerShell": {
        "shells": ["powershell.exe", "powershell"],
        "args": ["-NoLogo", "-NoProfile"],
        "kind": "powershell",
    },
    "Command Prompt": {
        "shells": ["cmd.exe", "cmd"],
        "args": [],
        "kind": "cmd",
    },
    "Git Bash": {
        "shells": [
            r"%ProgramFiles%\Git\bin\bash.exe",
            r"%ProgramFiles%\Git\usr\bin\bash.exe",
            r"%LocalAppData%\Programs\Git\bin\bash.exe",
            "bash.exe",
            "bash",
        ],
        "args": [],
        "kind": "posix",
    },
    "System Shell": {
        "shells": [],
        "args": [],
        "kind": "system",
    },
}


def _get_terminal_settings(gui_ref: Any = None) -> Dict[str, Any]:
    settings = getattr(gui_ref, "settings", None) if gui_ref else None
    if not settings:
        return {}
    try:
        ai_cfg = settings.get("ai_editor", {}) or {}
    except Exception:
        return {}
    terminal = ai_cfg.get("terminal", {}) if isinstance(ai_cfg, dict) else {}
    if not isinstance(terminal, dict) or not terminal:
        try:
            dotted = settings.get("ai_editor.terminal", {}) or {}
        except Exception:
            dotted = {}
        if isinstance(dotted, dict) and dotted:
            terminal = dotted
    return dict(terminal) if isinstance(terminal, dict) else {}


def _terminal_shell_path(value: Any) -> str:
    if not isinstance(value, str):
        return ""
    shell_path = value.strip().strip('"').strip("'")
    if not shell_path:
        return ""
    return os.path.expandvars(os.path.expanduser(shell_path))


def _terminal_args(value: Any) -> List[str]:
    if isinstance(value, (list, tuple)):
        return [str(arg) for arg in value if arg is not None and str(arg) != ""]
    if isinstance(value, str) and value.strip():
        try:
            return shlex.split(value, posix=os.name != "nt")
        except ValueError:
            return [value]
    return []


def _terminal_profile_name(value: Any) -> str:
    return str(value or "").strip()


def _terminal_profile_definition(profile: str) -> Dict[str, Any]:
    return dict(_TERMINAL_PROFILE_DEFINITIONS.get(profile) or {})


def _terminal_shell_candidate_path(candidate: Any) -> str:
    raw = _terminal_shell_path(candidate)
    if not raw:
        return ""
    if os.path.isabs(raw) or any(sep in raw for sep in (os.sep, "/", "\\")):
        return raw if os.path.isfile(raw) else ""
    return shutil.which(raw) or ""


def _resolve_terminal_profile_shell(profile: str) -> Tuple[str, List[str], Dict[str, Any]]:
    meta: Dict[str, Any] = {
        "profile": profile,
        "profileResolved": False,
        "profileFallback": False,
        "profileShellMissing": False,
    }
    definition = _terminal_profile_definition(profile)
    if not definition:
        if profile:
            meta["profileShellMissing"] = True
        return "", [], meta
    for candidate in definition.get("shells") or []:
        path = _terminal_shell_candidate_path(candidate)
        if path:
            meta["profileResolved"] = True
            return path, _terminal_args(definition.get("args")), meta
    for candidate in definition.get("fallbackShells") or []:
        path = _terminal_shell_candidate_path(candidate)
        if path:
            meta["profileResolved"] = True
            meta["profileFallback"] = True
            return path, _terminal_args(definition.get("args")), meta
    if profile and profile != "System Shell":
        meta["profileShellMissing"] = True
    return "", _terminal_args(definition.get("args")), meta


def _terminal_int(value: Any, default: int) -> int:
    try:
        parsed = int(float(value))
    except (TypeError, ValueError):
        return default
    return parsed if parsed > 0 else default


def _build_explicit_shell_command(shell_path: str, shell_args: List[str], command: str) -> List[str]:
    resolved_args = [arg.replace("{command}", command) for arg in shell_args]
    if any("{command}" in arg for arg in shell_args):
        return [shell_path, *resolved_args]

    lower_args = [arg.lower() for arg in resolved_args]
    has_command_switch = any(arg in {"-command", "-c", "/c"} for arg in lower_args)
    if has_command_switch:
        return [shell_path, *resolved_args, command]

    shell_name = os.path.basename(shell_path).lower()
    if shell_name in {"powershell.exe", "powershell", "pwsh.exe", "pwsh"}:
        return [shell_path, *resolved_args, "-Command", command]
    if shell_name in {"cmd.exe", "cmd"}:
        return [shell_path, *resolved_args, "/c", command]
    if shell_name in {"bash.exe", "bash", "sh.exe", "sh", "zsh.exe", "zsh"}:
        return [shell_path, *resolved_args, "-lc", command]
    return [shell_path, *resolved_args, command]


def _terminal_shell_kind(shell_path: str, explicit_shell: bool) -> str:
    if not explicit_shell:
        return "system"
    shell_name = os.path.basename(shell_path).lower()
    if shell_name in {"powershell.exe", "powershell", "pwsh.exe", "pwsh"}:
        return "powershell"
    if shell_name in {"cmd.exe", "cmd"}:
        return "cmd"
    if shell_name in {"bash.exe", "bash", "sh.exe", "sh", "zsh.exe", "zsh"}:
        return "posix"
    return shell_name or "custom"


def _terminal_time_label(value: float) -> str:
    if not value:
        return ""
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(value))


def _terminal_path_in_workspace(path: str, workspace_root: str) -> bool:
    if not path or not workspace_root:
        return False
    try:
        path_norm = os.path.normcase(os.path.realpath(os.path.abspath(path)))
        root_norm = os.path.normcase(os.path.realpath(os.path.abspath(workspace_root)))
        return os.path.commonpath([root_norm, path_norm]) == root_norm
    except (OSError, ValueError):
        return False


def _terminal_metadata(terminal: Dict[str, Any], timeout: int, output_limit: int,
                       explicit_shell: bool, cwd: str = "",
                       workspace_root: str = "") -> Dict[str, Any]:
    shell_path = _terminal_shell_path(terminal.get("shell_path"))
    profile = _terminal_profile_name(terminal.get("profile"))
    metadata = {
        "configured": bool(terminal),
        "timeout": timeout,
        "outputLimit": output_limit,
        "explicitShell": explicit_shell,
        "shellKind": _terminal_shell_kind(shell_path, explicit_shell),
        "profile": str(profile or ("Configured Shell" if explicit_shell else "System Shell")),
        "workspaceCwd": _terminal_path_in_workspace(cwd, workspace_root),
        "cwdSource": (
            "workspace" if cwd and workspace_root
            and os.path.abspath(cwd) == os.path.abspath(workspace_root)
            else ("explicit" if cwd else "none")
        ),
        "profileSource": "configured" if profile else "system",
        "shellIntegrationStatus": "process",
        "encoding": "utf-8",
    }
    for key in ("profileResolved", "profileFallback", "profileShellMissing"):
        if key in terminal:
            metadata[key] = bool(terminal.get(key))
    if cwd:
        metadata["cwd"] = cwd
    if workspace_root:
        metadata["workspaceRoot"] = workspace_root
    if shell_path:
        metadata["shellPath"] = shell_path
    shell_args = _terminal_args(terminal.get("shell_args"))
    if shell_args:
        metadata["shellArgs"] = shell_args
    auto_approve = terminal.get("auto_approve")
    if auto_approve not in (None, {}, [], ""):
        metadata["autoApproveConfigured"] = True
    return metadata


def _default_terminal_cwd(api_ref: Any = None) -> str:
    fn = getattr(api_ref, "_workspace_root", None)
    if callable(fn):
        try:
            root = fn()
            if isinstance(root, str) and os.path.isdir(root):
                return os.path.abspath(root)
        except Exception:
            pass
    return ""


_TERMINAL_JOB_LOCK = threading.Lock()
_TERMINAL_JOBS: Dict[str, Dict[str, Any]] = {}
_TERMINAL_JOB_MAX_OUTPUT = 256_000
_TERMINAL_JOB_TTL_SEC = 300
_TERMINAL_JOB_MAX_HISTORY = 64


class _PtyProcAdapter:
    """Adapts a winpty.PtyProcess to the subset of subprocess.Popen's
    interface the terminal job lifecycle (_terminal_waiter/_terminal_finish_job/
    _stop_terminal_job/_write_terminal_job) already relies on, so PTY-backed
    and pipe-backed jobs share one code path."""

    def __init__(self, pty_proc: Any) -> None:
        self._pty = pty_proc
        self.pid = getattr(pty_proc, "pid", None)
        self.stdin = self
        self.stdout = self
        self.stderr = None

    def poll(self) -> Optional[int]:
        if self._pty.isalive():
            return None
        code = getattr(self._pty, "exitstatus", None)
        return int(code) if code is not None else 0

    def wait(self, timeout: Optional[float] = None) -> int:
        deadline = None if timeout is None else time.monotonic() + timeout
        while self._pty.isalive():
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(cmd="", timeout=timeout or 0)
            time.sleep(0.05)
        code = getattr(self._pty, "exitstatus", None)
        return int(code) if code is not None else 0

    def terminate(self) -> None:
        try:
            self._pty.terminate()
        except Exception:
            pass

    def kill(self) -> None:
        try:
            self._pty.terminate(force=True)
        except Exception:
            pass

    def write(self, text: str) -> None:
        self._pty.write(text)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        pass


def _pty_default_shell() -> Tuple[str, List[str]]:
    if os.name == "nt":
        comspec = os.environ.get("COMSPEC") or shutil.which("cmd.exe") or "cmd.exe"
        return comspec, []
    shell = os.environ.get("SHELL") or shutil.which("bash") or shutil.which("sh") or "/bin/sh"
    return shell, []


def _pty_wrap_command_for_shell(shell_path: str, command: str) -> Tuple[List[str], str]:
    """winpty.PtyProcess.spawn always reconstructs its child's command line
    via subprocess.list2cmdline, which mangles a command that itself
    contains embedded double quotes (e.g. `python -c "..."`) when it's
    passed inline as a `cmd.exe /c <command>` argv element. Route through a
    throwaway script file instead so the shell's argv never has to carry
    quote characters at all."""
    shell_name = os.path.basename(shell_path).lower()
    is_powershell = shell_name in {"powershell.exe", "powershell", "pwsh.exe", "pwsh"}
    is_posix = shell_name in {"bash.exe", "bash", "sh.exe", "sh", "zsh.exe", "zsh"}
    suffix = ".ps1" if is_powershell else (".sh" if is_posix else ".cmd")
    fd, script_path = tempfile.mkstemp(prefix="sao_pty_cmd_", suffix=suffix)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        if suffix == ".cmd":
            fh.write("@echo off\n")
        fh.write(command)
    if is_powershell:
        return [shell_path, "-NoLogo", "-NoProfile", "-File", script_path], script_path
    if is_posix:
        return [shell_path, script_path], script_path
    return [shell_path, "/c", script_path], script_path


def _cleanup_pty_temp_script(job: Dict[str, Any]) -> None:
    script_path = job.pop("ptyTempScript", "") if job else ""
    if script_path:
        try:
            os.unlink(script_path)
        except OSError:
            pass


def _spawn_pty_process(argv: List[str], cwd: str, env: Dict[str, str],
                       cols: int, rows: int) -> Any:
    dimensions = (max(2, int(rows or 24)), max(2, int(cols or 80)))
    return _winpty.PtyProcess.spawn(argv, cwd=cwd or None, env=env, dimensions=dimensions)


def _terminal_pty_reader(job_id: str, pty_proc: Any) -> None:
    try:
        while True:
            try:
                chunk = pty_proc.read(4096)
            except EOFError:
                break
            if not chunk:
                break
            _terminal_append_job_output(job_id, "stdout", chunk)
    except Exception as exc:
        _terminal_append_job_output(job_id, "stdout", f"\n[terminal stream error: {exc}]\n")


def _resize_terminal_job(job_id: str, cols: int, rows: int) -> Dict[str, Any]:
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(str(job_id or ""))
        pty_obj = job.get("pty") if job else None
    if not job:
        return {"error": f"Unknown terminal job: {job_id}", "jobId": job_id}
    if pty_obj is None:
        return {"jobId": job_id, "ptyBacked": False}
    cols = max(2, int(cols or 0) or 80)
    rows = max(2, int(rows or 0) or 24)
    try:
        pty_obj.setwinsize(rows, cols)
    except Exception as exc:
        return {"error": str(exc), "jobId": job_id, "ptyBacked": True}
    return {"jobId": job_id, "cols": cols, "rows": rows, "ptyBacked": True}


def _terminal_command_context(command: str, cwd: str, gui_ref: Any,
                              api_ref: Any, profile_override: str = "") -> Tuple[Dict[str, Any], int, int, bool, str, str, Any]:
    terminal = _get_terminal_settings(gui_ref)
    override_profile = _terminal_profile_name(profile_override)
    if override_profile:
        terminal["profile"] = override_profile
    timeout = _terminal_int(terminal.get("timeout"), _DEFAULT_TERMINAL_TIMEOUT)
    output_limit = _terminal_int(terminal.get("output_limit"), _DEFAULT_STDOUT_LIMIT)
    shell_path = _terminal_shell_path(terminal.get("shell_path"))
    shell_args = _terminal_args(terminal.get("shell_args"))
    if not shell_path:
        profile = _terminal_profile_name(terminal.get("profile"))
        resolved_shell, resolved_args, profile_meta = _resolve_terminal_profile_shell(profile)
        terminal.update({k: v for k, v in profile_meta.items() if k != "profile" or v})
        if resolved_shell:
            shell_path = resolved_shell
            shell_args = resolved_args
            terminal["shell_path"] = resolved_shell
            terminal["shell_args"] = list(resolved_args)
    explicit_shell = bool(shell_path)
    workspace_root = _default_terminal_cwd(api_ref)
    effective_cwd = os.path.abspath(cwd) if cwd else workspace_root
    run_command: Any = command
    if explicit_shell:
        run_command = _build_explicit_shell_command(shell_path, shell_args, command)
    return (
        terminal, timeout, output_limit, explicit_shell, effective_cwd,
        workspace_root, run_command,
    )


def _terminal_failure_result(command: str, cwd: str, gui_ref: Any,
                             api_ref: Any, profile: str,
                             error: Any,
                             *, exit_code: int = 1,
                             state: str = "error",
                             duration_ms: Optional[int] = None,
                             started_wall: Optional[float] = None,
                             finished_wall: Optional[float] = None) -> Dict[str, Any]:
    terminal = _get_terminal_settings(gui_ref)
    override_profile = _terminal_profile_name(profile)
    if override_profile:
        terminal["profile"] = override_profile
    timeout = _terminal_int(
        terminal.get("timeout"), _DEFAULT_TERMINAL_TIMEOUT)
    output_limit = _terminal_int(
        terminal.get("output_limit"), _DEFAULT_STDOUT_LIMIT)
    workspace_root = _default_terminal_cwd(api_ref)
    effective_cwd = os.path.abspath(cwd) if cwd else workspace_root
    shell_path = _terminal_shell_path(terminal.get("shell_path"))
    if not shell_path:
        resolved_shell, resolved_args, profile_meta = _resolve_terminal_profile_shell(
            _terminal_profile_name(terminal.get("profile")))
        terminal.update({k: v for k, v in profile_meta.items()
                         if k != "profile" or v})
        if resolved_shell:
            terminal["shell_path"] = resolved_shell
            terminal["shell_args"] = list(resolved_args)
            shell_path = resolved_shell
    now_wall = time.time()
    terminal_meta = _terminal_metadata(
        terminal, timeout, output_limit, bool(shell_path),
        effective_cwd, workspace_root)
    terminal_meta["shellIntegrationStatus"] = "process-error"
    terminal_meta["processId"] = None
    return {
        "command": command,
        "cwd": effective_cwd,
        "error": str(error),
        "exitCode": exit_code,
        "state": state,
        "durationMs": duration_ms,
        "startedAt": _terminal_time_label(started_wall or 0),
        "finishedAt": _terminal_time_label(finished_wall or now_wall),
        "sessionId": uuid.uuid4().hex,
        "processId": None,
        "cwdSource": (
            "workspace" if effective_cwd and workspace_root
            and os.path.abspath(effective_cwd) == os.path.abspath(workspace_root)
            else ("explicit" if effective_cwd else "none")
        ),
        "profileSource": "configured" if _terminal_profile_name(
            terminal.get("profile")) else "system",
        "shellIntegrationStatus": "process-error",
        "encoding": "utf-8",
        "terminal": terminal_meta,
    }


def _terminal_subprocess_env(terminal: Dict[str, Any]) -> Dict[str, str]:
    env = os.environ.copy()
    env.setdefault("PYTHONIOENCODING", "utf-8")
    env.setdefault("PYTHONUTF8", "1")
    env["SAO_AI_EDITOR_TERMINAL_PROFILE"] = _terminal_profile_name(
        terminal.get("profile")) or "System Shell"
    shell_path = _terminal_shell_path(terminal.get("shell_path"))
    if shell_path:
        env["SAO_AI_EDITOR_TERMINAL_SHELL"] = shell_path
    return env


def _terminal_prune_jobs_locked(now: Optional[float] = None) -> None:
    now = time.time() if now is None else now
    terminal_states = {"done", "error", "cancelled", "timeout"}
    stale_ids = [
        job_id for job_id, job in _TERMINAL_JOBS.items()
        if job.get("state") in terminal_states
        and float(job.get("finishedWall") or job.get("startedWall") or 0)
        and now - float(job.get("finishedWall") or job.get("startedWall") or 0)
        > _TERMINAL_JOB_TTL_SEC
    ]
    for job_id in stale_ids:
        _TERMINAL_JOBS.pop(job_id, None)
    if len(_TERMINAL_JOBS) <= _TERMINAL_JOB_MAX_HISTORY:
        return
    completed = [
        (float(job.get("finishedWall") or job.get("startedWall") or 0), job_id)
        for job_id, job in _TERMINAL_JOBS.items()
        if job.get("state") in terminal_states
    ]
    completed.sort()
    overflow = max(0, len(_TERMINAL_JOBS) - _TERMINAL_JOB_MAX_HISTORY)
    for _, job_id in completed[:overflow]:
        _TERMINAL_JOBS.pop(job_id, None)


def _terminal_trim_job_output(job: Dict[str, Any]) -> None:
    for key in ("stdout", "stderr"):
        value = str(job.get(key) or "")
        if len(value) > _TERMINAL_JOB_MAX_OUTPUT:
            job[key] = value[-_TERMINAL_JOB_MAX_OUTPUT:]
            job[f"{key}Truncated"] = True


def _terminal_append_job_chunk(job: Dict[str, Any], stream_name: str,
                               text: str) -> int:
    seq = int(job.get("sequence") or 0) + 1
    job["sequence"] = seq
    chunks = job.setdefault("chunks", [])
    chunks.append({"seq": seq, "stream": stream_name, "text": text})
    if len(chunks) > 200:
        del chunks[:len(chunks) - 200]
    return seq


def _terminal_append_job_output(job_id: str, stream_name: str, text: str) -> None:
    if not text:
        return
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        if not job:
            return
        _terminal_append_job_chunk(job, stream_name, text)
        job[stream_name] = str(job.get(stream_name) or "") + text
        _terminal_trim_job_output(job)


def _terminal_reader(job_id: str, stream_name: str, pipe: Any) -> None:
    try:
        while True:
            chunk = pipe.readline()
            if not chunk:
                break
            _terminal_append_job_output(job_id, stream_name, chunk)
    except Exception as exc:
        _terminal_append_job_output(job_id, "stderr", f"\n[terminal stream error: {exc}]\n")
    finally:
        try:
            pipe.close()
        except Exception:
            pass


def _terminal_finish_job(job_id: str, exit_code: Optional[int] = None,
                         state: str = "done", error: str = "") -> None:
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        if not job:
            return
        if job.get("state") in {"done", "error", "cancelled", "timeout"}:
            return
        code = exit_code
        proc = job.get("process")
        if code is None and proc is not None:
            try:
                code = proc.poll()
            except Exception:
                code = None
        if code is None:
            code = -1
        job["exitCode"] = code
        job["state"] = state if state != "done" or code == 0 else "error"
        job["durationMs"] = int((time.monotonic() - float(job.get("started") or time.monotonic())) * 1000)
        job["finishedWall"] = time.time()
        if error:
            job["error"] = error
        _terminal_trim_job_output(job)
        _cleanup_pty_temp_script(job)


def _terminal_waiter(job_id: str, timeout: int) -> None:
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        proc = job.get("process") if job else None
    if proc is None:
        return
    try:
        exit_code = proc.wait(timeout=timeout)
        _terminal_finish_job(job_id, exit_code, "done")
    except subprocess.TimeoutExpired:
        try:
            if os.name == "nt" and getattr(proc, "pid", None):
                subprocess.run(
                    ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=3,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                )
            else:
                proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        _terminal_finish_job(job_id, -1, "timeout", f"Command timed out ({timeout}s)")
    except Exception as exc:
        _terminal_finish_job(job_id, -1, "error", str(exc))


def _terminal_job_snapshot(job_id: str, since_seq: int = 0) -> Dict[str, Any]:
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        if not job:
            return {"error": f"Unknown terminal job: {job_id}", "jobId": job_id}
        proc = job.get("process")
        if job.get("state") == "running" and proc is not None:
            try:
                polled = proc.poll()
            except Exception:
                polled = None
            if polled is not None:
                job["exitCode"] = polled
                job["state"] = "done" if polled == 0 else "error"
                job["durationMs"] = int((time.monotonic() - float(job.get("started") or time.monotonic())) * 1000)
                job["finishedWall"] = time.time()
        _terminal_prune_jobs_locked()
        chunks = [
            dict(chunk) for chunk in job.get("chunks", [])
            if int(chunk.get("seq") or 0) > since_seq
        ]
        terminal = dict(job.get("terminal") or {})
        stdout = str(job.get("stdout") or "")
        stderr = str(job.get("stderr") or "")
        state = str(job.get("state") or "running")
        stdin_closed = bool(job.get("stdinClosed"))
        process_running = state == "running" and proc is not None and (
            proc.poll() is None if proc is not None else False)
        snapshot = {
            "jobId": job_id,
            "pid": job.get("pid"),
            "processId": job.get("pid"),
            "sessionId": terminal.get("sessionId", job_id),
            "command": job.get("command", ""),
            "cwd": job.get("cwd", ""),
            "state": state,
            "running": process_running,
            "canWriteStdin": process_running and not stdin_closed,
            "stdinAvailable": process_running and getattr(proc, "stdin", None) is not None,
            "exitCode": job.get("exitCode"),
            "durationMs": job.get("durationMs"),
            "startedAt": _terminal_time_label(float(job.get("startedWall") or 0)),
            "finishedAt": _terminal_time_label(float(job.get("finishedWall") or 0)),
            "stdout": stdout,
            "stderr": stderr,
            "error": job.get("error", ""),
            "sequence": int(job.get("sequence") or 0),
            "chunks": chunks,
            "stdoutTruncated": bool(job.get("stdoutTruncated")),
            "stderrTruncated": bool(job.get("stderrTruncated")),
            "stdoutBytes": len(stdout),
            "stderrBytes": len(stderr),
            "outputBytes": len(stdout) + len(stderr),
            "outputLines": (
                len((stdout + stderr).splitlines())
                if (stdout or stderr) else 0),
            "stdinBytes": int(job.get("stdinBytes") or 0),
            "stdinClosed": stdin_closed,
            "profile": terminal.get("profile", ""),
            "shellKind": terminal.get("shellKind", ""),
            "shellPath": terminal.get("shellPath", ""),
            "shellArgs": terminal.get("shellArgs", []),
            "cwdSource": terminal.get("cwdSource", ""),
            "profileSource": terminal.get("profileSource", ""),
            "shellIntegrationStatus": terminal.get("shellIntegrationStatus", ""),
            "encoding": terminal.get("encoding", "utf-8"),
            "ptyRequested": bool(terminal.get("ptyRequested")),
            "ptyBacked": bool(terminal.get("ptyBacked")),
            "ptyFallbackReason": terminal.get("ptyFallbackReason", ""),
            "workspaceCwd": terminal.get("workspaceCwd"),
            "workspaceRoot": terminal.get("workspaceRoot", ""),
            "jobCount": len(_TERMINAL_JOBS),
            "terminal": terminal,
        }
    return snapshot


def _start_terminal_job(command: str, cwd: str, gui_ref: Any,
                        api_ref: Any, profile: str = "",
                        use_pty: bool = False, cols: int = 0,
                        rows: int = 0) -> Dict[str, Any]:
    try:
        terminal, timeout, output_limit, explicit_shell, effective_cwd, workspace_root, run_command = (
            _terminal_command_context(command, cwd, gui_ref, api_ref, profile))
    except Exception as exc:
        return _terminal_failure_result(
            command, cwd, gui_ref, api_ref, profile, exc)

    want_pty = bool(use_pty)
    use_real_pty = want_pty and _PTY_AVAILABLE
    pty_temp_script = ""
    if want_pty and not explicit_shell:
        shell_path, shell_args = _pty_default_shell()
        if use_real_pty:
            run_command, pty_temp_script = _pty_wrap_command_for_shell(shell_path, command)
        else:
            run_command = _build_explicit_shell_command(shell_path, shell_args, command)
        explicit_shell = True

    job_id = uuid.uuid4().hex
    env = _terminal_subprocess_env(terminal)
    pty_obj: Any = None
    if use_real_pty:
        argv = run_command if isinstance(run_command, list) else [run_command]
        try:
            pty_obj = _spawn_pty_process(argv, effective_cwd, env, cols, rows)
        except Exception as exc:
            if pty_temp_script:
                try:
                    os.unlink(pty_temp_script)
                except OSError:
                    pass
            return _terminal_failure_result(
                command, effective_cwd, gui_ref, api_ref, profile, exc,
                started_wall=time.time())
        proc: Any = _PtyProcAdapter(pty_obj)
    else:
        kwargs: Dict[str, Any] = {
            "shell": not explicit_shell,
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
            "stdin": subprocess.PIPE,
            "text": True,
            "encoding": "utf-8",
            "errors": "replace",
            "bufsize": 1,
            "env": env,
        }
        if effective_cwd:
            kwargs["cwd"] = effective_cwd
        if os.name == "nt":
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        try:
            proc = subprocess.Popen(run_command, **kwargs)
        except Exception as exc:
            return _terminal_failure_result(
                command, effective_cwd, gui_ref, api_ref, profile, exc,
                started_wall=time.time())
    terminal_meta = _terminal_metadata(
        terminal, timeout, output_limit, explicit_shell, effective_cwd,
        workspace_root)
    terminal_meta["processId"] = proc.pid
    terminal_meta["sessionId"] = job_id
    terminal_meta["ptyRequested"] = want_pty
    terminal_meta["ptyBacked"] = use_real_pty
    if want_pty and not use_real_pty:
        terminal_meta["ptyFallbackReason"] = (
            "pywinpty not installed" if not _PTY_AVAILABLE else "unsupported on this platform")
    if use_real_pty:
        terminal_meta["shellIntegrationStatus"] = "pty"
    job = {
        "jobId": job_id,
        "process": proc,
        "pty": pty_obj,
        "ptyTempScript": pty_temp_script,
        "pid": proc.pid,
        "command": command,
        "cwd": effective_cwd,
        "state": "running",
        "exitCode": None,
        "durationMs": None,
        "stdout": "",
        "stderr": "",
        "error": "",
        "sequence": 0,
        "chunks": [],
        "stdinBytes": 0,
        "stdinClosed": False,
        "started": time.monotonic(),
        "startedWall": time.time(),
        "finishedWall": 0,
        "terminal": terminal_meta,
    }
    with _TERMINAL_JOB_LOCK:
        _terminal_prune_jobs_locked()
        _TERMINAL_JOBS[job_id] = job
    if use_real_pty:
        threading.Thread(target=_terminal_pty_reader, args=(job_id, pty_obj),
                         daemon=True).start()
    else:
        threading.Thread(target=_terminal_reader, args=(job_id, "stdout", proc.stdout),
                         daemon=True).start()
        threading.Thread(target=_terminal_reader, args=(job_id, "stderr", proc.stderr),
                         daemon=True).start()
    threading.Thread(target=_terminal_waiter, args=(job_id, timeout),
                     daemon=True).start()
    return _terminal_job_snapshot(job_id)


def _write_terminal_job(job_id: str, data: str = "", close_stdin: bool = False) -> Dict[str, Any]:
    text = str(data or "")
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        if not job:
            return {"error": f"Unknown terminal job: {job_id}", "jobId": job_id}
        if job.get("state") != "running":
            return {
                "error": f"Terminal job is not running: {job_id}",
                "jobId": job_id,
                "state": job.get("state"),
            }
        if job.get("stdinClosed"):
            return {"error": f"Terminal job stdin is closed: {job_id}", "jobId": job_id}
        proc = job.get("process")
        stdin = getattr(proc, "stdin", None) if proc is not None else None
    if stdin is None:
        return {"error": f"Terminal job stdin is unavailable: {job_id}", "jobId": job_id}

    written = 0
    try:
        if text:
            stdin.write(text)
            stdin.flush()
            written = len(text)
        if close_stdin:
            stdin.close()
    except Exception as exc:
        with _TERMINAL_JOB_LOCK:
            job = _TERMINAL_JOBS.get(job_id)
            if job is not None:
                job["stdinClosed"] = True
        return {"error": str(exc), "jobId": job_id, "writtenBytes": written}

    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        if job is not None:
            job["stdinBytes"] = int(job.get("stdinBytes") or 0) + written
            if text:
                _terminal_append_job_chunk(job, "stdin", text)
            if close_stdin:
                job["stdinClosed"] = True
                if not text:
                    _terminal_append_job_chunk(job, "stdin", "[stdin closed]\n")
    snapshot = _terminal_job_snapshot(job_id)
    snapshot["writtenBytes"] = written
    return snapshot


def _stop_terminal_job(job_id: str) -> Dict[str, Any]:
    with _TERMINAL_JOB_LOCK:
        job = _TERMINAL_JOBS.get(job_id)
        proc = job.get("process") if job else None
        if not job:
            return {"error": f"Unknown terminal job: {job_id}", "jobId": job_id}
        job["state"] = "cancelled"
        job["durationMs"] = int((time.monotonic() - float(job.get("started") or time.monotonic())) * 1000)
        job["exitCode"] = None
        job["finishedWall"] = time.time()
    if proc is not None:
        try:
            if os.name == "nt" and getattr(proc, "pid", None):
                subprocess.run(
                    ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=3,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                )
            else:
                proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    if job is not None:
        _cleanup_pty_temp_script(job)
    return _terminal_job_snapshot(job_id)


def _run_terminal(command: str, cwd: str = "", gui_ref: Any = None,
                  api_ref: Any = None, mode: str = "run",
                  jobId: str = "", sinceSeq: int = 0,
                  profile: str = "", data: str = "",
                  closeStdin: bool = False, pty: bool = False,
                  cols: int = 0, rows: int = 0) -> Dict[str, Any]:
    mode = str(mode or "run").strip().lower()
    if mode == "start":
        return _start_terminal_job(command, cwd, gui_ref, api_ref, profile,
                                   bool(pty), int(cols or 0), int(rows or 0))
    if mode == "status":
        return _terminal_job_snapshot(str(jobId or ""), sinceSeq)
    if mode == "write":
        return _write_terminal_job(str(jobId or ""), data, closeStdin)
    if mode == "stop":
        return _stop_terminal_job(str(jobId or ""))
    if mode == "resize":
        return _resize_terminal_job(str(jobId or ""), int(cols or 0), int(rows or 0))
    try:
        terminal, timeout, output_limit, explicit_shell, effective_cwd, workspace_root, run_command = (
            _terminal_command_context(command, cwd, gui_ref, api_ref, profile))

        kwargs: Dict[str, Any] = {
            "shell": not explicit_shell,
            "capture_output": True,
            "text": True,
            "encoding": "utf-8",
            "errors": "replace",
            "timeout": timeout,
            "env": _terminal_subprocess_env(terminal),
        }
        if effective_cwd:
            kwargs["cwd"] = effective_cwd
        session_id = uuid.uuid4().hex
        started = time.monotonic()
        started_wall = time.time()
        result = subprocess.run(run_command, **kwargs)
        finished_wall = time.time()
        duration_ms = int((time.monotonic() - started) * 1000)
        stdout = result.stdout or ""
        stderr = result.stderr or ""
        stderr_limit = output_limit if terminal else _DEFAULT_STDERR_LIMIT
        return {
            "command": command,
            "cwd": effective_cwd,
            "sessionId": session_id,
            "processId": None,
            "exitCode": result.returncode,
            "durationMs": duration_ms,
            "startedAt": _terminal_time_label(started_wall),
            "finishedAt": _terminal_time_label(finished_wall),
            "stdout": stdout[:output_limit],
            "stderr": stderr[:stderr_limit],
            "stdoutTruncated": len(stdout) > output_limit,
            "stderrTruncated": len(stderr) > stderr_limit,
            "cwdSource": (
                "workspace" if effective_cwd and workspace_root
                and os.path.abspath(effective_cwd) == os.path.abspath(workspace_root)
                else ("explicit" if effective_cwd else "none")
            ),
            "profileSource": "configured" if _terminal_profile_name(
                terminal.get("profile")) else "system",
            "shellIntegrationStatus": "process",
            "encoding": "utf-8",
            "terminal": _terminal_metadata(
                terminal, timeout, output_limit, explicit_shell, effective_cwd,
                workspace_root),
        }
    except subprocess.TimeoutExpired:
        terminal = _get_terminal_settings(gui_ref)
        override_profile = _terminal_profile_name(profile)
        if override_profile:
            terminal["profile"] = override_profile
        timeout = _terminal_int(terminal.get("timeout"), _DEFAULT_TERMINAL_TIMEOUT)
        output_limit = _terminal_int(terminal.get("output_limit"), _DEFAULT_STDOUT_LIMIT)
        workspace_root = _default_terminal_cwd(api_ref)
        effective_cwd = os.path.abspath(cwd) if cwd else workspace_root
        shell_path = _terminal_shell_path(terminal.get("shell_path"))
        if not shell_path:
            resolved_shell, resolved_args, profile_meta = _resolve_terminal_profile_shell(
                _terminal_profile_name(terminal.get("profile")))
            terminal.update({k: v for k, v in profile_meta.items() if k != "profile" or v})
            if resolved_shell:
                terminal["shell_path"] = resolved_shell
                terminal["shell_args"] = list(resolved_args)
                shell_path = resolved_shell
        return {
            "command": command,
            "cwd": effective_cwd,
            "error": f"Command timed out ({timeout}s)",
            "exitCode": -1,
            "durationMs": timeout * 1000,
            "startedAt": "",
            "finishedAt": _terminal_time_label(time.time()),
            "sessionId": uuid.uuid4().hex,
            "processId": None,
            "cwdSource": (
                "workspace" if effective_cwd and workspace_root
                and os.path.abspath(effective_cwd) == os.path.abspath(workspace_root)
                else ("explicit" if effective_cwd else "none")
            ),
            "profileSource": "configured" if _terminal_profile_name(
                terminal.get("profile")) else "system",
            "shellIntegrationStatus": "process",
            "encoding": "utf-8",
            "terminal": _terminal_metadata(terminal, timeout, output_limit,
                                           bool(shell_path),
                                           effective_cwd,
                                           workspace_root),
        }
    except Exception as exc:
        return _terminal_failure_result(
            command, cwd, gui_ref, api_ref, profile, exc)


# ======================================================================
# Web Fetch handler
# ======================================================================

def _web_fetch(url: str, method: str = "GET",
               headers: Dict[str, str] = None,
               body: str = "") -> Dict[str, Any]:
    import urllib.request
    import urllib.error
    try:
        data = body.encode("utf-8") if body else None
        req = urllib.request.Request(url, data=data, method=method.upper())
        for k, v in (headers or {}).items():
            req.add_header(str(k), str(v))
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read()
            try:
                body_text = raw.decode("utf-8", errors="replace")
            except Exception:
                body_text = raw.decode("latin-1")
            resp_headers = {k: v for k, v in resp.getheaders()}
            return {
                "status": resp.status,
                "headers": resp_headers,
                "body": body_text[:5000],
                "truncated": len(body_text) > 5000,
            }
    except urllib.error.HTTPError as exc:
        try:
            err_body = exc.read().decode("utf-8", errors="replace")[:2000]
        except Exception:
            err_body = ""
        return {"status": exc.code, "error": str(exc.reason), "body": err_body}
    except Exception as exc:
        return {"error": str(exc)}


# ======================================================================
# Todo List handler
# ======================================================================

def _manage_todo(gui_ref: Any, action: str, text: str = "",
                 index: int = -1) -> Dict[str, Any]:
    if gui_ref is None:
        return {"error": "No GUI context available"}
    if not hasattr(gui_ref, '_todo_list'):
        gui_ref._todo_list = []
    todo = gui_ref._todo_list
    if action == "add":
        if not text:
            return {"error": "text is required for add"}
        todo.append(text)
        return {"ok": True, "index": len(todo) - 1, "count": len(todo)}
    elif action == "remove":
        if index < 0 or index >= len(todo):
            return {"error": f"Invalid index {index}, list has {len(todo)} items"}
        removed = todo.pop(index)
        return {
            "ok": True,
            "removed": removed,
            "count": len(todo),
            "items": [{"index": i, "text": t} for i, t in enumerate(todo)],
        }
    elif action == "list":
        return {"items": [{"index": i, "text": t} for i, t in enumerate(todo)],
                "count": len(todo)}
    elif action == "update":
        if index < 0 or index >= len(todo):
            return {"error": f"Invalid index {index}, list has {len(todo)} items"}
        if not text:
            return {"error": "text is required for update"}
        old = todo[index]
        todo[index] = text
        return {"ok": True, "old": old, "new": text, "index": index}
    else:
        return {"error": f"Unknown action: {action}. Use add/remove/list/update"}


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
        "list_processes": lambda: _list_processes(),
        "select_process": lambda: _select_process(gui_ref, kw.get("name", ""), kw.get("pid", 0)),
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
        if pb:
            bridge = getattr(pb, '_unified_data_source', None)
    if not bridge:
        return {"connected": False}
    fn = getattr(bridge, 'status', None)
    if callable(fn):
        try:
            status = fn()
            if isinstance(status, dict):
                return status
            return {"connected": True, "status": status, "type": type(bridge).__name__}
        except Exception as exc:
            return {
                "connected": True,
                "type": type(bridge).__name__,
                "status_error": str(exc),
            }
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


def _settings_container(settings: Any) -> Optional[Dict[str, Any]]:
    for attr in ("_data", "data"):
        value = getattr(settings, attr, None)
        if isinstance(value, dict):
            return value
    return None


def _settings_lookup(settings: Any, key: str) -> tuple[bool, Any]:
    container = _settings_container(settings)
    if not container or "." not in key:
        return False, None
    current: Any = container
    for part in [segment for segment in key.split(".") if segment]:
        if not isinstance(current, dict) or part not in current:
            return False, None
        current = current.get(part)
    return True, current


def _settings_assign(settings: Any, key: str, value: Any) -> bool:
    container = _settings_container(settings)
    if not container or "." not in key:
        return False
    parts = [segment for segment in key.split(".") if segment]
    if not parts:
        return False
    current = container
    for part in parts[:-1]:
        next_value = current.get(part)
        if not isinstance(next_value, dict):
            next_value = {}
            current[part] = next_value
        current = next_value
    current[parts[-1]] = value
    return True

def _settings_get(g: Any, key: str) -> Any:
    s = getattr(g, 'settings', None)
    if not s:
        return {"error": "Settings not available"}
    if not key:
        return {"error": "key is required"}
    try:
        found, value = _settings_lookup(s, key)
        if found:
            return {"key": key, "value": value, "found": True}
        return {"key": key, "value": s.get(key), "found": False}
    except Exception as exc:
        return {"error": str(exc)}

def _settings_set(g: Any, key: str, value: Any) -> Dict:
    s = getattr(g, 'settings', None)
    if not s:
        return {"error": "Settings not available"}
    if not key:
        return {"error": "key is required"}
    found, previous = False, None
    try:
        found, previous = _settings_lookup(s, key)
        if not _settings_assign(s, key, value):
            previous = s.get(key)
            found = previous is not None
            s.set(key, value)
    except Exception as exc:
        return {"error": f"Settings update failed: {exc}"}
    saved = False
    save_fn = getattr(s, "save", None)
    if callable(save_fn):
        try:
            save_fn()
            saved = True
        except Exception as exc:
            return {"error": f"Settings save failed: {exc}", "key": key}
    return {
        "ok": True,
        "key": key,
        "saved": saved,
        "previous": previous,
        "found": found,
        "updatedNested": "." in key,
    }

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


def _list_processes() -> Dict:
    try:
        from gui_modules.sao_gui_process_selector import list_processes
        procs = list_processes()
        return {"ok": True, "count": len(procs),
                "processes": [{"name": p["name"], "pid": p["pid"]} for p in procs[:200]]}
    except Exception as exc:
        return {"error": str(exc)}


def _select_process(g: Any, name: str = "", pid: int = 0) -> Dict:
    if not name and not pid:
        return {"error": "Provide name or pid"}
    try:
        import config
        from mem_probe.process import set_game_process_names
        if pid and not name:
            from mem_probe.process import _iter_process_entries_wide
            for exe, p in _iter_process_entries_wide():
                if p == int(pid):
                    name = os.path.basename(exe)
                    break
        if not name:
            return {"error": f"Process pid={pid} not found"}
        config.GAME_PROCESS_NAMES = [name]
        set_game_process_names([name])
        s = getattr(g, 'settings', None)
        if s:
            s.set("attached_process_name", name)
            s.set("attached_process_pid", int(pid or 0))
            s.save()
        return {"ok": True, "attached": name, "pid": int(pid or 0)}
    except Exception as exc:
        return {"error": str(exc)}
