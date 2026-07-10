from __future__ import annotations

import unittest
from types import SimpleNamespace
from unittest import mock

from ai_editor.chat_state import ChatController, ChatMessage, Conversation
from ai_editor.tool_registry import ToolRegistry


class HistorySaveErrorTests(unittest.TestCase):
    def test_save_failure_is_reported_and_retried(self) -> None:
        conversation = Conversation()
        conversation.add_message(ChatMessage(role="user", content="persist me"))
        engine = SimpleNamespace(
            config=SimpleNamespace(effective_model="test-model"))
        controller = ChatController(engine, ToolRegistry(), conversation)
        errors = []
        controller.on_save_error = errors.append

        with mock.patch(
                "ai_editor.history.save_conversation",
                side_effect=[OSError("disk full"), None]) as save:
            self.assertFalse(controller._auto_save())
            self.assertEqual(errors, ["disk full"])
            self.assertEqual(controller._last_save_error, "disk full")
            self.assertTrue(controller._auto_save())

        self.assertEqual(save.call_count, 2)
        self.assertEqual(controller._last_save_error, "")


if __name__ == "__main__":
    unittest.main()
