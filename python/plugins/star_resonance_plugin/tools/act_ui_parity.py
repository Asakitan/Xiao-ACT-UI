# Shared ACT capability parity contract for WebView and Entity UIs.
#
# This module is intentionally UI-framework neutral.  It defines the canonical
# capability matrix once, then lets WebView and Entity adapters declare how they
# expose the same capabilities, actions, and payload fields.
#
# Run from ``sao_auto`` with::
#
# python -m tools.act_ui_parity
#
# The command exits non-zero if the default WebView/Entity contracts drift.

from __future__ import annotations

from dataclasses import dataclass, field
import json
import sys
from typing import Any, Mapping, Sequence


UiKind = str


WEBVIEW_UI: UiKind = "webview"
ENTITY_UI: UiKind = "entity"


@dataclass(frozen=True)
class CapabilitySpec:
    # Canonical ACT capability shared by every UI surface.

    capability_id: str
    title: str
    required_actions: tuple[str, ...]
    required_payload_fields: tuple[str, ...]
    webview_render_hint: str
    entity_render_hint: str


@dataclass(frozen=True)
class CapabilityBinding:
    # One UI adapter's exposure of a canonical ACT capability.

    capability_id: str
    actions: tuple[str, ...]
    payload_fields: tuple[str, ...]
    render_hint: str
    route: str
    metadata: Mapping[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class UiAdapterContract:
    # Declared capability surface for one UI adapter.

    ui_kind: UiKind
    bindings: Mapping[str, CapabilityBinding]


@dataclass(frozen=True)
class ParityIssue:
    # A validation problem that must block ACT UI delivery.

    code: str
    message: str
    ui_kind: UiKind | None = None
    capability_id: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "message": self.message,
            "ui_kind": self.ui_kind,
            "capability_id": self.capability_id,
        }


