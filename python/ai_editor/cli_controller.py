"""CLI subprocess controller for provider tabs.

Wraps external CLI tools (claude, codex, gh copilot) as chat providers.
Instead of using our LLMEngine, spawns the real CLI process and streams output.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import threading
import time
from dataclasses import dataclass, field
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
    """Drives a CLI subprocess (claude/codex/gh copilot) as a chat provider.

    Provides the same callback interface as ChatController so
    ``_create_provider_controller`` can return either transparently.
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
        self._history: List[Dict[str, str]] = []

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
        self._history.append({"role": "user", "content": text})
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
        self._history.clear()
        return None

    def _run(self, text: str) -> None:
        try:
            cmd = self._build_command(text)
            env = self._build_env()

            self._proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                stdin=subprocess.DEVNULL,
                cwd=self._cwd,
                env=env,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )

            content_parts: List[str] = []
            thinking_parts: List[str] = []
            model_name = self._provider_id

            if self._provider_id == "claude-code":
                self._stream_claude(content_parts, thinking_parts)
                model_name = "claude"
            elif self._provider_id == "codex":
                self._stream_plain(content_parts)
                model_name = "codex"
            elif self._provider_id == "copilot":
                self._stream_plain(content_parts)
                model_name = "copilot"
            else:
                self._stream_plain(content_parts)

            self._proc.wait(timeout=5)
            stderr_out = ""
            if self._proc.stderr:
                try:
                    stderr_out = self._proc.stderr.read().decode("utf-8", errors="replace").strip()
                except Exception:
                    pass

            full_content = "".join(content_parts)
            full_thinking = "".join(thinking_parts)

            if self._proc.returncode != 0 and not full_content:
                full_content = stderr_out or f"CLI exited with code {self._proc.returncode}"
                is_error = True
            else:
                is_error = False

            self._history.append({"role": "assistant", "content": full_content})

            msg = CliMessage(
                content=full_content,
                model=model_name,
                thinking=full_thinking,
                is_error=is_error,
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

    def _stream_claude(self, content_parts: List[str],
                       thinking_parts: List[str]) -> None:
        """Stream claude CLI output in stream-json format."""
        assert self._proc and self._proc.stdout
        for raw_line in self._proc.stdout:
            if not self._running:
                break
            line = raw_line.decode("utf-8", errors="replace").rstrip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                if self.on_stream_delta:
                    self.on_stream_delta(None, line + "\n")
                content_parts.append(line + "\n")
                continue

            msg_type = obj.get("type", "")

            if msg_type == "assistant" and "message" in obj:
                text = obj["message"]
                if self.on_stream_delta:
                    self.on_stream_delta(None, text)
                content_parts.append(text)

            elif msg_type == "content_block_delta":
                delta = obj.get("delta", {})
                delta_type = delta.get("type", "")
                if delta_type == "text_delta":
                    text = delta.get("text", "")
                    if text and self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)
                elif delta_type == "thinking_delta":
                    text = delta.get("thinking", "")
                    if text and self.on_thinking_delta:
                        self.on_thinking_delta(None, text)
                    thinking_parts.append(text)

            elif msg_type == "result":
                text = obj.get("result", "")
                if text:
                    if self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)

            elif msg_type == "tool_use":
                tool_name = obj.get("name", obj.get("tool", ""))
                tool_input = obj.get("input", obj.get("arguments", {}))
                call_id = obj.get("id", f"cli_{time.monotonic()}")
                if self.on_tool_start:
                    self.on_tool_start(call_id, tool_name,
                                      json.dumps(tool_input, ensure_ascii=False))
                if self.on_tool_end:
                    self.on_tool_end(call_id, tool_name, "", "complete")

            elif msg_type in ("text", ""):
                text = obj.get("text", obj.get("content", ""))
                if text:
                    if self.on_stream_delta:
                        self.on_stream_delta(None, text)
                    content_parts.append(text)

    def _stream_plain(self, content_parts: List[str]) -> None:
        """Stream plain text output line by line."""
        assert self._proc and self._proc.stdout
        for raw_line in self._proc.stdout:
            if not self._running:
                break
            line = raw_line.decode("utf-8", errors="replace")
            if self.on_stream_delta:
                self.on_stream_delta(None, line)
            content_parts.append(line)

    def _build_command(self, text: str) -> List[str]:
        if self._provider_id == "claude-code":
            cmd = [self._cli_path, "-p", "--output-format", "stream-json"]
            cmd.extend(self._cli_args)
            cmd.append(text)
            return cmd

        if self._provider_id == "codex":
            cmd = [self._cli_path, "--quiet"]
            cmd.extend(self._cli_args)
            cmd.append(text)
            return cmd

        if self._provider_id == "copilot":
            cmd = [self._cli_path, "copilot", "explain"]
            cmd.extend(self._cli_args)
            cmd.append(text)
            return cmd

        cmd = [self._cli_path]
        cmd.extend(self._cli_args)
        cmd.append(text)
        return cmd

    def _build_env(self) -> Dict[str, str]:
        env = dict(os.environ)
        env["FORCE_COLOR"] = "0"
        env["NO_COLOR"] = "1"
        if self._provider_id == "claude-code":
            env["CLAUDE_CODE_ENTRYPOINT"] = "sao-ai-editor"
        return env


def find_cli(provider_id: str, settings_getter: Optional[Callable] = None) -> Optional[str]:
    """Find the CLI executable for a provider. Returns path or None."""
    from ai_editor.chat_providers import (
        _get_provider_cli_path, _cli_available,
    )

    cli_names = {
        "claude-code": "claude",
        "codex": "codex",
        "copilot": "gh",
    }
    section_names = {
        "claude-code": "claude_code",
        "codex": "codex",
        "copilot": "copilot",
    }

    section = section_names.get(provider_id, provider_id)
    default_name = cli_names.get(provider_id, "")

    configured = _get_provider_cli_path(section, settings_getter)
    if configured and _cli_available(configured):
        return configured

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
