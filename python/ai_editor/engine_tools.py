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
import subprocess
import time
from typing import Any, Dict, List, Optional

from ai_editor.tool_registry import ToolRegistry


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
            },
            "required": ["command"],
        },
        handler=lambda command, cwd="": _run_terminal(command, cwd, gui_ref),
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
    # SDK Dumper
    # ==================================================================

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


def _terminal_metadata(terminal: Dict[str, Any], timeout: int, output_limit: int,
                       explicit_shell: bool) -> Dict[str, Any]:
    metadata = {
        "configured": bool(terminal),
        "timeout": timeout,
        "outputLimit": output_limit,
        "explicitShell": explicit_shell,
    }
    profile = terminal.get("profile")
    if profile:
        metadata["profile"] = str(profile)
    shell_path = _terminal_shell_path(terminal.get("shell_path"))
    if shell_path:
        metadata["shellPath"] = shell_path
    auto_approve = terminal.get("auto_approve")
    if auto_approve not in (None, {}, [], ""):
        metadata["autoApproveConfigured"] = True
    return metadata


def _run_terminal(command: str, cwd: str = "", gui_ref: Any = None) -> Dict[str, Any]:
    try:
        terminal = _get_terminal_settings(gui_ref)
        timeout = _terminal_int(terminal.get("timeout"), _DEFAULT_TERMINAL_TIMEOUT)
        output_limit = _terminal_int(terminal.get("output_limit"), _DEFAULT_STDOUT_LIMIT)
        shell_path = _terminal_shell_path(terminal.get("shell_path"))
        shell_args = _terminal_args(terminal.get("shell_args"))
        explicit_shell = bool(shell_path)

        kwargs: Dict[str, Any] = {
            "shell": not explicit_shell,
            "capture_output": True,
            "text": True,
            "timeout": timeout,
        }
        if cwd:
            kwargs["cwd"] = os.path.abspath(cwd)
        run_command: Any = command
        if explicit_shell:
            run_command = _build_explicit_shell_command(shell_path, shell_args, command)
        result = subprocess.run(run_command, **kwargs)
        return {
            "exitCode": result.returncode,
            "stdout": result.stdout[:output_limit] if result.stdout else "",
            "stderr": result.stderr[:output_limit if terminal else _DEFAULT_STDERR_LIMIT] if result.stderr else "",
            "terminal": _terminal_metadata(terminal, timeout, output_limit, explicit_shell),
        }
    except subprocess.TimeoutExpired:
        terminal = _get_terminal_settings(gui_ref)
        timeout = _terminal_int(terminal.get("timeout"), _DEFAULT_TERMINAL_TIMEOUT)
        output_limit = _terminal_int(terminal.get("output_limit"), _DEFAULT_STDOUT_LIMIT)
        return {
            "error": f"Command timed out ({timeout}s)",
            "exitCode": -1,
            "terminal": _terminal_metadata(terminal, timeout, output_limit,
                                           bool(_terminal_shell_path(terminal.get("shell_path")))),
        }
    except Exception as exc:
        return {"error": str(exc)}


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
