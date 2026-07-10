"""AI Editor extension system — pulls from the real VSCode Marketplace.

Queries ``marketplace.visualstudio.com/_apis/public/gallery/extensionquery``
to search/list extensions, displays them with real icons, install counts, and
ratings.  "Install" downloads the VSIX and extracts the package.json to
register the extension's contributed providers into the AI Editor engine.

No hardcoded extension list — everything comes from the marketplace.
"""

from __future__ import annotations

import json
import logging
import ntpath
import os
import re
import shutil
import stat
import tempfile
import time
import zipfile
from typing import Any, Dict, List, Optional, Tuple

_MARKETPLACE_URL = "https://marketplace.visualstudio.com/_apis/public/gallery/extensionquery"
_API_VERSION = "6.1-preview.1"

# Flags: IncludeVersions | IncludeFiles | IncludeCategoryAndTags |
#        IncludeStatistics | IncludeLatestVersionOnly | ExcludeNonValidated
_QUERY_FLAGS = 0x200 | 0x2 | 0x20 | 0x80 | 0x100 | 0x10  # 914
_http_client = None

logger = logging.getLogger(__name__)

# VSIX packages are untrusted archives.  These limits are deliberately high
# enough for normal language/theme extensions while bounding memory, disk and
# decompression work before extension code can run.
MAX_VSIX_DOWNLOAD_BYTES = 256 * 1024 * 1024
MAX_VSIX_FILES = 20_000
MAX_VSIX_SINGLE_FILE_BYTES = 512 * 1024 * 1024
MAX_VSIX_EXPANDED_BYTES = 1024 * 1024 * 1024
MAX_VSIX_COMPRESSION_RATIO = 250.0
MAX_VSIX_MANIFEST_BYTES = 4 * 1024 * 1024
_DOWNLOAD_CHUNK_BYTES = 1024 * 1024
_EXTRACT_CHUNK_BYTES = 1024 * 1024

_EXTENSION_ID_PART_RE = re.compile(r"\A[A-Za-z0-9][A-Za-z0-9_-]{0,127}\Z")
_WINDOWS_DEVICE_NAMES = {
    "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$",
    *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
    *(f"COM{i}" for i in "¹²³"),
    *(f"LPT{i}" for i in "¹²³"),
}


def _is_windows_device_name(value: str) -> bool:
    """Return True for names Windows resolves as DOS device paths."""
    return value.rstrip(" .").split(".", 1)[0].upper() in _WINDOWS_DEVICE_NAMES


def _validate_extension_id(ext_id: str) -> str:
    """Validate an extension id before using it as a filesystem name."""
    value = str(ext_id or "")
    if not value or len(value) > 256 or value != value.strip():
        raise ValueError("Extension id must be a non-empty publisher.name value")
    publisher, separator, name = value.partition(".")
    name_parts = name.split(".") if separator else []
    if (
        not separator
        or not _EXTENSION_ID_PART_RE.fullmatch(publisher)
        or not name_parts
        or any(not _EXTENSION_ID_PART_RE.fullmatch(part) for part in name_parts)
        # Windows resolves the part before the first dot as the device stem.
        or _is_windows_device_name(publisher)
    ):
        raise ValueError(f"Unsafe extension id; expected publisher.name: {value}")
    return value


def _path_is_within(path: str, root: str, *, allow_root: bool = False) -> bool:
    """Check containment after resolving symlinks/junctions and case aliases."""
    try:
        root_real = os.path.normcase(os.path.realpath(os.path.abspath(root)))
        path_real = os.path.normcase(os.path.realpath(os.path.abspath(path)))
        common = os.path.normcase(os.path.commonpath([root_real, path_real]))
    except (OSError, ValueError):
        return False
    return common == root_real and (allow_root or path_real != root_real)


def _extension_storage_path(root: str, leaf: str) -> str:
    path = _extension_storage_entry(root, leaf)
    if not _path_is_within(path, root):
        raise ValueError(f"Extension storage path escapes its root: {leaf}")
    return path


