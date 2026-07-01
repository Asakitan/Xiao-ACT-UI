"""Workflow engine for the SAO AI Editor.

Claude-style multi-step workflows:
  - Sequential steps with variable interpolation (``{{var}}``)
  - Agent assignment per step
  - Built-in + custom from .sao/workflows/*.json
  - LLM invokes via engine(action="run_workflow", workflow_id="...", input="...")
"""

from __future__ import annotations

import concurrent.futures
import json
import logging
import os
import time
from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Dict, List, Optional, Tuple


logger = logging.getLogger(__name__)


class WorkflowCancelled(Exception):
    """Raised internally when a workflow run is cancelled."""


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
    # Steps sharing the same non-empty, *contiguous* group value run
    # concurrently against the same pre-batch context snapshot.
    group: str = ""
    # If set, the engine blocks this step (via ``confirm_step``) right
    # before its LLM call until a human approves or rejects it.
    requires_confirmation: bool = False

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
    """Executes workflows by chaining LLM calls per step.

    ``llm_factory``, if given, is a zero-arg callable returning a fresh,
    independently-cancellable LLM client (e.g. ``lambda: LLMEngine(cfg)``).
    It is used to isolate concurrent ``group`` steps from each other and
    from the shared ``llm_engine`` — required because callers typically
    reuse one ``llm_engine`` instance (with its own mutable cancel/http
    state) for both interactive chat and workflows. Without a factory,
    ``group`` steps still run — just one at a time, against the same
    shared context snapshot — instead of risking concurrent access to
    that shared mutable state.
    """

    def __init__(self, llm_engine: Any, agent_registry: Any,
                 llm_factory: Optional[Callable[[], Any]] = None) -> None:
        self._llm = llm_engine
        self._agents = agent_registry
        self._llm_factory = llm_factory

    def _step_messages(self, step: "WorkflowStep", context: Dict[str, str]
                       ) -> List[Dict[str, str]]:
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
        return messages

    @staticmethod
    def _call_llm(llm: Any, messages: List[Dict[str, str]],
                  cancelled: Callable[[], bool]) -> Tuple[str, Optional[str]]:
        output = ""
        error: Optional[str] = None
        try:
            llm.reset_cancel()
            if cancelled():
                raise WorkflowCancelled("Workflow cancelled")
            resp = llm.chat_completion_stream(
                messages=messages, tools=None, on_delta=lambda _d: None)
            output = resp.content or ""
            error = resp.error if resp.error else None
            if getattr(resp, "finish_reason", "") == "cancelled":
                raise WorkflowCancelled("Workflow cancelled")
            if cancelled():
                raise WorkflowCancelled("Workflow cancelled")
        except WorkflowCancelled as exc:
            error = str(exc)
            try:
                cancel = getattr(llm, "cancel", None)
                if callable(cancel):
                    cancel()
            except Exception:
                pass
        except Exception as exc:
            error = str(exc)
        return output, error

    @staticmethod
    def _make_entry(i: int, step: "WorkflowStep", output: str,
                    error: Optional[str], started_at: int, ended_at: int
                    ) -> Dict[str, Any]:
        status = "done"
        if error == "Workflow cancelled":
            status = "cancelled"
        elif error == "Workflow step rejected":
            status = "rejected"
        elif error:
            status = "error"
        entry: Dict[str, Any] = {
            "step": i,
            "agent": step.agent,
            "label": step.label or f"Step {i + 1}",
            "output_var": step.output_var,
            "output": output,
            "status": status,
            "startedAt": started_at,
            "endedAt": ended_at,
            "durationMs": max(0, ended_at - started_at),
        }
        if error:
            entry["error"] = error
        if error == "Workflow cancelled":
            entry["cancelled"] = True
        if error == "Workflow step rejected":
            entry["rejected"] = True
        return entry

    @staticmethod
    def _maybe_confirm(i: int, step: "WorkflowStep",
                       confirm_step: Optional[Callable[[int, "WorkflowStep"],
                                                        Optional[bool]]]
                       ) -> Optional[str]:
        """Returns an error string if the step must not run (rejected or
        cancelled while waiting), or ``None`` if it's clear to proceed."""
        if not getattr(step, "requires_confirmation", False):
            return None
        if confirm_step is None:
            return None
        decision = confirm_step(i, step)
        if decision is None:
            return "Workflow cancelled"
        if not decision:
            return "Workflow step rejected"
        return None

    @staticmethod
    def _plan_batches(steps: List["WorkflowStep"], start_index: int
                      ) -> List[List[Tuple[int, "WorkflowStep"]]]:
        """Group step indices into execution batches: a contiguous run of
        steps sharing the same non-empty ``group`` is one batch (run
        concurrently); every other step is its own singleton batch (run
        exactly as before)."""
        batches: List[List[Tuple[int, "WorkflowStep"]]] = []
        current: List[Tuple[int, "WorkflowStep"]] = []
        current_group: Optional[str] = None
        for i, step in enumerate(steps):
            if i < start_index:
                continue
            group = str(getattr(step, "group", "") or "")
            if group and group == current_group:
                current.append((i, step))
                continue
            if current:
                batches.append(current)
            current = [(i, step)]
            current_group = group or None
        if current:
            batches.append(current)
        return batches

    def _run_batch_isolated(self, batch: List[Tuple[int, "WorkflowStep"]],
                            total: int, context: Dict[str, str],
                            on_step_start: Optional[Callable],
                            on_step_end: Optional[Callable],
                            cancelled: Callable[[], bool],
                            confirm_step: Optional[Callable] = None,
                            ) -> Dict[int, Dict[str, Any]]:
        """Run each batch member one at a time against the same context
        snapshot, using the shared ``self._llm``. Used for singleton
        (non-grouped) steps, and as the safe fallback for ``group`` steps
        when no ``llm_factory`` is available for real concurrency."""
        entries: Dict[int, Dict[str, Any]] = {}
        for i, step in batch:
            if on_step_start:
                on_step_start(i, total, step)
            started_at = int(time.time() * 1000)
            blocked = self._maybe_confirm(i, step, confirm_step)
            if blocked is not None:
                output, error = "", blocked
            else:
                messages = self._step_messages(step, context)
                output, error = self._call_llm(self._llm, messages, cancelled)
            ended_at = int(time.time() * 1000)
            entries[i] = self._make_entry(i, step, output, error,
                                          started_at, ended_at)
            if on_step_end:
                on_step_end(i, total, step, output, error)
        return entries

    def _run_batch_parallel(self, batch: List[Tuple[int, "WorkflowStep"]],
                            total: int, context: Dict[str, str],
                            on_step_start: Optional[Callable],
                            on_step_end: Optional[Callable],
                            cancelled: Callable[[], bool],
                            confirm_step: Optional[Callable] = None,
                            ) -> Dict[int, Dict[str, Any]]:
        """Run every batch member concurrently, each against its own
        ``llm_factory()``-provided client so none share cancel/http state."""
        for i, step in batch:
            if on_step_start:
                on_step_start(i, total, step)
        started_ats = {i: int(time.time() * 1000) for i, _ in batch}
        entries: Dict[int, Dict[str, Any]] = {}

        def _call_isolated(i: int, step: "WorkflowStep"
                           ) -> Tuple[str, Optional[str]]:
            blocked = self._maybe_confirm(i, step, confirm_step)
            if blocked is not None:
                return "", blocked
            llm = self._llm_factory()
            try:
                return self._call_llm(
                    llm, self._step_messages(step, context), cancelled)
            finally:
                close = getattr(llm, "close", None)
                if callable(close):
                    try:
                        close()
                    except Exception:
                        pass

        with concurrent.futures.ThreadPoolExecutor(
                max_workers=len(batch)) as pool:
            futures = {
                pool.submit(_call_isolated, i, step): (i, step)
                for i, step in batch
            }
            for fut in concurrent.futures.as_completed(futures):
                i, step = futures[fut]
                try:
                    output, error = fut.result()
                except Exception as exc:
                    output, error = "", str(exc)
                ended_at = int(time.time() * 1000)
                entries[i] = self._make_entry(
                    i, step, output, error, started_ats[i], ended_at)
                if on_step_end:
                    on_step_end(i, total, step, output, error)
        return entries

    def run(self, workflow: WorkflowDef, input_text: str,
            on_step_start: Optional[Callable] = None,
            on_step_end: Optional[Callable] = None,
            run_id: str = "",
            cancel_requested: Optional[Callable[[], bool]] = None,
            start_index: int = 0,
            seed_context: Optional[Dict[str, str]] = None,
            wait_if_paused: Optional[Callable[[], None]] = None,
            confirm_step: Optional[Callable[[int, "WorkflowStep"],
                                            Optional[bool]]] = None,
            ) -> Dict[str, Any]:
        """Run synchronously — call from a background thread.

        ``start_index``/``seed_context`` let a caller retry a workflow from a
        single failed step: steps before ``start_index`` are skipped and
        ``seed_context`` seeds ``{{var}}`` interpolation with their
        previously computed ``output_var`` values.

        ``wait_if_paused``, if given, is called at every step boundary and is
        expected to block the calling thread until the run is resumed (or
        the run is cancelled, in which case it must return promptly so the
        cancellation check right after it can take effect).

        ``confirm_step``, if given, is called for any step with
        ``requires_confirmation=True`` right before its LLM call, and is
        expected to block until a decision is made: return ``True`` to
        proceed, ``False`` to reject the step, or ``None`` if the run was
        cancelled while waiting.
        """
        run_id = str(run_id or "").strip()
        start_index = max(0, int(start_index or 0))
        context: Dict[str, str] = {"input": input_text}
        if seed_context:
            context.update({str(k): str(v) for k, v in seed_context.items()})
        results: List[Dict[str, Any]] = []
        run_started_at = int(time.time() * 1000)

        def _cancelled() -> bool:
            if cancel_requested is None:
                return False
            try:
                return bool(cancel_requested())
            except Exception:
                return False

        total = len(workflow.steps)
        batches = self._plan_batches(workflow.steps, start_index)

        for batch in batches:
            if wait_if_paused:
                wait_if_paused()
            if _cancelled():
                ended_at = int(time.time() * 1000)
                return {
                    "workflow": workflow.id,
                    "steps": results,
                    "final_output": results[-1]["output"] if results else "",
                    "workflowRunId": run_id,
                    "startedAt": run_started_at,
                    "endedAt": ended_at,
                    "workflowDurationMs": max(0, ended_at - run_started_at),
                    "cancelled": True,
                    "message": "Workflow cancelled",
                }

            if len(batch) > 1 and self._llm_factory is not None:
                entries = self._run_batch_parallel(
                    batch, total, context, on_step_start, on_step_end,
                    _cancelled, confirm_step)
            else:
                entries = self._run_batch_isolated(
                    batch, total, context, on_step_start, on_step_end,
                    _cancelled, confirm_step)

            batch_cancelled = False
            batch_error: Optional[str] = None
            for i, step in batch:
                entry = entries[i]
                results.append(entry)
                if step.output_var:
                    context[step.output_var] = entry.get("output", "")
                if entry.get("cancelled"):
                    batch_cancelled = True
                elif entry.get("error") and batch_error is None:
                    batch_error = entry["error"]

            if batch_cancelled:
                ended_at = int(time.time() * 1000)
                return {
                    "workflow": workflow.id,
                    "steps": results,
                    "final_output": results[-1]["output"] if results else "",
                    "workflowRunId": run_id,
                    "startedAt": run_started_at,
                    "endedAt": ended_at,
                    "workflowDurationMs": max(0, ended_at - run_started_at),
                    "cancelled": True,
                    "message": "Workflow cancelled",
                }
            if batch_error:
                break

        final = results[-1]["output"] if results else ""
        ended_at = int(time.time() * 1000)
        return {"workflow": workflow.id, "steps": results,
                "final_output": final, "workflowRunId": run_id,
                "startedAt": run_started_at, "endedAt": ended_at,
                "workflowDurationMs": max(0, ended_at - run_started_at)}
