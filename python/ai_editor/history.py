"""Conversation history persistence for the AI Editor.

Stores conversations as JSON files under ``{BASE_DIR}/ai_editor_history/``.
Each conversation gets its own file: ``{conv_id}.json``.
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import asdict
from typing import Any, Dict, List, Optional


def _history_dir() -> str:
    try:
        from config import BASE_DIR
        d = os.path.join(BASE_DIR, "ai_editor_history")
    except ImportError:
        d = os.path.join(os.path.dirname(__file__), "..", "ai_editor_history")
    os.makedirs(d, exist_ok=True)
    return d


def save_conversation(conv_id: str, title: str, messages: List[Dict[str, Any]],
                      system_prompt: str = "", model: str = "") -> str:
    """Persist a conversation. Returns the file path."""
    path = os.path.join(_history_dir(), f"{conv_id}.json")
    data = {
        "id": conv_id,
        "title": title,
        "system_prompt": system_prompt,
        "model": model,
        "saved_at": time.time(),
        "messages": messages,
    }
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


def load_conversation(conv_id: str) -> Optional[Dict[str, Any]]:
    path = os.path.join(_history_dir(), f"{conv_id}.json")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return None


def list_conversations(limit: int = 50) -> List[Dict[str, Any]]:
    """Return recent conversations sorted by saved_at desc.

    Each entry has ``id``, ``title``, ``saved_at``, ``message_count``.
    """
    d = _history_dir()
    entries = []
    for fname in os.listdir(d):
        if not fname.endswith(".json"):
            continue
        fpath = os.path.join(d, fname)
        try:
            with open(fpath, "r", encoding="utf-8") as f:
                data = json.load(f)
            entries.append({
                "id": data.get("id", fname[:-5]),
                "title": data.get("title", "Untitled"),
                "saved_at": data.get("saved_at", 0),
                "message_count": len(data.get("messages", [])),
                "model": data.get("model", ""),
            })
        except (json.JSONDecodeError, OSError):
            continue
    entries.sort(key=lambda e: e["saved_at"], reverse=True)
    return entries[:limit]


def delete_conversation(conv_id: str) -> bool:
    path = os.path.join(_history_dir(), f"{conv_id}.json")
    try:
        os.remove(path)
        return True
    except OSError:
        return False