def _extension_storage_entry(root: str, leaf: str) -> str:
    """Build a lexically-contained path without following its final symlink."""
    root_abs = os.path.abspath(root)
    path = os.path.abspath(os.path.join(root_abs, leaf))
    try:
        common = os.path.commonpath([root_abs, path])
    except ValueError as exc:
        raise ValueError(f"Extension storage path escapes its root: {leaf}") from exc
    if os.path.normcase(common) != os.path.normcase(root_abs) or path == root_abs:
        raise ValueError(f"Extension storage path escapes its root: {leaf}")
    return path


def _safe_archive_parts(member_name: str) -> Tuple[str, ...]:
    """Normalize one ZIP member without accepting platform-specific escapes."""
    raw = str(member_name or "")
    if not raw or "\x00" in raw:
        raise ValueError("VSIX contains an empty or NUL-containing path")
    normalized = raw.replace("\\", "/")
    if normalized.startswith("/") or ntpath.splitdrive(normalized)[0]:
        raise ValueError(f"VSIX contains an absolute or drive path: {raw}")
    if normalized.endswith("/"):
        normalized = normalized[:-1]
    parts = normalized.split("/") if normalized else []
    if not parts or any(part in {"", ".", ".."} for part in parts):
        raise ValueError(f"VSIX contains an unsafe relative path: {raw}")
    for part in parts:
        # Colons include drive-relative paths and NTFS alternate data streams.
        if ":" in part or part != part.rstrip(" .") or _is_windows_device_name(part):
            raise ValueError(f"VSIX contains an unsafe Windows path: {raw}")
    return tuple(parts)


def _inspect_vsix(
    zf: zipfile.ZipFile,
) -> Tuple[List[Tuple[zipfile.ZipInfo, Tuple[str, ...], bool]], Tuple[str, ...]]:
    """Validate archive metadata and return entries plus the chosen manifest."""
    infos = zf.infolist()
    if len(infos) > MAX_VSIX_FILES:
        raise ValueError(f"VSIX contains too many entries ({len(infos)} > {MAX_VSIX_FILES})")

    entries: List[Tuple[zipfile.ZipInfo, Tuple[str, ...], bool]] = []
    manifests: List[Tuple[str, ...]] = []
    expanded_total = 0
    seen_paths: set[str] = set()
    for info in infos:
        parts = _safe_archive_parts(info.filename)
        is_dir = bool(info.is_dir() or info.filename.replace("\\", "/").endswith("/"))
        folded = "/".join(parts).casefold()
        if folded in seen_paths:
            raise ValueError(f"VSIX contains duplicate paths: {info.filename}")
        seen_paths.add(folded)

        mode = (info.external_attr >> 16) & 0xFFFF
        file_type = stat.S_IFMT(mode)
        if stat.S_ISLNK(mode):
            raise ValueError(f"VSIX symlinks are not allowed: {info.filename}")
        if file_type not in (0, stat.S_IFREG, stat.S_IFDIR):
            raise ValueError(f"VSIX special files are not allowed: {info.filename}")
        if info.flag_bits & 0x1:
            raise ValueError(f"Encrypted VSIX entries are not supported: {info.filename}")

        if not is_dir:
            if info.file_size < 0 or info.compress_size < 0:
                raise ValueError(f"VSIX contains invalid file sizes: {info.filename}")
            if info.file_size > MAX_VSIX_SINGLE_FILE_BYTES:
                raise ValueError(
                    f"VSIX file exceeds the per-file limit: {info.filename}"
                )
            expanded_total += info.file_size
            if expanded_total > MAX_VSIX_EXPANDED_BYTES:
                raise ValueError("VSIX exceeds the total expanded-size limit")
            if info.file_size:
                ratio = info.file_size / max(info.compress_size, 1)
                if ratio > MAX_VSIX_COMPRESSION_RATIO:
                    raise ValueError(
                        f"VSIX entry exceeds the compression-ratio limit: {info.filename}"
                    )
            if parts[-1] == "package.json":
                manifests.append(parts)
        entries.append((info, parts, is_dir))

    if not manifests:
        raise ValueError("Downloaded VSIX did not contain a package.json manifest")
    standard = ("extension", "package.json")
    manifest_parts = (
        standard
        if standard in manifests
        else min(manifests, key=lambda p: (len(p), p))
    )
    return entries, manifest_parts


