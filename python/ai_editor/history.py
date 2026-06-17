"""Conversation history persistence for the AI Editor.

Scope-aware storage:
  - Workspace (default): ``<BASE_DIR>/.sao/chat_history/``
  - System:              ``~/.sao/chat_history/``
  - Legacy fallback:     ``<BASE_DIR>/ai_editor_history/``

Each conversation gets its own file: ``{conv_id}.json``.
"""

from __future__ import annotations

import json
import os
import time
from typing import Any, Dict, List, Optional


def _history_dir(scope: str = "workspace") -> str:
    try:
        from ai_editor.scopes import _home_sao, _base_dir
        if scope == "system":
            d = os.path.join(_home_sao(), "chat_history")
        else:
            d = os.path.join(_base_dir(), ".sao", "chat_history")
    except Exception:
        try:
            from config import BASE_DIR
            d = os.path.join(BASE_DIR, ".sao", "chat_history")
        except ImportError:
            d = os.path.join(os.path.dirname(__file__), "..", "ai_editor_history")
    os.makedirs(d, exist_ok=True)
    return d


def _all_history_dirs() -> List[str]:
    """Return all history directories across scopes (for listing)."""
    dirs = []
    try:
        from ai_editor.scopes import _home_sao, _base_dir
        sys_d = os.path.join(_home_sao(), "chat_history")
        if os.path.isdir(sys_d):
            dirs.append(sys_d)
        ws_d = os.path.join(_base_dir(), ".sao", "chat_history")
        if os.path.isdir(ws_d):
            dirs.append(ws_d)
    except Exception:
        pass
    # Legacy path
    try:
        from config import BASE_DIR
        legacy = os.path.join(BASE_DIR, "ai_editor_history")
        if os.path.isdir(legacy) and legacy not in dirs:
            dirs.append(legacy)
    except Exception:
        pass
    if not dirs:
        dirs.append(_history_dir())
    return dirs


def save_conversation(conv_id: str, title: str, messages: List[Dict[str, Any]],
                      system_prompt: str = "", model: str = "",
                      scope: str = "workspace") -> str:
    path = os.path.join(_history_dir(scope), f"{conv_id}.json")
    data = {
        "id": conv_id,
        "title": title,
        "system_prompt": system_prompt,
        "model": model,
        "saved_at": time.time(),
        "message_count": len(messages),
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
    for d in _all_history_dirs():
        path = os.path.join(d, f"{conv_id}.json")
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, OSError):
            continue
    return None


def list_conversations(limit: int = 50,
                       scope: str = "all") -> List[Dict[str, Any]]:
    """Return recent conversations sorted by saved_at desc.

    scope="all" lists across all scopes; "workspace"/"system" scopes to one.
    """
    if scope == "all":
        dirs = _all_history_dirs()
    else:
        dirs = [_history_dir(scope)]

    entries: List[Dict[str, Any]] = []
    seen: set = set()
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fname in os.listdir(d):
            if not fname.endswith(".json"):
                continue
            cid = fname[:-5]
            if cid in seen:
                continue
            seen.add(cid)
            fpath = os.path.join(d, fname)
            try:
                with open(fpath, "r", encoding="utf-8") as f:
                    data = json.load(f)
                entries.append({
                    "id": data.get("id", cid),
                    "title": data.get("title", "Untitled"),
                    "saved_at": data.get("saved_at", 0),
                    "message_count": data.get("message_count",
                                              len(data.get("messages", []))),
                    "model": data.get("model", ""),
                })
            except (json.JSONDecodeError, OSError):
                continue
    entries.sort(key=lambda e: e["saved_at"], reverse=True)
    return entries[:limit]


def delete_conversation(conv_id: str) -> bool:
    for d in _all_history_dirs():
        path = os.path.join(d, f"{conv_id}.json")
        try:
            os.remove(path)
            return True
        except OSError:
            continue
    return False
