from __future__ import annotations

import unittest
from unittest import mock

from ai_editor import llm_engine
from ai_editor.llm_engine import LLMEngine, ProviderConfig


class ModelFamilyMatchingTests(unittest.TestCase):
    def test_longest_boundary_delimited_model_family_wins(self) -> None:
        registry = {
            "gpt-4o": {
                "max_input": 128_000, "max_output": 16_000,
                "tools": True, "vision": True,
            },
            "gpt-5": {
                "max_input": 400_000, "max_output": 128_000,
                "tools": True, "vision": False,
            },
        }
        with mock.patch.dict(llm_engine._model_registry, registry, clear=True):
            self.assertEqual(
                llm_engine.get_model_context("gpt-5.2-codex")["max_input"],
                400_000)
            self.assertFalse(
                llm_engine.get_model_capabilities("gpt-5.2-codex")["vision"])
            self.assertEqual(
                llm_engine.get_model_context("gpt-4o-2024-11-20")["max_input"],
                128_000)
            self.assertEqual(
                llm_engine.get_model_context("gpt-50-unrelated"),
                llm_engine._DEFAULT_CONTEXT)


class HttpClientConfigurationTests(unittest.TestCase):
    def test_config_override_timeout_rebuilds_client(self) -> None:
        engine = LLMEngine(ProviderConfig(timeout=180))
        clients = [mock.Mock(), mock.Mock()]
        with mock.patch("httpx.Client", side_effect=clients) as constructor:
            first = engine._client_for(ProviderConfig(timeout=1))
            again = engine._client_for(ProviderConfig(timeout=1))
            default = engine._client_for(ProviderConfig(timeout=180))
        self.assertIs(first, again)
        self.assertIs(default, clients[1])
        self.assertEqual(constructor.call_args_list[0].kwargs["timeout"], 1.0)
        self.assertEqual(constructor.call_args_list[1].kwargs["timeout"], 180.0)
        clients[0].close.assert_called_once()

    def test_retry_after_is_honored_and_wait_is_cancelable(self) -> None:
        engine = LLMEngine()
        response = mock.Mock(headers={"Retry-After": "7"})
        self.assertEqual(engine._retry_delay(response, 0), 7.0)
        engine.cancel()
        self.assertFalse(engine._wait_for_retry(30.0))


if __name__ == "__main__":
    unittest.main()
