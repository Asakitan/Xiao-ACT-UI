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
    'sao_gui',
    'sao_webview',
    'sao_theme',
    'sao_sound',
    'sao_menu_hud',
    'sao_gui_alert',
    'sao_gui_autokey',
    'sao_gui_bosshp',
    'sao_gui_bossraid',
    'sao_gui_commander',
    'sao_gui_dps',
    'sao_gui_hp',
    'sao_gui_skillfx',
    'gpu_renderer',
    'overlay_scheduler',
    'overlay_render_worker',
    '_sao_cy_pixels',
    'window_effects',
    'install_npcap',
    'sao_updater',
    # v2.1.2-h: 抓包链路 — 这些是 sao_gui 内 lazy-import 的, onedir +
    # noarchive 下 PyInstaller 静态分析有时遗漏 → packet_bridge ImportError
    'packet_bridge',
    'packet_capture',
    'packet_parser',
    'dps_tracker',
    'boss_raid_engine',
    'boss_autokey_linkage',
    'auto_key_engine',
    'character_profile',
    'recognition',
    'skill_recognition',
    'window_locator',
    'vision_accel',
    'game_state',
    'gpu_capture',
    # proto 包 — packet_parser 内 `from proto import star_resonance_pb2`
    'proto',
    'proto.star_resonance_pb2',
]

WEBVIEW_PLATFORM_HIDDENIMPORTS = collect_submodules('webview.platforms')
PROTOBUF_HIDDENIMPORTS = collect_submodules('google.protobuf')
CLR_LOADER_HIDDENIMPORTS = collect_submodules('clr_loader')

# v2.3.0 GUI 链路重置 — 收集 skia / numba / llvmlite / moderngl-window 原生二进制
GPU_RENDER_BINARIES = (
    collect_dynamic_libs('skia')
    + collect_dynamic_libs('numba')
    + collect_dynamic_libs('llvmlite')
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
    hiddenimports=LOCAL_HIDDENIMPORTS + WEBVIEW_PLATFORM_HIDDENIMPORTS + PROTOBUF_HIDDENIMPORTS + CLR_LOADER_HIDDENIMPORTS + [
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
        'numba',
        'numba.core',
        'numba.core.runtime',
        'numba.cpython',
        'numba.np',
        'llvmlite',
        'llvmlite.binding',
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
