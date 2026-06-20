"""Conversation history persistence for the AI Editor.

Scope-aware storage:
  - Workspace (default): ``<BASE_DIR>/.sao/chat_history/``
  - System:              ``~/.sao/chat_history/``
  - Legacy fallback:     ``<BASE_DIR>/ai_editor_history/``

Each conversation gets its own file: ``{conv_id}.json``.
"""

from __future__ import annotations

import json
import logging
import os
import time
from typing import Any, Dict, List, Optional


logger = logging.getLogger(__name__)


def _validate_conversation_id(conv_id: str) -> str:
    normalized = str(conv_id or "").strip()
    if not normalized:
        raise ValueError("Conversation id is required")
    if normalized != os.path.basename(normalized) or any(sep in normalized for sep in ("/", "\\")) or ".." in normalized:
        raise ValueError("Conversation id must be a simple file-safe identifier")
    return normalized


def _history_dir(scope: str = "workspace") -> str:
    try:
        from ai_editor.scopes import _home_sao, _base_dir
        if scope == "system":
            d = os.path.join(_home_sao(), "chat_history")
        else:
            d = os.path.join(_base_dir(), ".sao", "chat_history")
    except Exception as exc:
        logger.debug("Falling back to legacy history root for %s: %s",
                     scope, exc)
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
    except Exception as exc:
        logger.debug("Failed to resolve scoped history directories: %s", exc)
    # Legacy path
    try:
        from config import BASE_DIR
        legacy = os.path.join(BASE_DIR, "ai_editor_history")
        if os.path.isdir(legacy) and legacy not in dirs:
            dirs.append(legacy)
    except Exception as exc:
        logger.debug("Failed to inspect legacy history directory: %s", exc)
    if not dirs:
        dirs.append(_history_dir())
    return dirs


def save_conversation(conv_id: str, title: str, messages: List[Dict[str, Any]],
                      system_prompt: str = "", model: str = "",
                      scope: str = "workspace") -> str:
    conv_id = _validate_conversation_id(conv_id)
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
    conv_id = _validate_conversation_id(conv_id)
    for d in _all_history_dirs():
        path = os.path.join(d, f"{conv_id}.json")
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, OSError) as exc:
            logger.warning("Failed to load conversation %s from %s: %s",
                           conv_id, path, exc)
            continue
    return None


def _parse_header_fast(fpath: str, cid: str) -> Optional[Dict[str, Any]]:
    """Read only the first ~200 bytes of a conversation file to extract
    the header fields (id, title, saved_at, message_count, model) without
    parsing the full messages array.  Falls back to full parse on failure."""
    try:
        with open(fpath, "rb") as f:
            head = f.read(256)
        text = head.decode("utf-8", errors="replace")
        # The JSON is written with indent=1, so header fields appear in the
        # first ~200 bytes before the "messages" array.
        entry: Dict[str, Any] = {"id": cid, "title": "Untitled",
                                  "saved_at": 0, "message_count": 0, "model": ""}
        import re
        for key in ("id", "title", "model"):
            m = re.search(rf'"{key}":\s*"([^"]*)"', text)
            if m:
                entry[key] = m.group(1)
        for key in ("saved_at",):
            m = re.search(rf'"{key}":\s*([\d.]+)', text)
            if m:
                entry[key] = float(m.group(1))
        for key in ("message_count",):
            m = re.search(rf'"{key}":\s*(\d+)', text)
            if m:
                entry[key] = int(m.group(1))
        return entry
    except (OSError, ValueError) as exc:
        logger.debug("Fast header parse failed for %s: %s", fpath, exc)
        return None


def list_conversations(limit: int = 50,
                       scope: str = "all") -> List[Dict[str, Any]]:
    """Return recent conversations sorted by saved_at desc.

    scope="all" lists across all scopes; "workspace"/"system" scopes to one.
    Uses fast header-only reads to avoid parsing full message arrays.
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
            entry = _parse_header_fast(fpath, cid)
            if entry:
                entries.append(entry)
    entries.sort(key=lambda e: e["saved_at"], reverse=True)
    return entries[:limit]


def delete_conversation(conv_id: str) -> bool:
    conv_id = _validate_conversation_id(conv_id)
    removed = False
    for d in _all_history_dirs():
        path = os.path.join(d, f"{conv_id}.json")
        try:
            os.remove(path)
            removed = True
        except OSError:
            continue
    return removed