def build_capability_registry() -> dict[str, CapabilitySpec]:
    # Return the canonical ACT capability matrix.
    #
    # The registry is the source of truth.  A feature is not accepted as done
    # unless both WebView and Entity expose every capability ID here with the same
    # required actions and shared payload fields.

    specs = (
        CapabilitySpec(
            capability_id="live_overview",
            title="Live overview",
            required_actions=("open", "refresh", "filter", "pin"),
            required_payload_fields=(
                "encounter_id",
                "combatants",
                "duration_ms",
                "totals",
                "filters",
                "updated_at_ms",
            ),
            webview_render_hint="dashboard cards and sortable grid",
            entity_render_hint="compact entity panel with the same grid columns",
        ),
        CapabilitySpec(
            capability_id="combatant_drilldown",
            title="Combatant drilldown",
            required_actions=("open", "back", "filter", "focus_target"),
            required_payload_fields=(
                "encounter_id",
                "combatant_id",
                "summary",
                "skills",
                "incoming",
                "outgoing",
            ),
            webview_render_hint="details page with tabs",
            entity_render_hint="entity detail panel with equivalent tabs/sections",
        ),
        CapabilitySpec(
            capability_id="skill_drilldown",
            title="Skill drilldown",
            required_actions=("open", "back", "filter", "copy"),
            required_payload_fields=(
                "encounter_id",
                "combatant_id",
                "skill_id",
                "casts",
                "hits",
                "crit_rate",
                "timeline_refs",
            ),
            webview_render_hint="skill detail table and chart",
            entity_render_hint="skill detail table plus compact trend rows",
        ),
        CapabilitySpec(
            capability_id="action_log",
            title="Action log",
            required_actions=("open", "search", "filter", "jump_to_time", "copy"),
            required_payload_fields=(
                "encounter_id",
                "rows",
                "columns",
                "filters",
                "cursor",
            ),
            webview_render_hint="virtualized searchable table",
            entity_render_hint="paged/searchable entity table",
        ),
        CapabilitySpec(
            capability_id="death_recap",
            title="Death recap",
            required_actions=("open", "refresh", "filter", "copy"),
            required_payload_fields=(
                "encounter_id",
                "death",
                "rows",
                "summary",
                "window",
                "filters",
            ),
            webview_render_hint="death-window report table",
            entity_render_hint="compact death recap panel with same summary rows",
        ),
        CapabilitySpec(
            capability_id="graph_timeseries",
            title="Graph and timeseries",
            required_actions=("open", "select_metric", "zoom", "filter", "export"),
            required_payload_fields=(
                "encounter_id",
                "series",
                "metrics",
                "time_range_ms",
                "filters",
            ),
            webview_render_hint="rich chart canvas",
            entity_render_hint="compact chart/table representation with identical controls",
        ),
        CapabilitySpec(
            capability_id="encounter_timeline_vcr",
            title="Encounter timeline / VCR",
            required_actions=("open", "play", "pause", "step", "seek", "set_speed", "filter"),
            required_payload_fields=(
                "encounter_id",
                "events",
                "cursor_ms",
                "speed",
                "filters",
            ),
            webview_render_hint="timeline player",
            entity_render_hint="compact VCR controls plus event list",
        ),
        CapabilitySpec(
            capability_id="history_browser",
            title="History browser",
            required_actions=("open", "search", "filter", "load", "delete"),
            required_payload_fields=(
                "encounters",
                "filters",
                "cursor",
                "storage_status",
            ),
            webview_render_hint="history page",
            entity_render_hint="history menu/list with equivalent actions",
        ),
        CapabilitySpec(
            capability_id="export",
            title="Export",
            required_actions=("open", "choose_format", "copy", "save"),
            required_payload_fields=(
                "encounter_id",
                "formats",
                "selected_format",
                "preview",
            ),
            webview_render_hint="export dialog",
            entity_render_hint="export menu/detail panel",
        ),
        CapabilitySpec(
            capability_id="mini_parse",
            title="Mini-Parse and clipboard formatters",
            required_actions=("open", "select_formatter", "preview", "copy"),
            required_payload_fields=(
                "formatter_id",
                "formatters",
                "preview",
                "text",
                "errors",
            ),
            webview_render_hint="mini-parse formatter selector and copy action",
            entity_render_hint="compact Mini Copy action plus formatter/status payload",
        ),
        CapabilitySpec(
            capability_id="selective_parsing",
            title="Selective Parsing",
            required_actions=("open", "enable", "disable", "update", "clear"),
            required_payload_fields=(
                "enabled",
                "mode",
                "policy",
                "filters",
                "last_decision",
            ),
            webview_render_hint="filter management panel backed by shared runtime policy",
            entity_render_hint="entity status/action surface backed by the same policy",
        ),
        CapabilitySpec(
            capability_id="triggers_timers",
            title="Triggers and timers",
            required_actions=("open", "enable", "disable", "reload", "test"),
            required_payload_fields=(
                "triggers",
                "timers",
                "enabled",
                "last_reload_ms",
                "errors",
            ),
            webview_render_hint="trigger/timer management page",
            entity_render_hint="trigger/timer management panel with same operations",
        ),
        CapabilitySpec(
            capability_id="plugin_manager",
            title="Plugin manager",
            required_actions=("list_plugins", "enable", "disable", "reload", "status"),
            required_payload_fields=(
                "plugins",
                "enabled",
                "status",
                "version",
                "errors",
            ),
            webview_render_hint="plugin manager page",
            entity_render_hint="plugin menu/detail panel with identical operations",
        ),
        CapabilitySpec(
            capability_id="data_source_health",
            title="Data-source health",
            required_actions=("open", "refresh", "diagnose", "copy"),
            required_payload_fields=(
                "sources",
                "status",
                "latency_ms",
                "last_event_ms",
                "errors",
            ),
            webview_render_hint="health dashboard",
            entity_render_hint="health status panel with diagnostics actions",
        ),
        CapabilitySpec(
            capability_id="offline_import",
            title="Offline import",
            required_actions=("open", "choose_file", "import", "cancel", "status"),
            required_payload_fields=(
                "accepted_formats",
                "selected_file",
                "progress",
                "status",
                "errors",
            ),
            webview_render_hint="offline import wizard",
            entity_render_hint="offline import panel with same import/status flow",
        ),
    )
    return {spec.capability_id: spec for spec in specs}


