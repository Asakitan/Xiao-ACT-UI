# -*- coding: utf-8 -*-
"""Regression tests for Entity offline-import panel render signatures."""

from __future__ import annotations

import unittest

from gui_modules.sao_gui_offline_import import OfflineImportPanel


def _status(**overrides):
    data = {
        "status": "imported",
        "selected_file": "E:/tmp/replay.json",
        "last_result": {
            "source_path": "E:/tmp/replay.json",
            "format": "json",
            "event_count": 10,
            "importer": "normalized",
            "persisted": True,
            "preview": {"encounter_id": "enc-a"},
            "report": {},
        },
        "history": {
            "encounters": [
                {
                    "_history_index": 0,
                    "encounter_id": "enc-a",
                    "total_damage": 100,
                    "completed_local_time": "t1",
                }
            ]
        },
    }
    data.update(overrides)
    return data


class OfflineImportPanelSignatureTests(unittest.TestCase):
    def test_rows_signature_tracks_same_count_history_content(self) -> None:
        base = _status()
        changed = _status(history={
            "encounters": [
                {
                    "_history_index": 0,
                    "encounter_id": "enc-a",
                    "total_damage": 999,
                    "completed_local_time": "t2",
                }
            ]
        })

        self.assertNotEqual(OfflineImportPanel._rows_signature(base), OfflineImportPanel._rows_signature(changed))

    def test_rows_signature_tracks_import_preview_fields(self) -> None:
        base = _status()
        changed = _status(last_result={
            "source_path": "E:/tmp/replay.json",
            "format": "json",
            "event_count": 10,
            "importer": "xml",
            "persisted": False,
            "preview": {"encounter_id": "enc-b"},
            "report": {},
        })

        self.assertNotEqual(OfflineImportPanel._rows_signature(base), OfflineImportPanel._rows_signature(changed))

    def test_destroy_resets_rows_signature(self) -> None:
        panel = OfflineImportPanel.__new__(OfflineImportPanel)
        panel._win = None
        panel._rows = None
        panel._last_rows_sig = "stale"

        panel.destroy()

        self.assertEqual(panel._last_rows_sig, "")


if __name__ == "__main__":
    unittest.main()