def _read_json_file(path: str, *, max_bytes: Optional[int] = None) -> Dict[str, Any]:
    if max_bytes is None:
        max_bytes = MAX_VSIX_MANIFEST_BYTES
    size = os.path.getsize(path)
    if size > max_bytes:
        raise ValueError("VSIX package.json exceeds the manifest-size limit")
    with open(path, "rb") as fh:
        raw = fh.read(max_bytes + 1)
    if len(raw) > max_bytes:
        raise ValueError("VSIX package.json exceeds the manifest-size limit")
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("VSIX package.json must contain a JSON object")
    return value


def _write_json_atomic(path: str, value: Dict[str, Any]) -> None:
    root = os.path.dirname(path)
    fd, temporary = tempfile.mkstemp(prefix=".extension-state-", suffix=".json", dir=root)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(value, fh, ensure_ascii=False, indent=1)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.remove(temporary)
        except OSError:
            pass
        raise


def _remove_path_no_follow(path: str) -> None:
    if not os.path.lexists(path):
        return
    if os.path.islink(path):
        os.unlink(path)
    elif getattr(os.path, "isjunction", lambda _p: False)(path):
        os.rmdir(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)
    else:
        os.remove(path)


def _path_is_link_or_reparse(path: str, file_stat: Any = None) -> bool:
    """Return True for symlinks, junctions, or any Windows reparse entry."""
    try:
        if os.path.islink(path):
            return True
        is_junction = getattr(os.path, "isjunction", None)
        if callable(is_junction) and is_junction(path):
            return True
        info = file_stat if file_stat is not None else os.lstat(path)
    except OSError:
        # A path that cannot be inspected safely must not be copied.
        return True
    attributes = int(getattr(info, "st_file_attributes", 0) or 0)
    reparse_flag = int(getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))
    return bool(attributes & reparse_flag)


def _copy_local_extension_tree(source_dir: str, dest_dir: str) -> None:
    """Copy an unpacked extension without following filesystem indirections.

    Local folders are no more trusted than downloaded archives.  The walk is
    bounded by the same entry/per-file/expanded-size limits used for VSIX and
    rejects symlinks, junctions, reparse points, and special files before any
    extension code can reach the managed install directory.
    """
    source_root = os.path.abspath(source_dir)
    dest_root = os.path.abspath(dest_dir)
    try:
        root_stat = os.lstat(source_root)
    except OSError as exc:
        raise ValueError(f"Local extension directory is unreadable: {exc}") from exc
    if _path_is_link_or_reparse(source_root, root_stat):
        raise ValueError("Local extension source cannot be a link, junction, or reparse point")
    if not stat.S_ISDIR(root_stat.st_mode):
        raise ValueError("Local extension source must be a directory")
    try:
        common = os.path.normcase(os.path.commonpath([source_root, dest_root]))
    except ValueError:
        # Different Windows drives cannot contain one another and are valid
        # for a user-selected source copied into the managed extension drive.
        common = ""
    if common == os.path.normcase(source_root):
        raise ValueError(
            "Local extension source cannot contain its managed staging directory")
    os.makedirs(dest_root, exist_ok=True)
    if os.listdir(dest_root):
        raise ValueError("Local extension staging directory must be empty")

    entry_count = 0
    expanded_total = 0
    pending: List[Tuple[str, str]] = [(source_root, dest_root)]
    while pending:
        current_source, current_dest = pending.pop()
        try:
            current_stat = os.lstat(current_source)
        except OSError as exc:
            raise ValueError(
                f"Local extension entry became unreadable: {current_source}") from exc
        if (_path_is_link_or_reparse(current_source, current_stat)
                or not stat.S_ISDIR(current_stat.st_mode)):
            raise ValueError(
                f"Local extension contains a link, junction, or reparse directory: "
                f"{current_source}")
        try:
            entries = sorted(os.scandir(current_source), key=lambda item: item.name.casefold())
        except OSError as exc:
            raise ValueError(
                f"Local extension directory cannot be enumerated: {current_source}") from exc
        for entry in entries:
            entry_count += 1
            if entry_count > MAX_VSIX_FILES:
                raise ValueError(
                    f"Local extension contains too many entries "
                    f"({entry_count} > {MAX_VSIX_FILES})")
            source_path = entry.path
            relative = os.path.relpath(source_path, source_root)
            target_path = os.path.join(dest_root, relative)
            if (not _path_is_within(source_path, source_root)
                    or not _path_is_within(target_path, dest_root)):
                raise ValueError(
                    f"Local extension entry escapes its root: {relative}")
            try:
                entry_stat = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise ValueError(
                    f"Local extension entry cannot be inspected: {relative}") from exc
            if _path_is_link_or_reparse(source_path, entry_stat):
                raise ValueError(
                    f"Local extension links, junctions, and reparse points are not allowed: "
                    f"{relative}")
            if stat.S_ISDIR(entry_stat.st_mode):
                os.mkdir(target_path)
                pending.append((source_path, target_path))
                continue
            if not stat.S_ISREG(entry_stat.st_mode):
                raise ValueError(
                    f"Local extension special files are not allowed: {relative}")
            if entry_stat.st_size > MAX_VSIX_SINGLE_FILE_BYTES:
                raise ValueError(
                    f"Local extension file exceeds the per-file limit: {relative}")

            written = 0
            try:
                with open(source_path, "rb") as src, open(target_path, "xb") as dst:
                    while True:
                        chunk = src.read(_EXTRACT_CHUNK_BYTES)
                        if not chunk:
                            break
                        written += len(chunk)
                        expanded_total += len(chunk)
                        if written > MAX_VSIX_SINGLE_FILE_BYTES:
                            raise ValueError(
                                f"Local extension file exceeds the per-file limit: "
                                f"{relative}")
                        if expanded_total > MAX_VSIX_EXPANDED_BYTES:
                            raise ValueError(
                                "Local extension exceeds the total expanded-size limit")
                        dst.write(chunk)
            except ValueError:
                raise
            except OSError as exc:
                raise ValueError(
                    f"Local extension file could not be copied: {relative}") from exc
            try:
                final_stat = os.lstat(source_path)
            except OSError as exc:
                raise ValueError(
                    f"Local extension file changed while copying: {relative}") from exc
            if (_path_is_link_or_reparse(source_path, final_stat)
                    or not stat.S_ISREG(final_stat.st_mode)
                    or final_stat.st_size != entry_stat.st_size
                    or written != entry_stat.st_size):
                raise ValueError(
                    f"Local extension file changed while copying: {relative}")
