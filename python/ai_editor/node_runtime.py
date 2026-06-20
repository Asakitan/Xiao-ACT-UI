"""Embedded Node.js runtime discovery and bootstrap.

Provides helpers to locate a usable ``node.exe`` binary using a strict
priority order:

1. **Bundled** -- ``runtime/node/node.exe`` relative to the project base dir.
2. **User PATH** -- ``shutil.which("node")``.
3. **Common Windows install locations** (``%ProgramFiles%\\nodejs``, etc.).

The ``ensure_node()`` function can download the single ``node.exe`` binary
(~70 MB) from the official Node.js distribution when no local copy exists.
No npm or node_modules are required -- our ``node_ext_host.js`` has zero
npm dependencies.
"""

from __future__ import annotations

import logging
import os
import shutil
import subprocess
import sys
from typing import Callable, Optional

_log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_NODE_VERSION = "v22.16.0"
_NODE_DOWNLOAD_URL = (
    f"https://nodejs.org/dist/{_NODE_VERSION}/win-x64/node.exe"
)

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _project_base_dir() -> str:
    """Return the project base directory (mirrors config.BASE_DIR logic)."""
    try:
        from config import BASE_DIR
        return str(BASE_DIR)
    except Exception:
        # Fallback: two levels up from this file (ai_editor/ -> python/)
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _bundled_node_path() -> str:
    """Return the path where the bundled node.exe should live."""
    return os.path.join(_project_base_dir(), "runtime", "node", "node.exe")


def _common_install_locations() -> list[str]:
    """Return common Windows Node.js install directories to probe."""
    candidates: list[str] = []
    for env_var in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
        root = os.environ.get(env_var, "")
        if root:
            candidates.append(os.path.join(root, "nodejs", "node.exe"))
    # nvm-windows default
    nvm_home = os.environ.get("NVM_HOME", "")
    if nvm_home:
        nvm_symlink = os.environ.get("NVM_SYMLINK", "")
        if nvm_symlink:
            candidates.append(os.path.join(nvm_symlink, "node.exe"))
    # fnm / volta
    appdata = os.environ.get("APPDATA", "")
    if appdata:
        candidates.append(
            os.path.join(appdata, "fnm", "node-versions", "node.exe"))
        candidates.append(
            os.path.join(appdata, "volta", "bin", "node.exe"))
    local_appdata = os.environ.get("LOCALAPPDATA", "")
    if local_appdata:
        candidates.append(
            os.path.join(local_appdata, "fnm", "node-versions", "node.exe"))
        candidates.append(
            os.path.join(local_appdata, "volta", "bin", "node.exe"))
    return candidates


def _validate_node(path: str) -> bool:
    """Return True if *path* is an existing file that looks like node."""
    if not path or not os.path.isfile(path):
        return False
    # Quick sanity: run ``node --version`` and check exit code
    try:
        result = subprocess.run(
            [path, "--version"],
            capture_output=True,
            timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return result.returncode == 0
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def get_node_path() -> Optional[str]:
    """Locate a usable Node.js executable.

    Search order:
        1. Bundled ``runtime/node/node.exe``
        2. ``shutil.which("node")``
        3. Common Windows install locations

    Returns the absolute path to ``node.exe``, or ``None`` if not found.
    """
    # 1. Bundled
    bundled = _bundled_node_path()
    if os.path.isfile(bundled):
        return bundled

    # 2. User PATH
    on_path = shutil.which("node")
    if on_path:
        return on_path

    # 3. Common install locations
    for candidate in _common_install_locations():
        if os.path.isfile(candidate):
            return candidate

    return None


def is_available() -> bool:
    """Return ``True`` if a usable Node.js binary can be found."""
    return get_node_path() is not None


def node_version() -> Optional[str]:
    """Run ``node --version`` and return the version string, or ``None``."""
    node = get_node_path()
    if not node:
        return None
    try:
        result = subprocess.run(
            [node, "--version"],
            capture_output=True,
            timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        if result.returncode == 0:
            return result.stdout.decode("utf-8", errors="replace").strip()
    except Exception:
        pass
    return None


def ensure_node(
    progress: Optional[Callable[[int, int], None]] = None,
    *,
    force: bool = False,
) -> str:
    """Download ``node.exe`` to the bundled location if not already present.

    Parameters
    ----------
    progress : callable, optional
        Called as ``progress(bytes_downloaded, total_bytes)`` during the
        download.  *total_bytes* may be ``0`` if the server does not send
        a ``Content-Length`` header.
    force : bool
        Re-download even if the bundled binary already exists.

    Returns
    -------
    str
        Absolute path to the downloaded ``node.exe``.

    Raises
    ------
    RuntimeError
        If the download fails.
    """
    dest = _bundled_node_path()

    if os.path.isfile(dest) and not force:
        _log.info("[NodeRuntime] Bundled node.exe already exists: %s", dest)
        return dest

    dest_dir = os.path.dirname(dest)
    os.makedirs(dest_dir, exist_ok=True)

    # Use a temporary name during download to avoid partial files
    tmp_path = dest + ".downloading"
    _log.info("[NodeRuntime] Downloading node.exe from %s", _NODE_DOWNLOAD_URL)

    import urllib.request
    import urllib.error

    try:
        req = urllib.request.Request(
            _NODE_DOWNLOAD_URL,
            headers={"User-Agent": "SAO-AI-Editor/1.0"},
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            total = int(resp.headers.get("Content-Length", 0))
            downloaded = 0
            chunk_size = 256 * 1024  # 256 KB
            with open(tmp_path, "wb") as fh:
                while True:
                    chunk = resp.read(chunk_size)
                    if not chunk:
                        break
                    fh.write(chunk)
                    downloaded += len(chunk)
                    if progress is not None:
                        try:
                            progress(downloaded, total)
                        except Exception:
                            pass
    except (urllib.error.URLError, OSError) as exc:
        # Clean up partial download
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise RuntimeError(
            f"Failed to download node.exe: {exc}"
        ) from exc

    # Atomic rename
    try:
        if os.path.exists(dest):
            os.remove(dest)
        os.rename(tmp_path, dest)
    except OSError as exc:
        raise RuntimeError(
            f"Failed to place node.exe at {dest}: {exc}"
        ) from exc

    _log.info("[NodeRuntime] node.exe downloaded to %s (%d bytes)",
              dest, os.path.getsize(dest))
    return dest
