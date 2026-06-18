"""Custom agent definitions for the SAO AI Editor.

Architecture follows Claude Code / VSCode Copilot agent patterns:
  - Built-in agents with specialized system prompts
  - Custom agents loaded from .sao/agents/*.json
  - LLM invokes via engine(action="invoke_agent", agent_id="...", message="...")
  - Users create/edit/delete from the sidebar UI
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict
from typing import Any, Dict, List, Optional


@dataclass
class AgentDef:
    id: str
    name: str
    description: str = ""
    system_prompt: str = ""
    tools: Optional[List[str]] = None
    model: str = ""
    icon: str = "\U0001f916"
    when_to_use: str = ""
    builtin: bool = False

    def to_dict(self) -> Dict[str, Any]:
        d = asdict(self)
        d.pop("builtin", None)
        return d

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> AgentDef:
        known = {f.name for f in cls.__dataclass_fields__.values()}
        return cls(**{k: v for k, v in d.items() if k in known})


BUILTIN_AGENTS: List[AgentDef] = [
    AgentDef(
        id="code-reviewer",
        name="Code Reviewer",
        description="Reviews code for bugs, security issues, and style",
        system_prompt=(
            "You are a meticulous code reviewer. Analyze the given code for:\n"
            "1. Bugs and logic errors\n"
            "2. Security vulnerabilities (injection, XSS, SSRF, etc.)\n"
            "3. Performance issues\n"
            "4. Style and readability concerns\n\n"
            "Provide specific, actionable feedback with line references. "
            "Prioritize: Critical > Warning > Info."
        ),
        tools=["readFile", "searchFiles", "listFiles"],
        icon="\U0001f50d",
        when_to_use="When the user asks to review, audit, or check code quality",
        builtin=True,
    ),
    AgentDef(
        id="explainer",
        name="Code Explainer",
        description="Explains code structure, logic, and patterns in detail",
        system_prompt=(
            "You are an expert code explainer. When given code:\n"
            "1. Explain what it does at a high level\n"
            "2. Walk through the logic step by step\n"
            "3. Identify design patterns and architectural decisions\n"
            "4. Note non-obvious behavior or edge cases\n\n"
            "Use clear language. Include mermaid diagrams when helpful."
        ),
        tools=["readFile", "searchFiles", "listFiles"],
        icon="\U0001f4d6",
        when_to_use="When the user asks to explain, understand, or document code",
        builtin=True,
    ),
    AgentDef(
        id="debugger",
        name="Debugger",
        description="Systematically diagnoses and fixes bugs",
        system_prompt=(
            "You are a systematic debugger. Given a bug or error:\n"
            "1. Reproduce — understand the failing behavior\n"
            "2. Isolate — narrow down the root cause\n"
            "3. Diagnose — identify the exact issue\n"
            "4. Fix — provide the minimal correct fix\n"
            "5. Verify — explain how to confirm the fix\n\n"
            "Always read the relevant code before suggesting fixes."
        ),
        icon="\U0001f41b",
        when_to_use="When the user reports a bug, error, or unexpected behavior",
        builtin=True,
    ),
    AgentDef(
        id="optimizer",
        name="Performance Optimizer",
        description="Identifies and fixes performance bottlenecks",
        system_prompt=(
            "You are a performance optimization expert. Analyze code for:\n"
            "1. Algorithmic inefficiency\n"
            "2. Memory waste (unnecessary copies, leaks)\n"
            "3. I/O bottlenecks (blocking calls, missing caching)\n"
            "4. Concurrency issues (GIL, lock contention)\n\n"
            "Quantify impact when possible. Suggest Cython for hot paths."
        ),
        icon="⚡",
        when_to_use="When the user asks about performance, speed, or optimization",
        builtin=True,
    ),
    AgentDef(
        id="documenter",
        name="Documentation Writer",
        description="Generates clear, comprehensive documentation",
        system_prompt=(
            "You are a technical documentation expert. Generate:\n"
            "1. Module/function docstrings (Google style)\n"
            "2. API documentation with examples\n"
            "3. Architecture overviews with diagrams\n"
            "4. User guides and tutorials\n\n"
            "Keep documentation accurate and concise."
        ),
        tools=["readFile", "searchFiles", "listFiles", "editFile"],
        icon="\U0001f4dd",
        when_to_use="When the user asks to document or write docs",
        builtin=True,
    ),
]


class AgentRegistry:

    def __init__(self) -> None:
        self._agents: Dict[str, AgentDef] = {}
        self._version = 0
        for a in BUILTIN_AGENTS:
            self._agents[a.id] = a
        self._bump_version()

    def _bump_version(self) -> None:
        self._version += 1

    def load_custom(self, workspace_root: str = "") -> None:
        """Load from a single directory (legacy / testing)."""
        if workspace_root:
            self._load_dir(os.path.join(workspace_root, ".sao", "agents"),
                           "workspace")
            return
        self.load_all_scopes()

    def load_all_scopes(self) -> None:
        """Load agents from system → workspace → plugin scopes."""
        from ai_editor.scopes import scope_subdirs
        for entry in scope_subdirs("agents"):
            self._load_dir(entry["path"], entry.get("scope", "workspace"),
                           entry.get("plugin_id"))

    def _load_dir(self, agents_dir: str, scope: str = "workspace",
                  plugin_id: Optional[str] = None) -> None:
        if not os.path.isdir(agents_dir):
            return
        for fname in sorted(os.listdir(agents_dir)):
            if not fname.endswith(".json"):
                continue
            try:
                with open(os.path.join(agents_dir, fname), "r",
                          encoding="utf-8") as f:
                    data = json.load(f)
                agent = AgentDef.from_dict(data)
                agent.builtin = False
                agent._scope = scope              # type: ignore[attr-defined]
                agent._plugin_id = plugin_id      # type: ignore[attr-defined]
                self._agents[agent.id] = agent
                self._bump_version()
            except Exception:
                pass

    def get(self, agent_id: str) -> Optional[AgentDef]:
        return self._agents.get(agent_id)

    def list_all(self) -> List[AgentDef]:
        return list(self._agents.values())

    def save_custom(self, agent: AgentDef,
                    workspace_root: str = "",
                    scope: str = "workspace") -> Dict[str, Any]:
        """Save agent to the specified scope (system/workspace/plugin:<id>)."""
        target_dir = self._scope_dir(scope, workspace_root)
        os.makedirs(target_dir, exist_ok=True)
        fpath = os.path.join(target_dir, f"{agent.id}.json")
        with open(fpath, "w", encoding="utf-8") as f:
            json.dump(agent.to_dict(), f, ensure_ascii=False, indent=2)
        agent.builtin = False
        agent._scope = scope  # type: ignore[attr-defined]
        self._agents[agent.id] = agent
        self._bump_version()
        return {"ok": True, "path": fpath, "scope": scope}

    def delete_custom(self, agent_id: str,
                      workspace_root: str = "") -> Dict[str, Any]:
        a = self._agents.get(agent_id)
        if a and a.builtin:
            return {"ok": False, "error": "Cannot delete built-in agent"}
        # Try all scopes
        from ai_editor.scopes import scope_subdirs
        if workspace_root:
            dirs = [{"path": os.path.join(workspace_root, ".sao", "agents")}]
        else:
            dirs = scope_subdirs("agents")
        for entry in dirs:
            fpath = os.path.join(entry["path"], f"{agent_id}.json")
            if os.path.isfile(fpath):
                os.remove(fpath)
        if self._agents.pop(agent_id, None) is not None:
            self._bump_version()
        return {"ok": True}

    @staticmethod
    def _scope_dir(scope: str, workspace_root: str = "") -> str:
        if workspace_root:
            return os.path.join(workspace_root, ".sao", "agents")
        from ai_editor.scopes import _home_sao, _base_dir
        if scope == "system":
            return os.path.join(_home_sao(), "agents")
        if scope.startswith("plugin:"):
            plugin_id = scope.split(":", 1)[1]
            base = _base_dir()
            for parent in ("plugins", "user_plugins"):
                d = os.path.join(base, parent, plugin_id, ".sao", "agents")
                if os.path.isdir(os.path.dirname(d)):
                    return d
            return os.path.join(base, "plugins", plugin_id, ".sao", "agents")
        return os.path.join(_base_dir(), ".sao", "agents")

    def to_prompt_section(self) -> str:
        lines = ["\n## Available Agents\n"]
        for a in self.list_all():
            lines.append(f"- **{a.icon} {a.name}** (`{a.id}`): {a.description}")
            if a.when_to_use:
                lines.append(f"  Use when: {a.when_to_use}")
        lines.append(
            '\nInvoke: `engine(action="invoke_agent", agent_id="<id>", '
            'message="<task>")`'
        )
        return "\n".join(lines)


_singleton: Optional[AgentRegistry] = None


def get_agent_registry() -> AgentRegistry:
    global _singleton
    if _singleton is None:
        _singleton = AgentRegistry()
        _singleton.load_custom()
    return _singleton
