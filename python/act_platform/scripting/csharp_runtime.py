# -*- coding: utf-8 -*-
"""C# scripting runtime via *pythonnet* (CLR hosting).

Supports two modes:

1. **Source mode** (``.cs``): auto-compiles a ``.cs`` file using one of:
   - ``csc.exe`` / ``dotnet`` from .NET SDK (if installed)
   - Windows built-in .NET Framework ``csc.exe`` (Win10/11, always present)
   - Bundled Roslyn in-process compiler (``scripting/roslyn/`` DLLs,
     fetch via ``python -m act_platform.scripting.fetch_roslyn``)
   Users do NOT need a .NET SDK installed.

2. **Assembly mode** (``.dll``): directly loads a pre-compiled .NET
   assembly. The manifest ``entry`` should point at the ``.dll``.

The plugin class must be ``public`` and reside in the default or
declared namespace. Lifecycle hooks map to static or instance methods::

    // plugin.cs
    using System;
    using System.Collections.Generic;

    public class Plugin
    {
        private dynamic _ctx;

        // --- lifecycle hooks (static or instance) ---
        public void OnLoad(dynamic ctx)
        {
            _ctx = ctx;
            ctx.log("Hello from C#!");
            ctx.register_ui_panel("cs_panel",
                new Dictionary<string, object> { {"title", "C# Panel"} },
                new Func<object, object>(Render),
                new Func<string, object, object>(OnAction)
            );
        }

        public void OnEnable()  { }
        public void OnDisable() { }
        public void OnUnload()  { }

        public object Render(object payload)
        {
            return _ctx.ui.panel("C# Plugin", new object[] {
                _ctx.ui.text("Hello from C#!"),
            });
        }

        public object OnAction(string actionId, object payload)
        {
            _ctx.log("C# action: " + actionId);
            return new Dictionary<string, object> { {"ok", true} };
        }
    }

Requires: ``pip install pythonnet`` and .NET 6.0+ runtime (or .NET Framework on Windows).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from types import ModuleType
from typing import Any, Callable, TYPE_CHECKING

from .base import ContextProxy, ScriptModule, ScriptRuntime, wrap_callback

if TYPE_CHECKING:
    from act_platform.plugins import PluginContext, PluginRecord

_clr = None
_System = None
_SOURCE_ASSEMBLY_CACHE_LIMIT = 16
_SOURCE_ASSEMBLY_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_SOURCE_ASSEMBLY_CACHE_ORDER: list[tuple[Any, ...]] = []
_REFERENCE_ASSEMBLY_CACHE: dict[tuple[str, bool, int, int], Any] = {}


def _ensure_pythonnet():
    global _clr, _System
    if _clr is not None:
        return
    try:
        import clr as _clr_mod
        _clr = _clr_mod
        import System as _sys_mod
        _System = _sys_mod
    except ImportError:
        raise RuntimeError(
            "C# runtime requires the 'pythonnet' package and .NET runtime. "
            "Install with: pip install pythonnet"
        )


def _file_signature(path: str) -> tuple[str, bool, int, int]:
    abs_path = os.path.abspath(path)
    try:
        stat = os.stat(abs_path)
        mtime_ns = int(getattr(
            stat,
            "st_mtime_ns",
            int(stat.st_mtime * 1_000_000_000),
        ))
        return (abs_path, True, int(stat.st_size), mtime_ns)
    except OSError:
        return (abs_path, False, 0, 0)


def _source_cache_key(source_path: str, references: list[str]) -> tuple[Any, ...]:
    ref_sigs = tuple(_file_signature(path) for path in sorted(str(p) for p in references))
    source_dir = os.path.dirname(os.path.abspath(source_path))
    sibling_sigs: tuple[Any, ...] = ()
    try:
        sibling_sigs = tuple(
            _file_signature(os.path.join(source_dir, f))
            for f in sorted(os.listdir(source_dir))
            if f.endswith(".cs") and f != os.path.basename(source_path)
        )
    except OSError:
        pass
    return (_file_signature(source_path), sibling_sigs, ref_sigs)


def _remember_source_assembly(key: tuple[Any, ...], assembly: Any,
                              build_dir: str, dll_path: str) -> None:
    cached_build_dir = build_dir
    if build_dir and os.path.isdir(build_dir):
        try:
            shutil.rmtree(build_dir, ignore_errors=True)
            if not os.path.isdir(build_dir):
                cached_build_dir = ""
        except Exception:
            cached_build_dir = build_dir
    if key in _SOURCE_ASSEMBLY_CACHE:
        try:
            _SOURCE_ASSEMBLY_CACHE_ORDER.remove(key)
        except ValueError:
            pass
    _SOURCE_ASSEMBLY_CACHE[key] = {
        "assembly": assembly,
        "build_dir": cached_build_dir,
        "dll_path": dll_path,
    }
    _SOURCE_ASSEMBLY_CACHE_ORDER.append(key)
    while len(_SOURCE_ASSEMBLY_CACHE_ORDER) > _SOURCE_ASSEMBLY_CACHE_LIMIT:
        stale = _SOURCE_ASSEMBLY_CACHE_ORDER.pop(0)
        entry = _SOURCE_ASSEMBLY_CACHE.pop(stale, None)
        stale_dir = str((entry or {}).get("build_dir") or "")
        if stale_dir and os.path.isdir(stale_dir):
            try:
                shutil.rmtree(stale_dir, ignore_errors=True)
            except Exception:
                pass


def _load_compiled_source_assembly(dll_path: str) -> Any:
    with open(dll_path, "rb") as fp:
        data = fp.read()
    byte_array = _System.Array[_System.Byte](data)
    return _System.Reflection.Assembly.Load(byte_array)


def _ensure_reference_assemblies(references: list[str]) -> None:
    for path in references:
        sig = _file_signature(path)
        if not sig[1] or sig in _REFERENCE_ASSEMBLY_CACHE:
            continue
        _REFERENCE_ASSEMBLY_CACHE[sig] = _System.Reflection.Assembly.LoadFile(os.path.abspath(path))


def _find_csc() -> str | None:
    """Locate csc.exe or dotnet for source compilation.

    Search order: PATH → dotnet SDK → Windows built-in .NET Framework csc.exe.
    The Framework csc.exe ships with every Windows 10/11 installation so it
    serves as a reliable fallback even when no SDK is installed.
    """
    csc = shutil.which("csc") or shutil.which("csc.exe")
    if csc:
        return csc
    dotnet = shutil.which("dotnet") or shutil.which("dotnet.exe")
    if dotnet:
        return dotnet
    if sys.platform == "win32":
        for base in [
            os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "dotnet"),
            os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "dotnet"),
        ]:
            path = os.path.join(base, "dotnet.exe")
            if os.path.isfile(path):
                return path
        windir = os.environ.get("SystemRoot") or os.environ.get("WINDIR") or r"C:\Windows"
        for arch_dir in ("Framework64", "Framework"):
            candidate = os.path.join(
                windir, "Microsoft.NET", arch_dir, "v4.0.30319", "csc.exe",
            )
            if os.path.isfile(candidate):
                return candidate
    return None


def _python_runtime_reference() -> str | None:
    try:
        _ensure_pythonnet()
        py_object_type = _System.Type.GetType("Python.Runtime.PyObject, Python.Runtime")
        if py_object_type is None:
            return None
        path = str(py_object_type.Assembly.Location or "")
        return path if path and os.path.isfile(path) else None
    except Exception:
        return None


def _default_target_framework() -> str:
    """Return the TFM used for temporary C# source projects.

    Must match the CLR that pythonnet hosts.  When the active runtime is
    .NET Framework 4.x (``Environment.Version.Major < 5``), compile for
    ``netstandard2.0`` so the resulting assembly loads without
    ``System.Runtime`` mismatches.  For .NET 6+ runtimes the TFM matches
    the runtime major version.
    """
    override = str(os.environ.get("SAO_CSHARP_TARGET_FRAMEWORK") or "").strip()
    if override:
        return override
    try:
        if _System is not None:
            runtime_major = int(_System.Environment.Version.Major)
            if runtime_major >= 6:
                return f"net{runtime_major}.0"
            return "netstandard2.0"
    except Exception:
        pass
    dotnet = shutil.which("dotnet") or shutil.which("dotnet.exe")
    if dotnet:
        try:
            proc = subprocess.run(
                [dotnet, "--list-sdks"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            majors: list[int] = []
            for line in str(proc.stdout or "").splitlines():
                head = line.strip().split()[0] if line.strip() else ""
                major_text = head.split(".", 1)[0]
                if major_text.isdigit():
                    major = int(major_text)
                    if major >= 6:
                        majors.append(major)
            if majors:
                return f"net{max(majors)}.0"
        except Exception:
            pass
    return "netstandard2.0"


_ROSLYN_LOADED = False
_ROSLYN_AVAILABLE: bool | None = None


def _roslyn_dir() -> str | None:
    """Locate bundled Roslyn DLLs (Microsoft.CodeAnalysis.CSharp + deps)."""
    candidates = [
        os.path.join(os.path.dirname(__file__), "roslyn"),
        os.path.join(os.path.dirname(__file__), "..", "..", "vendor", "roslyn"),
    ]
    try:
        from config import BUNDLE_DIR
        candidates.append(
            os.path.join(BUNDLE_DIR, "act_platform", "scripting", "roslyn"))
    except Exception:
        pass
    required = ("Microsoft.CodeAnalysis.CSharp.dll", "Microsoft.CodeAnalysis.dll")
    for d in candidates:
        if os.path.isdir(d) and all(os.path.isfile(os.path.join(d, f)) for f in required):
            return os.path.abspath(d)
    return None


def _load_roslyn_assemblies() -> bool:
    """Load Roslyn DLLs into the CLR. Returns True on success."""
    global _ROSLYN_LOADED, _ROSLYN_AVAILABLE
    if _ROSLYN_LOADED:
        return True
    if _ROSLYN_AVAILABLE is False:
        return False
    _ensure_pythonnet()
    rdir = _roslyn_dir()
    if rdir is None:
        _ROSLYN_AVAILABLE = False
        return False
    for dll in sorted(os.listdir(rdir)):
        if dll.endswith(".dll"):
            full = os.path.join(rdir, dll)
            try:
                _clr.AddReference(full)
            except Exception:
                try:
                    _System.Reflection.Assembly.LoadFrom(full)
                except Exception:
                    pass
    try:
        from Microsoft.CodeAnalysis import MetadataReference  # noqa: F401
        from Microsoft.CodeAnalysis.CSharp import CSharpSyntaxTree  # noqa: F401
    except ImportError:
        _ROSLYN_AVAILABLE = False
        return False
    _ROSLYN_LOADED = True
    _ROSLYN_AVAILABLE = True
    return True


def _collect_bcl_reference_paths() -> list[str]:
    """Discover .NET BCL assembly paths for Roslyn compilation references."""
    paths: dict[str, str] = {}
    probe_names = [
        "System.Private.CoreLib", "System.Runtime", "System.Console",
        "System.Collections", "System.Linq", "System.Threading",
        "netstandard", "mscorlib", "System",
    ]
    for name in probe_names:
        try:
            asm = _System.Reflection.Assembly.Load(name)
            loc = str(asm.Location or "")
            if loc and os.path.isfile(loc):
                paths[os.path.basename(loc).lower()] = loc
        except Exception:
            pass
    try:
        core_loc = str(_System.Type.GetType("System.Object").Assembly.Location or "")
        if core_loc and os.path.isfile(core_loc):
            paths[os.path.basename(core_loc).lower()] = core_loc
            asm_dir = os.path.dirname(core_loc)
            for f in os.listdir(asm_dir):
                fl = f.lower()
                if fl.endswith(".dll") and fl.startswith("system.") and fl not in paths:
                    paths[fl] = os.path.join(asm_dir, f)
    except Exception:
        pass
    return list(paths.values())


def _compile_cs_roslyn(source_path: str,
                       references: list[str] | None = None) -> Any | None:
    """Compile .cs source to an in-memory Assembly via Roslyn.

    Returns the loaded Assembly on success, None if Roslyn is unavailable,
    or raises RuntimeError on compilation failure.
    """
    if not _load_roslyn_assemblies():
        return None

    import clr as _clr_mod  # noqa: F811
    from Microsoft.CodeAnalysis import MetadataReference, OutputKind  # type: ignore[import]
    from Microsoft.CodeAnalysis.CSharp import (  # type: ignore[import]
        CSharpCompilation,
        CSharpCompilationOptions,
        CSharpSyntaxTree,
    )

    with open(source_path, "r", encoding="utf-8") as fp:
        source_code = fp.read()

    trees = _System.Collections.Generic.List[object]()
    trees.Add(CSharpSyntaxTree.ParseText(source_code))
    source_dir = os.path.dirname(os.path.abspath(source_path))
    try:
        for fname in sorted(os.listdir(source_dir)):
            if (
                fname.endswith(".cs")
                and fname != os.path.basename(source_path)
                and not fname.startswith(".")
            ):
                with open(os.path.join(source_dir, fname), "r", encoding="utf-8") as fp:
                    trees.Add(CSharpSyntaxTree.ParseText(fp.read()))
    except OSError:
        pass

    refs = _System.Collections.Generic.List[MetadataReference]()
    for bcl_path in _collect_bcl_reference_paths():
        try:
            refs.Add(MetadataReference.CreateFromFile(bcl_path))
        except Exception:
            pass
    for ref_path in (references or []):
        if os.path.isfile(ref_path):
            try:
                refs.Add(MetadataReference.CreateFromFile(os.path.abspath(ref_path)))
            except Exception:
                pass
    runtime_ref = _python_runtime_reference()
    if runtime_ref:
        try:
            refs.Add(MetadataReference.CreateFromFile(runtime_ref))
        except Exception:
            pass

    stem = os.path.splitext(os.path.basename(source_path))[0]
    options = CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary)
    compilation = CSharpCompilation.Create(stem, trees, refs, options)

    ms = _System.IO.MemoryStream()
    emit_result = compilation.Emit(ms)

    if not emit_result.Success:
        errors = []
        for diag in emit_result.Diagnostics:
            sev = str(diag.Severity)
            if "Error" in sev:
                errors.append(str(diag))
        raise RuntimeError(
            "Roslyn compilation failed:\n" + "\n".join(errors[:30])
        )

    ms.Seek(0, _System.IO.SeekOrigin.Begin)
    return _System.Reflection.Assembly.Load(ms.ToArray())


def _compile_cs(source_path: str, output_dir: str, references: list[str] | None = None) -> str:
    """Compile a .cs file to a .dll assembly. Returns the output path."""
    stem = os.path.splitext(os.path.basename(source_path))[0]
    output_dll = os.path.join(output_dir, f"{stem}.dll")

    compiler = _find_csc()
    if compiler is None:
        raise RuntimeError(
            "Cannot find C# compiler (csc.exe or dotnet). "
            "Install the .NET SDK: https://dotnet.microsoft.com/download"
        )

    source_dir = os.path.dirname(os.path.abspath(source_path))
    extra_sources: list[str] = []
    try:
        for fname in sorted(os.listdir(source_dir)):
            if (
                fname.endswith(".cs")
                and fname != os.path.basename(source_path)
                and not fname.startswith(".")
            ):
                extra_sources.append(os.path.join(source_dir, fname))
    except OSError:
        pass

    basename = os.path.basename(compiler).lower()
    if "dotnet" in basename:
        csproj_content = _generate_csproj(stem, source_path, references,
                                          extra_sources=extra_sources or None)
        proj_dir = os.path.join(output_dir, f"{stem}_proj")
        os.makedirs(proj_dir, exist_ok=True)
        csproj_path = os.path.join(proj_dir, f"{stem}.csproj")
        with open(csproj_path, "w", encoding="utf-8") as fp:
            fp.write(csproj_content)

        src_dest = os.path.join(proj_dir, os.path.basename(source_path))
        if os.path.abspath(source_path) != os.path.abspath(src_dest):
            shutil.copy2(source_path, src_dest)
        for extra in extra_sources:
            extra_dest = os.path.join(proj_dir, os.path.basename(extra))
            if os.path.abspath(extra) != os.path.abspath(extra_dest):
                shutil.copy2(extra, extra_dest)

        proc = subprocess.run(
            [compiler, "build", "-c", "Release", "-o", output_dir, csproj_path],
            capture_output=True, text=True, timeout=60,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"dotnet build failed:\n{proc.stderr or proc.stdout}")
    else:
        cmd = [compiler, "-target:library", f"-out:{output_dll}", "-nologo"]
        if references:
            for ref in references:
                cmd.append(f"-reference:{ref}")
        cmd.append(source_path)
        for extra in extra_sources:
            cmd.append(extra)
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if proc.returncode != 0:
            raise RuntimeError(f"csc compilation failed:\n{proc.stderr or proc.stdout}")

    if not os.path.isfile(output_dll):
        candidates = [f for f in os.listdir(output_dir)
                      if f.endswith(".dll") and stem.lower() in f.lower()]
        if candidates:
            output_dll = os.path.join(output_dir, candidates[0])
        else:
            raise RuntimeError(f"compilation produced no .dll (expected {output_dll})")

    return output_dll


def _generate_csproj(name: str, source_path: str,
                     references: list[str] | None = None,
                     extra_sources: list[str] | None = None) -> str:
    source_name = os.path.basename(source_path)
    compile_items = [source_name]
    if extra_sources:
        compile_items.extend(os.path.basename(p) for p in extra_sources)
    compile_xml = "\n".join(
        f'        <Compile Include="{item}" />' for item in compile_items
    )
    refs_xml = ""
    if references:
        refs_xml = "\n  <ItemGroup>\n"
        for ref in references:
            refs_xml += f'    <Reference Include="{os.path.splitext(os.path.basename(ref))[0]}">\n'
            refs_xml += f'      <HintPath>{ref}</HintPath>\n'
            refs_xml += "    </Reference>\n"
        refs_xml += "  </ItemGroup>\n"

    target_framework = _default_target_framework()

    lang_version = "7.3" if "netstandard" in target_framework or "net4" in target_framework else "latest"

    return f"""<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>{target_framework}</TargetFramework>
    <AssemblyName>{name}</AssemblyName>
    <OutputType>Library</OutputType>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <ImplicitUsings>disable</ImplicitUsings>
    <Nullable>disable</Nullable>
    <LangVersion>{lang_version}</LangVersion>
  </PropertyGroup>
    <ItemGroup>
{compile_xml}
    </ItemGroup>
{refs_xml}</Project>
"""


class _CSharpProxy:
    """Wraps PluginContext so C# code sees a dynamic-friendly object.

    pythonnet passes Python objects to C# as ``dynamic``, so instance
    methods and properties are accessible. This proxy ensures naming
    conventions match (``log``, ``register_ui_panel``, etc.) and wraps
    C# delegates back to Python callables.
    """

    def __init__(self, ctx: "PluginContext") -> None:
        self._ctx = ctx
        self.ui = ctx.ui
        self.engine = ctx.engine
        self.plugin_id = ctx.plugin_id
        self.path = ctx.path
        self.web_path = ctx.web_path
        self.assets_path = ctx.assets_path

    def _to_python(self, val: Any) -> Any:
        if _System is not None and isinstance(val, _System.Object):
            return _clr_to_python(val)
        return val

    def _wrap_cs_callback(self, fn: Any) -> Callable:
        if not callable(fn):
            return fn

        def _py_cb(*args, **kwargs):
            result = fn(*args, **kwargs)
            return self._to_python(result)
        return _py_cb

    def log(self, message: Any) -> None:
        self._ctx.log(str(message or ""))

    def subscribe(self, topic, callback) -> str:
        return self._ctx.subscribe(str(topic), self._wrap_cs_callback(callback))

    def subscribe_once(self, topic, callback) -> str:
        return self._ctx.subscribe_once(str(topic), self._wrap_cs_callback(callback))

    def unsubscribe(self, token) -> bool:
        return self._ctx.unsubscribe(str(token))

    def on_damage(self, callback) -> str:
        return self._ctx.on_damage(self._wrap_cs_callback(callback))

    def on_heal(self, callback) -> str:
        return self._ctx.on_heal(self._wrap_cs_callback(callback))

    def on_skill(self, callback) -> str:
        return self._ctx.on_skill(self._wrap_cs_callback(callback))

    def on_boss(self, callback) -> str:
        return self._ctx.on_boss(self._wrap_cs_callback(callback))

    def on_snapshot(self, callback) -> str:
        return self._ctx.on_snapshot(self._wrap_cs_callback(callback))

    def on_encounter_finalized(self, callback) -> str:
        return self._ctx.on_encounter_finalized(self._wrap_cs_callback(callback))

    def emit(self, topic, payload=None) -> dict:
        p = self._to_python(payload) if payload is not None else None
        return self._ctx.emit(str(topic), p)

    def get_snapshot(self) -> dict:
        return self._ctx.get_snapshot()

    def snapshot_value(self, path, default=None):
        return self._ctx.snapshot_value(str(path), default)

    def time(self) -> float:
        return self._ctx.time()

    def recent_events(self, limit=20, topic="") -> list:
        return self._ctx.recent_events(int(limit), str(topic or ""))

    def get_setting(self, key, default=None):
        return self._ctx.get_setting(str(key), default)

    def setting(self, key, default=None):
        return self._ctx.setting(str(key), default)

    def set_setting(self, key, value=None) -> None:
        self._ctx.set_setting(str(key), self._to_python(value))

    def set_defaults(self, defaults) -> None:
        d = self._to_python(defaults)
        if isinstance(d, dict):
            self._ctx.set_defaults(d)

    def register_ui_panel(self, panel_id, metadata=None,
                          render=None, on_action=None) -> dict:
        meta = self._to_python(metadata) if metadata is not None else None

        def _render(payload=None):
            if callable(render):
                result = render(payload)
                return self._to_python(result)
            return {"version": 1, "title": "", "nodes": []}

        def _on_action(action_id, payload=None):
            if callable(on_action):
                result = on_action(str(action_id), payload)
                return self._to_python(result)
            return {"ok": True}

        return self._ctx.register_ui_panel(
            str(panel_id),
            meta if isinstance(meta, dict) else {},
            _render if callable(render) else None,
            _on_action if callable(on_action) else None,
        )

    def register_render_hook(self, surface, callback, priority=0.0) -> str:
        return self._ctx.register_render_hook(
            str(surface), self._wrap_cs_callback(callback), float(priority))

    def set_overlay(self, surface, spec) -> dict:
        return self._ctx.set_overlay(str(surface), self._to_python(spec))

    def clear_overlay(self, surface=None) -> None:
        self._ctx.clear_overlay(str(surface) if surface else None)

    def create_compositor_layer(self, name, width, height,
                                x=0, y=0, z=140, click_through=False):
        return self._ctx.create_compositor_layer(
            str(name), int(width), int(height),
            x=int(x), y=int(y), z=int(z),
            click_through=bool(click_through))

    def upload_compositor_frame(self, name, bgra_bytes, width, height,
                                x=None, y=None):
        raw = bytes(bgra_bytes) if not isinstance(bgra_bytes, bytes) else bgra_bytes
        self._ctx.upload_compositor_frame(
            str(name), raw, int(width), int(height),
            int(x) if x is not None else None,
            int(y) if y is not None else None)

    def destroy_compositor_layer(self, name):
        self._ctx.destroy_compositor_layer(str(name))

    def set_compositor_layer_visible(self, name, visible):
        self._ctx.set_compositor_layer_visible(str(name), bool(visible))

    def set_compositor_layer_input(self, name, cursor_pos_fn=None,
                                   mouse_button_fn=None, cursor_leave_fn=None,
                                   scroll_fn=None):
        self._ctx.set_compositor_layer_input(
            str(name),
            self._wrap_cs_callback(cursor_pos_fn) if callable(cursor_pos_fn) else None,
            self._wrap_cs_callback(mouse_button_fn) if callable(mouse_button_fn) else None,
            self._wrap_cs_callback(cursor_leave_fn) if callable(cursor_leave_fn) else None,
            self._wrap_cs_callback(scroll_fn) if callable(scroll_fn) else None)

    def register_hotkey(self, hotkey_id, callback,
                        default_key="", label="") -> str:
        return self._ctx.register_hotkey(
            str(hotkey_id), self._wrap_cs_callback(callback),
            str(default_key or ""), str(label or ""))

    def register_engine(self, name, engine_obj) -> None:
        self._ctx.register_engine(str(name), engine_obj)

    def register_data_source(self, source_id, metadata=None,
                             start=None, stop=None) -> dict:
        meta = self._to_python(metadata) if metadata is not None else None
        return self._ctx.register_data_source(
            str(source_id), meta,
            self._wrap_cs_callback(start) if callable(start) else None,
            self._wrap_cs_callback(stop) if callable(stop) else None,
        )

    def register_parser_adapter(self, adapter_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_cs_callback(handler) if callable(handler) else None
        return self._ctx.register_parser_adapter(str(adapter_id), meta, h)

    def register_exporter(self, exporter_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_cs_callback(handler) if callable(handler) else None
        return self._ctx.register_exporter(str(exporter_id), meta, h)

    def register_formatter(self, formatter_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_cs_callback(handler) if callable(handler) else None
        return self._ctx.register_formatter(str(formatter_id), meta, h)

    def register_trigger_type(self, trigger_type, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_cs_callback(handler) if callable(handler) else None
        return self._ctx.register_trigger_type(str(trigger_type), meta, h)

    def register_report_view(self, view_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_cs_callback(handler) if callable(handler) else None
        return self._ctx.register_report_view(str(view_id), meta, h)

    def register_timer(self, timer_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_cs_callback(handler) if callable(handler) else None
        return self._ctx.register_timer(str(timer_id), meta, h)

    def register_menu_category(self, name, icon, builder, priority=0.0):
        return self._ctx.register_menu_category(
            str(name), str(icon or ""),
            self._wrap_cs_callback(builder), float(priority))

    def register_menu_surface(self, surface_id, descriptor, priority=0.0):
        d = self._to_python(descriptor) if descriptor is not None else {}
        return self._ctx.register_menu_surface(str(surface_id), d, float(priority))

    def register_action_handler(self, handler):
        return self._ctx.register_action_handler(self._wrap_cs_callback(handler))

    def request_redraw(self, surface="", reason="") -> dict:
        return self._ctx.request_redraw(str(surface or ""), str(reason or ""))

    def open_window(self, panel_id="", width=0, height=0) -> dict:
        return self._ctx.open_window(str(panel_id or ""), int(width), int(height))

    def open_file(self, filters=None, title="选择文件", initial_dir="", hwnd_owner=0) -> str:
        return self._ctx.open_file(
            self._to_python(filters),
            str(title or "选择文件"),
            str(initial_dir or ""),
            int(hwnd_owner or 0),
        )

    def set_interval(self, callback, seconds) -> str:
        return self._ctx.set_interval(self._wrap_cs_callback(callback), float(seconds))

    def set_timeout(self, callback, seconds) -> str:
        return self._ctx.set_timeout(self._wrap_cs_callback(callback), float(seconds))

    def clear_timer(self, token) -> bool:
        return self._ctx.clear_timer(str(token))

    def run_on_ui(self, callback) -> None:
        self._ctx.run_on_ui(self._wrap_cs_callback(callback))

    def notify(self, title, message, duration_s=60.0, kind="plugin") -> bool:
        return self._ctx.notify(str(title), str(message), float(duration_s), str(kind))

    def dismiss_notify(self) -> bool:
        return self._ctx.dismiss_notify()

    def toast(self, message) -> bool:
        return self._ctx.toast(str(message))

    def get_engine(self, name, default=None):
        return self._ctx.get_engine(str(name), default)

    def require_engine(self, name):
        return self._ctx.require_engine(str(name))

    def call_engine(self, engine_name, method, *args, **kwargs):
        return self._ctx.call_engine(str(engine_name), str(method), *args, **kwargs)

    def call_runtime(self, action, *args, **kwargs):
        return self._ctx.call_runtime(str(action), *args, **kwargs)

    def ensure_requirements(self, install=True) -> dict:
        return self._ctx.ensure_requirements(bool(install))

    def load_local(self, relative_path):
        return self._ctx.load_local(str(relative_path))

    @property
    def mem(self):
        return self._ctx.mem

    @property
    def event_bus(self):
        return self._ctx.event_bus

    @property
    def owner(self):
        return self._ctx.owner


def _clr_to_python(obj: Any) -> Any:
    """Best-effort conversion of .NET objects to Python types."""
    if obj is None:
        return None
    if _System is None:
        return obj
    if isinstance(obj, str):
        return obj
    if isinstance(obj, (bool, int, float)):
        return obj
    try:
        if hasattr(obj, "GetType"):
            type_name = str(obj.GetType().FullName or "")
            if "Dictionary" in type_name:
                out = {}
                for entry in obj:
                    out[str(entry.Key)] = _clr_to_python(entry.Value)
                return out
            if "List" in type_name or type_name.endswith("[]"):
                return [_clr_to_python(item) for item in obj]
    except Exception:
        pass
    return obj


class CSharpRuntime(ScriptRuntime):
    """Load ``.cs`` or ``.dll`` plugin scripts via pythonnet."""

    def __init__(self) -> None:
        _ensure_pythonnet()
        self._assemblies: dict[str, Any] = {}
        self._instances: dict[str, Any] = {}
        self._build_dirs: dict[str, str] = {}

    @property
    def language(self) -> str:
        return "csharp"

    @property
    def engine_name(self) -> str:
        try:
            ver = str(_System.Environment.Version) if _System else "?"
        except Exception:
            ver = "?"
        return f"pythonnet (.NET {ver})"

    def load_script(self, entry_path: str, record: "PluginRecord",
                    ctx: "PluginContext") -> ModuleType:
        ext = os.path.splitext(entry_path)[1].lower()
        assembly = None

        if ext == ".cs":
            refs_dir = os.path.join(record.path, "refs")
            references = []
            if os.path.isdir(refs_dir):
                references = [os.path.join(refs_dir, f) for f in os.listdir(refs_dir)
                              if f.endswith(".dll")]
            runtime_ref = _python_runtime_reference()
            if runtime_ref and runtime_ref not in references:
                references.append(runtime_ref)
            source_key = _source_cache_key(entry_path, references)
            cached = _SOURCE_ASSEMBLY_CACHE.get(source_key)
            if cached is not None:
                assembly = cached.get("assembly")
            if assembly is None:
                compiler = _find_csc()
                if compiler is not None:
                    build_dir = tempfile.mkdtemp(prefix=f"sao_cs_{record.plugin_id}_")
                    try:
                        dll_path = _compile_cs(entry_path, build_dir, references or None)
                        _ensure_reference_assemblies(references)
                        assembly = _load_compiled_source_assembly(dll_path)
                        _remember_source_assembly(source_key, assembly, build_dir, dll_path)
                    except Exception:
                        shutil.rmtree(build_dir, ignore_errors=True)
                        raise
                else:
                    assembly = _compile_cs_roslyn(entry_path, references or None)
                    if assembly is not None:
                        _ensure_reference_assemblies(references)
                        _remember_source_assembly(source_key, assembly, "", "")
                    else:
                        raise RuntimeError(
                            "无法编译 .cs 插件：未找到 C# 编译器且 Roslyn 未就绪。\n"
                            "解决方案：\n"
                            "  1. 将插件打包为预编译 .dll（推荐）\n"
                            "  2. 安装 .NET SDK: https://dotnet.microsoft.com/download\n"
                            "  3. 运行 python -m act_platform.scripting.fetch_roslyn "
                            "下载 Roslyn 编译器到 scripting/roslyn/"
                        )
        elif ext == ".dll":
            dll_path = entry_path
        else:
            raise ValueError(f"C# runtime: unsupported entry extension {ext!r}")

        if assembly is None:
            assembly = _System.Reflection.Assembly.LoadFile(os.path.abspath(dll_path))
        self._assemblies[record.plugin_id] = assembly

        exported_types = list(assembly.GetExportedTypes())
        candidate_types = exported_types or list(assembly.GetTypes())

        plugin_type = None
        for t in candidate_types:
            name = str(t.Name)
            if name.lower() in ("plugin", "pluginentry", record.plugin_id.replace("-", "").replace("_", "")):
                plugin_type = t
                break
        if plugin_type is None:
            if candidate_types:
                plugin_type = candidate_types[0]
        if plugin_type is None:
            raise RuntimeError("C# assembly has no public types")

        is_static = all(
            m.IsStatic for m in plugin_type.GetMethods()
            if str(m.Name).lower().startswith("on")
        )

        instance = None
        if not is_static:
            try:
                instance = _System.Activator.CreateInstance(plugin_type)
                self._instances[record.plugin_id] = instance
            except Exception:
                is_static = True

        proxy = _CSharpProxy(ctx)

        module = ScriptModule(
            f"act_plugin_cs_{record.plugin_id}",
            "csharp", entry_path,
        )

        hook_map = {
            "on_load": ("OnLoad", "on_load"),
            "on_enable": ("OnEnable", "on_enable"),
            "on_disable": ("OnDisable", "on_disable"),
            "on_unload": ("OnUnload", "on_unload"),
        }

        for py_name, (cs_name, alt_name) in hook_map.items():
            method_info = (plugin_type.GetMethod(cs_name)
                           or plugin_type.GetMethod(alt_name))
            if method_info is None:
                continue

            if py_name == "on_load":
                if is_static:
                    def _on_load_static(_ctx_ignored=None, *, _p=proxy, _mi=method_info):
                        _mi.Invoke(None, _System.Array[_System.Object]([_p]))
                    module.set_hook("on_load", _on_load_static)
                else:
                    def _on_load_inst(_ctx_ignored=None, *, _p=proxy, _inst=instance, _mi=method_info):
                        _mi.Invoke(_inst, _System.Array[_System.Object]([_p]))
                    module.set_hook("on_load", _on_load_inst)
            else:
                if is_static:
                    def _hook_static(mi=method_info):
                        mi.Invoke(None, None)
                    module.set_hook(py_name, _hook_static)
                else:
                    def _hook_inst(inst=instance, mi=method_info):
                        mi.Invoke(inst, None)
                    module.set_hook(py_name, _hook_inst)

        return module

    def unload_script(self, record: "PluginRecord") -> None:
        self._assemblies.pop(record.plugin_id, None)
        self._instances.pop(record.plugin_id, None)
        build_dir = self._build_dirs.pop(record.plugin_id, None)
        if build_dir and os.path.isdir(build_dir):
            try:
                shutil.rmtree(build_dir, ignore_errors=True)
            except Exception:
                pass
        self._evict_source_cache_for(record)

    @staticmethod
    def _evict_source_cache_for(record: "PluginRecord") -> None:
        """Remove any _SOURCE_ASSEMBLY_CACHE entries whose source lives
        under this plugin's directory so a reinstall picks up fresh code."""
        plugin_dir = os.path.abspath(str(getattr(record, "path", "") or ""))
        if not plugin_dir:
            return
        stale_keys = []
        for key, entry in _SOURCE_ASSEMBLY_CACHE.items():
            dll_path = str(entry.get("dll_path") or "")
            if not dll_path:
                source_sig = key[0] if key else ()
                dll_path = str(source_sig[0]) if source_sig else ""
            try:
                if dll_path and os.path.abspath(dll_path).startswith(plugin_dir + os.sep):
                    stale_keys.append(key)
                    continue
            except Exception:
                pass
            source_sig = key[0] if key else ()
            source_path = str(source_sig[0]) if source_sig else ""
            try:
                if source_path and os.path.abspath(source_path).startswith(plugin_dir + os.sep):
                    stale_keys.append(key)
            except Exception:
                pass
        for key in stale_keys:
            entry = _SOURCE_ASSEMBLY_CACHE.pop(key, None)
            try:
                _SOURCE_ASSEMBLY_CACHE_ORDER.remove(key)
            except ValueError:
                pass
            stale_dir = str((entry or {}).get("build_dir") or "")
            if stale_dir and os.path.isdir(stale_dir):
                try:
                    shutil.rmtree(stale_dir, ignore_errors=True)
                except Exception:
                    pass
