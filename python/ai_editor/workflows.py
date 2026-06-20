"""Workflow engine for the SAO AI Editor.

Claude-style multi-step workflows:
  - Sequential steps with variable interpolation (``{{var}}``)
  - Agent assignment per step
  - Built-in + custom from .sao/workflows/*.json
  - LLM invokes via engine(action="run_workflow", workflow_id="...", input="...")
"""

from __future__ import annotations

import json
import logging
import os
from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Dict, List, Optional


logger = logging.getLogger(__name__)


def _validate_workflow_id(workflow_id: str) -> str:
    normalized = str(workflow_id or "").strip()
    if not normalized:
        raise ValueError("Workflow id is required")
    if normalized != os.path.basename(normalized) or any(sep in normalized for sep in ("/", "\\")) or ".." in normalized:
        raise ValueError("Workflow id must be a simple file-safe identifier")
    return normalized


@dataclass
class WorkflowStep:
    agent: str = "default"
    prompt: str = ""
    output_var: str = ""
    label: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> WorkflowStep:
        known = {f.name for f in cls.__dataclass_fields__.values()}
        return cls(**{k: v for k, v in d.items() if k in known})


@dataclass
class WorkflowDef:
    id: str
    name: str
    description: str = ""
    steps: List[WorkflowStep] = field(default_factory=list)
    icon: str = "⚡"
    when_to_use: str = ""
    builtin: bool = False

    def to_dict(self) -> Dict[str, Any]:
        d = asdict(self)
        d.pop("builtin", None)
        d["steps"] = [s.to_dict() for s in self.steps]
        return d

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> WorkflowDef:
        steps = [WorkflowStep.from_dict(s) for s in d.get("steps", [])]
        known = {f.name for f in cls.__dataclass_fields__.values()}
        kw = {k: v for k, v in d.items() if k in known and k != "steps"}
        return cls(steps=steps, **kw)


BUILTIN_WORKFLOWS: List[WorkflowDef] = [
    WorkflowDef(
        id="review-and-fix",
        name="Review & Fix",
        description="Review code for issues, then generate fixes",
        icon="\U0001f527",
        when_to_use="When the user wants both a review and fixes",
        builtin=True,
        steps=[
            WorkflowStep(
                agent="code-reviewer",
                prompt="Review the following and list all issues:\n\n{{input}}",
                output_var="review",
                label="Reviewing code",
            ),
            WorkflowStep(
                agent="default",
                prompt=(
                    "Based on this review:\n\n{{review}}\n\n"
                    "Generate the minimal fixes. Show only changes needed."
                ),
                output_var="fix",
                label="Generating fixes",
            ),
        ],
    ),
    WorkflowDef(
        id="explain-and-improve",
        name="Explain & Improve",
        description="Explain code in detail, then suggest improvements",
        icon="\U0001f4a1",
        when_to_use="When the user wants to understand and improve code",
        builtin=True,
        steps=[
            WorkflowStep(
                agent="explainer",
                prompt="Explain this code in detail:\n\n{{input}}",
                output_var="explanation",
                label="Analyzing code",
            ),
            WorkflowStep(
                agent="optimizer",
                prompt=(
                    "Given this analysis:\n\n{{explanation}}\n\n"
                    "Suggest concrete improvements for performance, "
                    "readability, and maintainability."
                ),
                output_var="improvements",
                label="Finding improvements",
            ),
        ],
    ),
    WorkflowDef(
        id="debug-trace",
        name="Debug Trace",
        description="Analyze an error, diagnose root cause, suggest fix",
        icon="\U0001f41b",
        when_to_use="When the user reports a bug and wants diagnosis + fix",
        builtin=True,
        steps=[
            WorkflowStep(
                agent="debugger",
                prompt=(
                    "Analyze this error/bug and identify the root cause:\n\n"
                    "{{input}}"
                ),
                output_var="diagnosis",
                label="Diagnosing issue",
            ),
            WorkflowStep(
                agent="default",
                prompt=(
                    "Based on this diagnosis:\n\n{{diagnosis}}\n\n"
                    "Provide the minimal fix with code."
                ),
                output_var="fix",
                label="Generating fix",
            ),
        ],
    ),
]


