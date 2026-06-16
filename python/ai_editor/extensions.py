"""AI Editor extension system — pulls from the real VSCode Marketplace.

Queries ``marketplace.visualstudio.com/_apis/public/gallery/extensionquery``
to search/list extensions, displays them with real icons, install counts, and
ratings.  "Install" downloads the VSIX and extracts the package.json to
register the extension's contributed providers into the AI Editor engine.

No hardcoded extension list — everything comes from the marketplace.
"""

from __future__ import annotations

import json
import os
import threading
import zipfile
import io
from typing import Any, Dict, List, Optional, Tuple

_MARKETPLACE_URL = "https://marketplace.visualstudio.com/_apis/public/gallery/extensionquery"
_API_VERSION = "6.1-preview.1"

# Flags: IncludeVersions | IncludeFiles | IncludeCategoryAndTags |
#        IncludeStatistics | IncludeLatestVersionOnly | ExcludeNonValidated
_QUERY_FLAGS = 0x200 | 0x2 | 0x20 | 0x80 | 0x100 | 0x10  # 914


# ---------------------------------------------------------------------------
# Marketplace API client
# ---------------------------------------------------------------------------

def _marketplace_query(
    search_text: str = "",
    category: str = "",
    page: int = 1,
    page_size: int = 30,
    sort_by: int = 4,  # 4=InstallCount, 6=Rating, 12=PublishedDate
    extension_name: str = "",
) -> Dict[str, Any]:
    """Query the VSCode Marketplace. Returns raw API response."""
    criteria = [
        {"filterType": 8, "value": "Microsoft.VisualStudio.Code"},
    ]
    if search_text:
        criteria.append({"filterType": 10, "value": search_text})
    if category:
        criteria.append({"filterType": 5, "value": category})
    if extension_name:
        criteria.append({"filterType": 7, "value": extension_name})

    body = {
        "filters": [{
            "criteria": criteria,
            "pageNumber": page,
            "pageSize": page_size,
            "sortBy": sort_by,
            "sortOrder": 0,
        }],
        "assetTypes": [],
        "flags": _QUERY_FLAGS,
    }

    import httpx
    headers = {
        "Content-Type": "application/json",
        "Accept": f"application/json;api-version={_API_VERSION}",
    }
    with httpx.Client(timeout=15.0) as client:
        resp = client.post(_MARKETPLACE_URL, json=body, headers=headers)
        resp.raise_for_status()
        return resp.json()


def search_extensions(query: str = "ai chat model", page: int = 1, page_size: int = 20) -> List[Dict[str, Any]]:
    """Search marketplace and return normalized extension list."""
    try:
        data = _marketplace_query(search_text=query, page=page, page_size=page_size)
    except Exception as exc:
        return [{"error": str(exc)}]
    return _parse_extensions(data)


def get_extension_detail(publisher: str, name: str) -> Optional[Dict[str, Any]]:
    """Fetch a single extension by publisher.name."""
    try:
        data = _marketplace_query(extension_name=f"{publisher}.{name}")
    except Exception:
        return None
    results = _parse_extensions(data)
    return results[0] if results else None


