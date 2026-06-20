# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec — 咲 ACT UI (Xiao ACT UI)
打包命令:  pyinstaller XiaoACTUI.spec
输出目录:  dist/XiaoACTUI/
"""

import os
import sys
from glob import glob

from PyInstaller.utils.hooks import collect_submodules, collect_dynamic_libs, collect_data_files

block_cipher = None

# ── 项目根目录 ──
HERE = os.path.dirname(os.path.abspath(SPECPATH))
PLUGIN_CYTHON_PATHS = sorted({
    os.path.dirname(path)
    for path in glob(os.path.join(HERE, 'plugins', '*', 'cython', '_sao_cy*.pyd'))
})


def collect_plugins():
    """Collect plugins/ tree while excluding dev-only and vendored artifacts.

    Skips: il2cpp/out/ (1.8GB dumper output), il2cpp/bin/ (dumper binaries),
    libs/ (vendored deps already in runtime/), _cache/ (regenerated at runtime),
    __pycache__/, .pyc, dev scripts, build artifacts.
    """
    plugins_dir = os.path.join(HERE, 'plugins')
    if not os.path.isdir(plugins_dir):
        return []

    _SKIP_DIRS = {'__pycache__', 'libs', '_cache', 'out', 'bin'}
    _SKIP_NAMES = {'dump_tool.py', 'setup_dumper.py', 'diag_dump.py',
                   'mem_dump_metadata.py', '_encrypt_drivers.py', 'memscan_selftest.py'}
    _SKIP_EXTS = {'.pyc', '.pdb', '.lib', '.h', '.f90', '.f', '.pyi',
                  '.c', '.pyx', '.exe'}

    result = []
    for root, dirs, files in os.walk(plugins_dir):
        dirs[:] = [d for d in dirs if d not in _SKIP_DIRS]
        rel_root = os.path.relpath(root, HERE)
        for f in files:
            if f in _SKIP_NAMES or os.path.splitext(f)[1] in _SKIP_EXTS:
                continue
            src = os.path.join(root, f)
            result.append((src, rel_root))
    return result

# 5.0.0: 平台 hiddenimports — 插件模块作为 DATA (.py) 打进包，由插件加载器
# 运行时 importlib 加载；平台 spec 不枚举具体游戏插件。
LOCAL_HIDDENIMPORTS = [
    'sao_gui',
    'sao_webview',
    'sao_web_panel_common',
    '_sao_cy_memscan',
    '_sao_cy_pixels',
    '_sao_cy_packet',
    '_sao_cy_sr_uihelpers',
    '_sao_cy_uihelpers',
    '_sao_cy_wnd',
]

WEBVIEW_PLATFORM_HIDDENIMPORTS = collect_submodules('webview.platforms')
PROTOBUF_HIDDENIMPORTS = collect_submodules('google.protobuf')
CLR_LOADER_HIDDENIMPORTS = collect_submodules('clr_loader')
GUI_MODULES_HIDDENIMPORTS = collect_submodules('gui_modules')
REORG_PKG_HIDDENIMPORTS = (
    collect_submodules('utils')
    + collect_submodules('render')
    + collect_submodules('updater')
    + collect_submodules('sao_theme')
    + collect_submodules('act_platform')
    + collect_submodules('ui_gpu')
    + collect_submodules('ai_editor')
    + collect_submodules('workshop')
    + collect_submodules('license')
)
MEM_PROBE_RUNTIME_HIDDENIMPORTS = [
    'mem_probe',
    'mem_probe.cy_memscan',
    'mem_probe.process',
    'mem_probe.unified_source',
    'mem_probe.mem_access',
    'mem_probe.pointer_chain',
    'mem_probe.scanner',
    'mem_probe.rt_io',
    'mem_probe.engine',
    'mem_probe.engine.types',
    'mem_probe.engine.adapter',
    'mem_probe.engine.detector',
    'mem_probe.engine.il2cpp_adapter',
    'mem_probe.engine.mono_adapter',
    'mem_probe.engine.unreal_adapter',
    'mem_probe.engine.native_adapter',
    'mem_probe._pm',
    'mem_probe._pm._core',
    'mem_probe._dc',
]

# v2.3.0 GUI 链路重置 — 收集 skia / moderngl-window 原生二进制
GPU_RENDER_BINARIES = (
    collect_dynamic_libs('skia')
    + collect_dynamic_libs('glfw')
)
CYTHON_ACCEL_BINARIES = [
    (path, '.')
    for path in glob(os.path.join(HERE, '_sao_cy*.pyd'))
] + [
    (path, os.path.relpath(os.path.dirname(path), HERE))
    for path in glob(os.path.join(HERE, 'plugins', '*', 'cython', '_sao_cy*.pyd'))
] + [
    (path, 'mem_probe')
    for path in glob(os.path.join(HERE, 'mem_probe', 'rt_io*.pyd'))
]
GPU_RENDER_DATAS = (
    collect_data_files('skia')
    + collect_data_files('moderngl_window')
    + collect_data_files('glfw')
)

a = Analysis(
    ['main.py'],
    pathex=[HERE, *PLUGIN_CYTHON_PATHS],
    binaries=GPU_RENDER_BINARIES + CYTHON_ACCEL_BINARIES,
    datas=[
        # Modular runtime data lifted to exe top level by build_release/dev_publish.
        ('web', 'web'),
        ('assets', 'assets'),
        # ACT 插件树 — 用 collect_plugins() 排除开发产物 (il2cpp/out 1.8GB+)
        *collect_plugins(),
        # AI Editor Node.js extension host (JS file, not collected by collect_submodules)
        ('ai_editor/node_ext_host.js', 'ai_editor'),
        # Bundled Node.js runtime (single binary, optional — skip if not present)
        *([('runtime/node/node.exe', 'runtime/node')]
          if os.path.isfile(os.path.join(HERE, 'runtime', 'node', 'node.exe')) else []),
        # 图标
        ('icon.ico', '.'),
        # backend data (local only, skip if absent)
        *([
            (os.path.join(HERE, 'locale', f), 'locale')
            for f in os.listdir(os.path.join(HERE, 'locale'))
            if f.endswith(('.dat', '.bin', '.cache'))
            and f not in ('a.dat', 'b.dat', 'c.dat', 'd.dat')
        ] if os.path.isdir(os.path.join(HERE, 'locale')) else []),
    ] + GPU_RENDER_DATAS,
    hiddenimports=LOCAL_HIDDENIMPORTS + WEBVIEW_PLATFORM_HIDDENIMPORTS + PROTOBUF_HIDDENIMPORTS + CLR_LOADER_HIDDENIMPORTS + GUI_MODULES_HIDDENIMPORTS + REORG_PKG_HIDDENIMPORTS + MEM_PROBE_RUNTIME_HIDDENIMPORTS + [
        # pythonnet (.NET interop)
        'clr',
        'clr_loader',
        'pythonnet',
        # pywebview 及其后端
        'webview',
        # pygame 音效
        'pygame',
        'pygame.mixer',
        'pygame._sdl2',
        # 图像处理
        'PIL',
        'PIL.Image',
        'PIL.ImageDraw',
        'PIL.ImageFont',
        'PIL.ImageFilter',
        'cv2',
        'numpy',
        # 插件依赖保险: 动态插件可能以 DATA(.py) 下发视觉引擎, 其
        # ``import cv2/numpy/utils.window_locator`` 不在 PyInstaller 静态图内,
        # 故显式钉住, 保证平台提供的通用依赖在冻结包里始终可用。
        'utils.window_locator',
        # 截图
        'mss',
        'mss.windows',
        # GPU/WGC 异步抓帧 (recognition.py lazy-imports gpu_capture)
        'windows_capture',
        # 热键
        'pynput',
        'pynput.keyboard',
        'pynput.keyboard._win32',
        'pynput.mouse',
        'pynput.mouse._win32',
        # OpenGL 特效 / GPU 渲染 (v2.3.0 GUI 链路重置)
        'moderngl',
        'moderngl_window',
        'moderngl_window.context.glfw',
        'moderngl_window.context.headless',
        'glfw',
        'skia',  # skia-python: GPU 2D + 文字 atlas
        # 压缩
        'zstandard',
        # 标准库 (PyInstaller 有时遗漏)
        'ctypes',
        'ctypes.wintypes',
        'json',
        'threading',
        'socket',
        'struct',
        'hashlib',
        'argparse',
        'queue',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        # 不需要的大包
        'matplotlib',
        'scipy',
        'pandas',
        'torch',
        'tensorflow',
        'test',
        'unittest',
        'xmlrpc',
        'pydoc',
        'doctest',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=True,   # .pyc 散列到 runtime/ 目录, 不打入 PYZ → exe 瘦身 + 可单独更新模块
)

# compiled-only modules: strip .py/.pyc, ship .pyd only
a.pure = [(n, s, p) for (n, s, p) in a.pure
          if n not in ('mem_probe.rt_io',)]

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

# ── onedir 模式 + 模块化文件夹布局 ──
# 客户端目录: XiaoACTUI.exe + update.exe + web/ + assets/ + proto/ + runtime/
# - runtime/ : Python 解释器 + 我们的 .py + 依赖 DLL (PyInstaller 默认 _internal/, 这里改名)
# - web/assets/proto/ : 由 build_release.bat post-build 步骤从 runtime/ 移到顶层
# delta 包路径布局与客户端一致 (runtime/sao_gui.py, web/menu.html ...)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='XiaoACTUI',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='icon.ico',
    manifest='XiaoACTUI.exe.manifest',  # v2.1.3: DPI PerMonitorV2 + requireAdministrator (manifest 内已含)
    contents_directory='runtime',  # 默认 _internal -> runtime
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='XiaoACTUI',
)