class WorkflowRegistry:

    def __init__(self) -> None:
        self._workflows: Dict[str, WorkflowDef] = {}
        self._version = 0
        for w in BUILTIN_WORKFLOWS:
            self._workflows[w.id] = w
        self._bump_version()

    def _bump_version(self) -> None:
        self._version += 1

    def load_custom(self, workspace_root: str = "") -> None:
        if workspace_root:
            self._load_dir(os.path.join(workspace_root, ".sao", "workflows"),
                           "workspace")
            return
        self.load_all_scopes()

    def load_all_scopes(self) -> None:
        from ai_editor.scopes import scope_subdirs
        for entry in scope_subdirs("workflows"):
            self._load_dir(entry["path"], entry.get("scope", "workspace"),
                           entry.get("plugin_id"))

    def _load_dir(self, wf_dir: str, scope: str = "workspace",
                  plugin_id: Optional[str] = None) -> None:
        if not os.path.isdir(wf_dir):
            return
        for fname in sorted(os.listdir(wf_dir)):
            if not fname.endswith(".json"):
                continue
            try:
                with open(os.path.join(wf_dir, fname), "r",
                          encoding="utf-8") as f:
                    data = json.load(f)
                wf = WorkflowDef.from_dict(data)
                wf.builtin = False
                wf._scope = scope              # type: ignore[attr-defined]
                wf._plugin_id = plugin_id      # type: ignore[attr-defined]
                self._workflows[wf.id] = wf
                self._bump_version()
            except Exception as exc:
                logger.warning("Failed to load workflow definition %s: %s",
                               os.path.join(wf_dir, fname), exc)

    def get(self, wf_id: str) -> Optional[WorkflowDef]:
        return self._workflows.get(wf_id)

    def list_all(self) -> List[WorkflowDef]:
        return list(self._workflows.values())

    def save_custom(self, wf: WorkflowDef,
                    workspace_root: str = "",
                    scope: str = "workspace") -> Dict[str, Any]:
        try:
            workflow_id = _validate_workflow_id(wf.id)
        except ValueError as exc:
            return {"ok": False, "error": str(exc)}
        if not str(wf.name).strip():
            return {"ok": False, "error": "Workflow name is required"}
        target_dir = self._scope_dir(scope, workspace_root)
        wf.id = workflow_id
        try:
            os.makedirs(target_dir, exist_ok=True)
            fpath = os.path.join(target_dir, f"{workflow_id}.json")
            with open(fpath, "w", encoding="utf-8") as f:
                json.dump(wf.to_dict(), f, ensure_ascii=False, indent=2)
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        wf.builtin = False
        wf._scope = scope  # type: ignore[attr-defined]
        self._workflows[workflow_id] = wf
        self._bump_version()
        return {"ok": True, "path": fpath, "scope": scope}

    def delete_custom(self, wf_id: str,
                      workspace_root: str = "") -> Dict[str, Any]:
        try:
            wf_id = _validate_workflow_id(wf_id)
        except ValueError as exc:
            return {"ok": False, "error": str(exc)}
        w = self._workflows.get(wf_id)
        if w and w.builtin:
            return {"ok": False, "error": "Cannot delete built-in workflow"}
        from ai_editor.scopes import scope_subdirs
        if workspace_root:
            dirs = [{"path": os.path.join(workspace_root, ".sao", "workflows")}]
        else:
            dirs = scope_subdirs("workflows")
        delete_errors: List[str] = []
        for entry in dirs:
            fpath = os.path.join(entry["path"], f"{wf_id}.json")
            if os.path.isfile(fpath):
                try:
                    os.remove(fpath)
                except OSError as exc:
                    delete_errors.append(f"{fpath}: {exc}")
        if delete_errors:
            return {"ok": False, "error": "; ".join(delete_errors)}
        if self._workflows.pop(wf_id, None) is not None:
            self._bump_version()
        return {"ok": True}

    @staticmethod
    def _scope_dir(scope: str, workspace_root: str = "") -> str:
        if workspace_root:
            return os.path.join(workspace_root, ".sao", "workflows")
        from ai_editor.scopes import _home_sao, _base_dir
        if scope == "system":
            return os.path.join(_home_sao(), "workflows")
        if scope.startswith("plugin:"):
            plugin_id = scope.split(":", 1)[1]
            base = _base_dir()
            for parent in ("plugins", "user_plugins"):
                d = os.path.join(base, parent, plugin_id, ".sao", "workflows")
                if os.path.isdir(os.path.dirname(d)):
                    return d
            return os.path.join(base, "plugins", plugin_id, ".sao", "workflows")
        return os.path.join(_base_dir(), ".sao", "workflows")

    def to_prompt_section(self) -> str:
        lines = ["\n## Available Workflows\n"]
        for w in self.list_all():
            chain = " → ".join(s.label or s.agent for s in w.steps)
            lines.append(
                f"- **{w.icon} {w.name}** (`{w.id}`): {w.description}"
            )
            lines.append(f"  Steps: {chain}")
            if w.when_to_use:
                lines.append(f"  Use when: {w.when_to_use}")
        lines.append(
            '\nInvoke: `engine(action="run_workflow", workflow_id="<id>", '
            'input="<context>")`'
        )
        return "\n".join(lines)


