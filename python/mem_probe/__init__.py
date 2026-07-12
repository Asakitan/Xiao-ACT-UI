# Generic memory scanning infrastructure.
from __future__ import annotations

import os as _os
import sys as _sys
import types as _types
import importlib.abc as _importlib_abc
import importlib.machinery as _importlib_machinery
import importlib.util as _importlib_util

# ── rt_io / rt_io_proxy 分流 ──────────────────────────────────
# 契约:
#   * 主进程 (dev + 打包版): 走 rt_io_proxy → IPC 到 helper subprocess
#   * helper subprocess: SAO_RT_IO_HELPER=1 → 走真 rt_io (含 driver handle)
#
# dev 环境曾经的坑: 主线程 `from mem_probe import rt_io` 直接拿真 rt_io.pyd,
# 但主线程从没调过 ensure_loaded → _r3h=None → 所有 `_pw` (物理内存写)
# 静默 return False → hide_exstyle / hide_window_rect 永远失败, tagWND 里
# rcWindow/ExStyle 一位没动. 打包版反而没这问题, 因为 --nofollow-import-to
# 剔掉了主 exe 的 rt_io, 兜底会走 proxy 的 IPC 到 helper.
#
# 方案 C: dev 和打包版都强制走 proxy, 只有 helper subprocess 在 entry
# 完成版本协商和认证后才显式 import 真 rt_io。helper entry 本身作为 package
# 子模块加载时必须保持 bootstrap-only，不能让 package import 抢先加载 backend。
_IS_HELPER = _os.environ.get("SAO_RT_IO_HELPER") == "1"
_IS_HELPER_BOOTSTRAP = (
    _IS_HELPER and _os.environ.get("SAO_RT_IO_BOOTSTRAP") == "1"
)
_IS_FROZEN = bool(
    getattr(_sys, "frozen", False)
    or _os.environ.get("SAO_RT_IO_FROZEN") == "1"
)


def _load_source_submodule(module_basename: str):
    # Development binaries can leave a stale adjacent .pyd behind. Load the
    # source proxy explicitly so the main process always follows this tree.
    module_name = __name__ + "." + module_basename
    source_path = _os.path.join(
        _os.path.dirname(_os.path.abspath(__file__)), module_basename + ".py"
    )
    spec = _importlib_util.spec_from_file_location(module_name, source_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load source module {module_name}")
    package = _sys.modules[__name__]
    stale = _sys.modules.pop(module_name, None)
    if stale is not None and getattr(package, module_basename, None) is stale:
        delattr(package, module_basename)
    module = _importlib_util.module_from_spec(spec)
    _sys.modules[module_name] = module
    setattr(package, module_basename, module)
    try:
        spec.loader.exec_module(module)
    except Exception:
        _sys.modules.pop(module_name, None)
        if getattr(package, module_basename, None) is module:
            delattr(package, module_basename)
        raise
    return module


class _DevHelperRtIoSourceFinder(_importlib_abc.MetaPathFinder):
    # After authenticated bootstrap, source helpers must not resolve rt_io.pyd.
    def find_spec(self, fullname, path=None, target=None):
        if (
            fullname != __name__ + ".rt_io"
            or _IS_FROZEN
            or not _IS_HELPER
            or _os.environ.get("SAO_RT_IO_BOOTSTRAP") == "1"
        ):
            return None
        try:
            from . import _rt_atomic_globals as _rag
            version = int(_os.environ.get("SAO_RT_IO_PROTOCOL_VERSION", "0"))
            if not _rag.bootstrap_ready(version):
                raise RuntimeError(
                    "refuse rt_io source import before helper authentication"
                )
        except ValueError as exc:
            raise RuntimeError("invalid helper protocol version") from exc
        source_path = _os.path.join(
            _os.path.dirname(_os.path.abspath(__file__)), "rt_io.py"
        )
        loader = _importlib_machinery.SourceFileLoader(fullname, source_path)
        return _importlib_util.spec_from_file_location(
            fullname, source_path, loader=loader
        )


if _IS_HELPER and not _IS_FROZEN:
    _sys.meta_path.insert(0, _DevHelperRtIoSourceFinder())

if _IS_HELPER_BOOTSTRAP:
    class _BootstrapBlockedRtIo(_types.ModuleType):
        __sao_bootstrap_block__ = True

        def __getattr__(self, name):
            raise RuntimeError(
                f"rt_io.{name} is unavailable before helper authentication"
            )

    _rt_blocker = _BootstrapBlockedRtIo(__name__ + ".rt_io")
    _sys.modules[__name__ + ".rt_io"] = _rt_blocker
    rt_io = _rt_blocker
else:
    if _IS_HELPER:
        from . import _rt_atomic_globals as _rag

        try:
            _advertised_protocol = int(
                _os.environ.get("SAO_RT_IO_PROTOCOL_VERSION", "0"), 10
            )
        except ValueError:
            _advertised_protocol = 0
        if not _rag.bootstrap_ready(_advertised_protocol):
            raise RuntimeError(
                "refuse to import rt_io before authenticated helper bootstrap"
            )
        from . import rt_io as _rt  # noqa: F401
    else:
        if _IS_FROZEN:
            from . import rt_io_proxy as _rt  # noqa: F401
        else:
            _rt = _load_source_submodule("rt_io_proxy")
        _sys.modules[__name__ + ".rt_io"] = _rt

    # Re-export the Cython facade so ``from mem_probe import cy_memscan`` works
    # in the normal main/helper runtime.  Bootstrap-only import deliberately
    # skips the extension because it may resolve rt_io before auth is complete.
    from . import cy_memscan  # noqa: F401,E402