def _commit_extension_install(
    staging_dir: str,
    ext_dir: str,
    state_path: str,
    state: Dict[str, Any],
) -> None:
    """Commit a staged directory and state, restoring the prior install on error."""
    root = os.path.dirname(ext_dir)
    if not _path_is_within(staging_dir, root) or not _path_is_within(ext_dir, root):
        raise ValueError("Extension install paths must remain inside the extension root")
    backup_root = tempfile.mkdtemp(prefix=".extension-backup-", dir=root)
    old_dir = os.path.join(backup_root, "extension")
    old_state = os.path.join(backup_root, "state.json")
    moved_old_dir = False
    moved_old_state = False
    moved_new_dir = False
    state_write_started = False
    preserve_backup = False
    try:
        if os.path.lexists(ext_dir):
            os.replace(ext_dir, old_dir)
            moved_old_dir = True
        if os.path.lexists(state_path):
            os.replace(state_path, old_state)
            moved_old_state = True
        os.replace(staging_dir, ext_dir)
        moved_new_dir = True
        state_write_started = True
        _write_json_atomic(state_path, state)
    except Exception as install_error:
        recovery_errors: List[str] = []
        try:
            if moved_new_dir and os.path.lexists(ext_dir):
                _remove_path_no_follow(ext_dir)
        except Exception as exc:
            recovery_errors.append(f"remove incomplete install: {exc}")
        try:
            if state_write_started and os.path.lexists(state_path):
                _remove_path_no_follow(state_path)
        except Exception as exc:
            recovery_errors.append(f"remove incomplete state: {exc}")
        try:
            if moved_old_dir and os.path.lexists(old_dir):
                os.replace(old_dir, ext_dir)
        except Exception as exc:
            recovery_errors.append(f"restore previous extension: {exc}")
        try:
            if moved_old_state and os.path.lexists(old_state):
                os.replace(old_state, state_path)
        except Exception as exc:
            recovery_errors.append(f"restore previous state: {exc}")
        if recovery_errors:
            preserve_backup = True
            raise RuntimeError(
                "Extension install failed and recovery was incomplete; "
                f"backup preserved at {backup_root}: {'; '.join(recovery_errors)}"
            ) from install_error
        raise
    finally:
        if not preserve_backup:
            try:
                _remove_path_no_follow(backup_root)
            except OSError:
                logger.warning("Failed to remove extension install backup %s", backup_root,
                               exc_info=True)

