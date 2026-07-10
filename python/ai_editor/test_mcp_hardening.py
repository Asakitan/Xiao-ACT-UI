from __future__ import annotations

import json
import os
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from ai_editor.app import AIEditorAPI
from ai_editor.mcp_client import (
    McpServerConfig,
    McpToolDef,
    _stdio_environment,
    load_mcp_configs,
)


class McpEnvironmentTests(unittest.TestCase):
    def test_stdio_environment_does_not_inherit_unknown_host_secrets(self) -> None:
        config = McpServerConfig(
            id="safe", name="safe", command="server",
            env={"EXPLICIT_VALUE": "allowed"})
        with mock.patch.dict(os.environ, {
            "PATH": os.environ.get("PATH", ""),
            "SAO_SENTINEL_SECRET": "must-not-leak",
        }, clear=False):
            child_env = _stdio_environment(config)
        self.assertNotIn("SAO_SENTINEL_SECRET", child_env)
        self.assertEqual(child_env["EXPLICIT_VALUE"], "allowed")
        self.assertIn("PATH", {key.upper() for key in child_env})

    def test_explicit_inherit_env_is_honored(self) -> None:
        config = McpServerConfig(
            id="explicit", name="explicit", command="server",
            inherit_env=["SAO_EXPLICIT_SENTINEL"])
        with mock.patch.dict(
                os.environ, {"SAO_EXPLICIT_SENTINEL": "allowed"}, clear=False):
            child_env = _stdio_environment(config)
        self.assertEqual(child_env["SAO_EXPLICIT_SENTINEL"], "allowed")


class McpWorkspaceTrustTests(unittest.TestCase):
    def test_workspace_config_is_inert_until_trusted(self) -> None:
        import config as app_config

        with tempfile.TemporaryDirectory(prefix="sao_mcp_trust_") as root:
            with open(os.path.join(root, "mcp.json"), "w", encoding="utf-8") as handle:
                json.dump({"mcpServers": {
                    "workspace-server": {"command": "do-not-run"}
                }}, handle)

            def settings_get_untrusted(key, default=None):
                if key == "ai_editor":
                    return {"mcp": {
                        "enabled": True, "discovery_enabled": True,
                        "autostart": True, "workspace_trusted": False,
                    }}
                return default

            def settings_get_trusted(key, default=None):
                if key == "ai_editor":
                    return {"mcp": {
                        "enabled": True, "discovery_enabled": True,
                        "autostart": True, "workspace_trusted": True,
                    }}
                return default

            with mock.patch.object(app_config, "BASE_DIR", root):
                untrusted = load_mcp_configs(settings_get_untrusted)
                trusted = load_mcp_configs(settings_get_trusted)
            self.assertNotIn("workspace-server", {item.id for item in untrusted})
            self.assertIn("workspace-server", {item.id for item in trusted})


class McpToolPolicyTests(unittest.TestCase):
    def _api(self, tool: McpToolDef, settings=None) -> AIEditorAPI:
        api = AIEditorAPI.__new__(AIEditorAPI)
        api._mcp = SimpleNamespace(all_tools=lambda: [tool])
        api._mode = "plan"
        api._perm_overrides = {}
        payload = settings or {"access": "read_only"}
        api._settings_getter = lambda key, default=None: (
            {"mcp": payload} if key == "ai_editor" else default)
        return api

    def test_untrusted_read_only_hint_and_safe_name_still_require_confirmation(self) -> None:
        tool = McpToolDef(
            name="get_and_delete", description="destructive", input_schema={},
            server_id="untrusted", annotations={"readOnlyHint": True},
            trusted_server=False)
        api = self._api(tool)
        name = api._mcp_tool_name(tool)
        self.assertTrue(api._mcp_tool_allowed(name))
        self.assertTrue(api._mcp_tool_requires_confirm(name))

    def test_trusted_read_only_hint_can_skip_confirmation(self) -> None:
        tool = McpToolDef(
            name="read_file", description="read", input_schema={},
            server_id="trusted", annotations={"readOnlyHint": True},
            trusted_server=True)
        api = self._api(tool)
        self.assertFalse(api._mcp_tool_requires_confirm(api._mcp_tool_name(tool)))

    def test_explicit_tool_policy_overrides_untrusted_annotation(self) -> None:
        tool = McpToolDef(
            name="inspect", description="read", input_schema={},
            server_id="server", annotations={}, trusted_server=False)
        full_name = f"{tool.server_id}.{tool.name}"
        api = self._api(tool, {
            "access": "read_only",
            "tool_policies": {full_name: "read_only"},
        })
        self.assertFalse(api._mcp_tool_requires_confirm(api._mcp_tool_name(tool)))


if __name__ == "__main__":
    unittest.main()
