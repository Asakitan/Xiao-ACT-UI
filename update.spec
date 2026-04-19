# -*- mode: python ; coding: utf-8 -*-
"""独立 update.exe（小助手），主 exe 退出后由它来应用 staging 中的更新包。

打包命令: pyinstaller --clean --noconfirm update.spec
输出: dist/update.exe (onefile)
"""
import os

block_cipher = None
HERE = os.path.dirname(os.path.abspath(SPECPATH))

a = Analysis(
    ['update_apply.py'],
    pathex=[HERE],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'numpy', 'PIL', 'cv2', 'pygame', 'webview', 'moderngl',
        'matplotlib', 'scipy', 'pandas', 'mss', 'pynput',
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
    name='update',
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
    uac_admin=True,
)