def build_default_adapter_contract(
    ui_kind: UiKind,
    registry: Mapping[str, CapabilitySpec] | None = None,
) -> UiAdapterContract:
    # Build the current expected contract for a first-party UI adapter.

    if ui_kind not in {WEBVIEW_UI, ENTITY_UI}:
        raise ValueError(f"unsupported ACT UI kind: {ui_kind!r}")

    registry = registry or build_capability_registry()
    bindings: dict[str, CapabilityBinding] = {}
    for spec in registry.values():
        if ui_kind == WEBVIEW_UI:
            route = f"/act/{spec.capability_id}"
            render_hint = spec.webview_render_hint
        else:
            route = f"entity://act/{spec.capability_id}"
            render_hint = spec.entity_render_hint

        bindings[spec.capability_id] = CapabilityBinding(
            capability_id=spec.capability_id,
            actions=spec.required_actions,
            payload_fields=spec.required_payload_fields,
            render_hint=render_hint,
            route=route,
        )

    return UiAdapterContract(ui_kind=ui_kind, bindings=bindings)


def build_default_contracts() -> tuple[UiAdapterContract, UiAdapterContract]:
    # Return the baseline WebView and Entity adapter contracts.

    registry = build_capability_registry()
    return (
        build_default_adapter_contract(WEBVIEW_UI, registry),
        build_default_adapter_contract(ENTITY_UI, registry),
    )


def validate_adapter_contract(
    adapter: UiAdapterContract,
    registry: Mapping[str, CapabilitySpec] | None = None,
) -> list[ParityIssue]:
    # Validate one UI adapter against the canonical registry.

    registry = registry or build_capability_registry()
    issues: list[ParityIssue] = []
    expected_ids = set(registry)
    actual_ids = set(adapter.bindings)

    for capability_id in sorted(expected_ids - actual_ids):
        issues.append(
            ParityIssue(
                code="missing_capability",
                message=f"{adapter.ui_kind} is missing capability {capability_id}",
                ui_kind=adapter.ui_kind,
                capability_id=capability_id,
            )
        )

    for capability_id in sorted(actual_ids - expected_ids):
        issues.append(
            ParityIssue(
                code="unknown_capability",
                message=f"{adapter.ui_kind} declares unknown capability {capability_id}",
                ui_kind=adapter.ui_kind,
                capability_id=capability_id,
            )
        )

    for capability_id in sorted(expected_ids & actual_ids):
        spec = registry[capability_id]
        binding = adapter.bindings[capability_id]
        if binding.capability_id != capability_id:
            issues.append(
                ParityIssue(
                    code="binding_id_mismatch",
                    message=(
                        f"{adapter.ui_kind} binding key {capability_id} points to "
                        f"{binding.capability_id}"
                    ),
                    ui_kind=adapter.ui_kind,
                    capability_id=capability_id,
                )
            )

        missing_actions = set(spec.required_actions) - set(binding.actions)
        for action in sorted(missing_actions):
            issues.append(
                ParityIssue(
                    code="missing_action",
                    message=f"{adapter.ui_kind} {capability_id} lacks action {action}",
                    ui_kind=adapter.ui_kind,
                    capability_id=capability_id,
                )
            )

        missing_fields = set(spec.required_payload_fields) - set(binding.payload_fields)
        for field_name in sorted(missing_fields):
            issues.append(
                ParityIssue(
                    code="missing_payload_field",
                    message=f"{adapter.ui_kind} {capability_id} lacks payload field {field_name}",
                    ui_kind=adapter.ui_kind,
                    capability_id=capability_id,
                )
            )

        if not binding.route:
            issues.append(
                ParityIssue(
                    code="missing_route",
                    message=f"{adapter.ui_kind} {capability_id} has no route/entry point",
                    ui_kind=adapter.ui_kind,
                    capability_id=capability_id,
                )
            )

        if not binding.render_hint:
            issues.append(
                ParityIssue(
                    code="missing_render_hint",
                    message=f"{adapter.ui_kind} {capability_id} has no render hint",
                    ui_kind=adapter.ui_kind,
                    capability_id=capability_id,
                )
            )

    return issues


