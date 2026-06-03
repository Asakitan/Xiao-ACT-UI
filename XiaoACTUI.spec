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

LOCAL_HIDDENIMPORTS = [
    # 根目录入口/壳模块（保留扁平名）
    'sao_gui',
    'sao_webview',
    'sao_web_panel_common',
    # Cython 加速器（裸名 import；.pyd 在根，由 CYTHON_ACCEL_BINARIES glob 收集 binaries）
    '_sao_cy_pixels',
    '_sao_cy_combat',
    '_sao_cy_packet',
    '_sao_cy_skillfx',
    '_sao_cy_uihelpers',
    # proto 包 — packet_parser 内 `from proto import star_resonance_pb2`
    'proto',
    'proto.star_resonance_pb2',
    # reorg 2026-06-02: sao_theme/sao_sound/gpu_*/overlay_*/packet_*/各引擎/recognition/
    # window_*/sao_updater 等已全部下沉到功能子包 + sao_theme/packet_parser 拆包，
    # 改由下方 REORG_PKG_HIDDENIMPORTS 的 collect_submodules 自动收集
    # （含 sao_gui lazy-import 的抓包/识别/GPU 链路，collect_submodules 真导入枚举比静态分析更全）。
]

WEBVIEW_PLATFORM_HIDDENIMPORTS = collect_submodules('webview.platforms')
PROTOBUF_HIDDENIMPORTS = collect_submodules('google.protobuf')
CLR_LOADER_HIDDENIMPORTS = collect_submodules('clr_loader')
# Round 38 (v3.2.4): all sao_gui_*.py and SAOPlayerGUI mixins now live in
# gui_modules/. The individual modules are listed in LOCAL_HIDDENIMPORTS
# above, but use collect_submodules as a safety net so future additions
# get picked up automatically.
GUI_MODULES_HIDDENIMPORTS = collect_submodules('gui_modules')
# reorg 2026-06-02: 运行时模块下沉到功能子包 + sao_theme/packet_parser 拆包，
# 全部用 collect_submodules 自动收集（真导入枚举子模块，覆盖 lazy-import，避免手列裸名漏一个就 ImportError）。
REORG_PKG_HIDDENIMPORTS = (
    collect_submodules('utils')
    + collect_submodules('render')
    + collect_submodules('vision')
    + collect_submodules('net')
    + collect_submodules('engines')
    + collect_submodules('updater')
    + collect_submodules('sao_theme')
    + collect_submodules('packet_parser')
    + collect_submodules('act_platform')
)
# 注: utils.window_effects / vision.skill_recognition 是重构前就无任何代码 import
# 的孤立模块（已核验 main 分支亦无引用）。PyInstaller noarchive 对「零引用包内子
# 模块」按依赖图优化不写出 runtime/.pyc（collect_submodules / hiddenimports / a.pure
# 注入均无法强制），但因全仓无人 import 它们，不影响运行时；一旦未来有代码引用，
# collect_submodules 即会随引用链收入。
# Round 70 (v3.2.15): mem_probe/ directory was removed in round 26.
# MEM_PROBE_HIDDENIMPORTS, MEM_PROBE_BINARIES, and the ('mem_probe',
# 'mem_probe') data entry below were all cleaned out together — none
# of them resolve to anything now that the package is gone.

# v2.3.0 GUI 链路重置 — 收集 skia / moderngl-window 原生二进制
GPU_RENDER_BINARIES = (
    collect_dynamic_libs('skia')
    + collect_dynamic_libs('glfw')
)
CYTHON_ACCEL_BINARIES = [
    (path, '.')
    for path in glob(os.path.join(HERE, '_sao_cy*.pyd'))
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
        # Web UI (HTML + 字体)
        ('web', 'web'),
        # 资源 (音效、字体、技能名表)
        ('assets', 'assets'),
        # Protobuf / schema
        ('proto', 'proto'),
        # GPU SkillFX SDF 片段着色器 (v2.3.8: 之前未打包 → onedir 启动后
        # skillfx_pipeline._load_fragment FileNotFoundError → _tls.failed=True
        # → SkillFX 永远走 CPU/PIL fallback)
        ('shaders', 'shaders'),
        # 图标
        ('icon.ico', '.'),
    ] + GPU_RENDER_DATAS,
    hiddenimports=LOCAL_HIDDENIMPORTS + WEBVIEW_PLATFORM_HIDDENIMPORTS + PROTOBUF_HIDDENIMPORTS + CLR_LOADER_HIDDENIMPORTS + GUI_MODULES_HIDDENIMPORTS + REORG_PKG_HIDDENIMPORTS + [
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
