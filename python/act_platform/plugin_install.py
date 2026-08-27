# -*- coding: utf-8 -*-
# Plugin package install — 一键导入一个 ``.zip`` 插件包。
#
# 可分享的插件 = 一个 ``.zip``，其内容构成**一个插件目录**：
#
# plugin.json (清单, 必需) + plugin.py (入口, 必需)
# [+ requirements.txt + vendor/ + libs/ + assets/ ...]
#
# 清单可以位于压缩包根部，也可以位于唯一的一层顶级文件夹内
# (``my_plugin.zip/plugin.json`` 与 ``my_plugin.zip/my_plugin/plugin.json`` 都接受)。
# 压缩包先以 **zip-slip 防穿越**方式解压到临时目录，校验后移动进
# ``<base>/user_plugins/<plugin_id>/``（更新不被覆盖的用户目录）。
#
# **纯 Python 插件无需任何编译**：加载器在运行时用 ``importlib`` 直接吃原始
# ``.py``，dev 态与 onedir 冻结态都一样（冻结 exe 内嵌完整 CPython）。需要原生
# 扩展 (.pyd) 或第三方依赖的插件，由**作者**预编译 / vendor 进包，用户端零编译。

from __future__ import annotations

import json
import os
import shutil
import stat
import tempfile
import zipfile
from typing import Any, Callable, Optional

from .plugins import MANIFEST_FILE, _safe_id


def _is_within(base: str, target: str) -> bool:
    # ``target`` 是否落在 ``base`` 之内（跨盘符 / 非法路径一律视为不安全）。
    try:
        base_abs = os.path.abspath(base)
        return os.path.commonpath([base_abs, os.path.abspath(target)]) == base_abs
    except ValueError:
        # commonpath 在不同盘符 / 混合相对绝对时抛 ValueError → 当作不安全。
        return False


_WINDOWS_RESERVED_NAMES = {
    "con", "prn", "aux", "nul",
    *(f"com{index}" for index in range(1, 10)),
    *(f"lpt{index}" for index in range(1, 10)),
}
_WINDOWS_INVALID_CHARS = set('<>:"|?*')


def _has_reparse_point(path: str) -> bool:
    if os.name == "nt":
        try:
            import ctypes

            get_attributes = ctypes.windll.kernel32.GetFileAttributesW
            get_attributes.argtypes = [ctypes.c_wchar_p]
            get_attributes.restype = ctypes.c_uint32
            attributes = get_attributes(os.path.abspath(path))
            return attributes == 0xFFFFFFFF or bool(attributes.__and__(0x400))
        except Exception:
            return True
    return os.path.islink(path)


def _assert_no_reparse_chain(path: str) -> None:
    absolute = os.path.abspath(path)
    drive, tail = os.path.splitdrive(absolute)
    current = drive + os.sep if tail.startswith(("\\", "/")) else drive
    for part in tail.lstrip("\\/").replace("/", os.sep).split(os.sep):
        if not part:
            continue
        current = os.path.join(current, part)
        if os.path.lexists(current) and _has_reparse_point(current):
            raise ValueError(f"路径包含 Windows reparse point: {current}")


def _assert_no_reparse_tree(path: str) -> None:
    _assert_no_reparse_chain(path)
    if not os.path.isdir(path):
        return
    for current, directories, files in os.walk(path, topdown=True, followlinks=False):
        for name in [*directories, *files]:
            _assert_no_reparse_chain(os.path.join(current, name))


def _safe_makedirs(path: str, root: str) -> None:
    if not _is_within(root, path):
        raise ValueError(f"路径逃出插件目录: {path}")
    _assert_no_reparse_chain(root)
    os.makedirs(path, exist_ok=True)
    _assert_no_reparse_chain(path)
    if not os.path.isdir(path):
        raise ValueError(f"目标不是目录: {path}")


def _validate_windows_member(member: str, directory: bool) -> str:
    raw = str(member or "").replace("\\", "/")
    if not raw or "\x00" in raw or raw.startswith("/"):
        raise ValueError(f"压缩包包含非法 Windows 路径: {member}")
    parts = raw.split("/")
    if directory and parts[-1] == "":
        parts.pop()
    if not parts or any(not part or part in {".", ".."} for part in parts):
        raise ValueError(f"压缩包包含非法 Windows 路径: {member}")
    for part in parts:
        if len(part) > 255 or any(ord(character) < 0x20 or character in _WINDOWS_INVALID_CHARS for character in part):
            raise ValueError(f"压缩包包含非法 Windows 文件名: {member}")
        if part.endswith((" ", ".")):
            raise ValueError(f"压缩包包含非法 Windows 文件名: {member}")
        if part.split(".", 1)[0].casefold() in _WINDOWS_RESERVED_NAMES:
            raise ValueError(f"压缩包包含 Windows 保留文件名: {member}")
    return "/".join(parts)


