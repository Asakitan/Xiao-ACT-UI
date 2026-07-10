from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


AI_EDITOR_DIR = Path(__file__).resolve().parent
TOOLS_DIR = AI_EDITOR_DIR / "tools"
BRIDGE_JS = AI_EDITOR_DIR / "browser_smoke_python_bridge.js"


class PythonPlaywrightBridgeTests(unittest.TestCase):
    def test_bridge_drives_a_real_browser_and_returns_dom_state(self) -> None:
        script = f"""
const bridge = require({json.dumps(str(BRIDGE_JS))});
(async () => {{
  const browser = await bridge.chromium.launch({{ headless: true }});
  const page = await browser.newPage({{ viewport: {{ width: 800, height: 600 }} }});
  page.on("pageerror", error => {{ throw error; }});
  await page.addInitScript(() => {{ window.__bridgeFixture = 41; }});
  await page.goto("data:text/html,<main id='ready'>bridge</main>", {{ waitUntil: "domcontentloaded" }});
  await page.waitForFunction(() => document.querySelector("#ready") && window.__bridgeFixture === 41, null, {{ timeout: 10000 }});
  const result = await page.evaluate(() => ({{
    text: document.querySelector("#ready").textContent,
    value: window.__bridgeFixture + 1,
    width: window.innerWidth
  }}));
  await browser.close();
  if (result.text !== "bridge" || result.value !== 42 || result.width !== 800) {{
    throw new Error("Unexpected bridge result: " + JSON.stringify(result));
  }}
  console.log("PASS python-playwright-bridge-selftest");
}})().catch(error => {{
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
}});
"""
        env = dict(os.environ)
        env["SAO_BROWSER_SMOKE_PYTHON"] = sys.executable
        result = subprocess.run(
            ["node", "-e", script],
            cwd=str(TOOLS_DIR),
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
            check=False,
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("INFO browser-smoke-runtime=python-playwright", output)
        self.assertIn("PASS python-playwright-bridge-selftest", output)

    def test_browser_page_errors_fail_closed(self) -> None:
        script = f"""
const bridge = require({json.dumps(str(BRIDGE_JS))});
(async () => {{
  const browser = await bridge.chromium.launch({{ headless: true }});
  const page = await browser.newPage();
  page.on("pageerror", error => {{ throw error; }});
  await page.goto("data:text/html,<script>throw new Error('bridge-pageerror-sentinel')</script>", {{ waitUntil: "domcontentloaded" }});
  await browser.close();
}})().catch(error => {{
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
}});
"""
        env = dict(os.environ)
        env["SAO_BROWSER_SMOKE_PYTHON"] = sys.executable
        result = subprocess.run(
            ["node", "-e", script],
            cwd=str(TOOLS_DIR),
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
            check=False,
        )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("Browser pageerror", output)
        self.assertIn("bridge-pageerror-sentinel", output)

    def test_explicit_missing_python_runtime_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            missing_python = Path(temp_dir) / "missing-python"
            script = f"require({json.dumps(str(BRIDGE_JS))});"
            env = dict(os.environ)
            env["SAO_BROWSER_SMOKE_PYTHON"] = str(missing_python)
            result = subprocess.run(
                ["node", "-e", script],
                cwd=str(TOOLS_DIR),
                env=env,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=20,
                check=False,
            )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("Python Playwright fallback is unavailable", output)


if __name__ == "__main__":
    unittest.main()