def _parse_extensions(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Normalize marketplace API response into simple dicts."""
    results: List[Dict[str, Any]] = []
    for result in data.get("results", []):
        for ext in result.get("extensions", []):
            try:
                results.append(_parse_one_extension(ext))
            except Exception:
                continue
    return results


def _parse_one_extension(ext: Dict[str, Any]) -> Dict[str, Any]:
    publisher = ext.get("publisher", {})
    pub_name = publisher.get("publisherName", "")
    pub_display = publisher.get("displayName", pub_name)

    versions = ext.get("versions", [])
    latest = versions[0] if versions else {}
    version = latest.get("version", "")

    # Extract icon URL
    icon_url = ""
    for f in latest.get("files", []):
        if f.get("assetType") == "Microsoft.VisualStudio.Services.Icons.Default":
            icon_url = f.get("source", "")
            break
        if f.get("assetType") == "Microsoft.VisualStudio.Services.Icons.Small":
            icon_url = f.get("source", "")

    # Statistics
    stats = {}
    for s in ext.get("statistics", []):
        stats[s.get("statisticName", "")] = s.get("value", 0)

    install_count = int(stats.get("install", 0) + stats.get("updateCount", 0))
    rating = round(stats.get("averagerating", 0), 1)
    rating_count = int(stats.get("ratingcount", 0))

    # VSIX download URL
    vsix_url = ""
    for f in latest.get("files", []):
        if f.get("assetType") == "Microsoft.VisualStudio.Services.VSIXPackage":
            vsix_url = f.get("source", "")
            break

    # Tags / categories
    tags = [t for t in (ext.get("tags") or []) if not t.startswith("__")]
    categories = ext.get("categories", [])

    return {
        "id": f"{pub_name}.{ext.get('extensionName', '')}",
        "name": ext.get("extensionName", ""),
        "displayName": ext.get("displayName", ""),
        "description": ext.get("shortDescription", ""),
        "version": version,
        "publisher": pub_display,
        "publisherId": pub_name,
        "iconUrl": icon_url,
        "installCount": install_count,
        "rating": rating,
        "ratingCount": rating_count,
        "categories": categories,
        "tags": tags,
        "vsixUrl": vsix_url,
        "lastUpdated": ext.get("lastUpdated", ""),
    }


# ---------------------------------------------------------------------------
# Extension installer
# ---------------------------------------------------------------------------

def _extensions_dir() -> str:
    try:
        from config import BASE_DIR
        d = os.path.join(BASE_DIR, "ai_editor_extensions")
    except ImportError:
        d = os.path.join(os.path.dirname(__file__), "..", "ai_editor_extensions")
    os.makedirs(d, exist_ok=True)
    return d


def download_vsix(vsix_url: str, ext_id: str) -> str:
    """Download a VSIX file. Returns local path."""
    if not vsix_url:
        raise ValueError("No VSIX URL")
    import httpx
    dest = os.path.join(_extensions_dir(), f"{ext_id}.vsix")
    with httpx.Client(timeout=60.0, follow_redirects=True) as client:
        resp = client.get(vsix_url)
        resp.raise_for_status()
        with open(dest, "wb") as f:
            f.write(resp.content)
    return dest


def extract_vsix_manifest(vsix_path: str) -> Dict[str, Any]:
    """Extract package.json from a VSIX file."""
    with zipfile.ZipFile(vsix_path, "r") as zf:
        for name in zf.namelist():
            if name.endswith("package.json"):
                with zf.open(name) as f:
                    return json.loads(f.read())
    return {}


def install_extension(ext_id: str, vsix_url: str = "") -> Dict[str, Any]:
    """Download, extract, and register an extension."""
    try:
        if vsix_url:
            vsix_path = download_vsix(vsix_url, ext_id)
            manifest = extract_vsix_manifest(vsix_path)
        else:
            manifest = {}
        # Save install state
        state_path = os.path.join(_extensions_dir(), f"{ext_id}.json")
        state = {
            "id": ext_id,
            "installed_at": __import__("time").time(),
            "manifest": manifest,
        }
        with open(state_path, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=1)
        return {"ok": True, "id": ext_id, "has_manifest": bool(manifest)}
    except Exception as exc:
        return {"error": str(exc)}


def uninstall_extension(ext_id: str) -> Dict[str, Any]:
    """Remove an installed extension."""
    d = _extensions_dir()
    for suffix in (".json", ".vsix"):
        p = os.path.join(d, f"{ext_id}{suffix}")
        try:
            os.remove(p)
        except OSError:
            pass
    return {"ok": True, "id": ext_id}


def list_installed() -> List[Dict[str, Any]]:
    """List locally installed extensions."""
    d = _extensions_dir()
    result = []
    if not os.path.isdir(d):
        return result
    for fname in os.listdir(d):
        if not fname.endswith(".json"):
            continue
        try:
            with open(os.path.join(d, fname), "r", encoding="utf-8") as f:
                data = json.load(f)
            result.append({
                "id": data.get("id", fname[:-5]),
                "installed_at": data.get("installed_at", 0),
                "has_manifest": bool(data.get("manifest")),
            })
        except Exception:
            continue
    return result


def is_installed(ext_id: str) -> bool:
    return os.path.isfile(os.path.join(_extensions_dir(), f"{ext_id}.json"))


# ---------------------------------------------------------------------------
# Extension tool loading — parse package.json contributes
# ---------------------------------------------------------------------------

def load_extension_tools(ext_id: str) -> List[Dict[str, Any]]:
    """Extract tool definitions from an installed extension's package.json.

    Reads ``contributes.chatParticipants``, ``contributes.menus``,
    ``contributes.commands``, and ``contributes.languageModelTools``
    to discover tools the extension provides.
    """
    state_path = os.path.join(_extensions_dir(), f"{ext_id}.json")
    if not os.path.isfile(state_path):
        return []
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
        manifest = state.get("manifest", {})
        if not manifest:
            return []
        contributes = manifest.get("contributes", {})
        tools: List[Dict[str, Any]] = []

        # languageModelTools (VSCode proposed API)
        for t in contributes.get("languageModelTools", []):
            tools.append({
                "type": "function",
                "function": {
                    "name": f"ext_{ext_id.replace('.','_')}_{t.get('name','')}",
                    "description": f"[{ext_id}] {t.get('modelDescription', t.get('displayName', ''))}",
                    "parameters": t.get("inputSchema", {"type": "object", "properties": {}}),
                },
                "source": "extension",
                "extension_id": ext_id,
                "original_name": t.get("name", ""),
            })

        # chatParticipants → register as tools
        for p in contributes.get("chatParticipants", []):
            pid = p.get("id", "")
            tools.append({
                "type": "function",
                "function": {
                    "name": f"ext_{ext_id.replace('.','_')}_chat_{pid.replace('.','_')}",
                    "description": f"[{ext_id}] Chat participant: {p.get('fullName', p.get('name', pid))}",
                    "parameters": {"type": "object", "properties": {
                        "message": {"type": "string", "description": "Message to send"},
                    }, "required": ["message"]},
                },
                "source": "extension",
                "extension_id": ext_id,
                "participant_id": pid,
            })

        # commands → register as callable tools
        for cmd in contributes.get("commands", []):
            cmd_id = cmd.get("command", "")
            if not cmd_id:
                continue
            tools.append({
                "type": "function",
                "function": {
                    "name": f"ext_{ext_id.replace('.','_')}_cmd_{cmd_id.replace('.','_')}",
                    "description": f"[{ext_id}] Command: {cmd.get('title', cmd_id)}",
                    "parameters": {"type": "object", "properties": {}},
                },
                "source": "extension",
                "extension_id": ext_id,
                "command_id": cmd_id,
            })

        return tools
    except Exception:
        return []


def load_all_extension_tools() -> List[Dict[str, Any]]:
    """Load tools from all installed extensions."""
    all_tools = []
    for entry in list_installed():
        all_tools.extend(load_extension_tools(entry["id"]))
    return all_tools