def _is_zip_symlink(info: zipfile.ZipInfo) -> bool:
    mode = (info.external_attr >> 16) & 0xFFFF
    return stat.S_ISLNK(mode)


def _extract_safe(zip_path: str, dest_dir: str) -> None:
    # 逐 entry 解压，拒绝 zip slip、Windows 非法名称、链接和 reparse 路径。
    dest_abs = os.path.abspath(dest_dir)
    _safe_makedirs(dest_abs, dest_abs)
    seen: set[str] = set()
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            directory = info.is_dir() or info.filename.endswith(("/", "\\"))
            if _is_zip_symlink(info):
                raise ValueError(f"压缩包包含链接 entry: {info.filename}")
            rel = _validate_windows_member(info.filename, directory)
            if rel.casefold() in seen:
                raise ValueError(f"压缩包包含重复 entry: {info.filename}")
            seen.add(rel.casefold())
            target = os.path.abspath(os.path.join(dest_abs, *rel.split("/")))
            if not _is_within(dest_abs, target):
                raise ValueError(f"压缩包包含不安全路径(zip slip): {info.filename}")
            _assert_no_reparse_chain(dest_abs)
            parent = os.path.dirname(target)
            _safe_makedirs(parent, dest_abs)
            _assert_no_reparse_chain(parent)
            if directory:
                _safe_makedirs(target, dest_abs)
                continue
            if os.path.lexists(target):
                raise ValueError(f"压缩包目标已存在: {info.filename}")
            with zf.open(info, "r") as source, open(target, "xb") as output:
                shutil.copyfileobj(source, output)
            _assert_no_reparse_chain(target)


def _read_manifest_dict(manifest_path: str) -> dict[str, Any]:
    with open(manifest_path, "r", encoding="utf-8") as fp:
        data = json.load(fp)
    if not isinstance(data, dict):
        raise ValueError("plugin.json 必须是一个 JSON 对象")
    return data


def _locate_plugin_root(extract_dir: str) -> Optional[str]:
    # 在解压目录中定位含 ``plugin.json`` 的插件根：根部优先，否则下探一层。
    if os.path.isfile(os.path.join(extract_dir, MANIFEST_FILE)):
        return extract_dir
    try:
        entries = sorted(e for e in os.listdir(extract_dir) if not e.startswith("."))
    except OSError:
        return None
    for name in entries:
        sub = os.path.join(extract_dir, name)
        if os.path.isdir(sub) and os.path.isfile(os.path.join(sub, MANIFEST_FILE)):
            return sub
    return None


