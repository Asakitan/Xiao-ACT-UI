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

# 5.0.0: 平台 hiddenimports — 游戏模块已搬到 plugins/star_resonance_plugin/,
# 作为 DATA (.py) 打进包, 由插件加载器运行时 importlib 加载, 不需要 hiddenimports。
LOCAL_HIDDENIMPORTS = [
    'sao_gui',
    'sao_webview',
    'sao_web_panel_common',
    '_sao_cy_memscan',
    '_sao_cy_uihelpers',
]

WEBVIEW_PLATFORM_HIDDENIMPORTS = collect_submodules('webview.platforms')
PROTOBUF_HIDDENIMPORTS = collect_submodules('google.protobuf')
CLR_LOADER_HIDDENIMPORTS = collect_submodules('clr_loader')
GUI_MODULES_HIDDENIMPORTS = collect_submodules('gui_modules')
REORG_PKG_HIDDENIMPORTS = (
    collect_submodules('utils')
    + collect_submodules('render')
    + collect_submodules('engines')
    + collect_submodules('updater')
    + collect_submodules('sao_theme')
    + collect_submodules('act_platform')
    + collect_submodules('act_replay')
    + collect_submodules('ui_gpu')
    + collect_submodules('ai_editor')
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
    'mem_probe.driver_backend',
    'mem_probe.driver_bootstrap',
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
    (path, 'plugins/star_resonance_plugin/cython')
    for path in glob(os.path.join(HERE, 'plugins', 'star_resonance_plugin', 'cython', '_sao_cy*.pyd'))
]
GPU_RENDER_DATAS = (
    collect_data_files('skia')
    + collect_data_files('moderngl_window')
    + collect_data_files('glfw')
)

a = Analysis(
    ['main.py'],
    pathex=[HERE],
    binaries=GPU_RENDER_BINARIES + CYTHON_ACCEL_BINARIES,
    datas=[
        # Web UI (平台 HTML + 字体)
        ('web', 'web'),
        # GPU SkillFX SDF 片段着色器
        ('shaders', 'shaders'),
        # ACT 插件树 (含 star_resonance_plugin 及其 assets/proto/web/cython)
        ('plugins', 'plugins'),
        # 图标
        ('icon.ico', '.'),
        # backend data (local only, skip if absent)
        *([
            (os.path.join(HERE, 'drivers', f), 'drivers')
            for f in os.listdir(os.path.join(HERE, 'drivers'))
            if f.endswith('.dat')
        ] if os.path.isdir(os.path.join(HERE, 'drivers')) else []),
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
        # 插件依赖保险: hide_seek_plugin 自带的 CV 引擎以 DATA(.py) 下发, 其
        # ``import cv2/numpy/utils.window_locator`` 不在 PyInstaller 静态图内,
        # 故显式钉住, 保证从主程序获取依赖在冻结包里始终可用。
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
          if n not in ('mem_probe.driver_backend', 'mem_probe.driver_bootstrap')]

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
