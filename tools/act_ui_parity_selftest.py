# -*- coding: utf-8 -*-
"""Unittest regression coverage for the ACT dual-UI parity contract."""

from __future__ import annotations

import unittest

from tools.act_ui_parity import (
    ENTITY_UI,
    WEBVIEW_UI,
    CapabilityBinding,
    UiAdapterContract,
    assert_ui_parity,
    build_capability_registry,
    build_default_adapter_contract,
    build_default_contracts,
    run_selftest,
    validate_ui_parity,
)


class ActUiParityTests(unittest.TestCase):
    def test_default_webview_entity_contracts_are_1_to_1(self) -> None:
        webview, entity = build_default_contracts()

        self.assertEqual(webview.ui_kind, WEBVIEW_UI)
        self.assertEqual(entity.ui_kind, ENTITY_UI)
        self.assertEqual(validate_ui_parity(webview, entity), [])
        assert_ui_parity(webview, entity)

    def test_capability_registry_contains_required_act_surface(self) -> None:
        registry = build_capability_registry()

        self.assertEqual(set(registry), {
            "live_overview",
            "combatant_drilldown",
            "skill_drilldown",
            "action_log",
            "death_recap",
            "graph_timeseries",
            "encounter_timeline_vcr",
            "history_browser",
            "export",
            "mini_parse",
            "selective_parsing",
            "triggers_timers",
            "plugin_manager",
            "data_source_health",
            "offline_import",
        })
        for spec in registry.values():
            self.assertTrue(spec.required_actions)
            self.assertTrue(spec.required_payload_fields)
            self.assertTrue(spec.webview_render_hint)
            self.assertTrue(spec.entity_render_hint)

    def test_parity_validator_rejects_entity_action_drift(self) -> None:
        registry = build_capability_registry()
        webview = build_default_adapter_contract(WEBVIEW_UI, registry)
        entity = build_default_adapter_contract(ENTITY_UI, registry)
        plugin_binding = entity.bindings["plugin_manager"]
        drifted_entity = UiAdapterContract(
            ui_kind=ENTITY_UI,
            bindings={
                **entity.bindings,
                "plugin_manager": CapabilityBinding(
                    capability_id=plugin_binding.capability_id,
                    actions=tuple(
                        action for action in plugin_binding.actions if action != "reload"
                    ),
                    payload_fields=plugin_binding.payload_fields,
                    render_hint=plugin_binding.render_hint,
                    route=plugin_binding.route,
                ),
            },
        )

        issues = validate_ui_parity(webview, drifted_entity, registry)

        self.assertTrue(any(
            issue.code == "missing_action"
            and issue.ui_kind == ENTITY_UI
            and issue.capability_id == "plugin_manager"
            for issue in issues
        ))
        self.assertTrue(any(
            issue.code == "action_parity_mismatch"
            and issue.capability_id == "plugin_manager"
            for issue in issues
        ))
        with self.assertRaises(AssertionError):
            assert_ui_parity(webview, drifted_entity, registry)

    def test_selftest_report_is_machine_readable(self) -> None:
        report = run_selftest()

        self.assertIs(report["ok"], True)
        self.assertEqual(report["summary"]["capability_count"], 15)
        self.assertEqual(report["summary"]["webview_capability_count"], 15)
        self.assertEqual(report["summary"]["entity_capability_count"], 15)
        self.assertEqual(report["summary"]["issue_count"], 0)
        self.assertIn(WEBVIEW_UI, report["adapters"])
        self.assertIn(ENTITY_UI, report["adapters"])


if __name__ == "__main__":
    unittest.main()
