from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ai_editor.auth import AuthService
from ai_editor.secret_store import (
    InMemorySecretStore,
    ProtectedSecretStore,
    SecretStoreError,
)


class AuthServiceSecretReferenceTests(unittest.TestCase):
    def test_colliding_provider_ids_keep_distinct_session_tokens(self) -> None:
        class _Provider:
            def __init__(self, token: str) -> None:
                self.token = token

            def create_session(self, scopes, options):
                return {
                    "id": "shared-session",
                    "accessToken": self.token,
                    "account": {"id": "account", "label": "Account"},
                    "scopes": list(scopes),
                }

        with tempfile.TemporaryDirectory(prefix="sao_auth_ref_collision_") as root:
            store = InMemorySecretStore()
            service = AuthService(storage_dir=root, secret_store=store)
            service.register_provider("a/b", "Slash", _Provider("slash-token"))
            service.register_provider("a_b", "Underscore", _Provider("underscore-token"))

            self.assertIsNotNone(service.create_session("a/b", ["repo"]))
            self.assertIsNotNone(service.create_session("a_b", ["repo"]))

            reloaded = AuthService(storage_dir=root, secret_store=store)
            self.assertEqual(
                reloaded.get_session("a/b").access_token,
                "slash-token",
            )
            self.assertEqual(
                reloaded.get_session("a_b").access_token,
                "underscore-token",
            )

            with open(os.path.join(root, "sessions.json"), "r", encoding="utf-8") as handle:
                metadata = json.load(handle)
            slash_ref = metadata["a/b"][0]["secretRef"]
            underscore_ref = metadata["a_b"][0]["secretRef"]
            self.assertNotEqual(slash_ref, underscore_ref)
            self.assertTrue(slash_ref.startswith("auth-session/v2-"))
            self.assertTrue(underscore_ref.startswith("auth-session/v2-"))

    def test_unique_legacy_session_secret_is_migrated_to_v2(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sao_auth_ref_migration_") as root:
            metadata_path = os.path.join(root, "sessions.json")
            legacy_ref = "auth-session/legacy/session-one"
            with open(metadata_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "legacy": [{
                        "id": "session-one",
                        "secretRef": legacy_ref,
                        "account": {"id": "one", "label": "One"},
                        "scopes": ["repo"],
                    }],
                }, handle)

            store = InMemorySecretStore()
            store.set(legacy_ref, "legacy-token")
            service = AuthService(storage_dir=root, secret_store=store)

            self.assertEqual(
                service.get_session("legacy").access_token,
                "legacy-token",
            )
            with open(metadata_path, "r", encoding="utf-8") as handle:
                migrated = json.load(handle)
            v2_ref = migrated["legacy"][0]["secretRef"]
            self.assertTrue(v2_ref.startswith("auth-session/v2-"))
            self.assertNotEqual(v2_ref, legacy_ref)
            self.assertTrue(store.has(v2_ref))
            self.assertFalse(store.has(legacy_ref))

    def test_ambiguous_legacy_session_secret_is_quarantined(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sao_auth_ref_ambiguous_") as root:
            metadata_path = os.path.join(root, "sessions.json")
            legacy_ref = "auth-session/a_b/shared-session"
            with open(metadata_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "a/b": [{
                        "id": "shared-session",
                        "secretRef": legacy_ref,
                        "account": {"id": "slash", "label": "Slash"},
                        "scopes": [],
                    }],
                    "a_b": [{
                        "id": "shared-session",
                        "secretRef": legacy_ref,
                        "account": {"id": "underscore", "label": "Underscore"},
                        "scopes": [],
                    }],
                }, handle)

            store = InMemorySecretStore()
            store.set(legacy_ref, "ambiguous-token")
            service = AuthService(storage_dir=root, secret_store=store)

            self.assertEqual(service.get_session("a/b").access_token, "")
            self.assertEqual(service.get_session("a_b").access_token, "")
            self.assertTrue(store.has(legacy_ref))

            reloaded = AuthService(storage_dir=root, secret_store=store)
            self.assertEqual(reloaded.get_session("a/b").access_token, "")
            self.assertEqual(reloaded.get_session("a_b").access_token, "")

    def test_remove_session_cleans_unique_v2_and_legacy_secrets(self) -> None:
        class _Provider:
            @staticmethod
            def create_session(scopes, options):
                return {
                    "id": "delete-session",
                    "accessToken": "delete-token",
                    "account": {"id": "delete", "label": "Delete"},
                    "scopes": list(scopes),
                }

        with tempfile.TemporaryDirectory(prefix="sao_auth_ref_delete_") as root:
            store = InMemorySecretStore()
            service = AuthService(storage_dir=root, secret_store=store)
            service.register_provider("legacy/delete", "Legacy", _Provider())
            session = service.create_session("legacy/delete", [])
            self.assertIsNotNone(session)

            with open(os.path.join(root, "sessions.json"), "r", encoding="utf-8") as handle:
                metadata = json.load(handle)
            v2_ref = metadata["legacy/delete"][0]["secretRef"]
            legacy_ref = "auth-session/legacy_delete/delete-session"
            store.set(legacy_ref, "stale-legacy-token")
            self.assertTrue(store.has(v2_ref))
            self.assertTrue(store.has(legacy_ref))

            self.assertTrue(service.remove_session(
                "legacy/delete", "delete-session"))
            self.assertFalse(store.has(v2_ref))
            self.assertFalse(store.has(legacy_ref))

    def test_remove_session_does_not_claim_ambiguous_legacy_secret(self) -> None:
        class _Provider:
            def __init__(self, token: str) -> None:
                self.token = token

            def create_session(self, scopes, options):
                return {
                    "id": "shared-session",
                    "accessToken": self.token,
                    "account": {"id": "account", "label": "Account"},
                    "scopes": list(scopes),
                }

        with tempfile.TemporaryDirectory(prefix="sao_auth_ref_delete_ambiguous_") as root:
            store = InMemorySecretStore()
            service = AuthService(storage_dir=root, secret_store=store)
            service.register_provider("a/b", "Slash", _Provider("slash-token"))
            service.register_provider("a_b", "Underscore", _Provider("underscore-token"))
            service.create_session("a/b", [])
            service.create_session("a_b", [])

            legacy_ref = "auth-session/a_b/shared-session"
            store.set(legacy_ref, "ambiguous-token")
            self.assertTrue(service.remove_session("a/b", "shared-session"))
            self.assertTrue(store.has(legacy_ref))

            with open(os.path.join(root, "sessions.json"), "r", encoding="utf-8") as handle:
                metadata = json.load(handle)
            remaining_ref = metadata["a_b"][0]["secretRef"]
            store.delete(remaining_ref)

            reloaded = AuthService(storage_dir=root, secret_store=store)
            self.assertEqual(reloaded.get_session("a_b").access_token, "")
            self.assertTrue(store.has(legacy_ref))


