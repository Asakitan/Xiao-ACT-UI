# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec — Star Resonance Auto Key Server
打包命令:  pyinstaller plugins/star_resonance_plugin/server/AutoKeyServer.spec
输出目录:  dist/AutoKeyServer/
"""

import os

block_cipher = None

HERE = os.path.dirname(os.path.abspath(SPECPATH))
PLUGIN_DIR = os.path.dirname(HERE)
PROJECT_ROOT = os.path.dirname(os.path.dirname(PLUGIN_DIR))

a = Analysis(
    [os.path.join(HERE, 'app.py')],
    pathex=[PROJECT_ROOT, PLUGIN_DIR, HERE],
    binaries=[],
    datas=[],
    hiddenimports=[
        # FastAPI 及其依赖
        'fastapi',
        'fastapi.responses',
        'fastapi.routing',
        'fastapi.middleware',
        'fastapi.middleware.cors',
        'starlette',
        'starlette.responses',
        'starlette.routing',
        'starlette.middleware',
        'pydantic',
        'pydantic.fields',
        # ASGI server
        'uvicorn',
        'uvicorn.logging',
        'uvicorn.loops',
        'uvicorn.loops.auto',
        'uvicorn.protocols',
        'uvicorn.protocols.http',
        'uvicorn.protocols.http.auto',
        'uvicorn.protocols.websockets',
        'uvicorn.protocols.websockets.auto',
        'uvicorn.lifespan',
        'uvicorn.lifespan.on',
        # HTTP 工具
        'httptools',
        'uvloop',
        'websockets',
        'anyio',
        'anyio._backends',
        'anyio._backends._asyncio',
        # 标准库
        'sqlite3',
        'json',
        'email.mime',
        'email.mime.multipart',
        'email.mime.text',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'matplotlib',
        'scipy',
        'pandas',
        'torch',
        'tensorflow',
        'tkinter',
        'test',
        'unittest',
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
    name='AutoKeyServer',
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
    icon=os.path.join(PROJECT_ROOT, 'icon.ico'),
)
