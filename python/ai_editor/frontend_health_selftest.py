from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HTML_PATH = ROOT / "web" / "ai_editor_app.html"

# Every _require() call is also recorded here (name, passed) so
# compute_parity_snapshot() can bucket the whole health sweep into VS Code
# parity phases without touching any of the ~100 existing call sites above.
_ALL_CHECKS: list[tuple[str, bool]] = []

# (phase label, keywords) - first matching keyword found in a lowercased
# check name wins; order matters. Mirrors the phases in
# AI_IDE_VSCODE_PARITY_LONG_TERM_PLAN.md so "which phase is behind" can be
# read off mechanically instead of re-deriving it by hand each round.
_PARITY_PHASES: list[tuple[str, tuple[str, ...]]] = [
    ("0-baseline", ("script ids", "style ids", "inline scripts parse", "aggregate diagnostics",
                     "stale global lock", "api health", "frontend health payload",
                     "frontend heartbeat", "frontend ready sends", "ui action error")),
    ("1-extension-host", ("extension", "webview", "task/debug", "quick input", "quickinput")),
    ("2-assistant", ("assistant", "workflow", "chat")),
    ("3-settings", ("settings",)),
    ("4-language-editor", ("language", "diff")),
    ("5-perf-stability", ("interaction", "control noop", "critical action", "long action",
                           "command health", "statusbar", "diagnostics maintenance",
                           "lifecycle", "high-frequency", "idle time", "failure classif",
                           "perf budget")),
    ("6-motion-ui-workspace", ("terminal", "workspace", "window closing", "drag overlay",
                               "keyboard recovery")),
]


def _classify_phase(name: str) -> str:
    lowered = name.lower()
    for phase, keywords in _PARITY_PHASES:
        if any(keyword in lowered for keyword in keywords):
            return phase
    return "unclassified"


def _read_html() -> str:
    return HTML_PATH.read_text(encoding="utf-8")


def _require(failures: list[str], name: str, condition: bool) -> None:
    _ALL_CHECKS.append((name, bool(condition)))
    if not condition:
        failures.append(name)


def compute_parity_snapshot() -> dict:
    """Bucket every frontend-health check into a VS Code parity phase.

    This is the 阶段0 dashboard from AI_IDE_VSCODE_PARITY_LONG_TERM_PLAN.md:
    a mechanical "what's ready vs partial vs missing" read instead of a
    manual re-derivation each round. A phase with any failing check is
    "partial"; a phase with zero checks recorded is "missing" (nothing here
    verifies it yet, which is itself a gap); otherwise "ready".
    """
    phases: dict[str, dict] = {}
    for name, passed in _ALL_CHECKS:
        phase = _classify_phase(name)
        bucket = phases.setdefault(phase, {"passed": 0, "total": 0, "failures": []})
        bucket["total"] += 1
        if passed:
            bucket["passed"] += 1
        else:
            bucket["failures"].append(name)
    for phase, _keywords in _PARITY_PHASES:
        phases.setdefault(phase, {"passed": 0, "total": 0, "failures": []})
    summary = {}
    for phase, bucket in phases.items():
        if bucket["total"] == 0:
            status = "missing"
        elif bucket["failures"]:
            status = "partial"
        else:
            status = "ready"
        summary[phase] = {
            "status": status,
            "passed": bucket["passed"],
            "total": bucket["total"],
            "failures": bucket["failures"],
        }
    phase_order = [phase for phase, _keywords in _PARITY_PHASES] + ["unclassified"]
    top_gaps: list[str] = []
    for phase in phase_order:
        bucket = summary.get(phase)
        if not bucket:
            continue
        for failure in bucket["failures"]:
            top_gaps.append(f"[{phase}] {failure}")
            if len(top_gaps) >= 5:
                break
        if len(top_gaps) >= 5:
            break
    return {"phases": summary, "top_gaps": top_gaps}


def _duplicate_ids(html: str, tag_name: str) -> list[str]:
    pattern = rf"<{tag_name}\b[^>]*\bid=[\"']([^\"']+)[\"']"
    ids = re.findall(pattern, html, flags=re.IGNORECASE)
    return sorted({item for item in ids if ids.count(item) > 1})


def _script_blocks_by_id(html: str, id_fragment: str) -> str:
    pattern = r"<script\b[^>]*\bid=[\"']([^\"']+)[\"'][^>]*>(.*?)</script>"
    blocks: list[str] = []
    for script_id, body in re.findall(pattern, html, flags=re.IGNORECASE | re.DOTALL):
        if id_fragment.lower() in script_id.lower():
            blocks.append(body)
    return "\n".join(blocks)