def validate_ui_parity(
    webview: UiAdapterContract,
    entity: UiAdapterContract,
    registry: Mapping[str, CapabilitySpec] | None = None,
) -> list[ParityIssue]:
    # Validate that WebView and Entity expose an equivalent ACT surface.

    registry = registry or build_capability_registry()
    issues = [
        *validate_adapter_contract(webview, registry),
        *validate_adapter_contract(entity, registry),
    ]

    if webview.ui_kind != WEBVIEW_UI:
        issues.append(
            ParityIssue(
                code="wrong_webview_kind",
                message=f"expected first adapter kind {WEBVIEW_UI}, got {webview.ui_kind}",
                ui_kind=webview.ui_kind,
            )
        )
    if entity.ui_kind != ENTITY_UI:
        issues.append(
            ParityIssue(
                code="wrong_entity_kind",
                message=f"expected second adapter kind {ENTITY_UI}, got {entity.ui_kind}",
                ui_kind=entity.ui_kind,
            )
        )

    for capability_id in sorted(set(webview.bindings) & set(entity.bindings)):
        web_binding = webview.bindings[capability_id]
        entity_binding = entity.bindings[capability_id]
        if set(web_binding.actions) != set(entity_binding.actions):
            issues.append(
                ParityIssue(
                    code="action_parity_mismatch",
                    message=(
                        f"{capability_id} action sets differ: "
                        f"webview={sorted(web_binding.actions)} "
                        f"entity={sorted(entity_binding.actions)}"
                    ),
                    capability_id=capability_id,
                )
            )
        if set(web_binding.payload_fields) != set(entity_binding.payload_fields):
            issues.append(
                ParityIssue(
                    code="payload_parity_mismatch",
                    message=(
                        f"{capability_id} payload fields differ: "
                        f"webview={sorted(web_binding.payload_fields)} "
                        f"entity={sorted(entity_binding.payload_fields)}"
                    ),
                    capability_id=capability_id,
                )
            )

    return issues


def contract_to_dict(adapter: UiAdapterContract) -> dict[str, Any]:
    # Serialize an adapter contract for selftest/report output.

    return {
        "ui_kind": adapter.ui_kind,
        "capabilities": [
            {
                "capability_id": binding.capability_id,
                "actions": list(binding.actions),
                "payload_fields": list(binding.payload_fields),
                "render_hint": binding.render_hint,
                "route": binding.route,
                "metadata": dict(binding.metadata),
            }
            for _, binding in sorted(adapter.bindings.items())
        ],
    }


def registry_to_dict(registry: Mapping[str, CapabilitySpec]) -> dict[str, Any]:
    # Serialize the canonical registry for diagnostics.

    return {
        capability_id: {
            "title": spec.title,
            "required_actions": list(spec.required_actions),
            "required_payload_fields": list(spec.required_payload_fields),
            "webview_render_hint": spec.webview_render_hint,
            "entity_render_hint": spec.entity_render_hint,
        }
        for capability_id, spec in sorted(registry.items())
    }


def run_selftest() -> dict[str, Any]:
    # Run the baseline ACT WebView/Entity parity selftest.

    registry = build_capability_registry()
    webview, entity = build_default_contracts()
    issues = validate_ui_parity(webview, entity, registry)
    return {
        "ok": not issues,
        "summary": {
            "capability_count": len(registry),
            "webview_capability_count": len(webview.bindings),
            "entity_capability_count": len(entity.bindings),
            "issue_count": len(issues),
        },
        "issues": [issue.to_dict() for issue in issues],
        "registry": registry_to_dict(registry),
        "adapters": {
            WEBVIEW_UI: contract_to_dict(webview),
            ENTITY_UI: contract_to_dict(entity),
        },
    }


def assert_ui_parity(
    webview: UiAdapterContract,
    entity: UiAdapterContract,
    registry: Mapping[str, CapabilitySpec] | None = None,
) -> None:
    # Raise ``AssertionError`` if the two UI adapters are not 1:1.

    issues = validate_ui_parity(webview, entity, registry)
    if issues:
        details = "; ".join(issue.message for issue in issues)
        raise AssertionError(f"ACT UI parity contract failed: {details}")


def main(argv: Sequence[str] | None = None) -> int:
    # CLI entry point used by smoke tests and future ``act_replay.selftest``.

    _ = argv
    report = run_selftest()
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
