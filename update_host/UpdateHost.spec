# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec — SAO Auto Update Host
打包命令: pyinstaller update_host/UpdateHost.spec
输出文件: dist/UpdateHost.exe
"""

import os

from PyInstaller.utils.hooks import collect_submodules

block_cipher = None

HERE = os.path.dirname(os.path.abspath(SPECPATH))

FASTAPI_HIDDENIMPORTS = collect_submodules("fastapi")
STARLETTE_HIDDENIMPORTS = collect_submodules("starlette")
UVICORN_HIDDENIMPORTS = collect_submodules("uvicorn")
PYDANTIC_HIDDENIMPORTS = collect_submodules("pydantic")
ANYIO_HIDDENIMPORTS = collect_submodules("anyio")

a = Analysis(
    ["update_host_main.py"],
    pathex=[HERE],
    binaries=[],
    datas=[],
    hiddenimports=(
        FASTAPI_HIDDENIMPORTS
        + STARLETTE_HIDDENIMPORTS
        + UVICORN_HIDDENIMPORTS
        + PYDANTIC_HIDDENIMPORTS
        + ANYIO_HIDDENIMPORTS
        + [
            "fastapi.staticfiles",
            "starlette.staticfiles",
            "email.mime",
            "email.mime.multipart",
            "email.mime.text",
        ]
    ),
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        "matplotlib",
        "scipy",
        "pandas",
        "torch",
        "tensorflow",
        "tkinter",
        "test",
        "unittest",
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
    name="UpdateHost",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon="../icon.ico",
)
