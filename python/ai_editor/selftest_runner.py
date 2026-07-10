from __future__ import annotations

import importlib
import os
import shutil
import subprocess
import sys
import traceback
import unittest
from collections.abc import Callable
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1]
AI_EDITOR_DIR = Path(__file__).resolve().parent
BROWSER_SMOKES = {
    "assistant": (
        AI_EDITOR_DIR / "tools" / "assistant_ui_browser_smoke.js",
        "PASS assistant-ui-browser-smoke",
    ),
    "settings": (
        AI_EDITOR_DIR / "tools" / "settings_ui_browser_smoke.js",
        "PASS settings-ui-browser-smoke",
    ),
}
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))


def _run_check(name: str, check: Callable[[], object]) -> list[str]:
    try:
        result = check()
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else 1
        return [] if code == 0 else [f"{name}: exited with {code}"]
    except Exception:
        return [f"{name}: {traceback.format_exc().strip()}"]
    if isinstance(result, list):
        return [f"{name}: {item}" for item in result]
    if isinstance(result, int) and result != 0:
        return [f"{name}: returned {result}"]
    return []


def _run_legacy_selftest() -> object:
    module = importlib.import_module("ai_editor.selftest")
    main = getattr(module, "main", None)
    if callable(main):
        return main()
    return None


def _run_frontend_health_selftest() -> object:
    module = importlib.import_module("ai_editor.frontend_health_selftest")
    return module.run_frontend_health_selftest()


def discover_unittest_suite(
    loader: unittest.TestLoader | None = None,
) -> unittest.TestSuite:
    """Discover focused regressions without importing the runner as a test."""
    return (loader or unittest.defaultTestLoader).discover(
        start_dir=str(AI_EDITOR_DIR),
        pattern="test_*.py",
        top_level_dir=str(PYTHON_ROOT),
    )


def run_unittest_discovery(
    suite: unittest.TestSuite | None = None,
    stream: object | None = None,
) -> list[str]:
    """Run the independent ``test_*.py`` regression suite fail-closed."""
    focused_suite = suite or discover_unittest_suite()
    count = focused_suite.countTestCases()
    if count == 0:
        return ["no test_*.py tests were discovered"]
    output = stream if stream is not None else sys.stdout
    result = unittest.TextTestRunner(stream=output, verbosity=2).run(focused_suite)
    if result.wasSuccessful():
        print(f"Focused unittest discovery passed: {result.testsRun} tests.", file=output)
        return []
    return [
        "focused unittest discovery failed: "
        f"{len(result.failures)} failure(s), {len(result.errors)} error(s), "
        f"{len(result.skipped)} skipped out of {result.testsRun} tests"
    ]


def _node_executable() -> str:
    try:
        from ai_editor.node_runtime import get_node_path
        configured = str(get_node_path() or "").strip()
        if configured:
            return configured
    except Exception:
        pass
    return str(shutil.which("node") or "")


def run_browser_smoke(
    name: str,
    *,
    node_path: str | None = None,
    run_process: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    stream: object | None = None,
    timeout: float = 180.0,
) -> list[str]:
    """Run one real-browser smoke and require its explicit PASS marker."""
    smoke = BROWSER_SMOKES.get(str(name or "").strip())
    if smoke is None:
        return [f"unknown browser smoke: {name}"]
    script_path, pass_marker = smoke
    if not script_path.is_file():
        return [f"{name} browser smoke script is missing: {script_path}"]
    executable = str(node_path or _node_executable()).strip()
    if not executable:
        return [f"{name} browser smoke cannot run because Node.js is unavailable"]
    env = dict(os.environ)
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    try:
        result = run_process(
            [executable, str(script_path)],
            cwd=str(PYTHON_ROOT),
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except Exception as exc:
        return [f"{name} browser smoke could not run: {exc}"]

    stdout = str(result.stdout or "").strip()
    stderr = str(result.stderr or "").strip()
    output = "\n".join(part for part in (stdout, stderr) if part)
    destination = stream if stream is not None else sys.stdout
    if output:
        print(output, file=destination)
    if result.returncode != 0:
        detail = output[-8000:] if output else "no subprocess output"
        return [f"{name} browser smoke failed with exit {result.returncode}: {detail}"]
    if pass_marker not in output:
        detail = output[-4000:] if output else "no subprocess output"
        return [
            f"{name} browser smoke exited successfully but did not report "
            f"{pass_marker!r}: {detail}"
        ]
    print(f"Browser smoke passed: {name}.", file=destination)
    return []


def release_checks() -> list[tuple[str, Callable[[], object]]]:
    """Return every layer required by the single AI Editor release gate."""
    return [
        ("legacy selftest", _run_legacy_selftest),
        ("focused unittest discovery", run_unittest_discovery),
        ("frontend health selftest", _run_frontend_health_selftest),
        ("assistant browser smoke", lambda: run_browser_smoke("assistant")),
        ("settings browser smoke", lambda: run_browser_smoke("settings")),
    ]


def _print_parity_snapshot() -> None:
    module = importlib.import_module("ai_editor.frontend_health_selftest")
    snapshot = module.compute_parity_snapshot()
    print("── AI IDE VS Code parity snapshot (阶段0) ──")
    for phase, bucket in snapshot["phases"].items():
        marker = {"ready": "OK", "partial": "~~", "missing": "!!"}.get(bucket["status"], "??")
        print(f"  [{marker}] {phase}: {bucket['passed']}/{bucket['total']} ({bucket['status']})")
    if snapshot["top_gaps"]:
        print("  Next gaps to fix:")
        for index, gap in enumerate(snapshot["top_gaps"], 1):
            print(f"    {index}. {gap}")


def main(
    checks: list[tuple[str, Callable[[], object]]] | None = None,
) -> int:
    gate_checks = checks if checks is not None else release_checks()
    failures: list[str] = []
    for name, check in gate_checks:
        print(f"── Release gate: {name} ──")
        failures.extend(_run_check(name, check))

    if failures:
        print("AI Editor release gate failed.")
        print("Failure points:")
        for index, failure in enumerate(failures, 1):
            print(f"{index}. {failure}")
        _print_parity_snapshot()
        return 1
    print("AI Editor release gate passed.")
    _print_parity_snapshot()
    return 0


if __name__ == "__main__":
    exit_code = main()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(exit_code)
