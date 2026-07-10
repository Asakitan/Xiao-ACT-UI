#!/usr/bin/env python3
"""Minimal JSON-lines bridge from the Node smoke scripts to Python Playwright.

The JavaScript smoke scripts remain the single source of truth for fixtures and
assertions.  This process only supplies the small Playwright surface they use
when the Node Playwright package is unavailable.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import traceback
from pathlib import Path
from typing import Any, Dict, Optional

from playwright.sync_api import Browser, Page, Playwright, sync_playwright


def _json_write(payload: Dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def _launch_options(raw: Any) -> Dict[str, Any]:
    source = raw if isinstance(raw, dict) else {}
    options: Dict[str, Any] = {"headless": bool(source.get("headless", True))}
    channel = str(source.get("channel") or "").strip()
    if channel:
        options["channel"] = channel
    executable_path = str(source.get("executablePath") or "").strip()
    if executable_path:
        options["executable_path"] = executable_path
    return options


class BrowserSmokeBridge:
    def __init__(self, playwright: Playwright) -> None:
        self._playwright = playwright
        self._browser: Optional[Browser] = None
        self._page: Optional[Page] = None
        self._page_errors: list[str] = []

    def _require_browser(self) -> Browser:
        if self._browser is None:
            raise RuntimeError("Browser has not been launched")
        return self._browser

    def _require_page(self) -> Page:
        if self._page is None:
            raise RuntimeError("Page has not been created")
        return self._page

    def _raise_page_errors(self) -> None:
        if not self._page_errors:
            return
        errors = list(self._page_errors)
        self._page_errors.clear()
        raise RuntimeError("Browser pageerror: " + " | ".join(errors))

    def _record_page_error(self, error: Any) -> None:
        message = str(error)
        stack = str(getattr(error, "stack", "") or "").strip()
        if stack and stack not in message:
            message = f"{message}\n{stack}"
        self._page_errors.append(message)

    def dispatch(self, operation: str, params: Any) -> Any:
        data = params if isinstance(params, dict) else {}
        if operation == "launch":
            if self._browser is not None:
                self._browser.close()
            self._browser = self._playwright.chromium.launch(**_launch_options(data))
            return {"browser": "chromium"}
        if operation == "new_page":
            browser = self._require_browser()
            viewport = data.get("viewport")
            page_options: Dict[str, Any] = {}
            if isinstance(viewport, dict):
                page_options["viewport"] = {
                    "width": int(viewport.get("width", 1280)),
                    "height": int(viewport.get("height", 720)),
                }
            self._page_errors.clear()
            self._page = browser.new_page(**page_options)
            self._page.on("pageerror", self._record_page_error)
            return {"page": 1}
        if operation == "add_init_script":
            page = self._require_page()
            source = str(data.get("source") or "")
            if not source:
                raise ValueError("addInitScript requires a function source")
            page.add_init_script(script=f"({source})();")
            return True
        if operation == "goto":
            page = self._require_page()
            url = str(data.get("url") or "")
            if not url:
                raise ValueError("goto requires a URL")
            wait_until = str(data.get("waitUntil") or "load")
            page.goto(url, wait_until=wait_until)
            self._raise_page_errors()
            return {"url": page.url}
        if operation == "wait_for_function":
            page = self._require_page()
            source = str(data.get("source") or "")
            timeout = float(data.get("timeout", 30000))
            if "arg" in data:
                handle = page.wait_for_function(f"({source})", arg=data.get("arg"), timeout=timeout)
            else:
                handle = page.wait_for_function(f"({source})", timeout=timeout)
            handle.dispose()
            self._raise_page_errors()
            return True
        if operation == "evaluate":
            page = self._require_page()
            source = str(data.get("source") or "")
            if "arg" in data:
                result = page.evaluate(f"({source})", data.get("arg"))
            else:
                result = page.evaluate(f"({source})")
            self._raise_page_errors()
            return result
        if operation == "wait_for_timeout":
            page = self._require_page()
            page.wait_for_timeout(float(data.get("timeout", 0)))
            self._raise_page_errors()
            return True
        if operation == "screenshot":
            page = self._require_page()
            screenshot_path = Path(str(data.get("path") or "")).expanduser().resolve()
            screenshot_path.parent.mkdir(parents=True, exist_ok=True)
            page.screenshot(path=str(screenshot_path), full_page=bool(data.get("fullPage", False)))
            self._raise_page_errors()
            return {"path": str(screenshot_path)}
        if operation == "close":
            self._raise_page_errors()
            if self._browser is not None:
                self._browser.close()
            self._browser = None
            self._page = None
            return True
        raise ValueError(f"Unsupported bridge operation: {operation}")

    def cleanup(self) -> None:
        if self._browser is not None:
            try:
                self._browser.close()
            except Exception:
                pass
        self._browser = None
        self._page = None


def _check_runtime() -> int:
    failures: list[str] = []
    with sync_playwright() as playwright:
        for options in ({"headless": True}, {"headless": True, "channel": "msedge"}):
            browser: Optional[Browser] = None
            try:
                browser = playwright.chromium.launch(**options)
                _json_write({
                    "ok": True,
                    "runtime": "python-playwright",
                    "python": sys.executable,
                    "browser": options.get("channel", "chromium"),
                })
                return 0
            except Exception as exc:
                failures.append(f"{options.get('channel', 'chromium')}: {exc}")
            finally:
                if browser is not None:
                    browser.close()
    _json_write({"ok": False, "error": " ; ".join(failures)})
    return 1


def _serve() -> int:
    os.environ.setdefault("PYTHONUTF8", "1")
    playwright = sync_playwright().start()
    bridge = BrowserSmokeBridge(playwright)
    try:
        for raw_line in sys.stdin:
            line = raw_line.strip()
            if not line:
                continue
            request: Dict[str, Any] = {}
            request_id: Any = None
            operation = ""
            try:
                request = json.loads(line)
                request_id = request.get("id")
                operation = str(request.get("operation") or "")
                result = bridge.dispatch(operation, request.get("params"))
                _json_write({"id": request_id, "ok": True, "result": result})
            except Exception as exc:
                _json_write({
                    "id": request_id,
                    "ok": False,
                    "error": str(exc),
                    "traceback": traceback.format_exc(),
                })
            if operation == "close":
                break
    finally:
        bridge.cleanup()
        playwright.stop()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Python Playwright bridge for Node browser smoke tests")
    parser.add_argument("--check", action="store_true", help="verify that a real browser can launch")
    args = parser.parse_args()
    return _check_runtime() if args.check else _serve()


if __name__ == "__main__":
    raise SystemExit(main())