_singleton: Optional[WorkflowRegistry] = None


def get_workflow_registry() -> WorkflowRegistry:
    global _singleton
    if _singleton is None:
        _singleton = WorkflowRegistry()
        _singleton.load_custom()
    return _singleton


class WorkflowEngine:
    """Executes workflows by chaining LLM calls per step."""

    def __init__(self, llm_engine: Any, agent_registry: Any) -> None:
        self._llm = llm_engine
        self._agents = agent_registry

    def run(self, workflow: WorkflowDef, input_text: str,
            on_step_start: Optional[Callable] = None,
            on_step_end: Optional[Callable] = None,
            ) -> Dict[str, Any]:
        """Run synchronously — call from a background thread."""
        context: Dict[str, str] = {"input": input_text}
        results: List[Dict[str, Any]] = []

        for i, step in enumerate(workflow.steps):
            if on_step_start:
                on_step_start(i, len(workflow.steps), step)

            prompt = step.prompt
            for var, val in context.items():
                prompt = prompt.replace("{{" + var + "}}", str(val))

            agent = (self._agents.get(step.agent)
                     if step.agent != "default" else None)
            system = agent.system_prompt if agent else ""

            messages: List[Dict[str, str]] = []
            if system:
                messages.append({"role": "system", "content": system})
            messages.append({"role": "user", "content": prompt})

            output = ""
            error: Optional[str] = None
            try:
                self._llm.reset_cancel()
                resp = self._llm.chat_completion_stream(
                    messages=messages, tools=None,
                    on_delta=lambda _d: None,
                )
                output = resp.content or ""
                error = resp.error if resp.error else None
            except Exception as exc:
                error = str(exc)

            if step.output_var:
                context[step.output_var] = output

            entry: Dict[str, Any] = {
                "step": i,
                "agent": step.agent,
                "label": step.label or f"Step {i + 1}",
                "output": output,
            }
            if error:
                entry["error"] = error
            results.append(entry)

            if on_step_end:
                on_step_end(i, len(workflow.steps), step, output, error)

            if error:
                break

        final = results[-1]["output"] if results else ""
        return {"workflow": workflow.id, "steps": results,
                "final_output": final}
