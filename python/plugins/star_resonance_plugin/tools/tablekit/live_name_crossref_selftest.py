# -*- coding: utf-8 -*-
"""Selftest for live name cross-reference runtime evidence handling."""
from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import os
import tempfile
import unittest

from tools.tablekit.live_name_crossref import crossref


class LiveNameCrossrefTests(unittest.TestCase):
    def test_missing_runtime_klass_is_not_filled_with_fixed_address(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            candidate_path = os.path.join(td, "candidates.json")
            output_path = os.path.join(td, "crossref.json")
            with open(candidate_path, "w", encoding="utf-8") as f:
                json.dump({
                    "process": "star.exe",
                    "pid": 1,
                    "groups": {
                        "x": [{"text": "无固定Klass", "obj": "0x1", "chars": "0x2"}],
                    },
                }, f, ensure_ascii=False)

            result = crossref(candidate_path, output_path)

        runtime = result["groups"]["x"][0]["runtime"]
        self.assertNotIn("klass", runtime)
        self.assertNotIn("0x30252490", json.dumps(result, ensure_ascii=False))
        self.assertEqual(runtime["klass_status"], "unverified_runtime_klass")

    def test_observed_runtime_klass_is_status_not_address(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            candidate_path = os.path.join(td, "candidates.json")
            output_path = os.path.join(td, "crossref.json")
            with open(candidate_path, "w", encoding="utf-8") as f:
                json.dump({
                    "process": "star.exe",
                    "pid": 1,
                    "groups": {
                        "x": [{"text": "旧会话Klass", "obj": "0x1", "chars": "0x2", "klass": "0x30252490"}],
                    },
                }, f, ensure_ascii=False)

            result = crossref(candidate_path, output_path)

        runtime = result["groups"]["x"][0]["runtime"]
        self.assertNotIn("klass", runtime)
        self.assertNotIn("0x30252490", json.dumps(result, ensure_ascii=False))
        self.assertEqual(runtime["klass_status"], "observed_volatile_runtime_klass")


if __name__ == "__main__":
    unittest.main(verbosity=2)