def _get_http_client():
    global _http_client
    if _http_client is None:
        import httpx
        _http_client = httpx.Client(
            timeout=60.0, follow_redirects=True,
            verify=True,
        )
    return _http_client


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

    headers = {
        "Content-Type": "application/json",
        "Accept": f"application/json;api-version={_API_VERSION}",
    }
    resp = _get_http_client().post(_MARKETPLACE_URL, json=body, headers=headers)
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
    except Exception as exc:
        logger.warning("Failed to fetch extension detail for %s.%s: %s",
                       publisher, name, exc)
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
            except Exception as exc:
                logger.warning("Failed to parse extension payload for %s: %s",
                               ext.get("extensionName", "<unknown>"), exc)
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

    # VSIX download URL — prefer the canonical marketplace download endpoint
    # over the CDN source URL which may 404 on .azure.cn mirrors.
    ext_name = ext.get("extensionName", "")
    vsix_url = (
        f"https://marketplace.visualstudio.com/_apis/public/gallery/publishers/"
        f"{pub_name}/vsextensions/{ext_name}/{version}/vspackage"
    ) if pub_name and ext_name and version else ""
    if not vsix_url:
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
        "latestVersion": version,
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
    ext_id = _validate_extension_id(ext_id)
    root = _extensions_dir()
    dest = _extension_storage_path(root, f"{ext_id}.vsix")
    fd, temporary = tempfile.mkstemp(prefix=".vsix-download-", suffix=".part", dir=root)
    try:
        total = 0
        with os.fdopen(fd, "wb") as fh:
            with _get_http_client().stream("GET", vsix_url) as resp:
                resp.raise_for_status()
                declared = resp.headers.get("Content-Length")
                if declared:
                    try:
                        declared_size = int(declared)
                    except (TypeError, ValueError) as exc:
                        raise ValueError("VSIX response has an invalid Content-Length") from exc
                    if declared_size < 0 or declared_size > MAX_VSIX_DOWNLOAD_BYTES:
                        raise ValueError("VSIX download exceeds the compressed-size limit")
                for chunk in resp.iter_bytes(chunk_size=_DOWNLOAD_CHUNK_BYTES):
                    if not chunk:
                        continue
                    total += len(chunk)
                    if total > MAX_VSIX_DOWNLOAD_BYTES:
                        raise ValueError("VSIX download exceeds the compressed-size limit")
                    fh.write(chunk)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(temporary, dest)
        return dest
    except Exception:
        try:
            os.remove(temporary)
        except OSError:
            pass
        raise


def extract_vsix_manifest(vsix_path: str) -> Dict[str, Any]:
    """Extract package.json from a VSIX file."""
    with zipfile.ZipFile(vsix_path, "r") as zf:
        entries, manifest_parts = _inspect_vsix(zf)
        manifest_info = next(
            info for info, parts, is_dir in entries
            if not is_dir and parts == manifest_parts
        )
        if manifest_info.file_size > MAX_VSIX_MANIFEST_BYTES:
            raise ValueError("VSIX package.json exceeds the manifest-size limit")
        with zf.open(manifest_info) as fh:
            raw = fh.read(MAX_VSIX_MANIFEST_BYTES + 1)
        if len(raw) > MAX_VSIX_MANIFEST_BYTES:
            raise ValueError("VSIX package.json exceeds the manifest-size limit")
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError("VSIX package.json must contain a JSON object")
        return value


