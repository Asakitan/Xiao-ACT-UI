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


def _native_summary_search_text(summary: Dict[str, Any]) -> str:
    terms: List[str] = []
    if summary.get("parts"):
        terms.append("response parts")
    if summary.get("todos"):
        terms.append("todo list")
    if summary.get("changes"):
        terms.append("modified files changes edits")
    if summary.get("refs"):
        terms.append("references anchors")
    if summary.get("usedContext"):
        terms.append("used context")
    if summary.get("trees"):
        terms.append("file tree")
    if summary.get("tokens"):
        terms.append("tokens usage")
    if summary.get("errors"):
        terms.append("error failed")
    terms.extend(str(v) for v in summary.get("modelLabels", []) if v)
    terms.extend(str(v) for v in summary.get("keywords", []) if v)
    return " ".join(terms)


def _native_summary_item(value: Any, fallback: str = "") -> Dict[str, str]:
    if not isinstance(value, dict):
        return {}
    path = str(
        value.get("path") or value.get("uri") or value.get("modifiedUri")
        or value.get("modifiedContentUri") or value.get("originalUri")
        or value.get("originalContentUri") or ""
    ).strip()
    label = str(value.get("label") or value.get("title") or value.get("name")
                or os.path.basename(path) or fallback or path).strip()
    item: Dict[str, str] = {}
    if path:
        item["path"] = path
    if label:
        item["label"] = label
    kind = str(value.get("kind") or "").strip()
    if kind:
        item["kind"] = kind
    return item


def _collect_tree_file_items(nodes: Any, out: List[Dict[str, str]]) -> None:
    if not isinstance(nodes, list):
        return
    for node in nodes:
        if not isinstance(node, dict):
            continue
        if not node.get("directory"):
            item = _native_summary_item(node)
            if item.get("path"):
                out.append(item)
        children = node.get("children")
        if isinstance(children, list):
            _collect_tree_file_items(children, out)


def native_summary_from_messages(messages: List[Dict[str, Any]]) -> Dict[str, Any]:
    summary: Dict[str, Any] = {
        "parts": 0,
        "todos": 0,
        "changes": 0,
        "refs": 0,
        "usedContext": 0,
        "trees": 0,
        "tokens": 0,
        "errors": 0,
        "modelLabels": [],
        "keywords": [],
        "referenceItems": [],
        "changeItems": [],
        "treeItems": [],
        "searchText": "",
    }
    models: Dict[str, bool] = {}
    keywords: Dict[str, bool] = {}
    for message in messages or []:
        if not isinstance(message, dict):
            continue
        payload = message.get("nativePayload") or message.get("native_payload")
        if not isinstance(payload, dict):
            continue
        model = str(payload.get("model") or "").strip()
        if model:
            models[model] = True
        if payload.get("error"):
            summary["errors"] += 1
        parts = payload.get("responseParts") or payload.get("response_parts") or []
        if isinstance(parts, list):
            summary["parts"] += len(parts)
            for part in parts:
                if not isinstance(part, dict):
                    continue
                kind = str(part.get("kind") or "").strip()
                if kind == "todoList":
                    todo = part.get("todo") if isinstance(part.get("todo"), dict) else {}
                    todo_list = todo.get("todoList") or part.get("todoList") or part.get("todo_list") or []
                    summary["todos"] += len(todo_list) if isinstance(todo_list, list) else 1
                elif kind == "modifiedFilesConfirmation":
                    modified = part.get("modifiedFiles") if isinstance(part.get("modifiedFiles"), dict) else {}
                    files = modified.get("files") or part.get("files") or []
                    summary["changes"] += len(files) if isinstance(files, list) else 1
                    if isinstance(files, list):
                        for file_info in files[:5]:
                            item = _native_summary_item(
                                file_info, "Modified file")
                            if item and len(summary["changeItems"]) < 5:
                                summary["changeItems"].append(item)
                elif kind:
                    keywords[kind] = True
        for key in ("contentReferences", "content_references", "responseReferences", "response_references"):
            refs = payload.get(key)
            if isinstance(refs, list):
                summary["refs"] += len(refs)
                for ref in refs[:5]:
                    item = _native_summary_item(ref, "Reference")
                    if item and len(summary["referenceItems"]) < 5:
                        summary["referenceItems"].append(item)
                break
        used_context = payload.get("usedContext") or payload.get("used_context") or []
        if isinstance(used_context, list):
            summary["usedContext"] += len(used_context)
        trees = payload.get("fileTrees") or payload.get("file_trees") or []
        if isinstance(trees, list):
            summary["trees"] += len(trees)
            tree_items: List[Dict[str, str]] = []
            for tree in trees:
                if isinstance(tree, dict):
                    _collect_tree_file_items(
                        tree.get("nodes") or tree.get("items"), tree_items)
                elif isinstance(tree, list):
                    _collect_tree_file_items(tree, tree_items)
            for item in tree_items[:5]:
                if item and len(summary["treeItems"]) < 5:
                    summary["treeItems"].append(item)
        usage = payload.get("usage") if isinstance(payload.get("usage"), dict) else {}
        total = usage.get("total_tokens") or usage.get("totalTokens") or usage.get("total") or 0
        try:
            total_int = int(total)
        except (TypeError, ValueError):
            total_int = 0
        if total_int > 0:
            summary["tokens"] += total_int
    summary["modelLabels"] = sorted(models)
    summary["keywords"] = sorted(keywords)
    summary["searchText"] = _native_summary_search_text(summary)
    return summary


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
        "native_summary": native_summary_from_messages(messages),
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
            head = f.read(8192)
        text = head.decode("utf-8", errors="replace")
        # The JSON is written with indent=1, so header fields appear in the
        # first ~200 bytes before the "messages" array.
        entry: Dict[str, Any] = {"id": cid, "title": "Untitled",
                                  "saved_at": 0, "message_count": 0,
                                  "model": "", "native_summary": {}}
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
        marker = '"native_summary"'
        idx = text.find(marker)
        if idx >= 0:
            colon = text.find(":", idx + len(marker))
            brace = text.find("{", colon + 1)
            if colon >= 0 and brace >= 0:
                try:
                    entry["native_summary"] = json.JSONDecoder().raw_decode(text[brace:])[0]
                except (json.JSONDecodeError, TypeError, ValueError):
                    entry["native_summary"] = {}
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
