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
    '_sao_cy_memscan',
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
    # v4.0.0 拆分新增的两个客户端子包，之前漏挂安全网：
    #   act_replay  — act_platform.runtime 在函数内懒导入 (harness/importer/timeline)，
    #                 PyInstaller 静态分析容易漏，ACT 回放/时间轴/离线导入靠它。
    #   ui_gpu      — sao_theme.__init__ 模块级导入 SAOPopUpMenu (GPU 弹出菜单)。
    # 两者都是带 __init__.py 的纯 .py 小包，collect_submodules 安全(不像 mem_probe)。
    + collect_submodules('act_replay')
    + collect_submodules('ui_gpu')
)
# 注: utils.window_effects / vision.skill_recognition 是重构前就无任何代码 import
# 的孤立模块（已核验 main 分支亦无引用）。PyInstaller noarchive 对「零引用包内子
# 模块」按依赖图优化不写出 runtime/.pyc（collect_submodules / hiddenimports / a.pure
# 注入均无法强制），但因全仓无人 import 它们，不影响运行时；一旦未来有代码引用，
# collect_submodules 即会随引用链收入。
# Round 71 (v4.0.0): mem_probe/ EXISTS and IS a genuine runtime dependency —
# the earlier "removed in round 26" note was wrong. The memory/hybrid/auto data
# source is lazily imported at runtime by:
#   net/packet_bridge.py:520            from mem_probe.unified_source import UnifiedDataSource
#   gui_modules/sao_gui_engine_lifecycle_mixin.py:425
#                                       from mem_probe.il2cpp.mem_state_bridge import MemStateBridge
# Both are function-level imports PyInstaller's static analysis will NOT follow,
# so memory-mode is dead in the frozen client unless we force-collect them.
# We list ONLY the verified runtime closure of unified_source + mem_state_bridge
# (16 modules, all confirmed on disk). We do NOT collect_submodules('mem_probe')
# — that would drag in the dev-only il2cpp DUMPER tree (GameAssembly.dll,
# global-metadata.dat, DummyDll/, cli/fingerprint/dump_tool/find_owner/refresh/
# rebuild_bundle/metadata_registration/code_registration/forward_walk/verify_klass).
# The AVX2 core _sao_cy_memscan.pyd already ships via CYTHON_ACCEL_BINARIES glob;
# mem_probe.cy_memscan is the pure-Python facade over it (no extra binary).
MEM_PROBE_RUNTIME_HIDDENIMPORTS = [
    'mem_probe',
    'mem_probe.cy_memscan',
    'mem_probe.process',
    'mem_probe.unified_source',
    'mem_probe.il2cpp',
    'mem_probe.il2cpp.mem_state_bridge',
    'mem_probe.il2cpp.mem_self_state_provider',
    'mem_probe.il2cpp.mem_state_anchor',
    'mem_probe.il2cpp.static_dps_source',
    'mem_probe.il2cpp.static_resolver',
    'mem_probe.il2cpp.script_parser',
    'mem_probe.il2cpp.dump_cs_parser',
    'mem_probe.il2cpp.instance_cache',
    'mem_probe.il2cpp.bundle_loader',
    'mem_probe.il2cpp.bundle_store',
    'mem_probe.il2cpp.mem_skill_slots',
    # memory-driven hybrid: entity/boss HP + names + version-robust klass resolution.
    # All function-level lazy imports -> invisible to PyInstaller static analysis.
    'mem_probe.il2cpp.mem_entity_mgr',
    'mem_probe.il2cpp.mem_entity_combat',
    'mem_probe.il2cpp.mem_entity_provider',
    'mem_probe.il2cpp.mem_attr_reader',
    'mem_probe.il2cpp.auto_registration_locator',
    'mem_probe.il2cpp.resolver',
    'mem_probe.il2cpp.mem_damage_reader',
    # map-name chain: mem_state_bridge lazily imports MapNameReader (function
    # level, invisible to static analysis); the string-pool bridge is its
    # module-level dependency.
    'mem_probe.il2cpp.mem_map_name_reader',
    'mem_probe.il2cpp.mem_string_pool',
]

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
        # ACT 插件树 — 与 web/assets/proto 同等待遇: dest 写 'plugins', 在
        # contents_directory='runtime' 下 PyInstaller 先把它放进 runtime/plugins/,
        # 再由 build_release.bat 的提升循环 move 到 exe 顶层 plugins/。
        # 加载器 act_platform/runtime.py project_base_dir() 冻结时解析到 config.BASE_DIR
        # (exe 顶层) → 扫 <exe>/plugins + <exe>/user_plugins, 用户自带插件放顶层不被更新覆盖。
        # ⚠ dest 必须是 'plugins'(→runtime/plugins) 不能写 'runtime/plugins'(会双层套娃成
        #   runtime/runtime/plugins)。plugin.py 以原始 .py 进包(加载器按路径 spec_from_file_location)。
        ('plugins', 'plugins'),
        # 图标
        ('icon.ico', '.'),
        # IL2CPP per-version offset bundles (klass RVAs + field offsets). Ships under
        # the mem_probe package (NOT lifted like assets), so bundle_store/static_dps_source
        # __file__-relative paths resolve in onedir. Field offsets are the durable payload;
        # for a NEW game version with no matching bundle, auto_registration_locator derives
        # klass pointers from process memory (no dump) and StaticResolver self-heals.
        ('mem_probe/il2cpp/_cache/bundle.json', 'mem_probe/il2cpp/_cache'),
        ('mem_probe/il2cpp/_cache/bundles', 'mem_probe/il2cpp/_cache/bundles'),
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
