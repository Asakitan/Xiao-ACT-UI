from __future__ import annotations

import importlib
import os
import sys
import traceback
from collections.abc import Callable
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1]
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


def main() -> int:
    checks: list[tuple[str, Callable[[], object]]] = [
        ("legacy selftest", _run_legacy_selftest),
        ("frontend health selftest", _run_frontend_health_selftest),
    ]
    failures: list[str] = []
    for name, check in checks:
        failures.extend(_run_check(name, check))

    if failures:
        print("AI Editor selftests failed.")
        print("Failure points:")
        for index, failure in enumerate(failures, 1):
            print(f"{index}. {failure}")
        return 1
    print("AI Editor selftests passed.")
    return 0


if __name__ == "__main__":
    exit_code = main()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(exit_code)
