"""CLI controller for provider tabs.

Two modes:
  CliChatController — pipes messages through the CLI in our chat panel
                      (claude -p / codex --quiet / gh copilot explain)
  CliLauncher       — opens CLI in its own terminal window (fallback)
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional


@dataclass
class CliMessage:
    role: str = "assistant"
    content: str = ""
    model: str = ""
    thinking: str = ""
    is_error: bool = False
    usage: Optional[Dict[str, Any]] = None


class CliChatController:
    """Pipes messages through a CLI tool, streaming output to our chat panel.

    Same callback interface as ChatController so app.py can use either.
    Each send() spawns the CLI in pipe mode for one exchange.
    """

    def __init__(self, cli_path: str, provider_id: str,
                 cli_args: Optional[List[str]] = None,
                 cwd: Optional[str] = None) -> None:
        self._cli_path = cli_path
        self._provider_id = provider_id
        self._cli_args = list(cli_args or [])
        self._cwd = cwd
        self._proc: Optional[subprocess.Popen] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None

        self.on_stream_delta: Optional[Callable] = None
        self.on_thinking_delta: Optional[Callable] = None
        self.on_stream_end: Optional[Callable] = None
        self.on_tool_start: Optional[Callable] = None
        self.on_tool_end: Optional[Callable] = None
        self.on_tool_confirm: Optional[Callable] = None
        self.on_tool_progress: Optional[Callable] = None
        self.on_token_warning: Optional[Callable] = None
        self.on_error: Optional[Callable] = None
        self.on_idle: Optional[Callable] = None
        self.resolve_variable: Optional[Callable] = None

    @property
    def is_running(self) -> bool:
        return self._running

    def send(self, text: str, agent_mode: bool = False) -> None:
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(
            target=self._run, args=(text,), daemon=True)
        self._thread.start()

    def cancel(self) -> None:
        self._running = False
        proc = self._proc
        if proc and proc.poll() is None:
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
        self._proc = None

    def new_conversation(self, system_prompt: str = "") -> Any:
        self.cancel()
        return None

    def _run(self, text: str) -> None:
        try:
            cmd = self._build_command(text)
            env = self._build_env()
            self._proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                stdin=subprocess.DEVNULL, cwd=self._cwd, env=env,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            content_parts: List[str] = []
            thinking_parts: List[str] = []

            if self._provider_id == "claude-code":
                self._stream_claude(content_parts, thinking_parts)
            else:
                self._stream_plain(content_parts)

            self._proc.wait(timeout=10)
            stderr_out = ""
            if self._proc.stderr:
                try:
                    stderr_out = self._proc.stderr.read().decode(
                        "utf-8", errors="replace").strip()
                except Exception:
                    pass

            full_content = "".join(content_parts)
            is_error = False
            if self._proc.returncode != 0 and not full_content:
                full_content = stderr_out or f"CLI exited with code {self._proc.returncode}"
                is_error = True

            msg = CliMessage(
                content=full_content, model=self._provider_id,
                thinking="".join(thinking_parts), is_error=is_error,
            )
            if self.on_stream_end:
                self.on_stream_end(msg)
        except Exception as exc:
            if self.on_error:
                self.on_error(str(exc))
        finally:
            self._running = False
            self._proc = None
            if self.on_idle:
                self.on_idle()

    def _stream_claude(self, content_parts, thinking_parts):
        assert self._proc and self._proc.stdout
        for raw in self._proc.stdout:
            if not self._running:
                break
            line = raw.decode("utf-8", errors="replace").rstrip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                if self.on_stream_delta:
                    self.on_stream_delta(None, line + "\n")
                content_parts.append(line + "\n")
                continue
            t = obj.get("type", "")
            if t == "assistant":
                text = obj.get("message", "")
                if text:
                    if self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)
            elif t == "content_block_delta":
                d = obj.get("delta", {})
                if d.get("type") == "text_delta":
                    text = d.get("text", "")
                    if text and self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)
                elif d.get("type") == "thinking_delta":
                    text = d.get("thinking", "")
                    if text and self.on_thinking_delta:
                        self.on_thinking_delta(None, text)
                    thinking_parts.append(text)
            elif t == "result":
                text = obj.get("result", "")
                if text:
                    if self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)
            elif t == "tool_use":
                name = obj.get("name", obj.get("tool", ""))
                inp = obj.get("input", obj.get("arguments", {}))
                cid = obj.get("id", f"cli_{time.monotonic()}")
                if self.on_tool_start:
                    self.on_tool_start(cid, name,
                                      json.dumps(inp, ensure_ascii=False))
                if self.on_tool_end:
                    self.on_tool_end(cid, name, "", "complete")
            else:
                text = obj.get("text", obj.get("content", obj.get("message", "")))
                if text:
                    if self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)

    def _stream_plain(self, content_parts):
        assert self._proc and self._proc.stdout
        for raw in self._proc.stdout:
            if not self._running:
                break
            line = raw.decode("utf-8", errors="replace")
            if self.on_stream_delta:
                self.on_stream_delta(None, line)
            content_parts.append(line)

    def _build_command(self, text: str) -> List[str]:
        if self._provider_id == "claude-code":
            return [self._cli_path, "-p", "--output-format", "stream-json",
                    *self._cli_args, text]
        if self._provider_id == "codex":
            return [self._cli_path, "--quiet", *self._cli_args, text]
        if self._provider_id == "copilot":
            base = os.path.basename(self._cli_path).lower().replace(".exe", "")
            if base in ("code", "code-insiders"):
                return [self._cli_path, "--command",
                        "workbench.action.chat.open", *self._cli_args]
            return [self._cli_path, "copilot", "explain",
                    *self._cli_args, text]
        return [self._cli_path, *self._cli_args, text]

    def _build_env(self) -> Dict[str, str]:
        env = dict(os.environ)
        env["FORCE_COLOR"] = "0"
        env["NO_COLOR"] = "1"
        if self._provider_id == "claude-code":
            env["CLAUDE_CODE_ENTRYPOINT"] = "sao-ai-editor"
        return env


class CliLauncher:
    """Opens a CLI tool in its own terminal window (external)."""

    def __init__(self, cli_path: str, provider_id: str,
                 cli_args: Optional[List[str]] = None,
                 cwd: Optional[str] = None) -> None:
        self._cli_path = cli_path
        self._provider_id = provider_id
        self._cli_args = list(cli_args or [])
        self._cwd = cwd
        self._proc: Optional[subprocess.Popen] = None
        self._lock = threading.Lock()

    @property
    def is_running(self) -> bool:
        with self._lock:
            if self._proc is None:
                return False
            if self._proc.poll() is not None:
                self._proc = None
                return False
            return True

    @property
    def pid(self) -> Optional[int]:
        with self._lock:
            return self._proc.pid if self._proc and self._proc.poll() is None else None

    def launch(self) -> Dict[str, Any]:
        with self._lock:
            if self._proc and self._proc.poll() is None:
                return {"ok": True, "already_running": True, "pid": self._proc.pid}
            cmd = self._build_command()
            try:
                flags = subprocess.CREATE_NEW_CONSOLE if os.name == "nt" else 0
                self._proc = subprocess.Popen(
                    cmd, cwd=self._cwd, env=self._build_env(),
                    creationflags=flags)
                return {"ok": True, "pid": self._proc.pid}
            except Exception as exc:
                self._proc = None
                return {"error": str(exc)}

    def stop(self) -> Dict[str, Any]:
        with self._lock:
            if self._proc is None or self._proc.poll() is not None:
                self._proc = None
                return {"ok": True, "was_running": False}
            try:
                self._proc.terminate()
                self._proc.wait(timeout=5)
            except Exception:
                try:
                    self._proc.kill()
                except Exception:
                    pass
            self._proc = None
            return {"ok": True, "was_running": True}

    def status(self) -> Dict[str, Any]:
        return {"running": self.is_running, "pid": self.pid,
                "provider": self._provider_id}

    def _build_command(self) -> List[str]:
        if self._provider_id == "copilot":
            base = os.path.basename(self._cli_path).lower().replace(".exe", "")
            if base in ("code", "code-insiders"):
                return [self._cli_path, "--command",
                        "workbench.action.chat.open", *self._cli_args]
            return [self._cli_path, "copilot", *self._cli_args]
        return [self._cli_path, *self._cli_args]

    def _build_env(self) -> Dict[str, str]:
        env = dict(os.environ)
        if self._provider_id == "claude-code":
            env["CLAUDE_CODE_ENTRYPOINT"] = "sao-ai-editor"
        return env


def find_cli(provider_id: str,
             settings_getter: Optional[Callable] = None) -> Optional[str]:
    """Find the CLI executable for a provider."""
    from ai_editor.chat_providers import (
        _get_provider_cli_path, _cli_available,
    )
    section_names = {
        "claude-code": "claude_code",
        "codex": "codex",
        "copilot": "copilot",
    }
    cli_names = {
        "claude-code": "claude",
        "codex": "codex",
    }

    section = section_names.get(provider_id, provider_id)
    configured = _get_provider_cli_path(section, settings_getter)
    if configured and _cli_available(configured):
        return configured

    if provider_id == "copilot":
        for cmd in ("code-insiders", "code", "gh"):
            if _cli_available(cmd):
                return shutil.which(cmd) or cmd
        return None

    default_name = cli_names.get(provider_id, "")
    if default_name and _cli_available(default_name):
        return shutil.which(default_name) or default_name
    return None


def get_cli_args(provider_id: str,
                 settings_getter: Optional[Callable] = None) -> List[str]:
    """Read extra CLI args from settings."""
    if not settings_getter:
        return []
    ai = settings_getter("ai_editor", {}) or {}
    if not isinstance(ai, dict):
        return []
    section_names = {
        "claude-code": "claude_code",
        "codex": "codex",
        "copilot": "copilot",
    }
    section = ai.get(section_names.get(provider_id, provider_id), {})
    if not isinstance(section, dict):
        return []
    raw = section.get("cli_args", section.get("args", []))
    if isinstance(raw, str):
        return [p.strip() for p in raw.splitlines() if p.strip()]
    if isinstance(raw, list):
        return [str(p).strip() for p in raw if str(p).strip()]
    return []