@unittest.skipUnless(os.name == "nt", "AI Editor protected persistence uses Windows DPAPI")
class ProtectedSecretStoreTests(unittest.TestCase):
    def test_vault_round_trip_never_writes_plaintext(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sao_secret_store_") as root:
            path = os.path.join(root, "vault.json")
            store = ProtectedSecretStore(path)
            store.set("provider/openai/api-key", "sentinel-secret")
            self.assertEqual(store.get("provider/openai/api-key"), "sentinel-secret")
            self.assertTrue(store.has("provider/openai/api-key"))
            on_disk = Path(path).read_text(encoding="utf-8")
            self.assertNotIn("sentinel-secret", on_disk)
            self.assertTrue(store.delete("provider/openai/api-key"))
            self.assertEqual(store.get("provider/openai/api-key"), "")

    def test_invalid_key_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sao_secret_store_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            with self.assertRaises(SecretStoreError):
                store.set("../escape", "secret")


@unittest.skipUnless(os.name == "nt", "AI Editor protected persistence uses Windows DPAPI")
class AuthServiceProtectedPersistenceTests(unittest.TestCase):
    def test_auth_metadata_is_public_and_token_is_protected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sao_auth_store_") as root:
            service = AuthService(storage_dir=root)
            session = service.create_session_from_token(
                "github", "sentinel-token", "Test User", ["repo"])

            metadata_path = os.path.join(root, "sessions.json")
            vault_path = os.path.join(root, "sessions.vault.json")
            metadata = Path(metadata_path).read_text(encoding="utf-8")
            vault = Path(vault_path).read_text(encoding="utf-8")
            self.assertNotIn("sentinel-token", metadata)
            self.assertNotIn("sentinel-token", vault)
            self.assertNotIn("accessToken", metadata)
            self.assertNotIn("accessToken", session.to_public_dict())
            self.assertTrue(session.to_public_dict()["hasToken"])

            reloaded = AuthService(storage_dir=root).get_session("github")
            self.assertIsNotNone(reloaded)
            self.assertEqual(reloaded.access_token, "sentinel-token")

    def test_plaintext_session_is_migrated_only_after_protected_write(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sao_auth_migrate_") as root:
            os.makedirs(root, exist_ok=True)
            metadata_path = os.path.join(root, "sessions.json")
            with open(metadata_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "legacy": [{
                        "id": "session-one",
                        "accessToken": "legacy-sentinel",
                        "account": {"id": "one", "label": "One"},
                        "scopes": [],
                    }]
                }, handle)

            service = AuthService(storage_dir=root)
            self.assertEqual(
                service.get_session("legacy").access_token, "legacy-sentinel")
            migrated = Path(metadata_path).read_text(encoding="utf-8")
            self.assertNotIn("legacy-sentinel", migrated)
            self.assertNotIn("accessToken", migrated)


@unittest.skipUnless(os.name == "nt", "AI Editor protected persistence uses Windows DPAPI")
class ProviderConfigProtectedPersistenceTests(unittest.TestCase):
    def test_colliding_provider_ids_keep_distinct_secrets(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import (
            _hydrate_ai_editor_config_secrets,
            _protect_ai_editor_config_secrets,
        )

        with tempfile.TemporaryDirectory(prefix="sao_provider_collision_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            with mock.patch.object(secret_module, "_singleton", store):
                protected = _protect_ai_editor_config_secrets({
                    "provider": "a/b",
                    "provider_keys": {
                        "a/b": "slash-provider-secret",
                        "a?b": "question-provider-secret",
                    },
                })
                hydrated = _hydrate_ai_editor_config_secrets(protected)

            self.assertEqual(
                hydrated["provider_keys"]["a/b"],
                "slash-provider-secret",
            )
            self.assertEqual(
                hydrated["provider_keys"]["a?b"],
                "question-provider-secret",
            )

    def test_unique_legacy_provider_secret_is_migrated(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import _hydrate_ai_editor_config_secrets

        with tempfile.TemporaryDirectory(prefix="sao_provider_legacy_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            legacy_ref = "provider/a_b/api-key"
            store.set(legacy_ref, "legacy-provider-secret")
            protected = {
                "provider": "a/b",
                "provider_keys": {},
                "_secret_state": {"provider_keys": ["a/b"]},
            }
            with mock.patch.object(secret_module, "_singleton", store):
                hydrated = _hydrate_ai_editor_config_secrets(protected)
                hydrated_again = _hydrate_ai_editor_config_secrets(protected)

            self.assertEqual(hydrated["api_key"], "legacy-provider-secret")
            self.assertEqual(
                hydrated_again["provider_keys"]["a/b"],
                "legacy-provider-secret",
            )
            self.assertFalse(store.has(legacy_ref))

    def test_ambiguous_legacy_provider_secret_is_not_exposed(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import _hydrate_ai_editor_config_secrets

        with tempfile.TemporaryDirectory(prefix="sao_provider_ambiguous_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            legacy_ref = "provider/a_b/api-key"
            store.set(legacy_ref, "ambiguous-provider-secret")
            protected = {
                "provider": "a/b",
                "provider_keys": {},
                "_secret_state": {"provider_keys": ["a/b", "a?b"]},
            }
            with mock.patch.object(secret_module, "_singleton", store):
                hydrated = _hydrate_ai_editor_config_secrets(protected)
                narrowed = _hydrate_ai_editor_config_secrets({
                    **protected,
                    "provider": "a?b",
                    "_secret_state": {"provider_keys": ["a?b"]},
                })

            self.assertEqual(hydrated["api_key"], "")
            self.assertNotIn("a/b", hydrated["provider_keys"])
            self.assertNotIn("a?b", hydrated["provider_keys"])
            self.assertEqual(narrowed["api_key"], "")
            self.assertNotIn("a?b", narrowed["provider_keys"])
            self.assertTrue(store.has(legacy_ref))

    def test_provider_runtime_hydrates_redacted_key_at_trusted_boundary(self) -> None:
        from ai_editor.app import AIEditorAPI
        from ai_editor.chat_providers import ChatProviderDef

        class Settings:
            def __init__(self) -> None:
                self.data = {"ai_editor": {
                    "provider_keys": {"anthropic": "runtime-sentinel"},
                }}

            def get(self, key, default=None):
                return self.data.get(key, default)

            def set(self, key, value) -> None:
                self.data[key] = value

            def save(self) -> None:
                return None

        class Gui:
            def __init__(self) -> None:
                self.settings = Settings()
                self._ai_engine_actions = {}
                self._plugin_manager = None
                self._packet_bridge = None
                self._mem_bridge = None
                self._start_time = 0.0

        gui = Gui()
        api = AIEditorAPI(gui)
        api._ensure_engine()
        api._fetch_provider_models = lambda *_args, **_kwargs: {
            "models": [{"id": "official-claude"}],
            "default_model": "official-claude",
        }
        provider = ChatProviderDef(
            id="secure-anthropic",
            name="Secure Anthropic",
            provider_type="anthropic",
            model="claude-sonnet-4-20250514",
        )
        controller = api._create_provider_controller(provider)
        self.assertEqual(controller.engine.config.api_key, "runtime-sentinel")
        self.assertEqual(controller.engine.config.model, "official-claude")
        persisted = gui.settings.get("ai_editor", {})
        self.assertNotIn("runtime-sentinel", json.dumps(persisted))

    def test_provider_secrets_are_redacted_and_hydrated(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import (
            _hydrate_ai_editor_config_secrets,
            _protect_ai_editor_config_secrets,
        )

        with tempfile.TemporaryDirectory(prefix="sao_provider_secret_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            with mock.patch.object(secret_module, "_singleton", store):
                protected = _protect_ai_editor_config_secrets({
                    "provider": "openai",
                    "api_key": "active-sentinel",
                    "provider_keys": {
                        "anthropic": "provider-sentinel",
                    },
                    "extra_headers": {
                        "Authorization": "Bearer header-sentinel",
                    },
                    "extra_body": {"tenantSecret": "body-sentinel"},
                })
                serialized = json.dumps(protected, ensure_ascii=False)
                vault = Path(store.path).read_text(encoding="utf-8")
                for sentinel in (
                    "active-sentinel", "provider-sentinel",
                    "header-sentinel", "body-sentinel",
                ):
                    self.assertNotIn(sentinel, serialized)
                    self.assertNotIn(sentinel, vault)

                hydrated = _hydrate_ai_editor_config_secrets(protected)
                self.assertEqual(hydrated["api_key"], "active-sentinel")
                self.assertEqual(
                    hydrated["provider_keys"]["anthropic"],
                    "provider-sentinel")
                self.assertEqual(
                    hydrated["extra_headers"]["Authorization"],
                    "Bearer header-sentinel")
                self.assertEqual(
                    hydrated["extra_body"]["tenantSecret"],
                    "body-sentinel")

    def test_mcp_headers_and_environment_are_protected(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import _protect_ai_editor_config_secrets
        from ai_editor.mcp_client import _parse_server_config

        with tempfile.TemporaryDirectory(prefix="sao_mcp_secret_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            with mock.patch.object(secret_module, "_singleton", store):
                protected = _protect_ai_editor_config_secrets({
                    "mcp": {
                        "servers": [{
                            "id": "remote",
                            "transport": "streamable_http",
                            "url": "https://example.invalid/mcp",
                            "headers": {"Authorization": "Bearer mcp-header-sentinel"},
                            "env": {"MCP_TOKEN": "mcp-env-sentinel"},
                        }]
                    }
                })
                server = protected["mcp"]["servers"][0]
                serialized = json.dumps(protected, ensure_ascii=False)
                vault = Path(store.path).read_text(encoding="utf-8")
                self.assertNotIn("mcp-header-sentinel", serialized)
                self.assertNotIn("mcp-env-sentinel", serialized)
                self.assertNotIn("mcp-header-sentinel", vault)
                self.assertNotIn("mcp-env-sentinel", vault)

                parsed = _parse_server_config("remote", server)
                self.assertEqual(
                    parsed.headers["Authorization"],
                    "Bearer mcp-header-sentinel")
                self.assertEqual(parsed.env["MCP_TOKEN"], "mcp-env-sentinel")

    def test_colliding_mcp_ids_keep_distinct_secrets(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import _protect_ai_editor_config_secrets
        from ai_editor.mcp_client import load_mcp_configs

        with tempfile.TemporaryDirectory(prefix="sao_mcp_collision_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            with mock.patch.object(secret_module, "_singleton", store):
                protected = _protect_ai_editor_config_secrets({
                    "mcp": {
                        "enabled": True,
                        "discovery_enabled": True,
                        "autostart": True,
                        "servers": [
                            {
                                "id": "a/b",
                                "headers": {"Authorization": "slash-header"},
                                "env": {"MCP_TOKEN": "slash-env"},
                            },
                            {
                                "id": "a?b",
                                "headers": {"Authorization": "question-header"},
                                "env": {"MCP_TOKEN": "question-env"},
                            },
                        ],
                    }
                })

                def settings_get(key, default=None):
                    return protected if key == "ai_editor" else default

                configs = {
                    item.id: item for item in load_mcp_configs(settings_get)
                }

            self.assertEqual(
                configs["a/b"].headers["Authorization"], "slash-header")
            self.assertEqual(configs["a/b"].env["MCP_TOKEN"], "slash-env")
            self.assertEqual(
                configs["a?b"].headers["Authorization"], "question-header")
            self.assertEqual(configs["a?b"].env["MCP_TOKEN"], "question-env")

    def test_mcp_ids_differing_only_by_whitespace_keep_distinct_secrets(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.app import _protect_ai_editor_config_secrets
        from ai_editor.mcp_client import load_mcp_configs

        with tempfile.TemporaryDirectory(prefix="sao_mcp_whitespace_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            with mock.patch.object(secret_module, "_singleton", store):
                protected = _protect_ai_editor_config_secrets({
                    "mcp": {
                        "enabled": True,
                        "discovery_enabled": True,
                        "autostart": True,
                        "servers": [
                            {"id": "a", "env": {"TOKEN": "plain-id-secret"}},
                            {"id": " a ", "env": {"TOKEN": "spaced-id-secret"}},
                        ],
                    }
                })

                def settings_get(key, default=None):
                    return protected if key == "ai_editor" else default

                configs = {
                    item.id: item for item in load_mcp_configs(settings_get)
                }

            self.assertEqual(configs["a"].env["TOKEN"], "plain-id-secret")
            self.assertEqual(configs[" a "].env["TOKEN"], "spaced-id-secret")

    def test_unique_legacy_mcp_secrets_are_migrated(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.mcp_client import load_mcp_configs
        from ai_editor.secret_store import SECRET_PRESENT

        with tempfile.TemporaryDirectory(prefix="sao_mcp_legacy_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            legacy_headers = "mcp/a_b/headers"
            legacy_env = "mcp/a_b/env"
            store.set_json(
                legacy_headers, {"Authorization": "legacy-mcp-header"})
            store.set_json(legacy_env, {"MCP_TOKEN": "legacy-mcp-env"})
            protected = {
                "mcp": {
                    "enabled": True,
                    "discovery_enabled": True,
                    "autostart": True,
                    "servers": [{
                        "id": "a/b",
                        "headers": {"Authorization": SECRET_PRESENT},
                        "env": {"MCP_TOKEN": SECRET_PRESENT},
                        "_secret_fields": ["headers", "env"],
                    }],
                }
            }

            def settings_get(key, default=None):
                return protected if key == "ai_editor" else default

            with mock.patch.object(secret_module, "_singleton", store):
                config = load_mcp_configs(settings_get)[0]
                config_again = load_mcp_configs(settings_get)[0]

            self.assertEqual(
                config.headers["Authorization"], "legacy-mcp-header")
            self.assertEqual(config.env["MCP_TOKEN"], "legacy-mcp-env")
            self.assertEqual(
                config_again.headers["Authorization"], "legacy-mcp-header")
            self.assertFalse(store.has(legacy_headers))
            self.assertFalse(store.has(legacy_env))

    def test_ambiguous_legacy_mcp_secret_is_not_exposed(self) -> None:
        from ai_editor import secret_store as secret_module
        from ai_editor.mcp_client import load_mcp_configs
        from ai_editor.secret_store import SECRET_PRESENT

        with tempfile.TemporaryDirectory(prefix="sao_mcp_ambiguous_") as root:
            store = ProtectedSecretStore(os.path.join(root, "vault.json"))
            legacy_ref = "mcp/a_b/headers"
            store.set_json(
                legacy_ref, {"Authorization": "ambiguous-mcp-header"})
            protected = {
                "mcp": {
                    "enabled": True,
                    "discovery_enabled": True,
                    "autostart": True,
                    "servers": [
                        {
                            "id": "a/b",
                            "headers": {"Authorization": SECRET_PRESENT},
                            "_secret_fields": ["headers"],
                        },
                        {
                            "id": "a?b",
                            "headers": {"Authorization": SECRET_PRESENT},
                            "_secret_fields": ["headers"],
                        },
                    ],
                }
            }

            def settings_get(key, default=None):
                return protected if key == "ai_editor" else default

            with mock.patch.object(secret_module, "_singleton", store):
                configs = {
                    item.id: item for item in load_mcp_configs(settings_get)
                }
                narrowed_protected = {
                    "mcp": {
                        **protected["mcp"],
                        "servers": [protected["mcp"]["servers"][1]],
                    }
                }

                def narrowed_settings_get(key, default=None):
                    return narrowed_protected if key == "ai_editor" else default

                narrowed = load_mcp_configs(narrowed_settings_get)[0]

            self.assertEqual(configs["a/b"].headers, {})
            self.assertEqual(configs["a?b"].headers, {})
            self.assertEqual(narrowed.headers, {})
            self.assertTrue(store.has(legacy_ref))


class SecretStorePackagingTests(unittest.TestCase):
    def test_frozen_builds_include_lazy_dpapi_modules(self) -> None:
        python_root = Path(__file__).resolve().parents[1]
        spec = (python_root / "XiaoACTUI.spec").read_text(encoding="utf-8")
        nuitka = (python_root / "build_nuitka.bat").read_text(encoding="utf-8")
        requirements = (python_root / "requirements.txt").read_text(
            encoding="utf-8")
        self.assertIn("'win32crypt'", spec)
        self.assertIn("'pywintypes'", spec)
        self.assertIn("--include-module=win32crypt", nuitka)
        self.assertIn("--include-module=pywintypes", nuitka)
        self.assertIn('pywin32; sys_platform == "win32"', requirements)


if __name__ == "__main__":
    unittest.main()
