# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec — 咲 ACT UI (Xiao ACT UI)
打包命令:  pyinstaller XiaoACTUI.spec
输出目录:  dist/XiaoACTUI/
"""

import os
import sys

from PyInstaller.utils.hooks import collect_submodules

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
    'window_effects',
    'install_npcap',
]

WEBVIEW_PLATFORM_HIDDENIMPORTS = collect_submodules('webview.platforms')

a = Analysis(
    ['main.py'],
    pathex=[HERE],
    binaries=[],
    datas=[
        # Web UI (HTML + 字体)
        ('web', 'web'),
        # 资源 (音效、字体、技能名表)
        ('assets', 'assets'),
        # Protobuf / schema
        ('proto', 'proto'),
        # 图标
        ('icon.ico', '.'),
    ],
    hiddenimports=LOCAL_HIDDENIMPORTS + WEBVIEW_PLATFORM_HIDDENIMPORTS + [
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
        # 热键
        'pynput',
        'pynput.keyboard',
        'pynput.keyboard._win32',
        'pynput.mouse',
        'pynput.mouse._win32',
        # OpenGL 特效
        'moderngl',
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
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='XiaoACTUI',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,                # 无控制台窗口
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='icon.ico',
    uac_admin=True,               # 抓包需要管理员权限
)
