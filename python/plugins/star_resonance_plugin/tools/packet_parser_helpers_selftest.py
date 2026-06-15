# -*- coding: utf-8 -*-
"""Focused regression tests for packet_parser.helpers."""

from __future__ import annotations

import builtins
import unittest
from unittest import mock

import _bootstrap  # noqa: F401

from packet_parser import helpers


class EnsureProtobufTests(unittest.TestCase):
    def setUp(self) -> None:
        self._state = (helpers._pb, helpers._pb_loaded, helpers._MessageToDict)

    def tearDown(self) -> None:
        helpers._pb, helpers._pb_loaded, helpers._MessageToDict = self._state

    def test_missing_proto_uses_visible_mini_decoder_fallback(self) -> None:
        original_import = builtins.__import__
        helpers._pb = None
        helpers._pb_loaded = False
        helpers._MessageToDict = None

        def fake_import(name, globals=None, locals=None, fromlist=(), level=0):
            if name == 'proto' or name.startswith('google.protobuf'):
                raise ImportError(name)
            return original_import(name, globals, locals, fromlist, level)

        with mock.patch('builtins.__import__', fake_import):
            with self.assertLogs('sao_auto.parser', level='INFO') as logs:
                self.assertIsNone(helpers._ensure_pb())

        self.assertTrue(helpers._pb_loaded)
        self.assertIsNone(helpers._MessageToDict)
        self.assertTrue(any('built-in mini protobuf decoder' in line for line in logs.output))


if __name__ == "__main__":
    unittest.main(verbosity=2)
