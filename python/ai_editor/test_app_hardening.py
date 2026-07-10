from __future__ import annotations

import threading
import time
import tempfile
import os
import unittest
from types import SimpleNamespace

from ai_editor.app import AIEditorAPI


class _BusyController:
    def __init__(self) -> None:
        self._lifecycle_lock = threading.RLock()
        self.is_running = True
        self.active_run_id = "active-run"


class AppRunAdmissionTests(unittest.TestCase):
    def test_busy_send_does_not_mutate_active_engine_config(self) -> None:
        api = AIEditorAPI.__new__(AIEditorAPI)
        api._controller = _BusyController()
        api._engine = SimpleNamespace(
            config=SimpleNamespace(provider="openai", model="current-model"))
        api._ensure_engine = lambda: None

        result = AIEditorAPI.send_message(
            api,
            "second request",
            {"provider": "anthropic", "model": "other-model"},
        )

        self.assertTrue(result["busy"])
        self.assertFalse(result["accepted"])
        self.assertEqual(result["run_id"], "active-run")
        self.assertEqual(api._engine.config.provider, "openai")
        self.assertEqual(api._engine.config.model, "current-model")


class ProviderConfirmationIsolationTests(unittest.TestCase):
    def _api(self) -> AIEditorAPI:
        api = AIEditorAPI.__new__(AIEditorAPI)
        api._mode = "plan"
        api._pending_confirm = {}
        api._confirm_results = {}
        api._pending_confirm_lock = threading.Lock()
        api._confirmation_timeout = 1.0
        api._controller = SimpleNamespace(is_running=False)
        api._events = []
        api._emit = lambda event, payload: api._events.append((event, payload))
        return api

    def test_provider_confirmation_uses_source_controller_and_unique_key(self) -> None:
        api = self._api()
        controller_one = SimpleNamespace(is_running=True)
        controller_two = SimpleNamespace(is_running=True)
        results = {}

        threads = [
            threading.Thread(
                target=lambda: results.setdefault(
                    "one", api._on_provider_tool_confirm(
                        "one", "same-call", "write", "{}", controller_one))),
            threading.Thread(
                target=lambda: results.setdefault(
                    "two", api._on_provider_tool_confirm(
                        "two", "same-call", "write", "{}", controller_two))),
        ]
        for thread in threads:
            thread.start()

        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            with api._pending_confirm_lock:
                if len(api._pending_confirm) == 2:
                    break
            time.sleep(0.01)

        self.assertEqual(
            set(api._pending_confirm),
            {"provider:one:same-call", "provider:two:same-call"})
        self.assertTrue(api.confirm_tool("provider:one:same-call", True)["ok"])
        self.assertTrue(api.confirm_tool("provider:two:same-call", False)["ok"])
        for thread in threads:
            thread.join(timeout=2.0)

        self.assertTrue(results["one"])
        self.assertFalse(results["two"])
        emitted_ids = {
            payload["id"] for event, payload in api._events
            if event == "provider_tool_confirm"
        }
        self.assertEqual(
            emitted_ids,
            {"provider:one:same-call", "provider:two:same-call"})


class WebviewTrustBoundaryTests(unittest.TestCase):
    def test_missing_resource_roots_do_not_allow_arbitrary_local_files(self) -> None:
        self.assertFalse(AIEditorAPI._webview_path_allowed(
            os.path.expanduser("~/.sao/auth/sessions.json"), None))

        with tempfile.TemporaryDirectory(prefix="sao_webview_workspace_") as root:
            inside = os.path.join(root, "asset.js")
            outside = os.path.join(os.path.dirname(root), "outside-secret.txt")
            api = AIEditorAPI.__new__(AIEditorAPI)
            api._vscode_ns = SimpleNamespace(_webview_view_providers={})
            api._workspace_root = lambda: root
            roots = api._effective_webview_resource_roots(None, "view")
            self.assertTrue(AIEditorAPI._webview_path_allowed(inside, roots))
            self.assertFalse(AIEditorAPI._webview_path_allowed(outside, roots))

    def test_webview_message_requires_registered_nonce(self) -> None:
        delivered = []

        class _Namespace:
            _webview_tokens = {"view": "registered-token"}

            @staticmethod
            def verify_webview_token(view_id, token):
                return view_id == "view" and token == "registered-token"

            @staticmethod
            def deliver_webview_message(view_id, message):
                delivered.append((view_id, message))
                return True

        api = AIEditorAPI.__new__(AIEditorAPI)
        api._vscode_ns = _Namespace()
        api._extension_webview_panels = {}
        api._node_ext_host = None

        missing = api.webview_post_message("view", {"kind": "blocked"})
        invalid = api.webview_post_message(
            "view", {"kind": "blocked"}, "wrong-token")
        accepted = api.webview_post_message(
            "view", {"kind": "accepted"}, "registered-token")

        self.assertIn("error", missing)
        self.assertIn("error", invalid)
        self.assertTrue(accepted["ok"])
        self.assertEqual(delivered, [("view", {"kind": "accepted"})])


if __name__ == "__main__":
    unittest.main()