def _inline_script_syntax_failures(html: str) -> list[str]:
    node = shutil.which("node")
    if not node:
        return ["node executable unavailable for inline script syntax check"]
    script = r"""
const fs = require("fs");
const html = fs.readFileSync(0, "utf8");
const re = /<script(?:\s+[^>]*)?>([\s\S]*?)<\/script>/gi;
const failures = [];
let index = 0;
for (const match of html.matchAll(re)) {
  index += 1;
  const body = match[1] || "";
  const line = html.slice(0, match.index).split(/\r?\n/).length;
  try {
    new Function(body);
  } catch (err) {
    failures.push({index, line, message: String(err && err.message || err)});
  }
}
process.stdout.write(JSON.stringify(failures));
"""
    try:
        result = subprocess.run(
            [node, "-e", script],
            input=html,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=15,
            check=False,
        )
    except Exception as exc:
        return [f"inline script syntax check failed to run: {exc}"]
    if result.returncode != 0:
        return [f"inline script syntax checker exited {result.returncode}: {result.stderr.strip()[:240]}"]
    try:
        parsed = json.loads(result.stdout or "[]")
    except Exception as exc:
        return [f"inline script syntax checker returned invalid JSON: {exc}"]
    return [
        f"inline script #{item.get('index')} line {item.get('line')}: {item.get('message')}"
        for item in parsed
        if isinstance(item, dict)
    ]


def _slice_between(html: str, start: str, end: str) -> str:
    start_index = html.find(start)
    if start_index < 0:
        return ""
    end_index = html.find(end, start_index + len(start))
    if end_index < 0:
        return html[start_index:]
    return html[start_index:end_index]