def install_plugin_archive(archive_path: str, user_plugins_dir: str, *,
                           log: Optional[Callable[[str], None]] = None,
                           allow_replace: bool = True) -> dict[str, Any]:
    # 把一个 ``.zip`` 插件包装进 ``user_plugins_dir/<plugin_id>/``。
    #
    # 返回 ``{ok, id, name, version, path, replaced, previous_version, message, errors}``。
    # 不加载、不启用——交由调用方 (runtime) 在 PluginManager 上 discover + enable。
    def _log(message: str) -> None:
        if callable(log):
            try:
                log(str(message))
            except Exception:
                pass

    archive_path = os.path.abspath(str(archive_path or ""))
    if not os.path.isfile(archive_path):
        return {"ok": False, "message": f"档案不存在: {archive_path}", "errors": ["archive not found"]}
    if not zipfile.is_zipfile(archive_path):
        return {"ok": False, "message": "不是有效的 .zip 插件包", "errors": ["not a zip archive"]}

    user_plugins_dir = os.path.abspath(str(user_plugins_dir or ""))
    _safe_makedirs(user_plugins_dir, user_plugins_dir)
    # 临时解压目录建在 user_plugins 内，保证与最终目标同盘，move 是原子 rename。
    tmp_dir = tempfile.mkdtemp(prefix=".import_", dir=user_plugins_dir)
    try:
        try:
            _extract_safe(archive_path, tmp_dir)
        except Exception as exc:
            return {"ok": False, "message": f"解压失败: {exc}", "errors": [str(exc)]}

        _assert_no_reparse_tree(tmp_dir)
        root = _locate_plugin_root(tmp_dir)
        if root is None:
            return {"ok": False, "message": "压缩包内未找到 plugin.json(根部或单层子目录)",
                    "errors": ["manifest not found"]}

        try:
            _assert_no_reparse_tree(root)
            manifest_path = os.path.join(root, MANIFEST_FILE)
            _assert_no_reparse_chain(manifest_path)
            manifest = _read_manifest_dict(manifest_path)
        except Exception as exc:
            return {"ok": False, "message": f"plugin.json 解析失败: {exc}", "errors": [str(exc)]}

        plugin_id = _safe_id(manifest.get("id")
                             or os.path.basename(root)
                             or os.path.splitext(os.path.basename(archive_path))[0])
        if not plugin_id:
            return {"ok": False, "message": "plugin.json 缺少有效的 id", "errors": ["invalid plugin id"]}

        # 受保护(native+加密)构建：真正随包的是 native_entry 指向的加密 blob，
        # entry 只是留作展示的语义名，源码文件在服务端构建时已被删除。
        protected = bool(manifest.get("protected", False))
        check_name = str(manifest.get("native_entry") or "").strip() if protected else ""
        if not check_name:
            check_name = str(manifest.get("entry") or "plugin.py").strip() or "plugin.py"
        check_abs = os.path.abspath(os.path.join(root, check_name))
        if not _is_within(root, check_abs):
            return {"ok": False, "id": plugin_id, "message": "entry 越出插件目录",
                    "errors": ["entry escapes plugin dir"]}
        try:
            _assert_no_reparse_chain(check_abs)
        except Exception as exc:
            return {"ok": False, "id": plugin_id, "message": f"入口路径不安全: {exc}",
                    "errors": [str(exc)]}
        if not os.path.isfile(check_abs):
            missing_kind = "受保护构建文件" if protected else "入口文件"
            return {"ok": False, "id": plugin_id, "message": f"{missing_kind}缺失: {check_name}",
                    "errors": ["entry file missing"]}

        target = os.path.join(user_plugins_dir, plugin_id)
        replaced = False
        previous_version = ""
        if os.path.lexists(target):
            _assert_no_reparse_tree(target)
            if not os.path.isdir(target):
                return {"ok": False, "id": plugin_id, "message": f"插件目标不是目录: {plugin_id}",
                        "errors": ["plugin target is not a directory"]}
            if not allow_replace:
                return {"ok": False, "id": plugin_id, "message": f"插件已存在: {plugin_id}",
                        "errors": ["already installed"]}
            try:
                previous_version = str(_read_manifest_dict(
                    os.path.join(target, MANIFEST_FILE)).get("version") or "")
            except Exception:
                previous_version = ""
            shutil.rmtree(target, ignore_errors=True)
            replaced = True

        try:
            _assert_no_reparse_chain(user_plugins_dir)
            _assert_no_reparse_tree(root)
            if os.path.lexists(target):
                raise ValueError(f"插件目标在安装前重新出现: {plugin_id}")
            os.rename(root, target)
            _assert_no_reparse_tree(target)
        except Exception as exc:
            return {"ok": False, "id": plugin_id, "message": f"安装到 user_plugins 失败: {exc}",
                    "errors": [str(exc)]}

        name = str(manifest.get("name") or plugin_id)
        version = str(manifest.get("version") or "")
        verb = "已更新" if replaced else "已导入"
        _log(f"{verb} 插件 {plugin_id} v{version or '?'} → {target}")
        return {
            "ok": True,
            "id": plugin_id,
            "name": name,
            "version": version,
            "path": target,
            "replaced": replaced,
            "previous_version": previous_version,
            "message": f"{verb} {name} ({plugin_id})" + (f"  v{previous_version}→v{version}" if replaced else ""),
            "errors": [],
        }
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def remove_installed_plugin(plugin_path: str, user_plugins_dir: str) -> dict[str, Any]:
    # 删除一个**用户安装**的插件目录。内置 ``plugins/`` 永不允许删除。
    plugin_path = os.path.abspath(str(plugin_path or ""))
    user_plugins_dir = os.path.abspath(str(user_plugins_dir or ""))
    if plugin_path == user_plugins_dir or not _is_within(user_plugins_dir, plugin_path):
        return {"ok": False, "message": "只能卸载 user_plugins 目录内的插件", "errors": ["not a user plugin"]}
    if not os.path.isdir(plugin_path):
        return {"ok": False, "message": "插件目录不存在", "errors": ["plugin dir not found"]}
    try:
        _assert_no_reparse_chain(user_plugins_dir)
        _assert_no_reparse_tree(plugin_path)
    except Exception as exc:
        return {"ok": False, "message": f"插件路径不安全: {exc}", "errors": [str(exc)]}
    shutil.rmtree(plugin_path, ignore_errors=True)
    return {"ok": True, "message": "已删除插件目录", "path": plugin_path, "errors": []}


__all__ = ["install_plugin_archive", "remove_installed_plugin"]
