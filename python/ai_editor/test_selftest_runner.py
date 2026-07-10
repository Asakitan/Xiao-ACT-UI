from __future__ import annotations

import io
import subprocess
import unittest
from contextlib import redirect_stdout

from ai_editor import selftest_runner


class SelftestRunnerDiscoveryTests(unittest.TestCase):
    def test_empty_focused_discovery_fails_closed(self) -> None:
        failures = selftest_runner.run_unittest_discovery(
            suite=unittest.TestSuite(),
            stream=io.StringIO(),
        )

        self.assertEqual(failures, ["no test_*.py tests were discovered"])

    def test_discovery_targets_only_test_modules(self) -> None:
        suite = selftest_runner.discover_unittest_suite()

        def test_ids(node: unittest.TestSuite) -> list[str]:
            ids: list[str] = []
            for item in node:
                if isinstance(item, unittest.TestSuite):
                    ids.extend(test_ids(item))
                else:
                    ids.append(item.id())
            return ids

        discovered = test_ids(suite)
        self.assertTrue(discovered)
        self.assertTrue(all(".test_" in test_id for test_id in discovered))
        self.assertFalse(any(
            test_id.startswith("ai_editor.selftest_runner.")
            for test_id in discovered
        ))


class SelftestRunnerBrowserSmokeTests(unittest.TestCase):
    def test_browser_smoke_propagates_process_failure(self) -> None:
        def failed_run(*_args, **_kwargs):
            return subprocess.CompletedProcess(
                args=["node"], returncode=7,
                stdout="browser stdout", stderr="browser sentinel",
            )

        failures = selftest_runner.run_browser_smoke(
            "assistant",
            node_path="node-test",
            run_process=failed_run,
            stream=io.StringIO(),
        )

        self.assertEqual(len(failures), 1)
        self.assertIn("assistant", failures[0])
        self.assertIn("exit 7", failures[0])
        self.assertIn("browser sentinel", failures[0])

    def test_browser_smoke_skip_is_not_green(self) -> None:
        def skipped_run(*_args, **_kwargs):
            return subprocess.CompletedProcess(
                args=["node"], returncode=0,
                stdout="SKIP assistant-ui-browser-smoke playwright unavailable",
                stderr="",
            )

        failures = selftest_runner.run_browser_smoke(
            "assistant",
            node_path="node-test",
            run_process=skipped_run,
            stream=io.StringIO(),
        )

        self.assertEqual(len(failures), 1)
        self.assertIn("did not report", failures[0])
        self.assertIn("PASS assistant-ui-browser-smoke", failures[0])

    def test_browser_smoke_requires_and_accepts_explicit_pass_marker(self) -> None:
        output = io.StringIO()

        def passed_run(*_args, **_kwargs):
            return subprocess.CompletedProcess(
                args=["node"], returncode=0,
                stdout="PASS settings-ui-browser-smoke rows=53",
                stderr="",
            )

        failures = selftest_runner.run_browser_smoke(
            "settings",
            node_path="node-test",
            run_process=passed_run,
            stream=output,
        )

        self.assertEqual(failures, [])
        self.assertIn("Browser smoke passed: settings.", output.getvalue())


class SelftestRunnerReleaseGateTests(unittest.TestCase):
    def test_release_gate_includes_every_real_validation_layer(self) -> None:
        names = [name for name, _check in selftest_runner.release_checks()]

        self.assertEqual(names, [
            "legacy selftest",
            "focused unittest discovery",
            "frontend health selftest",
            "assistant browser smoke",
            "settings browser smoke",
        ])

    def test_release_gate_returns_one_and_summarizes_any_layer_failure(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            exit_code = selftest_runner.main(checks=[
                ("passing layer", lambda: []),
                ("failing layer", lambda: ["failure sentinel"]),
            ])

        rendered = output.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("Release gate: passing layer", rendered)
        self.assertIn("Release gate: failing layer", rendered)
        self.assertIn("AI Editor release gate failed.", rendered)
        self.assertIn("failing layer: failure sentinel", rendered)


if __name__ == "__main__":
    unittest.main()