def install_extension(ext_id: str, vsix_url: str = "") -> Dict[str, Any]:
    """Download VSIX, extract to ext directory, and save install state."""
    vsix_path = ""
    staging_dir = ""
    try:
        ext_id = _validate_extension_id(ext_id)
        root = _extensions_dir()
        ext_dir = _extension_storage_path(root, ext_id)

        if not vsix_url:
            publisher, name = ext_id.split(".", 1)
            detail = get_extension_detail(publisher, name)
            if not detail or not detail.get("vsixUrl"):
                return {"error": f"Marketplace did not return a VSIX URL for {ext_id}"}
            vsix_url = detail["vsixUrl"]

        vsix_path = download_vsix(vsix_url, ext_id)
        staging_dir = tempfile.mkdtemp(prefix=".extension-staging-", dir=root)
        _extract_vsix_to_dir(vsix_path, staging_dir)
        manifest = _read_json_file(os.path.join(staging_dir, "package.json"))
        manifest_id = _validate_extension_id(
            f"{manifest.get('publisher', '')}.{manifest.get('name', '')}"
        )
        if manifest_id.lower() != ext_id.lower():
            raise ValueError(
                f"VSIX manifest id mismatch: expected {ext_id}, got {manifest_id}"
            )

        # Save install state
        state_path = _extension_storage_path(root, f"{ext_id}.json")
        state = {
            "id": ext_id,
            "installed_at": time.time(),
            "manifest": manifest,
            "ext_dir": ext_dir,
            "manifest_path": os.path.join(ext_dir, "package.json"),
        }
        _commit_extension_install(staging_dir, ext_dir, state_path, state)
        staging_dir = ""
        return {"ok": True, "id": ext_id, "has_manifest": bool(manifest),
                "ext_dir": ext_dir}
    except Exception as exc:
        return {"error": str(exc)}
    finally:
        if staging_dir and os.path.lexists(staging_dir):
            try:
                _remove_path_no_follow(staging_dir)
            except OSError:
                logger.debug("Failed to remove VSIX staging directory %s", staging_dir,
                             exc_info=True)
        if vsix_path and os.path.isfile(vsix_path):
            try:
                os.remove(vsix_path)
            except OSError:
                logger.debug("Failed to remove temporary VSIX %s", vsix_path,
                             exc_info=True)


def _extract_vsix_to_dir(vsix_path: str, dest_dir: str) -> None:
    """Safely extract the extension payload into an empty staging directory."""
    if os.path.lexists(dest_dir) and not os.path.isdir(dest_dir):
        raise ValueError("VSIX destination must be a directory")
    os.makedirs(dest_dir, exist_ok=True)
    if os.listdir(dest_dir):
        raise ValueError("VSIX staging directory must be empty")
    staging_root = os.path.realpath(dest_dir)
    with zipfile.ZipFile(vsix_path, "r") as zf:
        entries, manifest_parts = _inspect_vsix(zf)
        prefix = manifest_parts[:-1]
        extracted_paths: set[str] = set()
        expanded_total = 0
        for info, parts, is_dir in entries:
            if is_dir or parts[:len(prefix)] != prefix:
                continue
            rel_parts = parts[len(prefix):]
            if not rel_parts:
                continue
            rel_key = "/".join(rel_parts).casefold()
            if rel_key in extracted_paths:
                raise ValueError(f"VSIX payload contains duplicate output paths: {info.filename}")
            extracted_paths.add(rel_key)
            out = os.path.join(dest_dir, *rel_parts)
            if not _path_is_within(out, staging_root):
                raise ValueError(f"VSIX member escapes the staging directory: {info.filename}")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            written = 0
            with zf.open(info) as src, open(out, "xb") as dst:
                while True:
                    chunk = src.read(_EXTRACT_CHUNK_BYTES)
                    if not chunk:
                        break
                    written += len(chunk)
                    expanded_total += len(chunk)
                    if written > MAX_VSIX_SINGLE_FILE_BYTES:
                        raise ValueError(f"VSIX file exceeds the per-file limit: {info.filename}")
                    if expanded_total > MAX_VSIX_EXPANDED_BYTES:
                        raise ValueError("VSIX exceeds the total expanded-size limit")
                    dst.write(chunk)
            if written != info.file_size:
                raise ValueError(f"VSIX entry size changed while extracting: {info.filename}")