def run_frontend_health_selftest() -> list[str]:
    _ALL_CHECKS.clear()
    html = _read_html()
    failures: list[str] = []
    duplicate_script_ids = _duplicate_ids(html, "script")
    duplicate_style_ids = _duplicate_ids(html, "style")
    assistant_scripts = _script_blocks_by_id(html, "assistant")
    extension_registry_scripts = _script_blocks_by_id(html, "extension-surface-registry")
    terminal_runtime_source = _slice_between(
        html,
        "function terminalRuntimeSnapshot(){",
        "clearTerminal=function()",
    )
    script_syntax_failures = _inline_script_syntax_failures(html)

    _require(failures, "script ids are unique", not duplicate_script_ids)
    _require(failures, "style ids are unique", not duplicate_style_ids)
    _require(failures, "inline scripts parse without syntax errors", not script_syntax_failures)

    _require(failures, "interaction recovery script exists", "ai-editor-interaction-recovery" in html)
    _require(failures, "window closing does not disable all pointer input", "body.window-closing" in html and "pointer-events: auto !important" in html)
    _require(failures, "stale global lock clears on pointerdown", "pointerdown-stale-global-lock" in html)
    _require(failures, "hidden dialogs do not block interaction recovery", "function visibleBlockingDialog(node)" in html and "node.classList.contains(\"hidden\")" in html and "getClientRects().length" in html)
    _require(failures, "keyboard recovery shortcut exists", "keyboard-recover" in html)
    _require(failures, "interaction snapshot exposes blocker diagnostics", "aiEditorInteractionRecoverySnapshot" in html and "transientBlockerCount" in html)
    _require(failures, "drag overlay only activates for file drags", "function isFileDragEvent(e)" in html and "if(!isFileDragEvent(e))return;" in html and "types.includes('Files')" in html)
    _require(failures, "interaction recovery clears stale drag overlay", "\"#drag-overlay.active\"" in html and "el.id === \"drag-overlay\"" in html and "el.classList.remove(\"active\")" in html)

    _require(failures, "api health capsule exists", "ai-editor-api-health" in html)
    _require(failures, "api health exposes required method gaps", "missingMethods" in html and "requiredApiMethods" in html)
    _require(failures, "aggregate diagnostics exists", "aiEditorHealthAggregateSnapshot" in html and "aiEditorDiagnostics" in html)
    _require(failures, "frontend health payload exported for smoke checks", "window.aiEditorFrontendHealthPayload=aiEditorFrontendHealthPayload;" in html)
    _require(failures, "frontend heartbeat sends aggregate health payload", "aiEditorFrontendHealthPayload" in html and "update_frontend_health(String(phase||'heartbeat'),aiEditorFrontendHealthPayload())" in html)
    _require(failures, "frontend ready sends aggregate health payload", "mark_frontend_ready(String(phase||'ready'),aiEditorFrontendHealthPayload())" in html)
    _require(failures, "failure classifier exists", "ai-editor-failure-classifier" in html)
    _require(failures, "failure classifier recognizes process spawn failures", "0xc0000142" in html and "-1073741502" in html and "process-spawn-failure" in html)
    _require(failures, "api health marks failure classification", "markAiEditorFailureClassification((snap.lastApiFailureMethod" in html)
    _require(failures, "control noop detector exists", "ai-editor-control-noop-detector" in html)
    _require(failures, "control noop detector marks stale clicks", "lastControlNoopAt" in html and "control-noop" in html)
    _require(failures, "control noop state reaches launch health payload", "payload.lastControlNoopAt=noop.lastControlNoopAt||'';" in html and "payload.lastControlNoopCommand=noop.lastControlNoopCommand||'';" in html)
    _require(failures, "ui action error boundary exists", "ai-editor-ui-action-error-boundary" in html)
    _require(failures, "ui action errors clear stale locks", "clearAiEditorInteractionLocks(\"ui-action-error\")" in html)
    _require(failures, "ui action errors are exposed in health payload", "lastUiActionErrorLabel" in html and "lastUiActionErrorMessage" in html)
    _require(failures, "critical action health exists", "ai-editor-critical-action-health" in html and "aiEditorCriticalActionHealthSnapshot" in html)
    _require(failures, "critical action health covers settings theme terminal assistant", all(item in html for item in [
        "name:\"Settings\"",
        "name:\"Toggle Theme\"",
        "name:\"Terminal Input\"",
        "name:\"Assistant Send\"",
        "name:\"Extension Activity Host\"",
    ]))
    _require(failures, "critical action health reaches payload", "criticalActionMissingCount" in html and "criticalActionNoHandlerCount" in html and "criticalActionNonFocusableCount" in html)
    _require(failures, "editor long action health reaches payload", "editorLongActions: safeCall(\"editorLongActionHealthSnapshot\")" in html and "editorLongActionState" in html and "editorLongActionHistoryCount" in html)
    _require(failures, "editor long action globals exported", "window.beginEditorLongAction=beginEditorLongAction;" in html and "window.cancelEditorLongAction=cancelEditorLongAction;" in html and "window.editorLongActionHealthSnapshot=editorLongActionHealthSnapshot;" in html)
    _require(failures, "command health snapshot exists", "aiEditorCommandHealthSnapshot" in html)
    _require(failures, "command health tracks missing commands", "missingCommandCount" in html and "missingCommandSamples" in html)
    _require(failures, "statusbar entry health exists", "aiEditorStatusbarEntryHealthSnapshot" in html)
    _require(failures, "statusbar health detects settings sun icon regression", "settingsEntryLooksLikeSun" in html)
    _require(failures, "activity settings and theme entries have direct fallbacks", "data-settings-button=\"1\"" in html and "data-theme-button=\"1\"" in html and "stopImmediatePropagation" in html)
    _require(failures, "quick input frontend health exists", "function quickInputFrontendHealthSnapshot()" in html and "window.quickInputFrontendHealthSnapshot=quickInputFrontendHealthSnapshot;" in html and "quickInputVisibleCount" in html)
    _require(failures, "extension dynamic UI frontend health exists", "function extensionDynamicUiFrontendHealthSnapshot()" in html and "window.extensionDynamicUiFrontendHealthSnapshot=extensionDynamicUiFrontendHealthSnapshot;" in html and "extensionWindowMessageCount" in html and "extensionWindowDialogPaths" in html and "function extensionWindowMessageAction(requestId,index,payload)" in html)
    _require(failures, "aggregate diagnostics includes core domains", all(item in html for item in [
        "interaction: safeCall(\"aiEditorInteractionRecoverySnapshot\")",
        "api: safeCall(\"aiEditorApiHealthSnapshot\")",
        "terminal: safeCall(\"aiEditorTerminalHealthSnapshot\")",
        "terminalActions: safeCall(\"terminalActionHealthSnapshot\")",
        "workspace: safeCall(\"aiEditorWorkspaceHealthSnapshot\")",
        "editorLongActions: safeCall(\"editorLongActionHealthSnapshot\")",
        "quickInputs: safeCall(\"quickInputFrontendHealthSnapshot\")",
        "extensionDynamicUi: safeCall(\"extensionDynamicUiFrontendHealthSnapshot\")",
        "taskDebug: safeCall(\"extensionRuntimeTaskDebugSnapshot\")",
        "extensionWebviewResources: safeCall(\"extensionWebviewResourceHealthSnapshot\")",
        "controls: safeCall(\"aiEditorCommandHealthSnapshot\")",
        "uiActionErrors: safeCall(\"aiEditorUiActionErrorSnapshot\")",
        "criticalActions: safeCall(\"aiEditorCriticalActionHealthSnapshot\")",
        "statusbarEntries: safeCall(\"aiEditorStatusbarEntryHealthSnapshot\")",
    ]))

    _require(failures, "terminal health fallback exists", "aiEditorTerminalHealthResult" in html)
    _require(failures, "terminal entry guard exists", "ai-editor-terminal-entry-guard" in html)
    _require(failures, "terminal guard records failed runs", "lastTerminalRunState" in html and "lastTerminalRunError" in html)
    _require(failures, "terminal health classifies failures", "markAiEditorFailureClassification(error)" in html)
    _require(failures, "terminal runtime snapshot defines profile inspector", "const inspector=$('terminal-profile-inspector');" in terminal_runtime_source and "profileInspector:inspector&&inspector.dataset?{" in terminal_runtime_source)
    _require(failures, "terminal pending launch context reaches health payload", "window.terminalPendingLaunchContext=terminalPendingLaunchContext;" in html and "terminalPendingCwd" in html and "terminalPendingProfile" in html and "terminalPendingRunnable" in html)
    _require(failures, "terminal recovery decision reaches health payload", "function terminalRecoveryDecision(meta)" in html and "window.terminalRecoverySnapshot=terminalRecoverySnapshot;" in html and "terminalRecovery: safeCall(\"terminalRecoverySnapshot\")" in html and "payload.terminalRecoveryAction=terminalActions.recoveryAction||terminalRecovery.action||'';" in html)
    _require(failures, "terminal action health snapshot exists", "function terminalActionHealthSnapshot()" in html and "window.terminalActionHealthSnapshot=terminalActionHealthSnapshot;" in html and "id=\"terminal-action-summary\"" in html)
    _require(failures, "terminal action health reaches payload", "payload.terminalActionHealth=terminalActions.health||'';" in html and "payload.terminalActionFocusableCount=terminalActions.focusableActionCount||0;" in html and "payload.terminalActionCanWriteStdin=terminalActions.canWriteStdin===true;" in html)
    _require(failures, "terminal runtime health reaches payload", "payload.terminalRuntimeCwd=terminal.cwd||'';" in html and "payload.terminalRuntimeOutputBounded=terminal.outputBounded===true;" in html and "payload.terminalRuntimeHistoryCount=terminal.historyCount||0;" in html and "payload.terminalPendingActiveRunning=terminal.pendingActiveRunning===true;" in html)
    _require(failures, "terminal commands are exposed in command palette", "Terminal: Focus Terminal" in html and "Terminal: Copy Current CWD" in html and "Terminal: Use CWD as Workspace" in html and "Developer: Run Terminal UI Self Check" in html)
    _require(failures, "terminal panel actions are keyboard accessible", "function terminalPanelButtonKeydown(ev,node)" in html and "onkeydown=\"terminalPanelButtonKeydown(event,this)\"" in html and "data-terminal-action=\"clear\"" in html)
    _require(failures, "task/debug runtime health snapshot exists", "function extensionRuntimeTaskDebugSnapshot()" in html and "window.extensionRuntimeTaskDebugSnapshot=extensionRuntimeTaskDebugSnapshot;" in html and "extension-runtime-task-debug-summary" in html)
    _require(failures, "task/debug runtime health reaches payload", "payload.taskDebugHealth=taskDebug.health||'';" in html and "payload.taskDebugTaskRunning=taskDebug.taskRunning||0;" in html and "payload.taskDebugConsoleEntryCount=taskDebug.debugConsoleEntryCount||0;" in html)
    _require(failures, "webview resource health snapshot exists", "function extensionWebviewResourceHealthSnapshot(rows)" in html and "window.extensionWebviewResourceHealthSnapshot=extensionWebviewResourceHealthSnapshot;" in html and "extension-webview-resource-summary" in html)
    _require(failures, "webview resource health summarizes render resource message failures", all(item in html for item in [
        "resourceWarningRows",
        "resourceFailureRows",
        "queuedMessages",
        "droppedMessages",
        "failureReasons",
        "readinessIssues",
        "resourceEndpointSmokeReadyRows",
        "resourceEndpointSmokeFailureRows",
    ]))
    _require(failures, "webview resource health reaches aggregate and payload", "extensionWebviewResources: safeCall(\"extensionWebviewResourceHealthSnapshot\")" in html and "payload.extensionWebviewResourceHealth=webviewResources.health||'';" in html and "payload.extensionWebviewFailureReasons=Array.isArray(webviewResources.failureReasons)" in html and "payload.extensionWebviewEndpointSmokeReady=webviewResources.resourceEndpointSmokeReadyRows||0;" in html)
    _require(failures, "extension editor lifecycle health reaches payload", "payload.extensionCustomEditorLifecycleActions=runtimeSummary.customEditorLifecycleActions||0;" in html and "payload.extensionNotebookExecutionReady=runtimeSummary.notebookExecutionReady||0;" in html and "Editor Surfaces" in html)

    _require(failures, "workspace health exists", "aiEditorWorkspaceHealthSnapshot" in html)
    _require(failures, "workspace autosync exists", "ai-editor-workspace-autosync" in html)
    _require(failures, "workspace autosync respects explicit roots", "configured_root" in html and "auto_detect === false" in html)
    _require(failures, "workspace health uses terminal runtime and sync", "terminalRuntimeSnapshot" in html and "terminalWorkspaceSyncSnapshot" in html and "workspaceSyncProtected" in html and "workspaceTerminalRunning" in html)
    _require(failures, "workspace autosync source reaches payload", "workspaceAutosyncSource" in html and "workspaceAutosyncError" in html and "payload.workspaceAutosyncSource=workspace.workspaceAutosyncSource||'';" in html)

    _require(failures, "language and diff health exists", "ai-editor-language-diff-health" in html)
    _require(failures, "language and diff health exposes snapshot", "aiEditorLanguageDiffHealthSnapshot" in html)
    _require(failures, "language inference includes common IDE languages", "typescriptreact" in html and "powershell" in html and "python" in html)
    _require(failures, "language action health exists", "ai-editor-language-action-health" in html)
    _require(failures, "language action guard wraps format quickfix diff actions", "formatDocument" in html and "applyQuickFix" in html and "toggleDiffView" in html)
    _require(failures, "language action health exposes snapshot", "aiEditorLanguageActionHealthSnapshot" in html)
    _require(failures, "language diff health exposes action capabilities", "formatActionAvailable" in html and "quickFixActionAvailable" in html and "diffActionAvailable" in html)
    _require(failures, "diff visual fallback css exists", "ai-editor-diff-visual-fallback-css" in html)
    _require(failures, "diff visual fallback includes red and green states", "ai-editor-diff-added" in html and "ai-editor-diff-removed" in html)
    _require(failures, "aggregate diagnostics includes language diff", "languageDiff" in html)
    _require(failures, "aggregate diagnostics includes language actions", "languageActions: safeCall(\"aiEditorLanguageActionHealthSnapshot\")" in html)
    _require(failures, "aggregate diagnostics includes language feature aggregate", "languageFeatures: safeCall(\"editorLanguageFeatureAggregateSnapshot\")" in html)
    _require(failures, "language feature aggregate reaches health payload", "payload.languageSelectionRanges=languageFeatures.selectionRanges||0;" in html and "payload.languageDocumentColors=languageFeatures.documentColors||0;" in html and "payload.languageColorPresentationApplied=languageFeatures.colorPresentationApplied===true;" in html)

    _require(failures, "corrupt settings.json recovery is surfaced to the user on init", "c._settings_load_error" in html and "settings.json.corrupt" in html)
    _require(failures, "settings polish css exists", "ai-editor-settings-polish-css" in html)
    _require(failures, "settings health snapshot exists", "aiEditorSettingsHealthSnapshot" in html)
    _require(failures, "settings overflow guard exists", "overflow-y: auto !important" in html and "scrollbar-gutter: stable" in html)
    _require(failures, "settings modal avoids generic row layout overrides", "body[data-ai-editor-settings-visible=\"true\"] #settings-modal.open .settings-row" not in html and "body[data-ai-editor-settings-visible=\"true\"] #settings-modal.open .setting-row" not in html)
    _require(failures, "settings builtin rows stay single-column in calm mode", "#settings-modal.open .settings-vscode-calm .settings-field.builtin-setting" in html and "grid-template-columns: minmax(0, 1fr) !important;" in html and "grid-row: auto !important;" in html)
    _require(failures, "settings health tracks top clipping", "settingsHealthTopClipped" in html)
    _require(failures, "settings modal layout is scoped to real settings modal", "#settings-modal.open .settings-vscode-calm .settings-shell" in html and "#settings-modal.open .settings-vscode-calm .settings-nav" in html and "#settings-modal.open .settings-vscode-calm .settings-main" in html)
    _require(failures, "settings health tracks visible navigation categories", "navVisibleCategoryCount" in html and "settingsHealthNavVisibleCategoryCount" in html)
    _require(failures, "settings health tracks column overlap", "columnsOverlap" in html and "settingsHealthColumnsOverlap" in html)
    _require(failures, "settings health tracks sidebar search and readable columns", "navHasSearchbar" in html and "navSearchbarVisible" in html and "readableColumns" in html)
    _require(failures, "settings health tracks usable nav and main columns", "navUsable" in html and "mainUsable" in html and "settingsHealthNavUsable" in html and "settingsHealthMainUsable" in html)
    _require(failures, "settings final calm layout keeps left nav and readable main", "--settings-calm-sidebar-width: 300px;" in html and "grid-template-columns: var(--settings-calm-sidebar-width) minmax(0, 1fr) !important;" in html and "max-width: var(--settings-calm-content-width) !important;" in html)
    _require(failures, "settings calm row header avoids compressed single line", "grid-template-areas:" in html and '"category title state"' in html and "white-space: normal !important;" in html)
    _require(failures, "settings layout health reaches payload", "payload.settingsNavVisibleCategoryCount=settings.navVisibleCategoryCount||0;" in html and "payload.settingsColumnsOverlap=settings.columnsOverlap===true;" in html and "payload.settingsNavHasSearchbar=settings.navHasSearchbar===true;" in html and "payload.settingsReadableColumns=settings.readableColumns===true;" in html and "payload.settingsNavUsable=settings.navUsable===true;" in html and "payload.settingsMainUsable=settings.mainUsable===true;" in html)
    _require(failures, "settings health tracks target state", "currentTarget: typeof currentSettingsTarget === \"string\" ? currentSettingsTarget : \"\"" in html and "document.body.dataset.settingsHealthTarget = String(snap.currentTarget || \"\");" in html and "payload.settingsCurrentTarget=settings.currentTarget||settings.target||'';" in html)
    _require(failures, "aggregate diagnostics includes settings", "settings: safeCall(\"aiEditorSettingsHealthSnapshot\")" in html)
    _require(failures, "settings control health exists", "aiEditorSettingsControlHealthSnapshot" in html)
    _require(failures, "settings control health tracks search categories dirty state", "settingsSearchPresent" in html and "settingsCategoryCount" in html and "settingsDirtyCount" in html)
    _require(failures, "settings control health exposes unsaved state", "settingsHasUnsavedChanges" in html)
    _require(failures, "settings control health tracks control types", "checkboxCount" in html and "selectCount" in html and "sliderCount" in html and "colorControlCount" in html)
    _require(failures, "settings control health tracks save reset entries", "settingsSaveEntryPresent" in html and "settingsResetEntryPresent" in html)
    _require(failures, "aggregate diagnostics includes settings controls", "settingsControls: safeCall(\"aiEditorSettingsControlHealthSnapshot\")" in html)
    _require(failures, "settings accounts section is wired to the authentication session API", (
        'data-settings-id="accounts" data-settings-title="Accounts"' in html
        and "async function renderAuthSessions(){" in html
        and "call('list_auth_sessions',pid)" in html
        and "call('create_auth_session',pid,token,label)" in html
        and "call('remove_auth_session',providerId,sessionId)" in html
        and "window.renderAuthSessions=renderAuthSessions;" in html
    ))
    _require(failures, "settings tool permission dropdown calls set_tool_permission on change", (
        "class=\"perm-select\"" in html
        and "sel.onchange=async()=>{" in html
        and "call('set_tool_permission',tool,value||'default')" in html
    ))

    _require(failures, "assistant polish css exists", "ai-editor-assistant-polish-css" in html)
    _require(failures, "assistant health snapshot exists", "aiEditorAssistantHealthSnapshot" in html)
    _require(failures, "assistant health checks provider model workflow agent", "providerPresent" in html and "modelPresent" in html and "workflowPresent" in html and "agentPresent" in html)
    _require(failures, "assistant health exposes provider model workflow agent values", "providerValue" in html and "modelValue" in html and "workflowValue" in html and "agentValue" in html)
    _require(failures, "assistant health exposes option counts", "providerOptionCount" in html and "modelOptionCount" in html and "workflowOptionCount" in html and "agentOptionCount" in html)
    _require(failures, "assistant workflow mode supports on off custom", "workflowMode" in html and '"custom"' in html and '"off"' in html and '"on"' in html)
    _require(failures, "assistant workflow session timeline exists", "assistantWorkflowTimelineRuns" in html and "assistantWorkflowTimelineSnapshot" in html and "chat-workflow-session-timeline" in html and "chat-workflow-session-timeline-item" in html)
    _require(failures, "assistant workflow timeline health reaches payload", "workflowTimelineCount" in html and "workflowTimelineItemCount" in html and "payload.assistantWorkflowTimelineCount=assistant.workflowTimelineCount||0;" in html and "payload.assistantWorkflowLatestStatus=assistant.workflowLatestStatus||'';" in html)
    _require(failures, "assistant workflow retry targets the failed step, not a full restart", "function workflowRetryStepPlan(last){" in html and "retryOpts.stepIndex!=null" in html and "call('retry_workflow_step'" in html)
    _require(failures, "assistant workflow run supports pause and resume", "async function pauseAssistantWorkflowRun(){" in html and "async function resumeAssistantWorkflowRun(){" in html and "call('pause_workflow'" in html and "call('resume_workflow'" in html)
    _require(failures, "assistant workflow human-confirmation steps render a confirm bar", "function renderWorkflowConfirmBar(container,d,scrollEl){" in html and "onWorkflowConfirmationNeeded" in html and "call('confirm_workflow_step'" in html)
    _require(failures, "assistant workflow retry/continue state persists across reload", "function saveAssistantWorkflowLastLaunch(){" in html and "function restoreAssistantWorkflowLastLaunch(){" in html and "restoreAssistantWorkflowLastLaunch();" in html)
    _require(failures, "assistant token and context compact numbers exist", "tokenEstimateCompact" in html and "contextWindowCompact" in html and "compactNumber" in html)
    _require(failures, "assistant compact numbers are exposed on dom", "data-token-count-compact" in html and "data-context-window-compact" in html)
    _require(failures, "assistant mutation observer exists", "new MutationObserver" in html and "installAssistantObserver" in html)
    _require(failures, "assistant observer can be disposed", "aiEditorDisposeAssistantObserver" in html and ".disconnect()" in html)
    _require(failures, "assistant command palette entries exist", "Chat: Focus Assistant" in html and "Chat: Copy Composer Context" in html and "Chat: Run Workflow" in html and "Developer: Run Assistant UI Self Check" in html)
    _require(failures, "assistant health exposes composer and response state", "draftLength" in html and "queuedRequestCount" in html and "nativeCardCount" in html and "actionTrayVisible" in html and "commandPaletteChatEntryCount" in html)
    _require(failures, "assistant detailed health reaches payload", "payload.assistantDraftLength=assistant.draftLength||0;" in html and "payload.assistantNativeCardCount=assistant.nativeCardCount||0;" in html and "payload.assistantCommandPaletteChatEntryCount=assistant.commandPaletteChatEntryCount||0;" in html)
    _require(failures, "aggregate diagnostics includes assistant", "assistant: safeCall(\"aiEditorAssistantHealthSnapshot\")" in html)
    restricted_prompt_terms = ("safety", "policy", "refuse", "审核", "限制", "禁止")
    _require(failures, "assistant scripts do not inject restricted prompt text", not any(term in assistant_scripts.lower() for term in restricted_prompt_terms[:3]) and not any(term in assistant_scripts for term in restricted_prompt_terms[3:]))

    _require(failures, "extension surface diagnostics exist", "aiEditorExtensionSurfaceHealthSnapshot" in html)
    _require(failures, "extension surface diagnostics include dynamic surface selectors", "data-extension-runtime-surface" in html and "extension-webview" in html)
    _require(failures, "extension surface health can use registry", "registryBacked" in html)
    _require(failures, "extension surface registry exists", "ai-editor-extension-surface-registry" in html)
    _require(failures, "extension surface registry observes dynamic nodes", "new MutationObserver" in html and "aiEditorInstallExtensionSurfaceRegistry" in html)
    _require(failures, "extension surface registry exposes snapshot", "aiEditorExtensionSurfaceRegistrySnapshot" in html)
    _require(failures, "extension surface registry can be disposed", "aiEditorDisposeExtensionSurfaceRegistry" in html and "registryObserver.disconnect()" in html)
    _require(failures, "aggregate diagnostics includes extension registry", "extensionSurfaceRegistry: safeCall(\"aiEditorExtensionSurfaceRegistrySnapshot\")" in html)
    _require(failures, "extension surface registry css exists", "ai-editor-extension-surface-registry-css" in html)
    _require(
        failures,
        "extension registry does not create static plugin windows",
        "createElement(\"iframe\")" not in extension_registry_scripts
        and "createElement('iframe')" not in extension_registry_scripts,
    )

    _require(failures, "diagnostics maintenance exists", "ai-editor-diagnostics-maintenance" in html)
    _require(failures, "diagnostics maintenance trims duplicate api health nodes", "aiEditorTrimDiagnosticsDom" in html and "ai-editor-api-health" in html)
    _require(failures, "diagnostics maintenance can be disposed", "aiEditorDisposeDiagnostics" in html and "clearInterval(maintenanceTimer)" in html)
    _require(failures, "diagnostics maintenance is low frequency", "}, 15000)" in html)
    _require(failures, "lifecycle cleanup exists", "ai-editor-lifecycle-cleanup" in html)
    _require(failures, "lifecycle cleanup handles pagehide and beforeunload", "pagehide" in html and "beforeunload" in html and "aiEditorCleanupDiagnosticsForLifecycle" in html)
    _require(failures, "lifecycle restore reinstalls dynamic registries", "aiEditorRestoreDiagnosticsForLifecycle" in html and "aiEditorInstallExtensionSurfaceRegistry" in html)

    _require(failures, "health checks are not high-frequency polling", "setInterval(interactionWatchdog, 800)" in html)
    _require(failures, "health work can be scheduled during idle time", "aiEditorScheduleHealthWork" in html and "requestIdleCallback" in html)

    _require(failures, "perf budget tracker exists", "function recordAiEditorPerfSample(name,durationMs)" in html and "function aiEditorPerfBudgetSnapshot()" in html)
    _require(failures, "perf budget tracker is exported for the health aggregate", "window.recordAiEditorPerfSample=recordAiEditorPerfSample;" in html and "window.aiEditorPerfBudgetSnapshot=aiEditorPerfBudgetSnapshot;" in html and 'perfBudgets: safeCall("aiEditorPerfBudgetSnapshot")' in html)
    _require(failures, "settings open time is sampled against its plan budget", "const _settingsOpenT0=performance.now();" in html and "recordAiEditorPerfSample('settingsOpenVisible'" in html and "settingsOpenVisible:400" in html)
    _require(failures, "language provider round trip is sampled against its plan budget", "const _providerT0=performance.now();" in html and "recordAiEditorPerfSample('languageProvider'" in html and "languageProvider:1500" in html)
    _require(failures, "perf budget over-budget state reaches the health payload", "payload.perfBudgetOverCount=perfBudgets.overBudgetCount||0;" in html and "payload.perfBudgetOverNames=perfBudgets.overBudgetNames||'';" in html)

    if duplicate_script_ids:
        failures.append(f"duplicate script ids: {', '.join(duplicate_script_ids)}")
    if duplicate_style_ids:
        failures.append(f"duplicate style ids: {', '.join(duplicate_style_ids)}")
    failures.extend(script_syntax_failures)

    return failures


def _print_parity_snapshot() -> None:
    snapshot = compute_parity_snapshot()
    print("── AI IDE VS Code parity snapshot (阶段0) ──")
    for phase, bucket in snapshot["phases"].items():
        marker = {"ready": "✓", "partial": "~", "missing": "✗"}.get(bucket["status"], "?")
        print(f"  {marker} {phase}: {bucket['passed']}/{bucket['total']} ({bucket['status']})")
    if snapshot["top_gaps"]:
        print("  Next gaps to fix:")
        for index, gap in enumerate(snapshot["top_gaps"], 1):
            print(f"    {index}. {gap}")
    else:
        print("  No frontend-health gaps recorded this run.")


def main() -> int:
    failures = run_frontend_health_selftest()
    if failures:
        print("AI Editor frontend health selftest failed.")
        print("Failure points:")
        for index, failure in enumerate(failures, 1):
            print(f"{index}. {failure}")
        _print_parity_snapshot()
        return 1
    print("AI Editor frontend health selftest passed.")
    _print_parity_snapshot()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