def install_extension_from_local_dir(source_dir: str) -> Dict[str, Any]:
    """Copy an already-unpacked extension folder into this platform's own
    extension directory, so "install from folder" persists across restarts
    the same way a marketplace install does (source_dir itself, e.g. another
    VS Code install's extensions folder, is never referenced again)."""
    staging_dir = ""
    try:
        source_dir = os.path.abspath(str(source_dir or ""))
        manifest_path = os.path.join(source_dir, "package.json")
        if _path_is_link_or_reparse(source_dir):
            return {"error": "Local extension source cannot be a link, junction, or reparse point"}
        if not os.path.isfile(manifest_path) or _path_is_link_or_reparse(manifest_path):
            return {"error": f"No package.json found in {source_dir}"}
        manifest = _read_json_file(manifest_path)
        publisher = str(manifest.get("publisher", "")).strip()
        name = str(manifest.get("name", "")).strip()
        if not publisher or not name:
            return {"error": "package.json is missing publisher/name"}
        ext_id = _validate_extension_id(f"{publisher}.{name}")
        root = _extensions_dir()
        ext_dir = _extension_storage_path(root, ext_id)
        staging_dir = tempfile.mkdtemp(prefix=".extension-staging-", dir=root)
        _copy_local_extension_tree(source_dir, staging_dir)
        staged_manifest = _read_json_file(os.path.join(staging_dir, "package.json"))
        staged_id = _validate_extension_id(
            f"{staged_manifest.get('publisher', '')}.{staged_manifest.get('name', '')}"
        )
        if staged_id.lower() != ext_id.lower():
            raise ValueError("Local extension manifest changed while it was being copied")
        manifest = staged_manifest

        state_path = _extension_storage_path(root, f"{ext_id}.json")
        state = {
            "id": ext_id,
            "installed_at": time.time(),
            "manifest": manifest,
            "ext_dir": ext_dir,
            "manifest_path": os.path.join(ext_dir, "package.json"),
            "source": "local_dir",
            "source_dir": source_dir,
        }
        _commit_extension_install(staging_dir, ext_dir, state_path, state)
        staging_dir = ""
        return {"ok": True, "id": ext_id, "has_manifest": True, "ext_dir": ext_dir}
    except Exception as exc:
        return {"error": str(exc)}
    finally:
        if staging_dir and os.path.lexists(staging_dir):
            try:
                _remove_path_no_follow(staging_dir)
            except OSError:
                logger.debug("Failed to remove local-extension staging directory %s",
                             staging_dir, exc_info=True)


def uninstall_extension(ext_id: str) -> Dict[str, Any]:
    """Remove an installed extension."""
    try:
        ext_id = _validate_extension_id(ext_id)
    except ValueError as exc:
        return {"ok": False, "id": str(ext_id or ""), "error": str(exc)}
    d = _extensions_dir()
    # Use lexical containment here so a tampered symlink/junction at the
    # expected location can be removed as a link without following its target.
    state_path = _extension_storage_entry(d, f"{ext_id}.json")
    expected_ext_dir = _extension_storage_entry(d, ext_id)
    removed: List[str] = []
    warnings: List[str] = []
    if os.path.isfile(state_path) and not os.path.islink(state_path):
        try:
            with open(state_path, "r", encoding="utf-8") as f:
                state = json.load(f)
            claimed_ext_dir = str(state.get("ext_dir") or "")
            claimed_path = os.path.normcase(os.path.abspath(claimed_ext_dir))
            expected_path = os.path.normcase(os.path.abspath(expected_ext_dir))
            if claimed_ext_dir and claimed_path != expected_path:
                warnings.append("Ignored an unsafe ext_dir value in extension state")
        except Exception as exc:
            logger.warning("Failed to read extension state %s: %s",
                           state_path, exc)
    for suffix in (".json", ".vsix"):
        p = _extension_storage_entry(d, f"{ext_id}{suffix}")
        try:
            os.remove(p)
            removed.append(p)
        except OSError:
            continue
    if os.path.lexists(expected_ext_dir):
        try:
            if os.path.islink(expected_ext_dir):
                os.unlink(expected_ext_dir)
            elif getattr(os.path, "isjunction", lambda _p: False)(expected_ext_dir):
                os.rmdir(expected_ext_dir)
            elif not _path_is_within(expected_ext_dir, d):
                raise ValueError("Extension directory resolves outside the extension root")
            else:
                shutil.rmtree(expected_ext_dir)
            removed.append(expected_ext_dir)
        except OSError as exc:
            return {"ok": False, "id": ext_id, "error": str(exc)}
        except ValueError as exc:
            return {"ok": False, "id": ext_id, "error": str(exc), "warnings": warnings}
    result: Dict[str, Any] = {"ok": True, "id": ext_id, "removed": removed}
    if warnings:
        result["warnings"] = warnings
    return result


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
            file_id = _validate_extension_id(fname[:-5])
            state_path = _extension_storage_path(d, fname)
            if os.path.islink(state_path):
                logger.warning("Ignoring symlinked extension state %s", state_path)
                continue
            with open(state_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            state_id = _validate_extension_id(str(data.get("id") or file_id))
            if state_id.lower() != file_id.lower():
                raise ValueError("Extension state id does not match its filename")
            ext_dir = _extension_storage_path(d, file_id)
            manifest_path = os.path.join(ext_dir, "package.json")
            if not _path_is_within(ext_dir, d):
                raise ValueError("Extension directory resolves outside its root")
            manifest = data.get("manifest") or {}
            result.append({
                "id": state_id,
                "installed_at": data.get("installed_at", 0),
                "has_manifest": bool(data.get("manifest")),
                "displayName": manifest.get("displayName", ""),
                "version": manifest.get("version", ""),
                "publisher": manifest.get("publisher", ""),
                "ext_dir": ext_dir,
                "manifest_path": manifest_path,
                "contributes": sorted((manifest.get("contributes") or {}).keys()),
            })
        except Exception as exc:
            logger.warning("Failed to read installed extension state %s: %s",
                           os.path.join(d, fname), exc)
            continue
    return result


def is_installed(ext_id: str) -> bool:
    try:
        ext_id = _validate_extension_id(ext_id)
        state_path = _extension_storage_path(_extensions_dir(), f"{ext_id}.json")
    except ValueError:
        return False
    return os.path.isfile(state_path) and not os.path.islink(state_path)


# ---------------------------------------------------------------------------
# Extension tool loading — parse package.json contributes
# ---------------------------------------------------------------------------

def load_extension_tools(ext_id: str,
                         enabled_contributions: Optional[set[str]] = None) -> List[Dict[str, Any]]:
    """Extract tool definitions from an installed extension's package.json.

    Reads ``contributes.chatParticipants``, ``contributes.menus``,
    ``contributes.commands``, and ``contributes.languageModelTools``
    to discover tools the extension provides.
    """
    try:
        ext_id = _validate_extension_id(ext_id)
        state_path = _extension_storage_path(_extensions_dir(), f"{ext_id}.json")
    except ValueError:
        return []
    if not os.path.isfile(state_path) or os.path.islink(state_path):
        return []
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
        manifest = state.get("manifest", {})
        if not manifest:
            return []
        contributes = manifest.get("contributes", {})
        tools: List[Dict[str, Any]] = []

        def _enabled(name: str) -> bool:
            return enabled_contributions is None or name in enabled_contributions

        # languageModelTools (VSCode proposed API)
        for t in (contributes.get("languageModelTools", []) if _enabled("languageModelTools") else []):
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
                "sourceName": t.get("name", ""),
                "needsExtensionRuntime": True,
                "runtimeAvailable": False,
            })

        # chatParticipants → register as tools
        for p in (contributes.get("chatParticipants", []) if _enabled("chatParticipants") else []):
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
                "sourceName": pid,
                "needsExtensionRuntime": True,
                "runtimeAvailable": False,
            })

        # commands → register as callable tools
        for cmd in (contributes.get("commands", []) if _enabled("commands") else []):
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
                "sourceName": cmd_id,
                "needsExtensionRuntime": True,
                "runtimeAvailable": False,
            })

        return tools
    except Exception as exc:
        logger.warning("Failed to load extension tools for %s: %s",
                       ext_id, exc)
        return []


def load_all_extension_tools(enabled_contributions: Optional[set[str]] = None) -> List[Dict[str, Any]]:
    """Load tools from all installed extensions."""
    all_tools = []
    for entry in list_installed():
        all_tools.extend(load_extension_tools(entry["id"], enabled_contributions))
    return all_tools
