# SAO Auto 版本历史

逐版本变更记录, 最新在前。本文件由 config.py 内联的历史注释迁出。

## v5.2.7: AI Editor 消息回传 / 本地代理、镜像点击修复与 overlay 区域生成提速.

  - **AI Editor / 扩展宿主**:
    - 终端启动失败统一返回结构化 `process-error` 结果, 前端可稳定展示失败的 cwd / profile / shell / 时间戳信息。
    - Node extension host 新增 `window_message_request/response` 往返, 带 action 的 `showInformationMessage` / `showWarningMessage` 选择结果可回传给扩展。
    - 设置页新增 Claude Code 本地代理状态面板, 支持启动 / 停止 / 复制环境变量; `real_extension_probe` 与 `selftest` 补齐 CSS / JS / image / localhost 资源 smoke 覆盖, 并新增 `selftest_runner` 聚合入口。
  - **GUI / 面板交互**:
    - AI Editor 与 Workshop 分离窗口仅允许标题栏拖动, 避免正文区域点击误触发拖窗。
    - 插件管理面板刷新改为后台线程异步扫描, 减少 Tk 主线程卡顿; detached plugin panels 同步补做主题刷新。
  - **overlay / render**:
    - `tk_mirror` 改为直接按 Tk 控件树命中并 `event_generate()` 分发输入, 修复镜像窗口正文点击被吞、hover/press 不稳定的问题。
    - `overlay_compositor` 新增 alpha span `pad_and_merge_row_spans()` 合并与 `ExtCreateRegion` 批量建区路径, 默认帧率跟随主显示器刷新率并启用 1ms timer quantum。
    - `gpu_renderer` 复用源纹理缓存, 减少每帧 create/release 开销; `gpu_overlay_window` 的 Tk poller 降到 16ms, 只承载结构事件派发。
  - **Star Resonance / 打包**:
    - buffmon / skillfx / packet bridge / webview bridge 的运行期开关读取切到 `sr_config`, 避免误读全局 `config`。
    - Nuitka 打包显式补入 `lupa`, 保证 Lua 运行时随包可用。

## v5.2.6: overlay DComp 呈现桥、Tk mirror 时序收口、WDA 路径改写与非 AI 运行时批量刷新.

  - **overlay / mirror**:
    - unified overlay 新增 DirectComposition bridge 呈现路径, 初始化失败时重试并自动回退到旧 GLFW overlay。
    - Tk mirror 仅在 input proxy 就绪后才隐藏真实窗口, show / hide / 几何变化时同步 proxy, 并为镜像捕获补上圆角透明裁切。
    - overlay host 的 WGL 上下文创建 / 销毁增加串行化保护, `sao_gui` 启动阶段延迟确保 Tk poller 可恢复挂起的 unified compositor。
  - **WDA / capture exclusion**:
    - `_dc` 改为先通过动态 syscall 路径设置 `SetWindowDisplayAffinity`, 再 best-effort 清理 tagWND / ExStyle 痕迹, 校验改走 `GetWindowDisplayAffinity()`。
  - **GUI / 面板**:
    - WebView UI 菜单入口在主菜单与浮动菜单中统一标记为暂时废弃并禁用。
    - ACT / 进程选择器 / Workshop 面板显示时补做主题与样式刷新, 面板关闭按钮事件绑定兼容不同触发签名。
  - **Star Resonance Cython**:
    - 刷新 `_sao_cy_combat` / `_sao_cy_packet` / `_sao_cy_skillfx` / `_sao_cy_sr_uihelpers` 加速二进制, 并移除插件目录中陈旧的 `_sao_cy_pixels` 副本, 统一回到顶层 accelerator 布局。

## v5.2.5: ACT 插件交互条形控件、拖动刷新降噪与 Nuitka 打包自测裁剪.

  - **ACT UI spec**:
    - `bar` 节点在规范化时保留 `action` / `lo` / `hi` / `step`, 插件可声明可点击、可拖动的数值条形控件。
    - Tk 分离插件面板把带 `action` 的 `bar` 渲染为交互轨道, 支持点击 / 拖动计算值并通过 action payload 回传。
  - **插件面板刷新**:
    - 分离面板对 `drag_*` 动作不再立即标记 dirty, 减少连续拖动时的整面板重绘和闪烁。
  - **发布构建**:
    - Nuitka 打包显式排除 `ai_editor.selftest`, 避免发布包携带 AI Editor 自测入口。

## v5.2.4: 插件卸载线程收口、识别引擎停机清理、菜单与弹窗交互修整.

  - **插件生命周期**:
    - `PluginContext` 新增 `should_stop` 与 `register_thread()`, 插件卸载时先发停止信号并等待已登记 worker 退出。
    - `PluginManager.unload_plugin()` 在 `on_disable` / `on_unload` 前后统一触发停止事件, 避免脚本插件后台线程残留。
  - **识别 / 内存路径**:
    - WebView 停止识别引擎与 GUI 硬退出时同步关闭 ACT plugin manager, 防止平台插件继续持有 runtime 资源。
    - Process Selector 与 `_dc` capture exclusion 改为通过 `rt_io.has_write_engine()` 判断写引擎能力, 避免直接读取私有句柄字段。
  - **GUI 交互**:
    - 脚本插件 overlay 菜单切换后保持菜单打开, 方便连续开启/关闭同类插件入口。
    - GPU popup 动态扩展高度时向上补偿位置, 减少子菜单扩容造成的视觉跳动。

## v5.2.3: AI Editor text editor command 运行时追踪补齐, fisheye/弹窗/面板主题细节修整.

  - **AI Editor runtime surfaces**:
    - `ai_editor.node_ext_host.js` / `extension_host.py` 为动态注册命令补充 `kind` / `editorRequired` 元数据与 dispose 生命周期回传,
      `textEditorCommand` 现在会进入 Python 侧 runtime registry。
    - `ai_editor.app` 的 `list_extension_runtime_surfaces()` / 命令面板汇总加入 `nodeCommands`、`runtimeKind`、`textEditorCommands` 统计与 surface evidence。
    - `web/ai_editor_app.html` 与 `ai_editor.selftest.py` 同步支持 `TextEditorCommand` 行、筛选、动作和 DOM 自检快照。
  - **GUI / overlay polish**:
    - `sao_gui_fisheye_mixin` 微调 procedural 鱼眼淡入目标、fadeout 恢复值，以及 ring / beam 漂移轨迹。
    - `sao_gui_float_handlers_mixin` 在 SAO 菜单或 fisheye 活跃时暂停 GPU trigger button 的 topmost 重提, 避免按钮压住面板。
    - `ui_gpu.popup` 在子菜单行数超出预留空间时自动扩展 GPU popup 几何尺寸。
  - **面板主题 / 行为**:
    - `sao_gui_panels_mixin` 把 `_process_selector_panel` 纳入平台主题刷新列表, 调主题时优先走 `refresh_theme()`。
    - `sao_gui_dps_theme_mixin` 在 overlay 映射缺失时安全回退到统一 ACT panel 主题刷新。
    - `sao_gui_link_animation_mixin` / `sao_theme.link_start` 继续平滑 LinkStart 焦点与阶段 FX 过渡, `sao_gui_menu_mixin` 移除临时的 NervGear 关闭后按钮重定位逻辑。

## v5.2.2: 脚本 compositor API 扩展、MMF 零拷贝图层接入、AI Editor 扩展 webview panel 运行时可观测性补齐.

  - **脚本 / SDK API**:
    - `act_platform.plugins` 的 `create_compositor_layer()` 新增 `high_fps` / `target_fps` 参数;
      新增 `set_compositor_layer_mmf_source()` 和 `set_compositor_layer_position()`。
    - `act_platform.scripting.base` / `csharp_runtime` 同步暴露上述 API，Lua / C# / AngelScript / Emma / Python 脚本运行时保持一致能力。
  - **render / compositor**:
    - `overlay_compositor` 新增命名 MMF 零拷贝读帧路径 `_MMFReader`, 图层可直接从共享内存取 BGRA 帧, 规避 Python 字节拷贝上传。
    - host region 同步增加高帧率图层矩形快速路径与逐层 alpha span 缓存, 并按图层 `target_fps` 动态提升 compositor tick 频率。
  - **GUI / 视觉**:
    - `sao_gui_fisheye_mixin` 的 procedural 鱼眼背景重做为更亮的 HUD / ring / data-rain 风格, 支持更丰富的噪声与扫描效果。
    - `sao_theme.link_start` / `sao_gui_link_animation_mixin` 微调 LinkStart / NervGear 过渡焦点与 FX 衰减路径。
  - **AI Editor**:
    - `ai_editor.app` / `extension_host` 为扩展 `createWebviewPanel()` 运行时增加独立记录与 surface 汇总, `list_extension_runtime_surfaces()` 现在包含 `webviewPanels` 桶与 `viewType` / `renderCount` / `messageCount` / `disposed` 等状态。
  - **文档**:
    - 同步更新 `PLUGIN_SDK.md`、`MULTI_LANGUAGE_SCRIPTING.md`、`AI_EDITOR.md`。

## v5.2.1: unified overlay 输入/采集收口, streaming/fisheye 交互整理, Cython region 扫描与 bridge 默认点透更新.

  - **unified overlay / render**:
    - `overlay_adapter` / `overlay_compositor` / `overlay_host` 改为按交互层动态同步 host 输入模式,
      不再一刀切强制 host 永远 passthrough; 新增按 alpha 扫描的 host region 同步, 提升透明洞点击穿透精度。
    - `sao_plugin_unified_overlay` 跟随新的 host 输入路由, 共享 compositor 层支持 direct-host 交互与 input proxy 分流。
  - **streaming / capture exclusion**:
    - `mem_probe._dc` 把物理 capture exclusion 收口到 `streaming_mode` 控制;
      `overlay_compositor` 新增 `_VFence` 轮询/刷新补偿, `alert` / `map_banner` / `mech_banner` 的排除写入改成异步 best-effort。
  - **GUI / 交互**:
    - `sao_gui_menu_mixin` 新增 `Streaming Mode` 与鱼眼背景源切换入口;
      `sao_gui_fisheye_mixin` 默认切到 procedural 背景, live 模式按所在显示器抓屏, unified 模式下释放旧 hit-layer。
    - `sao_gui_link_animation_mixin` / `sao_gui_plugin_manager` / `ui_gpu.popup` 调整 NervGear 分支、刷新时序与 unified host 关闭路径,
      `utils.sao_sound` 增加字体 family 缓存。
  - **平台 / 构建**:
    - `act_platform.plugins` 与 `csharp_runtime` 的 `create_compositor_layer()` 默认 `click_through=True`,
      平台默认把脚本 overlay 视为被动显示层。
    - `_sao_cy_pixels.pyx` 新增 `bgra_alpha_spans()` 热路径, `build_cython_ext.py` 补充对应构建入口。

## v5.2.0: 脚本多文件拆分、插件 RGBA overlay 与 UI/设置健壮性改进.

  - **`act_platform` 脚本多文件拆分**:
    - Lua / AngelScript / Emma 新增 `load_script` / `dofile` / `import` 加载同语言子文件,
      经 `resolve_local_script_path` 沙箱防穿越 (`..` 拒绝、子目录允许), 函数和全局合并进同一解释器作用域。
    - C# 源码模式自动编译插件目录所有 `.cs` 进同一程序集; `_default_target_framework` 在
      .NET Framework 4.x 下回落 `netstandard2.0`; csproj 自动包含同目录 `.cs` 并按目标框架选 `LangVersion`。
    - `open_file_dialog` 支持 `hwnd_owner < 0` 强制无主对话框, 期间标记 `_sao_native_dialog_active`
      抑制 fisheye 关闭并关闭可见菜单。
    - `set_plugin_setting` 用 `_json_safe` 序列化, key 规范化为字符串。
    - 文档同步: `MULTI_LANGUAGE_SCRIPTING.md` / `PLUGIN_SDK.md` / `GAME_STATE_API.md`。
  - **`settings_manager`**: 原子写入 (`NamedTemporaryFile` + `fsync` + `os.replace`) 防止崩溃损坏;
    损坏 JSON 自动备份为 `settings.json.corrupt`; `_json_safe` 清洗非可序列化值。
  - **`render` / overlay 清理**: 移除平台内置 3D 节点、旧 native/software/托管渲染链和
    对应 Cython 构建入口; 3D/角色渲染由插件自行产出 RGBA 帧, 平台只负责 `rgba_frame` 解码、分层合成和输入路由。
  - **GUI / overlay**: `sao_gui_menu_mixin` 命令前关闭菜单时同步释放 fisheye 输入 z-order
    (`_close_sao_menu_for_external_command`); `sao_gui_fisheye_mixin` native 对话框期间抑制关闭并排除
    `act_plugin_manager`; `sao_gui_panels_mixin` 打开插件面板时停 fisheye;
    `sao_panel_components` 新增 `bind_canvas_mousewheel` 路由子控件滚轮到 canvas;
    `sao_gui_plugin_manager` 用 `bind_canvas_mousewheel` 绑定各 canvas 滚轮;
    `sao_plugin_unified_overlay` 支持 `canvas` / `rgba_frame` 独立图层、拖拽和输入代理生命周期;
    `ui_gpu/popup` `force_destroy_overlay` 释放 input zorder。
  - **selftest**: 移除旧 workspace-root 四插件专用 probe/selftest; 平台保留通用 UI spec、overlay 和设置持久化验证。
  - **其他**: `config.parse_hotkey` / `normalize_hotkey` 文档更新为 `HOTKEY_FKEY_VK` 命名键;
    `ui_spec` 删除多余空行。(commit `86e3b23`)

## v4.6.154: BossRaid profile + 机制示例 JSON 加 map_name 字段, 场景跟随 JSON 走.

  - **`engines/boss_raid_engine.py`**: profile 加 `map_name` 字段; reactions 场景下拉从 imported
    profiles 的 `dungeon_id` + `map_name` 派生 label, 没进本也能看到对应 raid 名字 (commit `3563151`)。
  - **机制示例 JSON**(`assets/boss_raids/*.json`): 同步加 `map_name` 字段 (commit `3d3fd0e`)。
  - **`engines/boss_raid_engine.py`**: `_seed_from_assets` 改用 `config.resource_path` 查找 assets 目录,
    修打包后路径不一致 (commit `56e2e16`)。

## v4.6.144 – v4.6.153: driver 内核后端 / BossRaid 机制全量 / ACT 13 面板视觉对齐 / 内存加速 / boss break / hide_seek 修复.

  本区间共 10 个版本, 跨 2026-06-14 ~ 06-15, commit 数 30+, 按主题合并如下 (精确版本号与 commit 对应见 git log):

  ### 1. Driver 内核后端 (XiaoACTprocessReading)
  - **Phase 1 kernel backend**(`1a8a619`): 内核驱动初步接入 mem_probe, 提供 CR3 通信 `XiaoACTprocessReading`(`a6c6d4c`, 跨 Python + C++ 工程)。
  - **Auto-load + 打包**(`a43b98f`): `driver_backend.ensure_loaded()` 三层查找(顶层 `drivers/` / PyInstaller
    `runtime/drivers/` / 根目录兜底) + `sc create+start`; `XiaoACTUI.spec` datas 加 `('drivers','drivers')`;
    首次 `import process` 即触发驱动加载。
  - **`StarProcess` 自动 probe+attach + fallback**(`9318820`): 构造时强制 `probe()+attach()`,
    失败 fallback 到 NtRVM/RPM; 修 `driver_mem_read` 的 `ctypes.byref()` item assignment TypeError。

  ### 2. BossRaid 机制全量 dump + 几何重写 (`54fce0f`)
  - `MemConfigTableReader` 扩 `col_f32`/`col_i64`/`col_f32_array` + 7 个新 TABLE_CLASS。
  - 9 个 raid 全量 dump: 360 技能 + SkillEffect 链 + AI 范围; BulletShapeTable 722 弹道;
    Boss combat 元数据 9/9 (breaking 8/15/18s, fracture 5s)。
  - 几何重写 132/148 (89% 填充率, 来源 semantic/bullet_shape/ai_range/reverse);
    mechanic skill 覆盖 14/33 → 33/33 (100%)。

  ### 3. ACT 13 面板 entity(Tk) → webview/design 视觉 1:1 对齐
  - **共享组件圆角化**(承接 v4.6.143): `rounded_panel`/`metric_tile`/`section_card`/`status_badge`/
    `action_button`; `_SaoScroll` 自绘暗色滚动条; `sao_entry`/`sao_option_menu`。
  - **字体分割**(`c7a041d`/`3543d84`): 英文走 SAO UI, 中文走 ZHUZIYUAN; 修处理包含 explicit CJK 字体
    label 的遗漏。
  - **Title bar alignment**(`8ea1c64`/`20f1562`): 13 面板统一标题栏 + 居中对齐(替代底部对齐)。
  - **Toolbar 清扫 + status badges**(`8825314`/`cd10b64`/`8e4ae4d`/`6833bcc`/`024261c`/`e9f7719`):
    橙色 × 关闭按钮 + 状态徽标; 简化 cluttered 工具栏; content-level 渲染对齐 webref。
  - **小修**(`43fa724`/`e74a0ff`/`4013948`/`3c75222`): death_recap 重复关闭按钮 / plugin_manager
    重复占位符; SAO 字体应用; webview 详情对齐。

  ### 4. 内存读取加速 (`2658e60` + `51fcc71`)
  - **6 项用户态优化**(`2658e60`): NtReadVirtualMemory 直调; attrs slab read; `read_slab_many`;
    VQ 区域缓存(5s TTL); PrefetchVirtualMemory; ThreadPoolExecutor 并行扫描(≥4 region)。
  - **Cython 全量 rebuild**(`4897c85`): 统一 toolchain metadata。
  - **boss_cast_loop GIL 优化**(`51fcc71`): 三阶段 nogil batch reads 消除 GIL 争用。

  ### 5. Boss Break 系统 (`58472f9` + `7fde65e`)
  - **新 trigger** `stop_breaking_ticking`(attr 453) False→True; 新恢复阶段 `'timed'` 单阶段 ease-out
    quadratic 0%→100%; 时间源 `BreakingContinueTime` from MonsterTable。
  - **双端 1:1**: Tk(`sao_gui_bosshp.py`) + Web(`boss_hp.html`) + Hybrid MEM entity combat reader。
  - **三级查找**(`engines/break_time_lookup.py`): cache 文件 → raid dump 种子 → live MEM fallback;
    cache miss 实时单条读 + 自动回写; bridge `_base_acquired` 后自动后台 build 全 cache。
  - **全表 cache**: 3018 个怪物的 BreakingContinueTime → `assets/break_time_cache.json`。

  ### 6. 其他修复
  - **update server fullpack fix**(`2ad6013`): 自愈 `_load_versions_index` + 新增 fullpack gate 步骤,
    修跳过链路中 fullpack 版本的 bug。
  - **hide_seek 插件修复**(`41f201d`): `_stopped` flag + `_deferred_dismiss` 修 alert 不消失;
    `_CLICK_GLOBAL_CD_S=2.0` + `_verify_color_presence()` 修 step3/4 误判; step1 ESC fallback。
  - **BossRaid 体验修复**(`16f0606` + `56e2e16`): assets 自动导入; ON↔OFF 可切换 + 文案纠正;
    机制卡片删除 fallback messagebox; 技能名解析 fallback; 联动勾选可见(`PANEL_CARD_ALT`);
    reactions 场景下拉补充 imported profiles 的 dungeon_id; `_seed_from_assets` 改用 `resource_path`。
  - **BossRaid profile map_name**(`3563151`): profile 加 `map_name` 字段, 场景跟随 JSON 走。

  ### 已知遗留
  - **selftest baseline 回归**: 8 个 panel-related selftest 失败(`act_combatant_drilldown` /
    `act_death_recap` / `act_graph_timeseries` / `act_skill_drilldown` / `panel_components` /
    `plugin_manager_panel` / `report_export_panel` / `trigger_timer_panel`), 多与 `_badge_frame` 有关,
    疑似 v4.6.143 共享组件重构后未同步 fixture。需补齐后再发起 release。
  - 实战验证: boss break 动画 / driver 加载 / BossRaid 9 个 raid 几何 / hide_seek 步骤循环, 均需进游戏确认。

## v4.6.143: ACT entity(Tk) 面板向 webview/design 视觉对齐 —— 圆角化 + SAO 字体 + 控件统一(全13面板).

  目标: entity(Tk) 面板外观贴近 webview(=design, HTML)。webview 用 [[design]] 设计稿渲染为
  参考(13 张 `.act-shell` 截图), entity 用离屏 Tk 截图谐架(`_panel_preview*.py`)逐面板核对。

  - **共享组件圆角化**(sao_panel_components.py): 新增 canvas 圆角底板 `rounded_panel`(定高/自适应高
    两种), `metric_tile`(KPI 卡)、`section_card`(分组框, pack 代理保持旧调用)、`status_badge`(药丸)、
    `action_button`(`_RoundedButton`, 支持 `configure(command=)` 兼容 dropdown_button) 全部改 canvas
    圆角; 原生 `tk.Scrollbar` 在 Windows 不可着色 → 自绘暗色细圆角滚动条 `_SaoScroll`(内容不溢出自动隐藏);
    新增 `sao_entry` / `sao_option_menu` 扁平暗色输入/下拉。
  - **字体**: KPI 数值走 SAOUI(get_sao_font), 标签/CJK 走 ZhuZi(get_cjk_font), 替换面板内 `('Segoe UI',…)`。
  - **逐面板清扫**(13 个 sao_gui_*): `tk.Scrollbar/OptionMenu/Entry` → `sao_scrollbar/sao_option_menu/sao_entry`;
    Aggregate 旗舰补 3×2 KPI 网格(不溢出)+ OVERVIEW/SOURCE-MIX 右侧栏 + 双语主标题, 去除 COCKPIT 药丸/汇总行/调试行。
  - 离屏谐架实测 13 面板全部正常构建(无破坏)。**注**: Tk 与 HTML 渲染内核不同(字形栅格/抗锯齿/亚像素),
    像素级逐点对齐有物理上限; 本版把可还原的结构/圆角/配色/字体/控件全部对齐, 细节继续逐面板打磨。

## v4.6.142: ACT entity(Tk) 端补 flat 同步 —— 3 面板漏传 flat=True 修复 + 4 面板令牌化(承接 v4.6.141 web).

  v4.6.141 只改了 web 端, entity(Tk) 端这 4 个面板还停在老装饰 chrome(金色条 / ◇ 标记 /
  角块) + 硬编码暗色 → web↔Tk 不再 1:1, entity 端显得"丑的老版本"。根因: 共享
  `_sao_panel_header` / `_sao_panel_body` 有 `flat=True` 才去装饰, 但
  mem_scope / plugin_manager / trigger_timer **漏传** (只有 data_source_health 传了)。

  - **3 面板补 `flat=True`**(mem_scope / plugin_manager / trigger_timer): 去金色装饰条 /
    ◇ 标记 / body 顶部 2px 强调条 / 角标, 与其余 ACT 面板一致。
  - **硬编码色改令牌跟随浅暗**(`_theme_color()` / `_SAO_PANEL_*`): 危险色 `#ff6b82`→danger;
    暗码框 `#07111c`/`#bfe6ff`→body_bg/value_fg; 禁用字 `#6e8190`→label_fg;
    plugin 错误框 `#33161f`/`#ffd8de`→card_bg/danger。
  - **plugin_manager 卡片**由「整框状态色」改为「中性框 + 3px 左侧状态色条」
    (绿=活动 / 青=启用 / 灰=停用), 与 web `.plugin-card` 及 Tk metric_tile 扁平单通道一致。
  - 仅动 gui_modules/ 下 4 个 Tk 文件; py_compile 4 模块通过。共享 sao_panel_ui.py /
    act_panel_theme.css 未改。

## v4.6.141: ACT 面板扁平化补全 —— 4 个落后 web 面板迁成 flat + token, 13 面板全部跟随主题(web↔Tk 1:1).

  落地 Claude Design 交付包 (`f5jEugrT73vywN82htRd7g`, 设计系统本就从本仓 3.0.0 逆出, flat
  是钦定方向) 的"实现设计"。此前 9 个 `act_*.html` 已带 per-page flat 覆盖, 但 4 个面板漏了:

  - **mem_scope.html**: 本就用 `--act-*` 令牌, 仅缺 flat 覆盖块 → 补上同 9 个兄弟一致的
    flat block(去共享 `act_panel_theme.css` 的角标 `::before/::after` + 按钮 clip-path 斜切
    + 辉光), Tk 孪生本就 flat → 恢复 1:1。
  - **plugin_manager.html / trigger_timer_manager.html**: 原 bespoke 暗色 → 加 flat block +
    把共享主题管不到的自定义类(`.pm-tab` / `.panel-card` / `select.card-more` /
    `.message`)改用 `var(--act-*)`。共享主题靠 `!important` 早把通用类拽到令牌, 故暗色面板
    大半已跟随, 只缺 flat + 少数字面亮色字。
  - **data_source_health.html**: **不再 force-dark**。原先强制暗底是因有字面亮色字
    (`pre` / `.diag-item.error/.warn`)在浅底看不见; 现把这几处改令牌(`pre` 用 gold 对齐
    Tk self-state `fg=_SAO_PANEL_GOLD`), 删暗底强制 → 浅暗两套都可读。
  - **零外溢**: 只动 4 个 web html 的 per-page `<style>`(各面板独立 WebView 文档), 共享
    `act_panel_theme.css` 与 Tk `.py` 一字未改; `git status` 仅这 4 文件。Tk 端早就 flat +
    跟随主题(`sao_panel_ui._apply_sao_panel_palette` 切色板, tk.Frame 1px 无角标)。

## v4.6.140: 修复切换场景不更新服务器 / 抓不到 full sync / 地图横幅不弹(net/packet_capture.py).

  抓包链路严重回归 + 切换瞬间 full-sync 丢失(直连/加速器都受影响):

  - **场景服务器切换死代码(根因)**: 诊断提交 f0713a7 给 TcpReassembler 加 raw-cap
    诊断时, 把「场景服务器切换」整段代码误嵌进了 `elif _RAW_CAP_DUMP_ENABLED and
    (C3SB_SHORT in payload)` 诊断分支。生产态(诊断开关 False)下该 elif 永不进入 →
    切场景时严格识别命中也永不切换 _server_addr、不触发 on_server_change、新场景服
    所有帧被 `addr != _server_addr` 丢弃 → cap_game 冻结、DPS=0、切换 title 不弹。
    修复: 把切换体移回 `if self._identify_strict(payload):` 命中分支内(旧服 ≥3s
    无数据后执行), 诊断 elif 退回成纯打印。
  - **切换瞬间 full-sync 丢失**: 新场景服在 v2.3.15「旧服 3s 保护」窗口里一次性发完
    SyncContainerData(自身全量: 名字/等级/装备) + 场景事件(地图横幅来源) + 内存桥
    full-sync 触发, 这些段在正式 switch 前被 `addr != _server_addr` 丢弃, 只进 24
    槽回放缓冲, 繁忙主城里和 ~24 玩家 Appear 包交织被挤掉(实测只回放 1~5 个)。
    回放缓冲 _RECENT_PKT_LIMIT 24→512, 让整段 full sync 存活到 switch 后由
    _replay_recent_for_addr 补喂 → 角色名/等级、地图横幅、NPC 全恢复。
  - 实测验证: 冒烟测试(切换触发 + on_server_change + 旧服活跃不误切)PASS; 直连
    live 日志确认两次 ⚡ 场景服务器切换 + cap_game 持续增长 + 玩家/怪物/横幅回来。

## v4.6.139: mem_scope web 补「复制快照 / 刷新」按钮(双端 parity; web 此前缺失).

  渲染不全 / 功能缺失(web mem_scope 工具栏比 Tk 少两个动作):

  - **复制快照**: Tk `copy_json`(sao_gui_mem_scope.py:191)有工具栏「复制 Copy」把整张
    扫描快照 `_last_status` 转 JSON 进剪贴板; web 此前**只有行内「复制」(复制单个地址)**,
    无法复制整张快照。补 `MemScope.copy()`→`doCopy()`(复用既有 `lastData` 快照 +
    `copyText` 剪贴板助手), 加「复制」按钮。
  - **刷新**: Tk 工具栏有「刷新 Refresh」; web 虽会自动轮询(搜索时)/dtype 改/加载时
    刷新, 但无手动刷新钮。补「刷新」钮调既有 `MemScope.refresh()`。
  - 两钮均 web/mem_scope.html, 复用既有函数/数据, 不删功能, 恢复与 Tk 1:1。

## v4.6.138: ACT 面板查询交互一致性补齐(web; 与 Tk 双端及兄弟面板对齐).

  用户交互友好性(交叉验证 6 个 ACT 面板的双端 parity 后, 落两处确认缺口):

  - `web/act_graph_timeseries.html`: 查询框此前只能"按 Enter 过滤"且**无可见过滤
    按钮**——而它的 Tk 孪生(`sao_gui_graph_timeseries` 第 267 行 `过滤 Filter`)及所有
    兄弟下钻面板(combatant/skill drilldown web 都已有 Filter 按钮)都有。补一个
    `<button>Filter</button>` 调既有 `GraphTimeseries.filter()`, 保留 Enter 快捷。
  - `web/act_death_recap.html`: entity / window 输入框此前**无 Enter 处理**(只能点
    刷新), 与兄弟面板(combatant/skill/graph 输入框都有 Enter→动作)不一致。给两个
    输入框加 `onkeydown Enter → DeathRecap.refresh()`(refresh 本就读这两个输入值),
    增加"回车即应用"友好性, 不删任何功能。
  - 交叉验证证伪(未动, 避免误改): combatant/skill drilldown web 已有 Filter 按钮;
    action_log web 已有 搜索/过滤 双按钮; aggregate/timeline web 输入即时过滤(input
    监听防抖); Tk combatant 的 Focus 按钮在 toolbar 循环外(第 220 行 tk.Button)真实存在。

## v4.6.137: AutoKey 档案列表按钮聚合(双端 1:1; 5 按钮→2 主+「更多 ▾」, 不删功能).

  按钮聚合(用户明列需求): AutoKey 档案卡片此前 Tk 单行 5 个按钮
  (OPEN/ON/COPY/EXP/DEL, 命中"同排 >4-5 按钮"聚合阈值), web 则 Edit/Activate 一行 +
  Copy/Export/Delete 一行。把**同类的「档案文件管理」动作(复制/导出/删除)**收进
  「更多 ▾」下拉, 主动作(编辑/启用 = OPEN/ON)保留在外。两端结构归一为
  [主动作 ×2] + [更多 ▾]:

  - `gui_modules/sao_gui_profile_editors.py` `AutoKeyDetailPanel`: 新增
    `_make_profile_more_button`, 弹 `tk.Menu`(复制/导出/删除), 与插件管理器
    `_more_button` 同款; 卡片单行从 5 按钮降为 OPEN/ON/更多▾。
  - `web/menu.html` `_akRenderLocal`: 头部动作行加 `<select class="auto-key-btn
    card-more">更多 ▾</select>`(复制/导出/删除), 删除底部三按钮行; 新增
    `_akCardAction` 派发器 + `.card-more` 样式。与插件管理器 web `card-more` 同款。
  - 聚合**不删功能**(5 动作全保留可达, 删除仍走 `_akDeleteProfile` 二次确认);
    BossRaid 档案列表仅 3 按钮(OPEN/ON/COPY, 未过阈值)故不动, 保持其两端现状。

## v4.6.136: DPS 面板每帧热路径去重(滚动计数 + 命中 FX 衰减), 像素零变化.

  性能(战斗热路径, 非 idle): `sao_gui_dps.py` 的 `_draw_row`/`_draw_list_frame`
  每帧每行执行, 战斗动画期间逐 tick submit, 两处冗余计算每帧重复:

  - **滚动条计数重建**: `_draw_list_frame` 已在 2179 行算出 `view_rows`
    (`_build_view_rows()` = `list()`+`sort()`+推导, O(N log N)), 但
    `_draw_scroll_affordance` 又在 2391 行 `len(self._build_view_rows())`
    把整张排序视图**重建一遍只为取行数**(玩家数 > MAX_ROWS、有滚动时每帧触发)。
    改为把 `len(view_rows)` 作参数传入(该方法仅一个调用点), 删除二次重建。
  - **命中 FX 衰减重算**: `_draw_row` 两个 `if row['fx_tier']` 块(轮廓描边 +
    名字文字阴影)各自 `time.time()` + 同一档 `_HIT_FX_TIERS`/`fx_start` 算同一个
    衰减强度。命中特效期间每行每帧多一次 syscall + 重复浮点。合并为每行算一次
    `fx_int` 复用。两改均**像素逐位一致**, 不动行为/不砍功能。

## v4.6.135: 插件卸载加二次确认(双端; 防误删不可撤销的已装插件).

  交互友好性: 卸载插件会移除已安装插件且不可撤销, 但两端此前都**点一下即卸载、
  无二次确认**(其他不可逆动作如档案删除两端都已有确认 → 卸载是漏网)。补:

  - `web/plugin_manager.html` `uninstall`: 前置 `window.confirm`(与 menu.html
    档案删除 `_akDeleteProfile`/`_brDeleteProfile` 的 window.confirm 同款)。
  - `gui_modules/sao_gui_plugin_manager.py` `_uninstall`: 前置 `SAODialog.ask`
    二次确认(与 BossRaid/AutoKey 档案删除同款), 确认后走新 `_uninstall_confirmed`
    执行; 对话框不可用(无窗口/headless)时回退直接执行不挡。

  双端均加确认, parity 不破; 不砍功能(仅加安全门)。基线 81/81 绿;
  plugin_manager_panel 3/3。

## v4.6.134: Web BossRaid 时间线条件补比较符控件(双端 1:1; web 之前只能 HP>=).

  enum/控件 parity 脉补漏: BossRaid 时间线条件(timeline condition)的 hp_pct 类型
  需要比较符。Tk 时间线编辑器有 Cmp 下拉(`COMPARATORS = >=/<=/>/</==`,
  profile_editors.py), engine `_eval_comparator`(boss_raid_engine.py) 按
  `cond.comparator or '>='` 求值; 但 **web 时间线条件只有 类型 + 数值, 无比较符
  控件**——web 用户只能建 `HP >= X` 的时间线条件, 无法建 `<=`/`>`/`<`/`==`。

  修(仅 `web/menu.html`): hp_pct 时间线条件渲染处补比较符 `<select>`(5 种,
  读 `cond.comparator || '>='`), 新增 `_brDraftTlCondComparator` 写
  `condition.comparator`(归一到 5 种, 其余回退 '>=')。web 之前已保留(不剥)既有
  comparator(`_brDraftTlCondType/Value` 只改 type/value), 故非损坏只是编辑残缺;
  现 web 可建/改全部 5 种比较符, 与 Tk/engine 1:1。menu.html 内联脚本 node 检 0 错。

  基线 81/81 绿。

## v4.6.133: Tk BossRaid 阶段触发补 buff_event + Tk AutoKey 档案列表导出按钮(双端 1:1 收尾).

  清 enum/列表 parity queue 两小项:

  1) **Tk 阶段触发补 `buff_event`** — web/engine(`boss_raid_engine.py` L459)有,
    Tk `BossRaidDetailPanel.TRIGGER_TYPES` 漏(batch 267 的反向缺口)。加进
    TRIGGER_TYPES(在 breaking 后, 对齐 engine 顺序); 其 value 走 float 保存,
    engine 端 `_coerce_int` 接收(buff_event value=event_type int), 无碍。
    现 BossRaid 阶段触发两端 15 类型全 1:1。

  2) **Tk AutoKey 档案列表导出按钮** — web AutoKey 档案卡有「导出」
    (`_akExportProfile(id)`), Tk 档案列表(batch 266 后 OPEN/ON/COPY/DEL)无,
    导出只在打开后的编辑器里。档案卡加 EXP 按钮(COPY 与 DEL 之间, 同 web 顺序)
    + 新 `_export_profile(pid)`(用 `find_auto_key_profile` 按 id 取存档版本 →
    normalize → export, 不必先打开)。Tk 列表现 OPEN/ON/COPY/EXP/DEL, 与 web
    五按钮 1:1。

  基线 81/81 绿。

## v4.6.132: Web BossRaid 阶段触发补齐 5 个机制/技能类型 + 字符串触发值修复(双端 1:1).

  延续 batch 266 的 allowlist/类型 parity 修, 这次是 BossRaid 阶段触发:
  - Tk 阶段编辑器用全 14 种 `TRIGGER_TYPES`(profile_editors.py), engine
    (`boss_raid_engine.py` normalize_phase) 也支持; 但 web 阶段触发下拉只有 10 种,
    **缺 `boss_mechanic`/`boss_mechanic_family`/`boss_skill`/`boss_mechanic_skill`/
    `ultimate_skill` 5 种**——web 端无法新建/正确显示这些阶段触发(含这些触发的
    profile 在 web 打开时下拉错显为「手动」)。
  - 且这 5 类的 `value` 是**字符串**(机制 key / 技能 id 文本, 见 engine
    normalize_phase `_string(value)` 分支), 而 web 触发值框是 `type=number`,
    会把字符串触发值清成数字。
  - 修(仅 `web/menu.html`): 下拉补 5 个 `<option>`; 触发值框按
    `stringValueTriggers` 集判定渲染 text 框(字符串类)还是 number 框;
    `_brDraftTriggerValue` 对这 5 类存原始字符串(不 `_clampNum`)。load 是
    JSON 深拷无归一, 字符串往返不丢。web 现可建/改这 5 类阶段触发, 与 Tk/engine 1:1。

  注: Tk 阶段触发缺 web/engine 有的 `buff_event`(反向小缺口), 留后续。基线 81/81 绿。

## v4.6.131: Web AutoKey 4 类触发条件不再被静默改写(防数据损坏) + Tk 档案列表删除按钮.

  1) **Web 条件类型数据损坏修复(重要)** — `web/menu.html` `_akConditionType` 的
    allowed 集只含 7 种条件, 其余一律 `return 'hp_pct_gte'`; 而 entity/engine
    支持 11 种(`auto_key_engine.py` + `AutoKeyDetailPanel.CONDITION_TYPES`)。
    后果: 在 Tk 建的含 `dungeon_is`/`last_skill_is`/`boss_mechanic_is`/
    `boss_mechanic_family_is` 条件的 profile, 一旦在 web 菜单打开(normalize 即
    强制改型)再保存, 这些条件被**静默改写成 hp_pct_gte**(连同其 value 语义丢失)。
    现把这 4 种(均 string `value`, 同 profession_is/player_name_is)补进
    `_akConditionType` allowlist + `_akDefaultCondition` + `_akNormalizeCondition`
    文本分支 + `_akConditionTypeOptions` 下拉 + `_akRenderConditionFields` 文本框,
    web 端不再损坏且可正常建/改这 4 类条件, 与 Tk/engine 1:1。
    `web_menu_layout_selftest` 的 allowlist 快照同步更新。

  2) **Tk AutoKey 档案列表删除按钮** — web 档案卡有「删除」, Tk 档案列表只有
    OPEN/ON/COPY、删除藏在打开后的编辑器工具栏里。`sao_gui_profile_editors.py`
    `AutoKeyDetailPanel` 档案卡加 DEL 按钮(kind=danger), 新 `_delete_profile(pid)`
    复用 `_confirm_delete_profile`(SAODialog 二次确认), 不必先打开编辑器。

  基线 81/81 绿; combat_preparse 15/15。

## v4.6.130: Entity 联动 Debug 开关 + DPS 空闲隐藏秒数控件补齐(主菜单设置 parity).

  延续 batch 264, 补两处 web 菜单有控件、entity 端缺的设置:

  1) **联动 Debug Log 开关** — web menu 联动区有 Debug 复选(set_linkage_debug →
    `debug_log`)。`engines/boss_autokey_linkage.py` `build_boss_reactions_state`
    新增下发 `debug_log`; `sao_gui_bossraid.py` `_render_reactions` 的「联动」行
    在 ON/OFF 与全局CD 之间加 Debug 复选, 复用 batch 264 的 `_set_linkage` 回调。
    至此 entity 联动区与 web 三控件(ON/OFF + Debug + 全局CD)1:1。

  2) **DPS 空闲隐藏秒数** — web menu DPS 区有数字框(`dps_fade_timeout_s`,
    0-120s, 控制战后多久自动隐藏 DPS 面板); Tk 此前**读**该设置
    (`_combat_damage_timeout_s` 实时读)但无控件。Tk 菜单是命令列表(无数字框),
    故在 DPS 开关下方加循环命令「DPS空闲隐藏: Xs」, 点一下切下一预设档
    (0=常驻/3/5/8/10/15/30/60), 写 `dps_fade_timeout_s` + 刷菜单; 消费者实时读
    无需重建。新增 `_cycle_dps_fade_timeout`/`_dps_fade_timeout_label` 等
    (dps_theme_mixin)。

  基线 81/81 绿; boss_reactions_linkage 19/19。

## v4.6.129: Entity Boss↔AutoKey 联动总开关 + 全局CD 控件补齐(双端 1:1).

  web 主菜单一直有「BOSS ↔ AUTOKEY LINKAGE」区(联动 ON/OFF + Debug + 全局CD +
  映射表), entity/Tk 端的 Boss 反应编辑器此前只能编辑单条反应映射, **没有联动
  总开关和全局CD 控件**——Tk 用户无法全局开关联动或调全局冷却(只能逐条改),
  尽管引擎两端都读这些值(`load_linkage_config` 的 `enabled`/`global_cooldown_s`
  门控所有联动)。典型「web 有 entity 漏」的半成品。

  - `gui_modules/sao_gui_bossraid.py` `_render_reactions` 顶部新增「联动」行:
    ON/OFF 复选 + 全局CD(s) 输入框, 读 `st.enabled`/`st.global_cooldown_s`
    (build_boss_reactions_state 早已下发), 写经新 `set_linkage_fn` 回调; 保存失败
    弹 `_mech_toast`。控件以 `getattr(self,'_set_linkage',None)` 守卫, 未接线则
    不渲染(不崩)。
  - 回调链: `_init_reactions_state` + `BossRaidPanel`/`BossRaidDetailPanel`
    (sao_gui_profile_editors) 构造器加 `set_linkage_fn`; actions_mixin 两处构造
    都接 `_set_linkage_field`(load→set→save_linkage_config, global_cooldown_s
    钳 0-60, 与 web `set_linkage_*` 一致)。

  注: web 的 Debug Log 开关本轮未补(build_boss_reactions_state 未下发 debug_log,
  且属诊断项), 留后续。映射编辑两端早已有。基线 81/81 绿。

## v4.6.128: WebView 更新错误弹窗与 Tk 对齐(不再吓人, 双端 1:1).

  `sao_webview.py` `_build_update_popup_payload` 的 `state=='error'` 分支改为
  返回 None, 与 entity/Tk 端 `sao_gui_status_updater_mixin` 一致。

  背景: 自动更新检查在 DNS/网络预热期可能瞬时失败。Tk 端早有注释明确"这类
  错误留在更新面板与手动检查反馈里, 不弹吓人的 alert", 但 web 端一直照弹一个
  「UPDATE ERROR」identity alert(还特意优先于其他 alert)——同一个 UX 决策只在
  Tk 落地、web 漏做(典型半成品)。现 web 也不再弹错误弹窗; 错误状态仍随 snapshot
  推到 menu 更新面板(sao-updater-meta/badge)显示, 不丢信息; 手动检查失败仍有
  反馈。available/downloading/ready 弹窗不变。

  连带: `_maybe_show_update_popup` 移除已失效的 error 优先分支(error 不再产生
  payload), 简化 `_identity_alert_visible` 守卫; 清理同方法内不再使用的 error 局部。

## v4.6.127: Tk BossRaid 机制写操作失败反馈 + 反应保存成功反馈(双端 1:1).

  诚实反馈脉最后一处(bossraid), 补 `gui_modules/sao_gui_bossraid.py` 两处与
  web `raid_editor.html` 不一致的反馈:

  1) **机制写操作失败被吞** — web `_mechReply` 在 save_mech/delete_mech 失败时弹
    `_mechNotice('操作失败')`, Tk `_mech_call` 此前 `except: return None` 静默吞,
    4 处写调用(机制启停 L826 / 新建 L895 / 删除 L939 / 详情保存 L1523)失败都无
    提示。`_mech_call` 加 `_toast_error` 仅写操作传 True, 失败弹「操作失败: {exc}」。
    读操作(load/test/search)不传保持静默。

  2) **反应保存成功无反馈** — web `saveReaction` 成功弹「Boss 反应已保存」, Tk
    `_save_reaction_row` 此前只在接口缺失/异常时弹错误、成功完全静默(只重渲)。
    补 else 成功分支弹「Boss 反应已保存」。`upsert_mapping` 成功返回 config、
    失败抛异常, 故 异常=失败 / 无异常=成功 判定成立。

  注: bossraid 删除早有二次确认(SAODialog 不可撤销提示); 机制创建/保存成功两端
    均靠重渲(无成功 toast)对称, 不动。基线 81/81 绿(含并发方 boss 引擎改动)。

## v4.6.126: Tk 插件卡片补 LOADED 标记(双端 1:1 渲染补全).

  `gui_modules/sao_gui_plugin_manager.py` `_format_meta` 的 flags 行补 `LOADED`
  标记(当 `plugin.get('loaded')`)。web 卡片一直有独立 `LOADED` 徽章
  (`plugin_manager.html:521 if(plugin.loaded)`), 表示模块已载入内存——区别于
  状态牌的 active/enabled。Tk 此前完全不渲染 loaded → 用户无法区分「已启用但
  加载失败」(enabled 但 loaded=false)与「已加载未激活」(loaded=true,active=false)。
  补 LOADED 标记后 Tk「ENABLED 牌 + 有/无 LOADED」与 web「LOADED OFF 牌 +
  有/无 LOADED 徽章」表达一致。

  本轮核净未改: 插件卡片 logs/last_error/failures/event_failures/订阅/版本/
  entry 均双端已渲染; 更多▾菜单(reload/pin/uninstall, uninstall 按 user_installed
  门控)与 web select 1:1; data_source_health/mem_scope/offline_import 等剩余面板
  反馈与渲染对称(详见 batch258 handoff)。基线 81/81 绿。

## v4.6.125: Tk 插件管理器/触发器面板成功反馈补齐(双端 1:1).

  延续 v4.6.124 的诚实反馈线: web 这两个面板的 enable/disable/reload 等动作
  一直弹成功 toast(`id enabled`/`触发器已重载`...), Tk 侧此前只在 `ok is False`
  时写 `_status_var`、成功完全静默 → 用户点完不知是否生效。补成功反馈, 文案与
  web 1:1:

  1) `gui_modules/sao_gui_plugin_manager.py`: `_reload_all`(插件已重载)/
    `_reload`({id} reloaded)/`_enable`({id} enabled)/`_disable`({id} disabled)/
    `_pin`({id} pinned|unpinned) 五处补 else 成功分支。uninstall/import 早已
    双态反馈, 不动。

  2) `gui_modules/sao_gui_trigger_timer_manager.py`: `_reload`(触发器已重载)/
    `_enable`({id} enabled)/`_disable`({id} disabled) 三处补成功分支。_test
    早已有反馈, 不动。

  `refresh()` 不写 `_status_var`(经核), 成功文案不被刷新覆盖。基线 81/81 绿,
  plugin_manager_panel 3/3 + trigger_timer_panel 5/5。

## v4.6.124: Tk AutoKey 保存诚实反馈补齐(双端 1:1).

  `gui_modules/sao_gui_actions_mixin.py` `_save_autokey_burst_actions` 补齐两处
  与 web `autokey_editor.saveActions()` 不一致的反馈缺口:

  1) **成功无反馈** — web 保存成功弹「已保存 N 条动作」, Tk 此前只在引擎应用
    失败时提示、成功完全静默 → 用户点 SAVE 后不知是否保存。现成功弹
    「已保存 N 条动作」(2s), 与 web 1:1。

  2) **持久化失败被吞** — web 保存异常弹「保存失败」, Tk 此前 `_set_setting`
    抛异常会被 `_save_actions` 的 `except Exception: pass` 静默吞掉、无任何
    提示。现 `_set_setting` 包 try, 失败弹「保存失败: {exc}」(4s) 后返回。

  引擎应用失败的「已保存, 但引擎应用失败」分支保持不变(比 web 更细)。本修
  补全 v4.6.88(web 全反馈)/v4.6.89(Tk 仅错误反馈)未做完的 Tk 成功反馈。
  回调仅 SAVE 按钮触发(非加载/程序化), 不会误弹。基线 81/81 绿。

## v4.6.123: ACT 触发器事件浅拷贝替 deepcopy + BossHP 死读字段清理.

  1) `engines/act_trigger_engine.py` 的 `evaluate()` 存档与 `snapshot()` 返回
    把 `copy.deepcopy(event)` 改为 `dict(event)` — event 是 `_build_event`
    产出的扁平 dict(值全为 uuid.hex/str/float/int 等不可变标量, 无嵌套容器),
    浅拷贝与 deepcopy 语义完全等价但省去 memo/递归开销; `snapshot()` 由
    触发器/计时器面板按轮询调用(可达数 Hz × 至多 20 事件), 是真实重复分配。
    规则(可嵌套)的 `add_rule` deepcopy 故意不动。trigger 自检 5/5 绿。

  2) `gui_modules/sao_gui_bosshp.py` 删除死读字段 `stage_text` — Tk 侧
    `update()` 把 `data.get('stage_text')` 读进 `self._stage_text` 并初始化,
    但全工程无任何生产者下发该键(恒为 ''), 渲染管线也从不引用它; Web 侧的
    `#stage-text` 徽章另由 `breaking_stage` 派生(且因 breaking_stage 上限 1,
    `Math.max(1,stage)` 实际恒显 "P1" 的装饰), 不值得在 Tk 复刻该恒定装饰。
    一并修正 docstring 的 `data` 键清单。纯死代码移除, 不动任何功能。

## v4.6.122: 渲染 lane 外层守卫防静默死亡 + compose 错误日志限频.

  1) `render/overlay_render_worker.py` `_RenderLane._loop` 加外层 try
    守卫 — 旧实现仅 compose 工作有 try, 而取锁/`cond.wait`/取 job 抛异常
    会让该 lane 线程静默退出 → 对应面板永久停帧、无重启、无日志(GUI 下
    console 也看不到)。现捕获 + 60s 限频日志 + 0.05s 退避, 线程存活继续。

  2) 同文件单帧 compose 失败的 `print` 改 60s 限频 — 旧实现每个失败帧都
    print 一次, compose_fn 持续失败时按帧率(可达 60Hz)刷屏; 现每 60s 窗口
    首帧记一次(含 lane 号), 既给停层信号又不淹没日志。冒烟: lane 在抛异常
    compose 后存活并继续产帧, 两类日志均 60s 限频。

## v4.6.121: 技能识别 HSV 去重转换 + GameState 无订阅者跳快照.

  1) `vision/skill_recognition.py` 消除每帧冗余 BGR→HSV 转换 —
    `_compare_to_baseline` 旧实现对同一当前帧再转一次 HSV (analyze 内
    `_measure_slot` 已转), 又对恒定的基线图每帧重转; 现 analyze 单次
    `_prepare_hsv(img)` 复用给 `_measure_from_hsv` 与 compare, 基线 HSV
    按 (h,w) 缓存在 slot state(reset 随 _slot_cache 清空)。10Hz × 9 槽
    战斗中约省 180 次 cvt_color/秒; compare 输出逐值等价(<1e-9 实测)。

  2) `engines/game_state.py` `update()` 无订阅者(启动/关窗/headless)时
    跳过整份 47 字段状态 copy.copy — 没人消费; listeners 在锁内定格成
    tuple 再于锁外通知, 消除 packet 线程迭代 live list 时 GUI 线程并发
    subscribe/unsubscribe 的跨线程竞态(可致迭代异常/漏调/重复调)。

## v4.6.120: 工具条按钮防挤尾扫(再 6 面板, 该类收口).

  1) v4.6.119 同类尾扫 — mem_scope / data_source_health /
    plugin_manager / graph_timeseries / offline_import /
    trigger_timer_manager 的工具条统一改按钮先 pack、summary 标签
    后 pack。至此全部 13 个带 summary 工具条的 ACT 面板 + commander
    成员卡(v4.6.115)该 bug 类收口。

## v4.6.119: ACT 面板工具条按钮防挤横扫(7 面板).

  1) Tk pack 挤出坑横扫 — 工具条 summary 标签先 pack(LEFT) 时,
    长文本会把后 pack 的右侧按钮挤出窗口(pack 后包者只分剩余空间);
    combatant/skill drilldown 与 death_recap 的 summary 内插未截断
    玩家/技能/实体名, 实际可触发; aggregate/action_log/timeline_vcr/
    report_export 窄窗下同理。7 面板统一改按钮先 pack、标签后 pack
    (长文本自然裁切, 按钮永不丢); web 端 .value 卡片已有 ellipsis,
    双端行为等价。与 v4.6.115 commander 成员卡同 bug 类。

## v4.6.118: Tk 面板滚动保留收尾(剩余 7 面板, 全覆盖).

  1) keep_canvas_scroll 接线剩余 7 面板: death_recap / mem_scope /
    offline_import / trigger_timer_manager / data_source_health(左列表,
    右诊断区无滚动) / report_export(左预览区, 右历史区无滚动) /
    plugin_manager(管理列表 + 分离插件窗口热键区两处)。

  2) 至此 11 个带滚动条的 Tk 面板全部具备重建滚动保留,
    与 web 端 setContentHtml(preserveScroll) 全面对偶。

## v4.6.117: Tk 面板滚动保留模式化(第一批 4 面板).

  1) `gui_modules/sao_panel_components.py` 新共享组件
    `keep_canvas_scroll(canvas, inner)` — 全量重建前记滚动分数,
    after_idle 在重建完成后还原(单插入点覆盖渲染函数全部 return
    路径); web setContentHtml(preserveScroll) 的 Tk 对偶。

  2) 第一批接线 4 个实时刷新面板: action_log / timeline_vcr /
    skill_drilldown / combatant_drilldown — 此前战斗中每次签名刷新
    滚动都跳回顶部(11 个带滚动条的 Tk 面板全都没还原, aggregate
    在 v4.6.116 单独修过); 其余 7 面板下批接线。

## v4.6.116: aggregate raw 视图解析名键修正 + Tk 滚动位置保留.

  1) aggregate 展开 raw 事件视图(双端)补 'skill'/'monster' 键 —
    行级解析名由 _action_log_group_metadata 写在这两个键下,
    双端键列表此前只引用不存在的行级 'skill_name'/'monster_name',
    解析出的人话技能/怪物名从未在 raw 视图出现(payload 自带时除外);
    Tk/web 键列表同步对齐 1:1。

  2) `gui_modules/sao_gui_act_aggregate.py` 重建时保留滚动位置 —
    web 端 setContentHtml 早就 preserveScroll, Tk 端每次签名变化
    全量重建后滚动跳回顶部; 现重建前记 canvas.yview 分数、重建后
    update_idletasks + scrollregion + yview_moveto 还原。

## v4.6.115: report_export 指标卡暗色残留 + Tk commander 长名防挤防裁.

  1) `gui_modules/sao_gui_report_export.py` 指标卡(Damage/DPS/Heal/
    Duration)修复 — 与 v4.6.114 action_log 同 bug 类: 硬编码 '#07111c'
    近黑底配浅底灰 LABEL_FG; 改主题常量。两类对比度残留(深底浅灰字/
    浅底白字)已 grep 全量排查穷尽, plugin_manager/trigger_timer/
    aggregate/skill_drilldown 的暗盒是亮字自洽搭配(刻意)不动。

  2) `gui_modules/sao_gui_commander.py` 成员卡长名修复 — 名字 Label
    先 pack(LEFT,expand) 会把后 pack 的职业徽章/队长星整个挤出卡片
    (pack 后包者只分剩余空间); 徽章改先 pack, 名字按像素预算
    `_tk_ellipsize`(tkfont.measure)省略号截断, 与 web 端 batch 241
    的 .member-name ellipsis 对偶。

## v4.6.114: action_log 明细盒暗色残留修复 + Tk 裸按钮统一.

  1) `gui_modules/sao_gui_action_log.py` 组展开明细盒修复 — 硬编码
    '#081521' 近黑底是 ACT 扁平化(浅色系)前的残留, 浅色调
    LABEL/VALUE_FG 灰字打上去对比度严重不足几乎不可读; 改主题常量
    (HEADER_BG 嵌套盒+BORDER 边线)浅/深主题都自动适配; 明细列宽
    同步对齐主行 (10,12,34,14)。

  2) Tk 裸默认按钮统一 — combatant drilldown 'Focus' 与 graph
    timeseries '过滤 Filter' 是 Win 灰凸起默认样式, 在扁平浅色
    工具条里突兀且无按压反馈; 分别套用同面板 toolbar 按钮样式 /
    action_button 共享组件。

## v4.6.113: DPS 条形图缓存免拷贝 + 三处 Web 视觉修缮.

  1) `gui_modules/sao_gui_dps.py` `_make_bar` 缓存命中/存入不再 .copy()
    — 唯一调用方只把返回图当 alpha_composite 源(不在其上作画),
    每行每帧一次的 PIL 拷贝纯属浪费。

  2) Web 视觉修缮: commander.html `.member-name` 与 boss_hp.html
    `.additional-unit .name` 补 `min-width:0`(flex 子项默认
    min-width:auto 不收缩, 长中文名不省略号截断而是撑爆行);
    plugin_manager.html `.pm-tab` 补 :hover 态(有 cursor:pointer
    无悬停反馈, 与同页卡片按钮不一致)。

## v4.6.112: 令牌常数时间比较 + action_log live 行新鲜度缓存.

  1) `server/app.py`(两处 legacy 上传令牌) + `update_host/app.py`
    (publish API key) 改 `hmac.compare_digest` 常数时间比较 —
    与签名令牌路径既有姿势一致, 防时序侧信道; 自托管低风险,
    顺手补齐。

  2) `act_platform/runtime.py` `act_action_log_status` live 分支补
    aggregate/timeline 同款 retained 缓存 — 键含取段跨度+query+topic,
    无新事件时跳过 recent_events 深拷贝 + 逐事件 compact + 过滤
    (query 非空时每行一次 json.dumps); is_cursor 当前页逐行全量重写
    自纠正, 共享 dict 缓存安全; history 分支过滤位置随重构内移,
    语义不变。

## v4.6.111: 历史库异常双端透出 + schema 单次 ensure.

  1) 历史库(sqlite/archive)出错不再装 READY — web report_export
    Storage 行出 ERROR(红色, title 带错误详情), Tk 状态行追加
    「历史库异常: …」; 此前 DB 锁死/损坏时双端都显示 READY/OK +
    空历史, 用户无从分辨"没数据"和"库坏了"。

  2) `engines/dps_history.py` `_ensure_sqlite_schema_locked` 每进程
    每路径只跑一次(17 条 DDL + meta 写, 此前读路径每次轮询都重跑,
    含一次 meta 表写入); `_sqlite_connect_locked` 发现文件被删时
    复位守卫, 重建库不受影响。

## v4.6.110: 历史报告 N+1 批取 + SkillFX FBO 缓存上限.

  1) `engines/dps_history.py` `list_sqlite_reports` 对 payload 缺
    entities 的旧 schema 行改单查批取 — 此前每行一查 combatants
    (N+1, 整页 100 行 = 101 查), 现 IN 子句 400/块一次取回按
    encounter_row_id 回填, 排序语义不变(rank ASC, id ASC)。

  2) `render/skillfx_pipeline.py` `_get_fbo` 缓存加 16 键上限 —
    面板反复 resize 时旧尺寸 FBO+纹理不再命中却永驻显存;
    超限按插入序淘汰并释放 color_attachments(与 release()/
    gpu_compositor _LRU_CAP=16 同款姿势)。

## v4.6.109: sqlite_status 写代缓存 + popup HUD 层失败可见.

  1) `engines/dps_history.py` `sqlite_status` 按写代缓存 — 此前
    每次 UI 轮询(history browser / report export)都开新连接 +
    ensure_schema + commit + 6 个 COUNT(*) 全表扫; 本进程是唯一
    写者(sqlite 追加型, 无 DELETE), `_sqlite_write_gen` 在
    `_append_sqlite_locked` 递增即失效; 出错结果不缓存(锁竞争是
    暂态), 命中返回副本防调用方改坏缓存。

  2) `ui_gpu/composer.py` popup HUD 层(括弓/轨道/扫描线)合成失败
    不再无声 — 裸 except 60Hz 静默丢层改 60s 限频日志,
    用户丢视觉特性时日志有迹可循, 帧本身仍优雅降级。

## v4.6.108: buffmon shell 部件缓存 + DPS 文字阴影缓存.

  1) `gui_modules/sao_gui_buffmon.py` `_draw_shell` 拆出 `_shell_parts`
    单槽缓存(键 = 尺寸 + 主题) — base 重建在 buff 倒计时期间高达 10Hz
    (签名含 0.1s 秒数桶), 此前每次都重算高斯模糊 sheen + numpy 渐变 +
    扫描线 + 底部渐变线逐像素 Python 循环; 部件 tile 本地坐标贴回带偏移,
    输出逐像素等价(冒烟: 命中==未命中, 主题键失效正确)。

  2) `gui_modules/sao_gui_dps.py` `_draw_text_shadow` 阴影位图按
    (文本, 字体, 颜色, blur, 间距) 缓存复用(上限 64 防膨胀) —
    阴影与 x/y 无关, 此前每次 compose 都重画字形 + 高斯模糊。

## v4.6.107: 菜单快捷键标签跟随改键 + AutoKey 注入失败可见.

  1) web 菜单 F5/F6 快捷键标签不再硬编码 — `_sync_menu_settings` 下发
    `hotkey_labels`(走 `_resolved_hotkey` 的 {**DEFAULT_HOTKEYS,**saved}
    合并), menu.html restoreMenuSettings 按 data-action 刷新 .shortcut;
    用户在 settings.json 改键后菜单标签不再撒谎(Tk 端本就读配置)。

  2) `engines/auto_key_engine.py` SendInput 返回值不再吞 — 注入失败
    (被拦/0 事件插入)时 last_reason 报 "inject-fail xN"(菜单状态行
    直接可见)并 60s 限频打日志; 此前注入全失败状态仍标 "fired",
    用户以为自动按键在工作而按键根本没到游戏。

## v4.6.106: 数据源/Boss血条端点诚实返回 + Tk 数据源切换失败反馈.

  1) `sao_webview.py` `set_data_source` / `set_boss_bar_mode` 补诚实
    JSON 返回 — menu.html 早就实现了 data.mode 回读 + 失败回滚回调,
    但 Python 端从不返回(shim 把 None 当成功), 引擎重启失败只 print
    就被吞; 现在 ok:False 会触发 menu 既有的回滚 + 红色 alert,
    set_boss_bar_mode 同时补 _sync_menu_settings 推送。

  2) `gui_modules/sao_gui_panels_mixin.py` `_cycle_mem_data_source`
    引擎重启包 try/except — 失败时 Tk 端此前完全静默(设置已写但引擎
    挂了用户毫不知情), 现弹 entity alert 报错, 与 web 端反馈对偶。

## v4.6.105: timeline/graph 状态生产者补 retained 新鲜度缓存.

  1) `act_platform/runtime.py` `act_timeline_status` 补 aggregate 同款
    新鲜度缓存 — 事件列表按 (bus.retained, query, limit) 缓存, 无新事件
    时跳过 recent_events 深拷贝 + 逐事件 compact + query 过滤(每行一次
    json.dumps); cursor/speed/playing 播放态不进缓存每次现读保持实时。

  2) 同文件 `act_graph_timeseries_status` 同款 — series/row_count/
    observed_range 按 (retained, metric, query, topic, range, limit)
    缓存, 无新事件时跳过整套 series 重建(4 序列 × N 点 dict churn);
    encounter_id 等轻字段每次现算。Tk 350ms 轮询 + web 异步轮询
    双端同收益, 战斗后面板空转成本归零。

## v4.6.104: ACT 事件总线热路径降本 + DPS 打击特效去冗余合成.

  1) `act_platform/event_bus.py` `_recent` 改 deque(maxlen) — 旧 list
    切片淘汰在缓冲打满后(默认 200, runtime 可上千)每次 publish 都持锁
    整段拷贝; 留存 deepcopy 移到锁外(发布频次=战斗事件频次),
    `recent_events` 改为锁内浅引用快照+锁外克隆, 大 limit(最高 1000)
    读取不再持锁 deepcopy 上百条。订阅者各自克隆的防变异语义不变。

  2) `gui_modules/sao_gui_dps.py` `_overlay_panel_flash` 删掉无效的
    mask + Image.composite 步骤 — overlay 圆角矩形外像素本就全透明,
    再套同形状 mask 结果逐像素相同; 特效期间每帧(60Hz)省 2 块
    全面板分配 + 一次 composite。

## v4.6.103: Tk combatant drilldown 补渲 incoming + buffmon 行渲染降配额.

  1) `gui_modules/sao_gui_combatant_drilldown.py` 侧栏补渲后端一直在发的
    `incoming` 列表(承伤/承疗 mini 行) — web 端 INCOMING/TARGET 区早已渲染,
    Tk 端此前只渲 outgoing, 双端不对等; 字段回退链(name→topic,
    amount→value)与 web 逐字一致, `_signature` 同步纳入 incoming
    防白名单签名陈旧渲染。

  2) `gui_modules/sao_gui_buffmon.py` `_draw_row` 行背景胶囊从
    "整画布 RGBA overlay + 全图 alpha_composite" 改为行 bbox 尺寸
    overlay 平移贴回 — 每帧每行省一块全面板分配(12.5Hz × 行数,
    8 行约省 8-22MB/s 堆翻动), 合成结果逐像素等价。

## v4.6.102: 更新下载写盘限频 + 插件面板轮询隐藏门.

  1) `updater/sao_updater.py` `_set_state` 对 progress-only 更新限频 1s
    写盘 — 下载回调每 64KB 一次, 原先大包下载期间重写 update_state.json
    数百次; 状态/错误等其他字段变化仍即时落盘, UI 进度读内存不受影响。

  2) `web/plugin_manager.html` 1Hz 插件面板轮询加 document.hidden 门,
    窗口隐藏时不再空转 API 往返(与 v4.6.96 menu 分离面板同模式)。

## v4.6.101: Tk DPS 头部 MORE 聚合补齐 Web 端 1:1.

  1) `gui_modules/sao_gui_dps.py` 头部按钮排 EXPORT/RESET 收进「MORE ▾」
    (点开内联展开, 再点收起), 与 38bafcf 的 web/dps.html MORE 菜单对齐;
    按钮 7→6 平铺。

  2) Tk RESET 同步获得二段确认: 第一次点击变「CONFIRM?」, 3 秒内再点才
    真正重置, 超时自动回退 — 消除 Tk 端单击误触清空战斗数据的风险。

## v4.6.100: STA 离线诊断提示 + HP 面板右键菜单失败日志.

  1) `vision/recognition.py` STA 识别转 OFFLINE 时, 若 ROI 尺寸明显偏小
    (宽<150 或 高<8 像素)在日志附「检查游戏窗口是否 16:9 且未被遮挡/裁切」
    提示 — 非 16:9/裁切窗口导致识别失准时用户有排查线索。

  2) `gui_modules/sao_gui_dialogs_mixin.py` HP 面板右键菜单构建/弹出失败
    不再静默吞掉, 控制台打印原因(菜单本身失败无法用 UI 反馈)。

## v4.6.99: 更新下载进度兜底与残损更新包防误报.

  1) `updater/sao_updater.py` 下载进度在服务器不给 Content-Length(chunked)
    时回退用 manifest size 计算, 进度条不再全程停 0%; 下载失败报错带异常
    类型与已收字节数(如「下载失败: TimeoutError: … (已收 28.5 MB)」),
    便于判断是否值得重试。

  2) `has_pending_update()` 改为只读校验 pending.json 可解析、包文件存在
    且 zip 头有效 — 包被磁盘清理/杀软删除/截断后不再误报「更新就绪」,
    避免退出时 update.exe 解包报错; 不动残留文件。

## v4.6.98: Commander 空态区分「无队伍」与「数据源未就绪」.

  1) 数据源(packet bridge)未起来时 Commander 后端 fallback 带
    `status: backend_not_ready`, Tk 与 Web 面板显示「⌛ 数据源未就绪 —
    启动识别后显示队伍」; 数据源正常但确实没队伍时仍显示原「⚔ 暂无队伍
    信息」。冷启动先开面板不再误以为没队伍。

  2) `commander_data_signature`/`commander_panel_signature` 白名单补
    `status` 字段 — 否则「未就绪→真没队伍」转变签名不变, 面板会卡在旧
    空态不重渲。

## v4.6.97: Graph 导出 ok:false 假成功修正 + Commander 推送失败可诊断.

  1) Graph/Timeseries 导出(Tk `gui_modules/sao_gui_graph_timeseries.py` +
    Web `web/act_graph_timeseries.html` 双端 1:1)在后端返回 ok:false 时
    显示 `Export failed: 原因 — fallback JSON copied`, 不再统一报
    「已复制」假成功; 成功路径措辞不变。

  2) `gui_modules/sao_gui_panels_mixin.py` Commander 数据推送异常不再
    `except: pass` 全吞, 60s 去重打印根因, 面板空白时可从控制台定位。

## v4.6.96: 插件卡片按钮聚合 + 菜单分离面板隐藏时停渲.

  1) 插件管理卡片(Web `web/plugin_manager.html` + Tk
    `gui_modules/sao_gui_plugin_manager.py` 双端 1:1)按钮 5→3: 重载/置顶/
    卸载收进「更多 ▾」(Web 用 select 即开即用, Tk 用 tk.Menu 弹出),
    启用/禁用保持直显; 全部原功能保留, 禁用态/条件显示语义不变。

  2) `web/menu.html` 插件分离面板 800ms 轮询在 document.hidden(菜单窗口
    隐藏)时跳过重渲染, 计时器保留待恢复, 不再后台空转。

## v4.6.95: BuffMon compose 签名去 id() + Mem Scope 搜索轮询减半.

  1) `gui_modules/sao_gui_buffmon.py` GPU compose 签名从 `id(base_img)` 改为
    base 重建代数 `_cached_seq`, 消除与 v4.6.94 同类的地址复用碰撞隐患
    (碰撞会让真实新帧被当旧帧跳过合成)。

  2) `web/mem_scope.html` 搜索期间轮询从每 tick 两次 API (`mem_search_status`
    + `get_mem_scope_status`) 减为一次: 任务状态直接读 `get_mem_scope_status`
    返回的 `search` 字段, 渲染行为与停表条件不变。

## v4.6.94: GPU presenter 帧去重改单调序号, 消除 id() 复用碰撞.

  1) `render/gpu_overlay_window.py` BgraPresenter 纹理上传去重不再比较
    bytes 对象 id(): 同尺寸帧缓冲高频释放/重分配时地址复用会与上次上传
    的 id 碰撞, 静默跳过真实新帧(面板显示陈旧帧)。改为帧以
    (bytes, w, h, seq) 单元组交接 + 单调 seq 去重, 渲染线程也不再可能
    读到半更新的 bytes/宽高组合; alpha-only fade tick 跳上传的优化保持。

  2) `gui_modules/sao_gui_fisheye_mixin.py` _FisheyeTexturePresenter 同款
    id() 去重改为单元组快照 + 单调 seq。新增
    `tools/gpu_presenter_dedupe_selftest.py` (13 项)。

## v4.6.93: Boss 反应保存与资料编辑打开失败反馈.

  1) `gui_modules/sao_gui_bossraid.py` 反应行「保存」失败或保存接口缺失
    时 `_mech_toast` 提示, 不再静默假成功; `_mech_toast` 锚点支持反应
    容器(_rx_container)回退, 反应 tab 上也能显示。

  2) `gui_modules/sao_gui_dialogs_mixin.py` 「修改角色资料」对话框打开
    失败时提示「资料编辑: 打开失败」, 不再点击后无任何反应。

## v4.6.92: Tk 配置编辑器导出失败反馈.

  1) `gui_modules/sao_gui_profile_editors.py` AutoKey 编辑器「导出」失败
    (磁盘满/权限/路径异常)不再让 Tk 回调裸抛、界面零反馈, 状态栏显示
    「Export failed: 原因」(红色), 与导入失败反馈同款。

  2) 同文件 BossRaid 编辑器「导出」同样补上失败反馈; 两处导出成功路径
    与导出文件内容均不变。新增 `tools/profile_editor_export_selftest.py`
    静态守卫。

## v4.6.91: 设置保存失败不再静默.

  1) Tk 与 WebView 两套 UI 的 `_set_setting` 在 settings.json 写盘失败
    (磁盘满/权限)时提示「设置保存失败 — 修改重启后会丢失」(Tk entity
    alert / Web SAO.showToast), 持续失败 60s 内只提示一次, 恢复后重新
    armed; 设置在本会话内仍生效, 持久化行为不变。

## v4.6.90: Tk Mem Scope/技能钻取剪贴板失败反馈补齐 Web 端 1:1.

  1) `gui_modules/sao_gui_mem_scope.py` 搜索结果地址复制失败不再静默
    (`except: pass`), 状态栏显示「复制失败: 原因」, 对齐 v4.6.85 的
    `web/mem_scope.html` 地址复制失败反馈。

  2) `gui_modules/sao_gui_skill_drilldown.py` 技能钻取「复制 Copy」剪贴板
    写入失败时状态栏显示 `Copy payload ready, but clipboard copy failed`,
    对齐 v4.6.82 的 `web/act_skill_drilldown.html` 同款提示。

  另: `tools/web_small_panels_numeric_selftest.py` 机制目录绑定断言更新为
    buff/技能双绑定形态(`fn` 动态分发), 与 raid_editor.html 现状一致;
    该自测自 30a955d 起为红, 验证门恢复绿。

## v4.6.89: 钢琴面板关闭卡死态与 Tk 爆发动作引擎应用反馈.

  1) `web/panel.html` 关闭按钮在 `close_panel` 接口缺失时直接提示而不进入
    closing 动画; 调用异常时回退 closing 态并提示, 面板不再卡在隐藏态。

  2) `gui_modules/sao_gui_actions_mixin.py` Tk AutoKey 面板保存爆发动作时,
    引擎 set_burst_actions/invalidate 失败不再静默, 提示「已保存, 但引擎
    应用失败」; 配置持久化行为不变。

## v4.6.88: AutoKey SAVE 与钢琴面板按钮诚实反馈.

  1) `web/autokey_editor.html` SAVE 按钮现在显示保存结果(已保存 N 条/失败原因),
    接口不可用时提示「保存接口不可用」; `AutoKeyEditorAPI.save_autokey_actions`
    改为返回 `{'ok': bool}` JSON, 不再静默吞掉保存异常。

  2) `web/panel.html` 钢琴面板控制按钮(速度/转调/旋律开关等)在接口不可用或
    调用异常时给出提示, 不再点击后静默无反应。

## v4.6.87: Raid 编辑器按钮缺 API/失败反馈.

  1) `web/raid_editor.html` 「从内存导入」与「+ 新建机制」在接口不可用、
    后端返回失败或接口异常时给出明确提示, 不再点击后静默无反馈。

  2) `web/raid_editor.html` 「NEXT PHASE」「RESET」按钮在接口不可用或调用
    异常时给出明确提示, 不再静默忽略点击。

## v4.6.86: Mem Scope 搜索/收敛缺 API 与未搜索反馈.

  1) `web/mem_scope.html` 搜索按钮在 `mem_search` API 不可用时会将状态置为
    `NO API` 并提示 `搜索 API 不可用`, 不再点击后静默无反馈。

  2) `web/mem_scope.html` 收敛按钮在尚未完成搜索时提示 `请先完成一次搜索`,
    在 `mem_narrow` API 不可用时提示 `收敛 API 不可用`。

## v4.6.85: Mem Scope 地址复制与 attr 按钮反馈修正.

  1) `web/mem_scope.html` 搜索结果地址复制现在会等待 clipboard 写入结果,
    async clipboard 失败时回退到 textarea/execCommand, 两条路径都失败时显示复制失败。

  2) `web/mem_scope.html` 的 attr 按钮在 `mem_attr_map` API 不可用时会提示
    `attr_map API 不可用`, 不再点击后静默无反馈。

## v4.6.84: 报告导出/数据源健康剪贴板 fallback 与失败提示.

  1) `web/act_report_export.html` 的报告复制与 Mini-Parse 复制现在会在
    async clipboard 被拒绝时回退到 textarea/execCommand 路径, 两条路径都失败
    时显示明确失败提示, 不再静默无反馈。

  2) `web/data_source_health.html` 健康快照复制同样补齐 async clipboard 拒绝
    后的 fallback 与失败提示, API 查询失败时仍会复制当前本地快照。

## v4.6.83: ACT 行为日志/死亡回放复制失败状态修正.

  1) `web/act_action_log.html` 复制行为日志时现在等待浏览器 clipboard
    写入结果。剪贴板不可用、被拒绝或 payload 为空时会显示失败/空内容提示,
    不再立即弹出 copied 成功提示。

  2) `web/act_death_recap.html` 死亡回放复制同样等待 clipboard 写入结果,
    避免 WebView2 剪贴板失败时误导用户以为已复制。

## v4.6.82: protobuf fallback 诊断恢复 + ACT Web 复制失败不再假报成功.

  1) `packet_parser/helpers.py` `_ensure_pb` 移除早退后的不可达 fallback
    骨架, 在缺少编译 proto 时会明确记录使用内置 mini protobuf decoder,
    避免抓包解析环境缺依赖时无诊断可查。

  2) `web/act_graph_timeseries.html` 与 `web/act_skill_drilldown.html`
    现在区分后端导出/复制 payload 成功与浏览器 clipboard 写入成功。
    剪贴板不可用或拒绝时状态栏显示 copy failed, 不再误导用户以为已复制。

## v4.6.81: 技能槽权威映射接管推断标记 + Web 插件签名回退稳定.

  1) `packet_parser/parser.py` 现在在 ProfessionList 或职业槽位缓存提供权威
    `skill_slot_map` 时同步清除 `_inferred_skill_count`, 避免先由 CD 推断出的
    技能槽在后续场景切换/重推时仍被当作推断缓存清掉或重复合并。

  2) `web/plugin_layer.js` 的 `specSignature` 在遇到不可 JSON 序列化的插件
    render spec 时改用稳定浅层签名, 不再返回 `Date.now()` 导致每次轮询都
    重建 DOM、打断输入焦点或产生额外重排; 同时仅首次输出诊断 warning。

## v4.6.80: Timeline VCR 失败不再假报成功 + overlay 推送错误去重记录.

  1) `gui_modules/sao_gui_timeline_vcr.py` `_apply_result` 现在检查返回的
     `ok` 标志, 失败时状态栏显示后端 message 而不是固定的
     PLAYING/FILTER APPLIED 等成功文案; 与 Web 端 status pill 的
     ERROR 行为对齐 (双 UI 1:1)。

  2) `gui_modules/sao_gui_state_mixin.py` `_push_packet_overlays` 异常
     从"只记第一个错误就永久沉默"改为按不同错误消息去重各记一次
     (上限 20 条), 后续不同根因不再被吞。

## v4.6.79: Web DPS 面板脏签名守卫 + 抓包消费错误计数可观测.

  1) `web/dps.html` 新增 `_setHtml` 脏签名守卫并收口全部 7 个 innerHTML
     写点 (列表/详情卡/技能行/空态)。推送数据未变时跳过整块 DOM 重建,
     消除空闲期/重复快照下的重排开销, 命中特效类也不再被无关重建掐断。

  2) `net/packet_capture.py` 消费循环异常新增 `parse_consumer_errors`
     计数, 经 `stats` 自动流入 data source health 面板与 Bridge 5s 诊断行
     (`consume_err=`), 帧处理失败从只进日志变为用户可见。

## v4.6.78: 更新失败回滚真实生效 + Mem Scope 操作失败反馈.

  1) `update_apply.py` 修复更新中途失败时回滚空转的问题。
     `_apply_zip_package` 原先把备份/新建清单存在局部变量、仅在成功时返回,
     中途抛异常 (如目标文件被锁) 时调用方拿空列表回滚 = 实际不回滚、应用半更新。
     现在清单由调用方传入共享, 部分进度在异常后仍可回滚;
     已用临时 zip + 强制失败用例验证部分备份记录保留。

  2) `web/mem_scope.html` 搜索/收敛/attr_map 读取/状态轮询的 Promise
     catch 不再静默吞错, 失败时 toast 提示, 轮询停止时告知用户。

## v4.6.77: 内存源依赖透传失败可见化 + 插件卸载清除启用残留.

  1) `net/packet_bridge.py` `set_dps_tracker`/`set_boss_raid_engine` 向 mem source
     透传失败时不再静默吞错, 输出 `[Bridge] ... passthrough failed` 警告;
     该透传链断裂曾导致 DPS 面板缺 MEM 数据 (历史 7afd689), 现在可被日志定位。

  2) `act_platform/plugins.py` 新增 `clear_persisted_enabled`,
     `act_plugin_uninstall` 卸载成功后清除 `act_plugin_enabled` 持久化残留,
     避免之后导入同名插件复用旧启用状态; install selftest 10 绿。

## v4.6.76: DPS 技能语义匹配热路径提速 + 设置保存失败可见化.

  1) `engines/dps_tracker.py` 重构 `_semantic_base_skill_id` 的候选匹配。
     四张名字表的 id 并集提为模块级缓存 (按底表对象身份失效, 底表重载自动重建),
     候选搜索从全表 O(n) 子串扫描改为枚举 sid 的 4+ 位子串查集合 (等价语义, 已对拍验证)。
     消除战斗开局大量新技能 id 首见时的卡顿。

  2) `config.py` 设置保存 fallback 直写失败时不再静默吞错,
     输出 `[Settings] Fallback write also failed` 日志, 用户可知设置未落盘。

## v4.6.75: Web menu sound/import picker bridge fallback 修复.

  1) `web/menu.html` 修复声音播放 bridge 的 Promise reject 兜底。
     `playSound` 现在通过静默 bridge helper 调用 `play_sound`,
     保持音效失败不打扰用户, 同时避免异步失败冒成未处理错误。

  2) `web/menu.html` 修复 AutoKey 导入选择器缺少失败反馈的问题。
     `_akImportProfile` 现在会在 API 缺失、同步异常或 Promise reject 时显示 `AUTO KEYS` 错误,
     成功返回时仍按原逻辑打开文件选择器。

## v4.6.74: Web menu AutoKey/BossRaid API helper fallback 修复.

  1) `web/menu.html` 修复 AutoKey 通用 API helper 的同步异常与非 Promise 返回处理。
     `_akCallApi` 现在会把 bridge 返回值统一进入 Promise/解析链,
     同步 throw 会显示 `AUTO KEYS` 错误, 普通对象或字符串返回不再被静默丢弃。

  2) `web/menu.html` 修复 BossRaid 通用 API helper 的同步异常与非 Promise 返回处理。
     `_brCallApi` 现在会把 bridge 返回值统一进入 Promise/解析链,
     同步 throw 会显示 `BOSS RAID` 错误, 普通对象或字符串返回不再卡在裸 `.then`。

## v4.6.73: Web menu cloud server bridge fallback 修复.

  1) `web/menu.html` 修复 AutoKey server URL 保存的 bridge 失败反馈。
     保存服务器地址现在通过标准 helper 调用 `set_auto_key_server_url`,
     API 缺失、同步异常、失败返回和 Promise reject 都会显示错误。

  2) `web/menu.html` 修复 BossRaid server URL 保存的 bridge 失败反馈。
     保存服务器地址现在通过标准 helper 调用 `set_boss_raid_server_url`,
     成功后同步 state, 失败时不再因为裸 `Promise.all` 静默中断。

## v4.6.72: Web menu exit/alert bridge fallback 修复.

  1) `web/menu.html` 修复退出命令 bridge 失败后卡 pending 的问题。
     `exit_app` 现在通过安静 bridge helper 调用,
     失败时回退到 `menu_action('exit')`, 全部失败则重置 pending 并提示。

  2) `web/menu.html` 修复 Alert OK 回执的 fire-and-forget 异常。
     `alert_ok` 现在通过同一个 helper 调用,
     API 缺失、同步异常和 Promise reject 都不会留下未处理异常。

## v4.6.71: Web menu close/theme bridge fallback 修复.

  1) `web/menu.html` 修复 Web menu close/toggle bridge 失败兜底。
     背景点击和 Escape 关闭菜单现在通过安全 helper 调用 `toggle_menu`,
     API 缺失、同步异常和 Promise reject 都会回退到本地 `closeMenu()`。

  2) `web/menu.html` 修复 panel theme 初始拉取的同步异常 fallback。
     `get_panel_themes` 现在通过 Promise 包裹调用,
     非 Promise 返回、同步异常和 reject 都会安全回退到当前主题缓存。

## v4.6.70: Web menu initial state bridge 响应修复.

  1) `web/menu.html` 修复 AutoKey 初始状态拉取的失败处理。
     启动初始化现在通过安全 loader 调用 `get_auto_key_state`,
     同步异常、失败返回和 Promise reject 都不会中断后续菜单初始化。

  2) `web/menu.html` 修复 BossRaid 初始状态拉取的失败处理。
     启动初始化现在通过安全 loader 调用 `get_boss_raid_state`,
     失败时显示短提示并保留现有本地状态, 避免裸 Promise 静默失败。

## v4.6.69: Web menu updater bridge 响应修复.

  1) `web/menu.html` 修复 updater apply/download 的失败恢复。
     更新按钮现在通过 updater 专用 bridge helper 调用,
     能兼容布尔值、JSON、空返回、同步异常和 Promise reject,
     失败时恢复原 UI 状态并显示错误。

  2) `web/menu.html` 修复 skip update 失败被吞掉的问题。
     跳过更新现在只在 bridge 成功后隐藏面板,
     失败时解锁按钮、恢复状态并显示错误提示。

## v4.6.68: Web menu plugin detached bridge 响应修复.

  1) `web/menu.html` 修复插件独立面板动作的 bridge 失败反馈。
     插件面板按钮现在通过标准 helper 调用 `invoke_ui_action`,
     API 缺失、失败返回、同步异常和 Promise reject 都会显示错误。

  2) `web/menu.html` 修复插件独立面板快捷键设置失败后的交互。
     快捷键下拉现在通过标准 helper 调用 `set_plugin_hotkey`,
     成功后刷新并提示, 失败后刷新回弹旧值并显示错误。

## v4.6.67: Web menu action/plugin popup bridge 响应修复.

  1) `web/menu.html` 修复 child-menu 通用 `menu_action` 静默失败。
     普通菜单动作现在通过标准 bridge helper 调用,
     API 缺失、失败返回、同步异常和 Promise reject 会显示错误。

  2) `web/menu.html` 修复插件弹窗管理、置顶和启禁用按钮的失败反馈。
     这些按钮现在统一处理 bridge 失败, 成功后刷新弹窗或关闭弹窗,
     并对置顶、取消置顶、启用和禁用显示明确 toast。

## v4.6.66: Web menu plugin/editor bridge 响应修复.

  1) `web/menu.html` 修复插件全部重载的 bridge 失败反馈。
     Reload All Plugins 现在通过标准 setting helper 调用 `reload_plugins`,
     成功后重绘插件列表并提示, 失败时显示错误而不是静默无响应。

  2) `web/menu.html` 修复 AutoKey/BossRaid editor toggle 的 bridge 失败反馈。
     两个 editor toggle 现在统一处理 API 缺失、失败返回和 Promise reject,
     成功时显示明确 toast, 失败时给出对应面板错误提示。

## v4.6.65: Web menu leaderboard/DPS bridge 响应修复.

  1) `web/menu.html` 修复 leaderboard sort fetch 失败交互。
     排行榜切换排序现在会检查 `fetch_leaderboard` 是否可用, 处理失败返回/Promise reject,
     失败时恢复上一排序并重绘旧数据, 避免列表永久停在 loading。

  2) `web/menu.html` 修复 DPS fade timeout bridge 响应处理。
     fade timeout 现在通过标准 setting helper 保存, 成功时按后端 timeout/seconds 重新同步,
     失败时回滚到上一确认值; setting helper 也会捕获同步 throw。

## v4.6.64: Web menu file picker bridge 修复.

  1) `web/menu.html` 与 `web/pywebview-shim.js` 修复 file picker consumer 参数传递。
     文件导入现在会在关闭 picker 前把 auto_key/boss_raid consumer 显式传给 bridge,
     避免 WebView2 shim 因 `_pickerConsumer` 被清空而丢失导入目标。

  2) `web/menu.html` 修复 file picker bridge 失败反馈。
     `select_file`、`browse_dir` 和 `select_folder` 现在会处理 Promise reject/错误返回,
     在失败时显示对应 AUTO KEYS/BOSS RAID/FILE PICKER 提示, 不再静默失败。

## v4.6.63: Web menu slots/theme bridge 响应修复.

  1) `web/menu.html` 与 `sao_webview.py` 修复 watched slots bridge 响应处理。
     技能槽勾选现在会等待 `set_watched_slots` ack, 成功时按后端 slots 重画,
     失败时回滚到上一确认槽位; Python bridge 也会过滤 1..9 并返回结构化结果。

  2) `web/menu.html` 与 `sao_webview.py` 修复 panel theme bridge 响应处理。
     单面板主题切换现在会消费后端 panel/theme/panel_themes,
     全量主题切换失败时会恢复整组主题; Python 与 WebView2 shim 均返回/归一标准 ack。

## v4.6.62: Web menu burst/sound bridge 响应修复.

  1) `web/menu.html` 修复 burst/sound 控件 bridge 失败时的交互回滚。
     Burst 和 sound 开关现在复用标准 bridge checkbox helper,
     明确失败或 Promise reject 时恢复原选中状态并提示用户。

  2) `web/menu.html` 与 `sao_webview.py` 修复 sound volume 响应与参数处理。
     音量滑杆现在按后端返回的 volume 重新同步, 失败时回滚到上一确认值;
     Python bridge 也会夹取 0..100、容忍坏输入, 并返回结构化 ack。

## v4.6.61: Web menu setting bridge 响应修复.

  1) `web/menu.html` 修复 boss bar mode bridge 响应处理。
     点击切换后现在会等待 `set_boss_bar_mode` 返回, 按后端返回的 mode 重新同步 UI,
     失败时回滚到原 active mode 并提示用户。

  2) `web/menu.html` 修复 data source bridge 响应处理。
     component source 和 mem data source 现在会消费后端返回的标准化 mode/map,
     明确失败或 Promise reject 时回滚到原设置, 避免界面与真实配置分叉。

## v4.6.60: Web menu toggle/boss-bar fallback 修复.

  1) `web/menu.html` 修复 boss bar mode 前端归一。
     配置恢复和点击切换现在先通过 allowlist 收敛到
     `always`/`boss_raid`/`off`, 避免坏配置让按钮没有 active 状态或传出异常模式。

  2) `web/menu.html` 修复 DPS/buffmon toggle 响应处理。
     开关现在会解析 bridge 返回值, 明确失败或 Promise reject 时回滚 checkbox 并弹出错误,
     避免界面显示 ON/OFF 但后端设置没有成功落地。

## v4.6.59: DPS fade timeout fallback 修复.

  1) `web/menu.html` 修复 DPS fade timeout 设置恢复。
     `restoreMenuSettings()` 现在将 `dps_fade_timeout_s` 夹到 0..120 再回填,
     避免坏配置或越界值让输入框显示异常并误导用户。

  2) Python/C# bridge 修复 DPS fade timeout 参数归一。
     `set_dps_fade_timeout()` 现在对坏输入恢复为 0, 并统一夹到 0..120,
     与 WebView2 shim 和菜单输入范围保持一致。

## v4.6.58: Web menu data source 参数传递修复.

  1) `web/menu.html` 修复 data source 设置归一。
     组件源和内存源按钮现在先通过 allowlist/默认值归一再更新 UI 与调用 bridge,
     避免异常配置让按钮无选中、summary 不一致或把无效参数传给后端。

  2) Python/C# WebView bridge 修复 component source 持久化。
     `set_component_source()` 不再是 no-op, 会保存标准化后的 `data_source_map`,
     菜单同步也会回传当前 map, 避免用户切换 HP/LEVEL/STA/SKILL/NAME 来源后丢失。

## v4.6.57: Web menu theme/info fallback 修复.

  1) `web/menu.html` 修复 panel theme 设置参数归一。
     theme panel 和 theme value 现在通过 allowlist 归一后再更新 UI/调用后端,
     避免异常 panel/theme 值破坏按钮状态或传给 `set_panel_theme`。

  2) `web/menu.html` 修复 info panel 文本 fallback。
     username、profession、description 和 current file 现在保留合法 `0`,
     且 updateInfo 会先防护 payload shape, 避免空 payload 打断菜单刷新。

## v4.6.56: Web menu AutoKey condition value/text 修复.

  1) `web/menu.html` 修复 AutoKey condition value 归一。
     高级 JSON 导入的 bool、slot index、文本匹配和百分比条件现在在渲染与保存前清洗,
     避免 `false` 字符串显示成 ready 或异常数值传给后端。

  2) `web/menu.html` 修复 AutoKey runtime 状态文本 fallback。
     active profile、last reason 与 last action label 现在保留合法 `0`,
     避免运行状态把有效文本显示成默认值或直接隐藏。

## v4.6.55: Web menu AutoKey condition type 归一修复.

  1) `web/menu.html` 修复 AutoKey condition type / slot state 渲染与更新。
     条件类型和槽位状态现在通过 allowlist 归一,
     避免异常枚举导致下拉框无选中或把无效条件传入草稿。

  2) `web/menu.html` 修复 AutoKey press mode 与保存 payload 归一。
     press mode 和 serialize 出口现在会清洗 action/condition entry,
     避免 malformed draft 或高级 JSON 把无效枚举传给后端。

## v4.6.54: Web menu AutoKey draft action fallback 修复.

  1) `web/menu.html` 修复 AutoKey draft action entry 初始化。
     action id/label 与 conditions 现在先做 entry/array 防护并保留合法 `0`,
     避免载入草稿时覆盖有效 ID/标签或因 malformed action 中断编辑器。

  2) `web/menu.html` 修复 AutoKey action key/conditions JSON 输入 fallback。
     key、advanced JSON 文本、复制标签和创建后选中 ID 现在使用安全文本 helper,
     避免直接传入 `0` 被静默当成空值或错误应用空条件。

## v4.6.53: Web menu profile selection id 防护修复.

  1) `web/menu.html` 修复 AutoKey profile 选择/保存/导出 ID fallback。
     本地列表 active/selected、选择、删除、保存后选中与导出 path 显示
     现在保留合法 `0`, 避免有效 profile 被当成空选择。

  2) `web/menu.html` 修复 BossRaid profile 选择/保存/导出 ID fallback。
     查找、加载、激活/编辑、创建/删除后的选中 ID 与导出 path 显示
     现在统一使用安全文本 helper, 避免数字/字符串 ID 被错误清空。

## v4.6.52: Web menu cloud settings 参数传递修复.

  1) `web/menu.html` 修复 AutoKey/BossRaid cloud 表单回填与读取。
     server/search 输入现在通过统一 helper 读写,
     避免合法 `0` query/server 文本被 fallback 清空或缺失元素打断搜索。

  2) `web/menu.html` 修复 AutoKey/BossRaid 上传 ID 与 remote_id 显示。
     上传目标 profile id 与上传完成 remote_id toast 现在保留合法 `0`,
     避免有效 ID 被误判为未选择或显示为空。

## v4.6.51: Web menu plugin popup id/text 防护修复.

  1) `web/menu.html` 修复 plugin popup/detached id 传递。
     plugin、panel 与 hotkey id 现在会统一保留合法 `0` 文本并归一比较,
     避免跨桥返回数字/字符串 ID 时漏渲染面板或热键。

  2) `web/menu.html` 修复 plugin popup label/hotkey fallback。
     插件标题、热键标签和占用者文本现在使用安全 fallback,
     且占用检测改为 key-exists 判断, 避免 owner 为 `0` 时显示成可用。

## v4.6.50: Web menu updater class/text 防护修复.

  1) `web/menu.html` 修复 updater badge class token 归一。
     updater badge variant 现在通过 allowlist 转成 class token,
     避免异常 variant 注入额外 class 或破坏 badge 样式。

  2) `web/menu.html` 修复 updater 文本 fallback。
     notes、error、prompt key 与 latest_version 显示现在保留合法 `0`,
     避免更新提示把有效文本显示成空值或占位符。

## v4.6.49: Web menu editor text fallback 修复.

  1) `web/menu.html` 修复 AutoKey editor profile/action 文本渲染。
     profile 字段与 action entries 现在会先做 entry/array 防护,
     action label/key 和 conditions JSON 的合法 `0` 文本也会保留显示。

  2) `web/menu.html` 修复 BossRaid editor/runtime 文本渲染。
     summary、editor fields 与 runtime phase name 现在使用安全文本 helper,
     避免合法 `0` 被渲染为空、local 或占位符。

## v4.6.48: Web menu BossRaid startup/editor payload 防护修复.

  1) `web/menu.html` 修复 BossRaid startup state response 解析。
     菜单启动拉取 BossRaid state 时复用安全 parser,
     malformed response 不再以 JSON SyntaxError 打断初始化。

  2) `web/menu.html` 修复 BossRaid editor phase/timeline 渲染。
     阶段与时间线 payload 现在会先做数组和 entry 防护,
     避免异常 phases/timelines 破坏编辑器渲染, 合法 `0` 名称/标签也会保留显示。

## v4.6.47: Web menu DPS/Linkage response 防护修复.

  1) `web/menu.html` 修复 DPS last report response 解析。
     打开上一场 DPS 报告时会安全处理字符串、空值和非对象响应,
     malformed response 不再以 JSON SyntaxError 打断用户提示。

  2) `web/menu.html` 修复 Boss/AutoKey Linkage 初始化 response 解析。
     Linkage state 现在会先校验 API response 与 state 对象再同步控件,
     避免异常 response 破坏初始化或把当前 UI 状态清成错误默认值。

## v4.6.46: Web menu BossRaid API response 防护修复.

  1) `web/menu.html` 修复 BossRaid bridge API response 解析。
     BossRaid bridge 回调现在会安全处理字符串、空值和非对象响应,
     malformed API response 不再以 JSON SyntaxError 打断流程或显示晦涩错误。

  2) `web/menu.html` 修复 BossRaid cloud settings 多结果解析。
     保存云端设置时会校验 Promise 结果数组并逐项安全解析,
     避免异常 response 破坏设置保存状态同步或错误提示。

## v4.6.45: Web menu local profile payload 渲染修复.

  1) `web/menu.html` 修复 AutoKey 本地 profile payload 渲染。
     profiles_full、profiles summary 和 profile actions 现在会做 entry/array shape 防护,
     malformed profile 不再中断本地配置列表, 合法 `0` 文本也会保留显示。

  2) `web/menu.html` 修复 BossRaid 本地 profile payload 渲染。
     BossRaid profiles/profiles_full 现在会先归一 entry 再选择与渲染,
     避免异常 profile entry 破坏本地卡片、选择状态或草稿加载。

## v4.6.44: Web menu tab 参数归一修复.

  1) `web/menu.html` 修复 AutoKey tab 状态传播。
     AutoKey profile select 与 tab 切换现在只接受 local/editor/cloud,
     避免异常 tab 值让所有 tab panel 同时隐藏或进入不可恢复状态。

  2) `web/menu.html` 修复 BossRaid tab selector 参数传播。
     BossRaid tab 名称现在归一后再写入状态和 querySelector,
     避免异常 tab payload 破坏 selector 或让编辑/云端面板渲染为空。

## v4.6.43: Web menu cloud payload 渲染修复.

  1) `web/menu.html` 修复 AutoKey Cloud 身份/结果 payload 渲染。
     identity.missing 与 search.results entries 现在会做 shape 防护,
     malformed cloud payload 不再中断云端页, 合法 `0` 身份/结果文本也会保留显示。

  2) `web/menu.html` 修复 BossRaid Cloud 身份/结果 payload 渲染。
     BossRaid 云端页现在复用安全的 cloud helper 渲染身份、缺失字段和远端结果,
     避免异常 payload 破坏上传提示或云端卡片渲染。

## v4.6.42: Web menu picker/leaderboard payload 防护修复.

  1) `web/menu.html` 修复 File Picker payload 渲染。
     browser payload 现在会安全解析并校验 dirs/files shape,
     malformed entries 不再中断 picker, `0` 路径/名称也会保留显示。

  2) `web/menu.html` 修复 Leaderboard sort 参数传播。
     sort 现在归一到 xp/level/songs_played/play_time,
     避免异常 bridge payload 破坏 tab selector 或让榜单统计列渲染为空。

## v4.6.41: Web menu Session/Leaderboard payload 渲染修复.

  1) `web/menu.html` 修复 Session Players 行渲染。
     玩家行现在会兜底 malformed/null row, 并保留 name、uid、fight_power 的合法 `0`,
     避免异常 players payload 中断批量渲染或把有效文本显示成占位。

  2) `web/menu.html` 修复 Leaderboard payload 与身份文本渲染。
     leaderboard JSON/entries 现在会做 payload shape 防护,
     player id、device name 与 self id 也会保留合法 `0`,
     避免异常 payload 崩溃或隐藏数值型玩家标识。

## v4.6.40: Web menu Boss/AutoKey linkage 映射渲染修复.

  1) `web/menu.html` 修复 Boss ↔ AutoKey Linkage mappings payload shape。
     从 bridge 回来的 `mappings` 现在必须是数组才会渲染,
     避免异常 payload 被当作字符串/对象迭代后生成错误映射行。

  2) `web/menu.html` 修复 Linkage 映射文本字段 hydration。
     trigger_match、action_key 与 action_label 现在保留合法 `0`,
     避免 `value || ''` 把用户配置的数值型匹配/标签显示为空。

## v4.6.39: Web menu AutoKey 数值表单默认值修复.

  1) `web/menu.html` 修复 AutoKey editor summary 值渲染。
     summary helper 现在保留合法 `0` 显示值,
     避免 `value || '--'` 把有效数值渲染成缺失占位。

  2) `web/menu.html` 修复 AutoKey action/engine 数值输入 hydration。
     slot、tick、press_count、interval、hold、ready_delay、min_rearm 与 post_delay
     现在直接交给 `_akIntValue()` 使用字段默认值和范围,
     避免缺失或异常 profile payload 在编辑器中显示成错误的 0 值。

## v4.6.38: Web menu AutoKey/BossRaid 表单值防护修复.

  1) `web/menu.html` 修复 AutoKey 文本型条件值渲染。
     profession/player-name match value 现在保留合法 `0`,
     避免 `condition.value || ''` 把用户配置显示为空。

  2) `web/menu.html` 修复 BossRaid 编辑器数值输入 hydration。
     boss_total_hp 与 enrage_time_s 现在通过 `_brNumText()` 归一,
     避免异常 profile payload 写入 `Infinity`、`NaN` 或原始坏数值。

## v4.6.37: Web menu 插件 hotkey count 显示防护修复.

  1) `web/menu.html` 修复插件弹窗列表 hotkey count 显示。
     插件 popup 行现在会把 `hotkey_count` 归一为有限非负整数,
     避免异常插件 payload 在主菜单弹窗中显示 `Infinity`、`NaN` 或原始坏文本。

  2) `web/menu.html` 修复插件分类列表 hotkey count 显示。
     插件 category 行现在复用同一 hotkey count formatter,
     避免面板入口列表出现不可读快捷键计数。

## v4.6.36: Plugin Manager Web count 显示防护修复.

  1) `web/plugin_manager.html` 修复插件卡片 badge 与 subscription count 显示。
     hotkey、failure、event failure 与 subscription count 现在归一为有限非负整数,
     避免异常插件 payload 在卡片上显示 `Infinity`、`NaN` 或原始坏文本。

  2) `web/plugin_manager.html` 修复顶部 active/plugin/event-bus summary count 显示。
     active count、plugin total、topic count 与 subscriber count 现在通过统一 count fallback 渲染,
     避免插件管理器摘要区出现不可读计数。

## v4.6.35: ACT Death/Timeline Web 数值显示防护修复.

  1) `web/act_death_recap.html` 修复死亡回放 summary 与 row amount 显示。
     incoming damage、healing、shield、row count 与事件 amount 现在通过有限数值 formatter 渲染,
     避免异常 payload 显示为 `Infinity`、`NaN` 或隐藏合法 0。

  2) `web/act_timeline_vcr.html` 修复时间线事件 key/value 与事件计数显示。
     fallback event key 与 event value 现在过滤非有限数值,
     避免坏 time/value payload 造成折叠状态碰撞或不可读文本。

## v4.6.34: ACT Report/Offline Web 数值显示防护修复.

  1) `web/act_report_export.html` 修复 report preview/history/top-row 数值显示。
     damage、dps、heal、duration、storage count 与 import toast 现在通过有限数值 formatter 渲染,
     避免导出面板出现 `Infinity`、`NaN` 或原始坏 payload 文本。

  2) `web/act_offline_import.html` 修复 offline import preview/history/summary 数值显示。
     total damage、event count、history count 与历史 damage 现在归一为有限显示文本,
     避免离线导入面板出现不可读数值。

## v4.6.33: ACT Action Log Web 可见数值显示防护修复.

  1) `web/act_action_log.html` 修复分组卡片 count 与 total 显示。
     group count、uid_count 与 total_value 现在通过有限数值 formatter 渲染,
     避免异常 payload 显示为 `Infinity rows`、`NaN` 或原始坏文本。

  2) `web/act_action_log.html` 修复 summary/page/topic count 显示。
     total rows、page index/page count 与 topic count 现在保留 0 值并过滤非有限值,
     避免分页和汇总区域出现不可读计数。

## v4.6.32: ACT Aggregate Web count 显示防护修复.

  1) `web/act_aggregate.html` 修复聚合行与趋势 badge 的 count 显示。
     group count 与 graph row_count 现在归一为有限非负整数,
     避免异常 payload 显示为 `Infinityx`、`NaN` 或原始坏文本。

  2) `web/act_aggregate.html` 修复 KPI 与 source mix count 显示。
     raw_counts 与 source_mix count 现在通过统一 count formatter 渲染,
     避免侧栏、徽章和概览卡片出现不可读计数。

## v4.6.31: ACT Data Source Health 数值显示防护修复.

  1) `web/data_source_health.html` 修复运行时 summary 延迟数值显示。
     latency 与 last-event 现在通过有限数值 fallback 渲染,
     避免非有限 payload 显示为 `Infinity ms` 或 `NaN ms`。

  2) `web/data_source_health.html` 修复 source uptime 数值显示。
     数据源卡片 uptime 现在归一为有限数值后再拼接单位,
     避免异常 source payload 生成不可读的运行时文本。

## v4.6.30: ACT Graph Web 点列与数值显示防护修复.

  1) `web/act_graph_timeseries.html` 修复 timeseries points 条目归一。
     图表与最近点列表现在只渲染对象 point 条目,
     避免异常数组项生成 0ms/0 值幽灵点。

  2) `web/act_graph_timeseries.html` 修复图表数值格式化回退。
     `fmt()` 现在用有限数值 fallback,
     避免非有限值显示为 `Infinity` 或原始坏 payload 文本。

## v4.6.29: Commander Web payload shape 防护修复.

  1) `web/commander.html` 修复成员列表 payload 归一。
     team 与 boss overview 现在只渲染对象成员条目,
     避免字符串或异常 `members` payload 生成空 UID 假队友。

  2) `web/commander.html` 修复技能槽列表 payload 归一。
     self skill slots 现在只渲染对象槽位条目,
     避免异常 `skill_slots` payload 生成假的 ready 技能格。

## v4.6.28: Web plugin layer 样式 class token 防护修复.

  1) `web/plugin_layer.js` 修复插件 text/kv/badge/button 样式 token 归一。
     插件 payload 中的异常 style 现在只会映射到允许的 SAO class,
     避免未知或带空格的 token 破坏插件层视觉样式。

  2) `web/plugin_layer.js` 修复插件 row/table 对齐 token 归一。
     row、table header 与 table cell 对齐现在只接受 left/center/right,
     避免异常 align payload 拼接进 className 导致布局或渲染不完整。

## v4.6.27: Web plugin layer 尺寸与 canvas 数值防护修复.

  1) `web/plugin_layer.js` 修复插件 UI bar/spacer/input 尺寸归一。
     异常或非有限 pct/size/width payload 现在会 clamp 到安全范围,
     避免插件控件出现 `Infinitypx`、`badpx` 或撑破面板的样式。

  2) `web/plugin_layer.js` 修复插件 canvas 尺寸与绘图参数归一。
     canvas 宽高、rect/oval/line/text 坐标、尺寸、线宽和字号现在会安全取整并 clamp,
     避免异常插件 spec 导致 canvas 渲染不全或绘图 API 收到非有限数值。

## v4.6.26: Web panel speed 与 BPM 数值渲染防护修复.

  1) `web/panel.html` 修复 control/status speed 文本格式化。
     speed payload 为字符串或异常值时现在会先归一再 `toFixed`,
     避免小面板状态更新因为单个 speed 字段抛错而中断。

  2) `web/panel.html` 修复 status BPM 文本格式化。
     BPM payload 为坏值时现在回退为 `—`,
     避免状态面板显示 `NaN` 这类对用户不友好的数值。

## v4.6.25: DPS 拖拽坐标与几何保存防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复拖拽开始/移动坐标归一。
     小数字符串本地/根坐标和异常窗口坐标现在会先归一再进入 resize、列表拖拽和窗口移动路径,
     避免输入封装层格式变化导致拖拽开始或移动静默失效。

  2) `gui_modules/sao_gui_dps.py` 修复拖拽结束几何保存归一。
     小数字符串位置与详情尺寸现在会安全保存到 `dps_ov_x/y` 和 `dps_detail_w/h`,
     避免成功拖动或缩放后因为设置值格式异常丢失持久化。

## v4.6.24: DPS wheel 与 resize 拖拽数值防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复鼠标滚轮 delta 归一。
     字符串或小数字符串 delta 现在仍能触发列表/详情滚动,
     避免输入封装层格式变化打断滚轮交互。

  2) `gui_modules/sao_gui_dps.py` 修复 detail resize 拖拽状态归一。
     坏或小数字符串 resize 起点/尺寸现在会归一后继续计算,
     避免详情窗口拖拽缩放因为运行态格式异常失效。

## v4.6.23: DPS dirty signature 与 GPU 事件数值防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复 compose signature 数值归一。
     坏 target/detail/fade 运行态不再让 `_compose_signature()` 退化为 `None`,
     避免 dirty-skip 失效后持续提交重复帧。

  2) `gui_modules/sao_gui_dps.py` 修复 GPU 事件坐标归一。
     坏本地坐标、根坐标或 wheel delta 现在回退为安全数值,
     避免 GPU 鼠标事件构造阶段打断拖拽/滚轮交互。

## v4.6.22: DPS detail 尺寸与动画状态防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复 detail 模式尺寸状态归一。
     坏 `_detail_w` / `_detail_h` 运行态现在回退到默认详情尺寸并继续 clamp,
     避免详情窗口 compose size 计算失败导致面板渲染中断。

  2) `gui_modules/sao_gui_dps.py` 修复详情技能滚动动画状态归一。
     坏 `_skill_scroll_disp` / `_skill_scroll_target` 现在在动画判断和推进时回退为安全数值,
     避免 tick/dirty-signature 流程被异常滚动状态打断。

## v4.6.21: DPS hit FX 与 UID 参数归一修复.

  1) `gui_modules/sao_gui_dps.py` 修复 hit FX seq/uid 参数归一。
     `hit_fx.seq` 与 `hit_fx.uid` 传入小数字符串时仍能触发面板和行级闪光,
     避免上游 JSON 数值格式变化导致命中特效丢失。

  2) `gui_modules/sao_gui_dps.py` 修复 self/detail UID 参数归一。
     `set_self_uid()`、详情打开/选择/update 现在容忍小数字符串 UID,
     避免 self 高亮或详情请求因为等价 UID 格式未归一而丢失。

## v4.6.20: DPS 布局设置与滚动状态防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复 DPS 布局设置逐项归一。
     坏 `dps_ov_x` 等单个设置值不再吞掉后续有效 y/detail/minimized 设置,
     避免配置局部损坏导致面板位置、详情尺寸或最小化状态无法恢复。

  2) `gui_modules/sao_gui_dps.py` 修复列表与详情技能滚动状态归一。
     坏 scroll offset / skill-scroll 状态现在回退到安全范围,
     避免滚轮、列表绘制或详情技能列表因为异常状态打断渲染。

## v4.6.19: DPS header 与通知数值防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复 ACT badge 版本号归一。
     坏 `render_spec.version` 现在回退为 `ACT V1` 并保留 mode/source/boss 上下文,
     避免头部状态徽标因为单个异常字段整体消失。

  2) `gui_modules/sao_gui_dps.py` 修复 panel notice 时长与过期时间归一。
     坏 notice `seconds` 或过期时间现在安全回退,
     避免最小化/导出/历史提示等通知路径打断面板渲染。

## v4.6.18: DPS detail-view 数值防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复详情实体查找 uid 归一。
     最近战报详情现在会跳过坏 `uid` 记录继续查找有效实体,
     避免一条异常记录阻断详情页打开。

  2) `gui_modules/sao_gui_dps.py` 修复详情统计卡与技能列表数值归一。
     坏 fight point、crit、hits、耗时与技能 total/heal/hit 字段不再打断详情渲染,
     异常技能条目会被忽略或回退为安全默认值。

## v4.6.17: DPS overlay payload 数值防护修复.

  1) `gui_modules/sao_gui_dps.py` 修复 live snapshot 与实体行数值归一。
     坏 total/entity damage、heal、DPS/HPS、MEM 与比例字段现在回退到有限默认值,
     避免单次异常 DPS payload 打断面板刷新或让条形渲染失真。

  2) `gui_modules/sao_gui_dps.py` 修复 report/ACT/list row 数值与点击区归一。
     最近战报、ACT 渲染行和列表点击区域现在容忍坏 uid/数值,
     避免单条异常记录让整张 DPS 列表空白或交互区注册失败。

## v4.6.16: BossRaid 摘要与更新弹窗数值防护修复.

  1) `gui_modules/sao_gui_bossraid.py` 修复 Boss 反应摘要计数归一。
     从内存导入的 `skill_count` / `mechanic_count` / `hp_line_count`
     与持续时间坏值不再打断 BossRaid 面板摘要渲染。

  2) `gui_modules/sao_gui_status_updater_mixin.py` 修复更新弹窗时长归一。
     坏 `display_time` 现在回退到安全默认值并限制最小显示时间,
     避免异常更新 payload 打断重要更新提醒。

## v4.6.15: Tk shared panel 搜索与来源徽标数值防护修复.

  1) `gui_modules/sao_gui_mem_scope.py` 修复 Mem Scope 搜索状态数值归一。
     坏 `count` / `progress` 不再打断搜索结果区域渲染,
     扫描中进度会被限制在 0-100% 的安全范围内。

  2) `gui_modules/sao_panel_components.py` 修复共享来源徽标计数归一。
     ACT 聚合等面板的 `source_mix.count` 坏值现在显示为 0,
     避免单个异常来源计数让整组来源徽标创建失败。

## v4.6.14: ACT aggregate 与 BossRaid monster 数值防护修复.

  1) `engines/act_aggregate.py` 修复 ACT 聚合行数值归一。
     坏 `index`、预归一化行的坏时间/计数/伤害值不再打断聚合摘要,
     避免 ACT 聚合驾驶舱因为单条异常历史行渲染失败。

  2) `engines/boss_raid_engine.py` 修复 BossRaid monster 更新数值归一。
     `hp`、`max_hp`、护盾/破韧/灭绝进度等 TCP 字段现在先归一成有限数,
     避免单个异常字段让有效 boss UUID/最大血量与机制观察更新整段丢失。

## v4.6.13: Entity packet callback 数值防护修复.

  1) `gui_modules/sao_gui_packet_callbacks_mixin.py` 修复 dungeon/scene event ID 归一。
     地图事件现在容忍坏 dungeon/difficulty 数值并保留有效 scene_id,
     避免异常 packet 字段让地图横幅、状态更新和 ACT 发布整段丢失。

  2) `gui_modules/sao_gui_float_handlers_mixin.py` 修复 pending combat reset 延迟归一。
     同副本重开候选现在容忍坏 reset_delay 与既有 scene grace 值,
     避免异常数值打断 DPS/BossHP 延迟重置流程。

## v4.6.12: ACT panel numeric payload 防护修复.

  1) `gui_modules/sao_gui_data_source_health.py` 修复状态栏数值归一。
     Data Source Health 面板现在容忍坏 latency/last_event 值,
     避免异常诊断 payload 中断状态栏渲染。

  2) `gui_modules/sao_gui_panels_mixin.py` 修复插件 detached panel 尺寸归一。
     插件 `open_window` payload 的 width/height 现在容忍坏字符串与 NaN/Inf,
     避免异常尺寸参数导致插件独立面板无法打开。

## v4.6.11: HP/STA HUD public setter 数值防护修复.

  1) `gui_modules/sao_gui_hp.py` 修复 `HpOverlay.update_hp()` 数值归一。
     HP HUD 现在容忍坏 current/total live 值,
     避免异常血量 payload 中断 HP 条刷新。

  2) `gui_modules/sao_gui_hp.py` 修复 `HpOverlay.update_sta()` 数值归一。
     STA HUD 现在容忍坏 current/total live 值并保护文本格式化,
     避免异常耐力 payload 中断 STA 条刷新。

## v4.6.10: BossHP payload 与 additional unit 数值防护修复.

  1) `gui_modules/sao_gui_bosshp.py` 修复 BossHP additional unit 数值归一。
     附属单位 HP、破韧与护盾值现在统一过滤 NaN/Inf/坏字符串,
     避免 mini bar 错显示为满值或绘制阶段抛错。

  2) `gui_modules/sao_gui_state_mixin.py` 修复 BossHP 推送前 payload 数值归一。
     状态 worker 现在先归一 direct/additional BossHP 数值与签名字段,
     避免坏 TCP/内存字段让 BossHP payload 静默变成 `None`。

## v4.6.9: GPU LeftInfo/BossHP 数值防护修复.

  1) `gui_modules/sao_left_info_gpu.py` 修复 LeftInfo GPU snapshot 数值归一。
     左侧信息 GPU 面板现在容忍坏尺寸、扫描相位与扫描强度参数,
     避免异常 snapshot 值打断签名计算或本帧提交。

  2) `gui_modules/sao_gui_bosshp.py` 修复 BossHP live payload 数值归一。
     Boss 血条现在容忍坏 HP、护盾、破韧与 additional unit 数值,
     避免异常 TCP/UI payload 中断 BossHP 渲染更新。

## v4.6.8: 菜单 GPU snapshot 数值防护修复.

  1) `gui_modules/sao_child_bar_gpu.py` 修复 ChildBar GPU snapshot 数值归一。
     子菜单 GPU 面板现在容忍坏 hover、行宽、线宽、箭头宽与 fade 参数,
     避免异常动画/几何值打断子菜单帧提交。

  2) `gui_modules/sao_menu_bar_gpu.py` 修复 MenuBar button snapshot 数值归一。
     主菜单 GPU 按钮现在容忍坏 size/hover 参数,
     避免 NaN/Inf 进入签名或 compose 路径造成按钮条渲染异常。

## v4.6.7: GPU snapshot 数值防护修复.

  1) `gui_modules/sao_left_info_gpu.py` 修复 Session Players GPU snapshot 数值归一。
     会话玩家 GPU 面板现在容忍坏 `total`、分页、尺寸与 reveal 参数,
     避免异常 payload 让列表面板本帧不刷新。

  2) `gui_modules/sao_left_info_gpu.py` 修复 Player Panel GPU snapshot 数值归一。
     玩家信息 GPU 面板现在容忍坏等级、经验、HP/STA、尺寸与 scan phase,
     避免异常 live 值打断玩家面板渲染。

## v4.6.6: BuffMon 缓存签名渲染修复.

  1) `gui_modules/sao_gui_buffmon.py` 修复 self buff 覆盖率与触发次数缓存签名。
     BuffMon 底图缓存现在跟踪 `uptime_pct` 与 `apply_count`,
     避免 ACT 覆盖率细条或触发次数更新时仍复用旧底图。

  2) `gui_modules/sao_gui_buffmon.py` 修复动态 header 缓存签名。
     Boss buff 面板现在把动态目标标题、kicker 与 badge 纳入底图签名,
     避免目标切换但 buff 行相同的时候继续显示旧目标名。

## v4.6.5: Menu plugin payload 防护修复.

  1) `web/menu.html` 修复 detached plugin panel payload 形状处理。
     主菜单插件面板现在归一 `panels` 列表与 `render_ui_panel` 返回 spec,
     避免坏插件面板 payload 打断浮层渲染或保留过期卡片。

  2) `web/menu.html` 修复插件 popup/category/hotkey payload 形状处理。
     主菜单插件入口现在归一 `plugins`、`hotkeys` 与 `occupied` payload,
     避免坏插件菜单数据打断插件列表和快捷键下拉渲染。

## v4.6.4: DPS Web payload 防护修复.

  1) `web/dps.html` 修复实体列表与报告入口 payload 形状处理。
     DPS Web 现在归一 live/report/history/detail 根对象与实体列表,
     避免坏实体 payload 打断列表、详情和报告渲染。

  2) `web/dps.html` 修复 ACT trigger 提示 payload 形状处理。
     DPS Web 现在归一 ACT snapshot、trigger 对象与 emitted/recent 事件列表,
     避免坏 trigger payload 打断页眉战斗提示渲染。

## v4.6.3: Plugin Layer payload 防护修复.

  1) `web/plugin_layer.js` 修复插件 spec 节点树 payload 形状处理。
     Plugin Layer 现在归一 `nodes`、`children`、`ops`、`rows` 与 `columns`,
     避免坏插件 UI spec 打断覆盖层或 takeover 渲染。

  2) `web/plugin_layer.js` 修复 overlay/hook 返回 payload 形状处理。
     Plugin Layer 现在归一 overlay 列表、overlay spec 与 hook 返回对象,
     避免坏插件返回值打断全局 WebView 插件渲染层。

## v4.6.2: Mem Scope payload 防护修复.

  1) `web/mem_scope.html` 修复状态、目录、实体与伤害 payload 形状处理。
     Mem Scope Web 现在归一根对象、状态对象、目录列表、实体列表与伤害映射,
     避免坏 payload 打断面板主体渲染。

  2) `web/mem_scope.html` 修复搜索结果与结果提示行 payload 形状处理。
     Mem Scope Web 现在只渲染对象搜索结果并归一 `as`/`in` 嵌套对象,
     避免坏搜索结果打断地址提示和搜索列表渲染。

## v4.6.1: Trigger Timer Manager payload 防护修复.

  1) `web/trigger_timer_manager.html` 修复 recent 事件列表 payload 形状处理。
     Trigger Timer Manager Web 现在只渲染对象事件行,
     避免字符串或坏事件 payload 打断最近事件列表渲染。

  2) `web/trigger_timer_manager.html` 修复 status/rules/timers payload 形状处理。
     Trigger Timer Manager Web 现在归一状态对象、规则列表、计时器列表与错误列表,
     避免坏 status payload 打断摘要和规则卡片渲染。

## v4.5.91: Plugin Manager payload 防护修复.

  1) `web/plugin_manager.html` 修复插件管理列表 payload 形状处理。
     Plugin Manager Web 现在归一状态对象并只渲染对象插件行,
     避免 null 或坏插件条目打断插件卡片渲染。

  2) `web/plugin_manager.html` 修复插件 Panels 列表 payload 形状处理。
     Plugin Manager Web 现在归一 event bus、数组字段与 UI panel 列表,
     避免坏 panel payload 打断插件面板页增量渲染。

## v4.5.90: Raid Editor mechanics ID 参数修复.

  1) `web/raid_editor.html` 修复 mechanics 检测 chip 删除参数处理。
     Raid Editor Web 现在归一 skill/buff 检测 ID 后再渲染删除 handler,
     避免字符串或坏 ID 打断删除绑定的参数传递。

  2) `web/raid_editor.html` 修复 mechanics observed/catalog 添加参数处理。
     Raid Editor Web 现在归一观测技能与技能库搜索结果 ID 后再渲染添加入口,
     避免坏搜索结果或坏观测项打断绑定技能的交互。

## v4.5.89: Raid Editor mechanics payload 防护修复.

  1) `web/raid_editor.html` 修复 mechanics state/list/form payload 形状处理。
     Raid Editor Web 现在归一机制总状态、主开关、收件箱、机制列表、档案与阶段列表,
     避免坏 payload 打断机制列表或编辑表单渲染。

  2) `web/raid_editor.html` 修复 mechanics draft/catalog/sequence 交互 payload 形状处理。
     Raid Editor Web 现在归一草稿检测 ID、搜索结果、按键序列与保存前序列,
     避免坏草稿或坏搜索结果打断添加/删除/保存机制的参数传递。

## v4.5.88: Raid Editor reaction payload 防护修复.

  1) `web/raid_editor.html` 修复 reaction badges/tags payload 形状处理。
     Raid Editor Web 现在归一 badge 记录与 tags 列表,
     避免字符串或坏 tags payload 打断反应标签渲染。

  2) `web/raid_editor.html` 修复 reaction scenes/bosses/detail 列表形状处理。
     Raid Editor Web 现在归一反应状态、场景、Boss、技能、机制与时间线列表,
     避免坏 payload 打断 Boss 反应选择器和明细区域渲染。

## v4.5.87: Raid Editor payload 防护修复.

  1) `web/raid_editor.html` 修复 entities payload 与 entity row 形状处理。
     Raid Editor Web 现在只渲染对象实体列表,
     避免字符串或 null 实体打断实体卡片渲染。

  2) `web/raid_editor.html` 修复 phases/status/full-state payload 形状处理。
     Raid Editor Web 现在归一阶段列表、状态对象与全量推送对象,
     避免坏 payload 打断阶段列表、状态栏或全量刷新。

## v4.5.86: AutoKey Editor payload 防护修复.

  1) `web/autokey_editor.html` 修复 slots payload 与 slot row 形状处理。
     AutoKey Editor Web 现在只渲染对象技能槽并归一状态 payload,
     避免字符串或 null 槽位打断技能卡片渲染。

  2) `web/autokey_editor.html` 修复 actions payload 与 action row 形状处理。
     AutoKey Editor Web 现在只消费对象动作列表并归一每条动作,
     避免坏动作条目打断录制动作列表渲染或保存前状态。

## v4.5.85: Buff Coverage payload 防护修复.

  1) `web/buff_coverage.html` 修复 update payload 与 buffs 列表形状处理。
     Buff Coverage Web 现在归一 update 数据并只消费对象列表,
     避免字符串或坏列表造成覆盖率面板隐藏/渲染异常。

  2) `web/buff_coverage.html` 修复单条 buff row 形状处理。
     Buff Coverage Web 现在归一每条 buff 行后再读取数值与名称,
     避免 null/坏条目打断覆盖率行渲染。

## v4.5.84: ACT Data Source Health payload 防护修复.

  1) `web/data_source_health.html` 修复 diagnostics/errors payload 形状处理。
     Data Source Health Web 现在归一诊断列表与错误列表,
     避免字符串或坏 payload 造成诊断区域渲染中断或错误数量失真。

  2) `web/data_source_health.html` 修复 sources/watchers payload 形状处理。
     Data Source Health Web 现在只渲染对象数据源并归一 watcher/selection/self,
     避免坏数据源 payload 造成来源卡片与运行摘要渲染中断。

## v4.5.83: ACT Web Graph payload 防护修复.

  1) `web/act_graph_timeseries.html` 修复 metrics payload 形状处理。
     Graph Timeseries Web 现在只渲染对象指标列表,
     避免字符串或坏条目造成指标查找与图例渲染中断。

  2) `web/act_graph_timeseries.html` 修复 series/points payload 形状处理。
     Graph Timeseries Web 现在归一 series bucket 后再读取 points,
     避免坏 payload 造成趋势图和点列表渲染中断。

## v4.5.82: ACT Web Report/Offline payload 防护修复.

  1) `web/act_report_export.html` 修复 preview top_rows/history/storage_status payload 形状处理。
     Report Export Web 现在只渲染对象行与对象历史条目,
     避免字符串 payload 造成预览表、历史列表或历史数量渲染中断。

  2) `web/act_offline_import.html` 修复 last_result/report/history.encounters payload 形状处理。
     Offline Import Web 现在归一导入预览对象并只渲染对象历史条目,
     避免坏 payload 造成导入结果或历史回放列表渲染中断。

## v4.5.81: ACT Web Timeline/Aggregate payload 防护修复.

  1) `web/act_timeline_vcr.html` 修复 events/errors payload 形状处理。
     Timeline VCR Web 现在只渲染对象事件列表并归一事件 payload,
     避免字符串 payload 造成时间线渲染中断或错误事件数量。

  2) `web/act_aggregate.html` 修复 groups/source_mix/group rows/points/errors payload 形状处理。
     Aggregate Web 现在对对象行与普通列表分别归一,
     避免坏 payload 造成聚合行、drawer、趋势预览或错误数量渲染中断。

## v4.5.80: ACT Web Drilldown payload 防护修复.

  1) `web/act_combatant_drilldown.html` 修复 summary/skills/incoming/outgoing payload 形状处理。
     Combatant Drilldown Web 现在只渲染对象列表中的 skill/side rows,
     避免字符串 payload 造成技能行或侧边列表渲染中断。

  2) `web/act_skill_drilldown.html` 修复 summary/timeline_refs/filters payload 形状处理。
     Skill Drilldown Web 现在只渲染对象 timeline refs 并归一 payload 展开内容,
     避免坏 payload 造成 timeline 渲染中断或错误引用数量。

## v4.5.79: ACT Web 列表 payload 防护修复.

  1) `web/act_action_log.html` 修复 rows/grouped_rows/errors payload 形状处理。
     Action Log Web 现在只统计和渲染真正的列表与对象行,
     避免字符串 payload 造成表格渲染中断或错误数量。

  2) `web/act_death_recap.html` 修复 rows/summary/death/window/errors payload 形状处理。
     Death Recap Web 现在对列表与对象 payload 做显式归一,
     避免坏 payload 造成死亡回放渲染中断或错误数量。

## v4.5.78: ACT Drilldown Tk payload 计数修复.

  1) `gui_modules/sao_gui_combatant_drilldown.py` 修复 skills/errors payload 计数。
     Combatant Drilldown Tk 现在只统计真正的列表并只渲染 Mapping skill/outgoing 条目,
     避免字符串 payload 显示错误技能数或打断行渲染。

  2) `gui_modules/sao_gui_skill_drilldown.py` 修复 timeline_refs/errors payload 计数。
     Skill Drilldown Tk 现在只统计和渲染 Mapping timeline ref 条目,
     避免字符串 payload 显示错误引用数或打断 timeline 渲染。

## v4.5.77: Entity 插件菜单 payload 防护修复.

  1) `gui_modules/sao_gui_menu_mixin.py` 修复插件菜单列表 payload 形状处理。
     插件菜单、插件状态弹窗和首插件切换现在只消费 Mapping 插件条目,
     避免字符串或坏条目造成菜单构建/交互中断。

  2) `gui_modules/sao_gui_menu_mixin.py` 修复插件菜单数值 payload 归一化。
     插件 hotkey_count、active_count、plugin_count 与刷新签名现在过滤非有限或畸形数值,
     避免坏插件状态导致菜单渲染崩溃或签名异常。

## v4.5.76: Entity 菜单 ACT summary 计数修复.

  1) `gui_modules/sao_gui_menu_mixin.py` 修复 ACT 菜单 list payload 计数。
     Timeline/Action Log/Combatant/Skill 菜单摘要现在只统计真正的 list/tuple,
     避免字符串 payload 被拆成字符后显示错误数量。

  2) `gui_modules/sao_gui_menu_mixin.py` 修复 ACT 菜单 numeric payload 归一化。
     Report/Aggregate/Death/Graph/Plugin/Trigger 等菜单摘要和刷新签名现在过滤非有限或畸形数值,
     避免坏 payload 造成菜单构建中断或签名误判。

## v4.5.75: ACT Graph Timeseries Tk points 渲染修复.

  1) `gui_modules/sao_gui_graph_timeseries.py` 修复 points payload 形状处理。
     图表面板现在只统计和渲染列表中的 Mapping 点位,
     避免字符串 payload 被拆成字符后在 latest 计算中崩溃。

  2) `gui_modules/sao_gui_graph_timeseries.py` 修复图表数值渲染。
     time/range、row_count、bar width、point ratio、Action Log 跳转时间和 `_fmt`
     现在过滤非有限数, 避免异常点位造成渲染中断或显示 `inf` 文本。

## v4.5.74: ACT Aggregate Tk 数值渲染修复.

  1) `gui_modules/sao_gui_act_aggregate.py` 修复 raw_counts 数值渲染。
     聚合摘要、Header metrics 和空状态判断现在统一过滤坏 counts,
     避免异常 payload 造成面板渲染中断。

  2) `gui_modules/sao_gui_act_aggregate.py` 修复聚合值格式化。
     分组值/count、趋势预览 points、持续时间和 `_fmt` 现在过滤非有限数,
     避免 `nan`/`inf` 在 Aggregate Tk 面板中崩溃或显示异常文本。

## v4.5.73: ACT Death Recap Tk payload 渲染修复.

  1) `gui_modules/sao_gui_death_recap.py` 修复 rows payload 形状处理。
     Death Recap Tk 现在只统计和渲染列表中的 Mapping 行,
     避免字符串/dict payload 被拆成字符或键后显示错误事件数。

  2) `gui_modules/sao_gui_death_recap.py` 修复 summary 数值渲染。
     伤害、治疗、death event、relative time 和格式化数值现在过滤非有限值,
     避免异常 ACT payload 造成面板渲染中断。

## v4.5.72: ACT Timeline Tk 数值边界修复.

  1) `gui_modules/sao_gui_timeline_vcr.py` 修复 VCR speed 控制参数。
     播放与设置速度现在复用有限数夹取到 0.1..8,
     避免 `nan`/`inf` 从 Tk 输入直传到底层 timeline runtime。

  2) `gui_modules/sao_gui_timeline_vcr.py` 修复 Timeline 数值渲染。
     cursor、speed、聚合桶值/count 与事件 value 现在过滤非有限数,
     避免异常 ACT payload 造成 Tk 面板渲染中断或显示 `inf` 文本。

## v4.5.71: ACT Tk 报表历史参数归一化修复.

  1) `gui_modules/sao_gui_report_export.py` 修复 Report Export 预览与历史操作数值防护。
     报表预览总伤害和历史载入/删除索引现在统一归一到有限非负整数,
     避免异常 ACT payload 或手动调用导致面板渲染中断。

  2) `gui_modules/sao_gui_offline_import.py` 修复 Offline Import 历史载入索引防护。
     历史回放按钮和直接调用现在夹取到有限非负整数,
     避免坏 `_history_index` 或外部参数阻断导入历史回放。

## v4.5.70: ACT Timeline 与历史报表 fallback 参数修复.

  1) `web/act_timeline_vcr.html` 修复 Timeline fallback 数值参数。
     timeline status/step/seek payload 现在夹取 limit、delta_ms 与 cursor_ms,
     避免异常参数导致跳转范围或单步跨度失控。

  2) `web/act_report_export.html` 与 `web/act_offline_import.html` 修复历史 limit。
     report/history/offline-import fallback payload 现在夹取 limit 到安全范围,
     避免坏 limit 进入报表历史查询。

## v4.5.69: ACT Drilldown 与 Graph fallback 参数修复.

  1) `web/act_skill_drilldown.html` 修复 fallback limit 参数。
     bridge fallback payload 现在夹取 `limit`,
     避免异常调用把非有限或过大 limit 透传到 ACT skill 查询。

  2) `web/act_graph_timeseries.html` 修复 Graph fallback 数值参数。
     graph status/filter/export/zoom payload 现在夹取 limit 与时间范围,
     避免坏 query 参数导致渲染查询范围异常。

## v4.5.68: Raid Editor 反应匹配与机制预设参数修复.

  1) `web/raid_editor.html` 修复 Boss reaction mapping ID 比较。
     boss base id / skill id 现在通过有限数 helper 比较,
     避免坏 ID 值进入 `NaN` 比较导致误匹配或漏匹配。

  2) `web/raid_editor.html` 修复机制 dash 预设识别。
     sequence 的 hold/delay 毫秒值现在有限化后再比较,
     避免坏 `hold_ms` 被 `!Number(...)` 误当成 0。

## v4.5.67: Boss HP 碎片 wave 与监听槽位参数修复.

  1) `web/boss_hp.html` 修复碎片特效 wave 参数夹取。
     break/shield shards 现在把 wave 归一化到有限小范围,
     避免坏值导致碎片不渲染或异常生成过多节点。

  2) `web/menu.html` 修复 watched slots 保存/恢复参数。
     槽位 ID 现在统一归一化到 1..9,
     避免 `NaN` 或字符串槽位破坏桥接 payload 与恢复高亮。

## v4.5.66: DPS resize 与 HP burst 槽位参数修复.

  1) `web/dps.html` 修复详情面板 resize 参数夹取。
     resize grip 的宽高 clamp 现在复用有限数 helper,
     避免异常鼠标/窗口值把 `NaN` 传给 `resize_dps`。

  2) `web/hp.html` 修复 Burst Ready 锚点槽位解析。
     burst slot index 现在通过有限数 helper 夹到 1..9,
     避免坏 slot 参数导致目标锚点取错或渲染位置异常。

## v4.5.65: ACT 时间工具与聚合时长渲染修复.

  1) `web/act_panel_util.js` 修复共享时间格式化数值边界。
     `fmtClock`、`fmtDur`、`fmtRel` 和 `fmtSigned` 现在统一过滤非有限数并夹取范围,
     避免坏时间字段在多个 ACT Web 面板中显示异常时间文本。

  2) `web/act_aggregate.html` 修复聚合分组持续时长渲染。
     group duration/span 现在使用有限数差值并夹到一天内,
     避免异常 `duration_ms` 或首末时间戳导致聚合行显示不合理跨度。

## v4.5.64: pywebview shim ACT 查询参数修复.

  1) `web/pywebview-shim.js` 修复 Action Log 查询参数边界。
     status/search/filter/jump/copy 的 limit、cursor_ms 和 offset 现在统一夹取,
     避免异常 Web 调用把无效分页或时间游标传到桥接层。

  2) `web/pywebview-shim.js` 修复 Graph Timeseries 与 Skill Drilldown limit 参数。
     图表 time_range_ms、图表 limit 和技能明细 limit 现在使用安全范围,
     避免渲染请求过大或非有限值导致后端/前端状态异常。

## v4.5.63: pywebview shim 面板数值参数修复.

  1) `web/pywebview-shim.js` 修复通用面板数值参数边界。
     拖拽位移、history/report/death-recap/timeline/aggregate 的 limit、index、window、
     cursor 和 step 参数现在统一使用有限数兜底和上下限夹取。

  2) `web/pywebview-shim.js` 修复 MEM scope 搜索对齐参数。
     `mem_search()` 的 align 参数现在夹到安全范围,
     避免直接 shim 调用把异常对齐值传给内存搜索桥接层。

## v4.5.62: 排行榜与音量 shim 数值修复.

  1) `web/menu.html` 修复排行榜数值标签渲染。
     rank、level、XP、曲数和演奏时长现在统一有限化/夹取,
     搜索跳转和高亮也复用同一 rank helper, 避免坏排行 payload 污染可见 UI。

  2) `web/pywebview-shim.js` 修复音量参数传递。
     `set_sound_volume()` 现在使用有限数兜底并夹到 0..100,
     避免直接调用 shim 时把越界音量传给桥接层。

## v4.5.61: DPS shim 与 BossRaid 运行态数值修复.

  1) `web/pywebview-shim.js` 修复 DPS 实体详情 UID 参数传递。
     `get_entity_detail(uid)` 现在保持字符串 UID 传给 `dps.entity_detail`,
     避免大 UID 被 JavaScript Number 截断后详情查不到或串到错误实体。

  2) `web/menu.html` 修复 BossRaid 运行态摘要渲染。
     elapsed、DPS、DMG、HP 百分比和狂暴倒计时现在使用既有夹取 helper,
     避免异常 runtime 状态把 `Infinity`/`NaN` 显示到主菜单。

## v4.5.60: Trigger Timer 与主菜单数值状态修复.

  1) `web/trigger_timer_manager.html` 修复触发器摘要和规则数值渲染。
     规则数、计时数、重载时间、错误数、测试事件数、阈值和冷却秒数现在统一有限化,
     避免坏状态 payload 在面板中显示 `Infinity` 或 `NaN`。

  2) `web/menu.html` 修复主菜单摘要数值渲染。
     Session Players 总数、AutoKey 动作/启用计数和职业 ID 现在使用既有夹取 helper,
     避免异常配置或桥接状态污染可见 UI。

## v4.5.59: Raid Editor 与 DPS Web 数值状态修复.

  1) `web/raid_editor.html` 修复机制面板可见数值渲染。
     狂暴时间、收件箱技能 ID、观测次数和施法耗时现在统一夹取为有限整数,
     避免坏 payload 在机制列表中显示 `NaN` 或传递非法建机制参数。

  2) `web/dps.html` 修复命中特效序号/时间戳状态。
     hit_fx seq 和 generated_at 现在使用既有非负数 helper,
     避免 `Infinity`/`NaN` 锁死后续命中特效或触发过期判断异常。

## v4.5.58: SkillFX 与 Mem Scope Web 数值渲染修复.

  1) `web/skillfx.html` 修复 burst ready 定位参数渲染。
     slot、viewport 和 callout 矩形现在统一过滤非有限数并夹到合理范围,
     避免异常 payload 把 CSS 写成 `NaNpx` 或负尺寸导致特效不可见。

  2) `web/mem_scope.html` 修复搜索进度与伤害排序数值渲染。
     search count/progress 和 damage totals 现在使用有限数兜底,
     避免扫描状态显示 `NaN%` 或坏 total 影响排序/表格渲染。

## v4.5.57: BuffMon 数值渲染与输入解析修复.

  1) `gui_modules/sao_gui_buffmon.py` 修复 BuffMon row 渲染数值处理。
     rem_s/layer/count/apply_count/uptime_pct 与 row signature 现在过滤非有限数,
     避免坏 row 让整帧 buff overlay 渲染失败或显示非法进度。

  2) `gui_modules/sao_gui_buffmon.py` 修复 self/boss buff payload 解析。
     buff id/uuid/begin/duration/layer/count/server offset 与 uptime 统计现在使用有限数兜底,
     避免单个异常 buff 包中断整批缓存更新。

## v4.5.56: ACT Action Log Tk 数值渲染修复.

  1) `gui_modules/sao_gui_action_log.py` 修复分页/摘要数值渲染。
     cursor、offset、page 和 total rows 现在过滤非有限数,
     避免异常 analytics/cursor payload 让 Action Log 面板整体不渲染。

  2) `gui_modules/sao_gui_action_log.py` 修复聚合分组数值渲染。
     group total/count/uid/time 与格式化值现在统一过滤 `NaN`/`Infinity`,
     避免异常分组数据导致分组区渲染中断或比例条异常。

## v4.5.55: 插件 Tk 面板数值渲染修复.

  1) `gui_modules/sao_plugin_ui_render.py` 修复插件声明式 UI 数值渲染。
     bar pct、spacer 高度、canvas 尺寸和绘制 op 坐标/线宽/字号现在过滤非有限数,
     避免坏插件 spec 让 Tk 插件面板渲染中断或画布缺失。

  2) `gui_modules/sao_gui_plugin_manager.py` 修复 detached plugin panel 尺寸读取。
     显式窗口尺寸和插件 meta 的 width/height/min_width/min_height 现在使用有限整数兜底,
     避免异常插件声明导致面板窗口打不开。

## v4.5.54: ACT Tk 管理面板数值文本修复.

  1) `gui_modules/sao_gui_plugin_manager.py` 修复插件管理器计数渲染。
     plugin/active/hotkey/subscription/failure 计数现在过滤非有限数,
     避免异常插件状态让摘要或 meta 文本显示 `NaN`/`Infinity` 或抛错。

  2) `gui_modules/sao_gui_trigger_timer_manager.py` 修复触发器规则数值文本。
     threshold 与 cooldown 现在统一格式化为有限非负数,
     避免异常规则配置在 Tk 面板中显示 `NaN`/`Infinity`。

## v4.5.53: ACT Tk 钻取面板数值渲染修复.

  1) `gui_modules/sao_gui_combatant_drilldown.py` 修复成员钻取技能条渲染。
     技能 amount/hits 和百分比指标现在使用有限数 helper,
     避免异常 ACT 数据让技能条或百分比渲染中断。

  2) `gui_modules/sao_gui_skill_drilldown.py` 修复技能钻取摘要与事实卡渲染。
     casts/hits、数值文本和百分比文本现在统一过滤 `NaN`/`Infinity`,
     避免坏数据导致 Tk 面板显示不全或抛出转换异常。

## v4.5.52: Web 主菜单配置与更新器数值修复.

  1) `web/menu.html` 修复菜单配置回填的音量值。
     `sound_volume` 从配置恢复时现在夹到 0..100,
     避免异常配置让滑块显示越界或非法值。

  2) `web/menu.html` 修复更新器包大小与下载进度显示。
     `size` 和 `progress` 现在过滤非有限数并限制到合理范围,
     避免更新提示显示 `Infinity MB`、`NaN%` 或越界进度条。

## v4.5.51: Web 主菜单运行时数值归一化修复.

  1) `web/menu.html` 修复 Info 面板 XP 条渲染。
     `xp_pct` 现在先归一化为有限数并夹到 0..100,
     避免异常状态 payload 生成 `NaN%` 或越界宽度。

  2) `web/menu.html` 修复音量滑块桥接参数。
     `set_sound_volume` 现在传递 0..100 的整数并同步回输入框,
     避免非法输入把 `NaN` 或越界值交给后端。

## v4.5.50: Web AutoKey 与机制横幅数值渲染修复.

  1) `web/autokey_editor.html` 修复技能 CD 条和槽位参数渲染。
     cooldown_pct、remaining_ms、charge_count 和 slot index 现在先归一化,
     避免异常槽位数据生成 `NaN%`、越界 CD 条或 raw inline click 参数。

  2) `web/mech_banner.html` 修复机制横幅倒计时进度渲染。
     countdown/remaining/pre-warn 毫秒值现在过滤非有限数并避免除以 0,
     避免横幅进度条宽度和倒计时显示 `NaN`/`Infinity`。

## v4.5.49: Web HUD 数值条与反应标记渲染修复.

  1) `web/hp.html` 与 `web/stamina.html` 修复 HP/STA 条宽和文本渲染。
     HP/STA current/max 现在先归一化为有限非负数, 条宽统一夹到 0..100,
     避免异常识别数据生成 `NaN%`、越界宽度或 `NaN/NaN` 文本。

  2) `web/raid_editor.html` 修复反应时间线 badge 数值渲染。
     血线百分比与定时秒数现在使用既有 clamp helper 过滤非有限数和越界值,
     避免 badge 显示 `NaN%`、`Infinitys` 或不合理百分比。

## v4.5.48: Web 小面板条形数值渲染修复.

  1) `web/raid_editor.html` 修复实体 HP 条渲染。
     实体当前 HP/max HP 现在先归一化为有限非负数, HP 百分比夹到 0..100,
     避免异常实体数据生成 `NaN%`、越界宽度或异常伤害文本。

  2) `web/buff_coverage.html` 修复 Buff 覆盖率与时间渲染。
     uptime、剩余秒数、elapsed、层数和触发次数现在统一过滤非有限数,
     避免覆盖率条宽、倒计时和计数显示 `NaN`/`Infinity`。

## v4.5.47: Commander Web 数值渲染修复.

  1) `web/commander.html` 修复成员 HP 小条渲染。
     HP/max HP 现在先归一化为有限非负数, 再计算 0..1 比例,
     避免异常 Commander 数据渲染出 `NaN%` 或无效条宽。

  2) `web/commander.html` 修复 Boss tab Dungeon ID 判断与显示。
     Dungeon ID 现在必须是有限正整数才进入 ACTIVE 状态并显示,
     避免 `Infinity`、`NaN` 或异常字符串让面板误判为已进入副本。

## v4.5.46: Commander Tk 数据传递与渲染修复.

  1) `gui_modules/sao_gui_commander.py` 修复 Commander Tk 面板签名与渲染数值归一化。
     成员 UID/战力/等级/HP、技能槽位、冷却百分比和剩余时间现在进入签名前统一过滤坏值,
     避免 `NaN`/`Infinity` 或非数字字符串中断面板刷新。

  2) `gui_modules/sao_gui_panels_mixin.py` 修复 Commander 数据 push 去重签名。
     `_push_commander_data()` 现在复用面板侧安全签名 helper,
     避免某个成员或技能槽的异常数值被吞掉后导致整次 Commander 数据不再传给面板。

## v4.5.45: Tk Live 面板签名与数值渲染修复.

  1) `gui_modules/sao_gui_bossraid.py` 修复 BossRaid Tk 实体列表渲染签名。
     实体名称现在纳入签名, 名称解析或目标切换后即使 HP/伤害不变也会重绘,
     避免面板继续显示旧 Boss/实体名。

  2) `gui_modules/sao_gui_bossraid.py` 与 `gui_modules/sao_gui_autokey.py` 修复非有限数值处理。
     HP 百分比、DPS、阶段、技能冷却、剩余时间和充能数现在统一过滤 `NaN`/`Infinity` 并夹到可渲染范围,
     避免轮询期间重复整页重绘、条形宽度异常或 Tk 渲染中断。

## v4.5.44: Boss HP 主条数值渲染修复.

  1) `web/boss_hp.html` 修复主 Boss HP/Shield 百分比和文本渲染。
     主血量百分比、packet HP 文本、护盾百分比与护盾 ghost 宽度现在统一走有限数钳制,
     避免异常运行时数据渲染出 `NaN%`、`NaN/NaN` 或错误 class 状态。

  2) `web/boss_hp.html` 修复破防条数值和阶段状态传递。
     extinction/max extinction、预计算 extinction_pct 和 breaking_stage 现在统一规整,
     避免 `NaN` 进入破防条宽度、文本和状态机。

## v4.5.43: Web 菜单运行时数值参数修复.

  1) `web/menu.html` 修复 DPS idle timeout 控件传参。
     菜单中的 DPS 空闲隐藏/结算秒数现在夹到 0..120 并回写控件显示,
     避免负数或异常值传入设置接口。

  2) `web/menu.html` 与 `web/pywebview-shim.js` 修复运行时 API 数值转发。
     Boss/AutoKey 联动全局 CD 在菜单侧夹到 0..60,
     WebView2 shim 的 `set_dps_fade_timeout()` 也会把直接调用夹到 0..120。

## v4.5.42: Web 菜单表单数值参数修复.

  1) `web/menu.html` 修复 AutoKey 编辑器整数表单。
     职业 ID、引擎 tick、槽位、按键次数和各类延迟现在按字段使用同一套有限整数范围,
     避免负数、`NaN` 或超大值写入草稿配置。

  2) `web/menu.html` 修复 BossRaid 阶段/时间线数值表单。
     阶段触发值、时间点、重复间隔、预警、持续时间和 HP 条件值现在统一夹为非负有限数,
     HP/破灭百分比输入额外限制在 0..100。

## v4.5.41: DPS Web 数值渲染修复.

  1) `web/dps.html` 修复 DPS 列表数值与条宽渲染。
     列表总量、速率、百分比和条形宽度现在统一过滤为有限非负数,
     避免异常 ACT/MEM 数据显示 `NaN%`、`Infinity` 或生成越界宽度。

  2) `web/dps.html` 修复 DPS 详情技能行渲染。
     技能排序、伤害/治疗量、命中数、暴击率、占比和技能条宽度现在统一钳制,
     防止坏数据导致技能条溢出、消失或显示异常百分比。

## v4.5.40: Raid Editor/Boss HP 数值渲染修复.

  1) `web/raid_editor.html` 修复 Boss 反应与机制编辑表单的数值输入。
     反应延迟/冷却、TTS 音量、自动躲避提前量/按住/序列延迟、
     移动超时与距离参数现在统一规范为有限数并夹到运行时范围,
     避免 `Infinity` 被 JSON 写成 `null` 或负数/超大值进入运行时。

  2) `web/boss_hp.html` 修复附属单位小条渲染。
     附属单位名称现在转义后再写入 HTML, HP/破防小条宽度统一夹到 0..100%,
     避免异常数据造成渲染溢出、`NaN%` 或文本破坏布局。

## v4.5.39: ACT Timeline VCR 速度参数修复.

  1) `web/act_timeline_vcr.html` 修复播放速度输入。
     Timeline VCR 的 play/set speed 现在统一使用 `safeSpeed()` 夹到 0.1..8x,
     避免负速、`NaN` 或超大速度造成播放状态和显示异常。

  2) `web/pywebview-shim.js` 与 `sao_webview.py` 修复 Timeline speed 桥接参数。
     WebView2 shim 和 Python Web API 也会夹取速度参数, 防止绕过页面直接调用时
     把异常值传进 runtime 或触发 float 转换错误。

## v4.5.38: ACT Action Log/Death Recap 数值参数修复.

  1) `web/act_action_log.html` 修复 cursor、offset 和分页参数规范化。
     Action Log 现在用统一 helper 把 cursor/page offset/limit 夹成有限非负整数,
     避免 `NaN`、负数或异常输入导致翻页卡住或把脏参数传入行为日志接口。

  2) `web/act_death_recap.html` 修复死亡回放窗口秒数传参。
     Death Recap 的 refresh/copy 和 bridge fallback 现在统一使用正数 window helper,
     避免空值、负数或异常输入生成错误回放窗口。

## v4.5.37: ACT Graph Timeseries 数值渲染与跳转参数修复.

  1) `web/act_graph_timeseries.html` 修复时间序列点位数值渲染。
     图表绘制、最新值和点位列表现在统一使用有限数 helper 规整
     `time_ms/value`, 避免异常数据生成 `NaN` 坐标导致曲线断绘或列表显示异常。

  2) `web/act_graph_timeseries.html`、`web/pywebview-shim.js`、`sao_webview.py`
     与 `act_platform/runtime.py` 修复 Action Log 跳转的 topic 传参。
     从图表点钻取到行为日志时会把点位 topic 作为可选过滤参数一路传递,
     旧调用未传 topic 时仍保持原过滤状态。

## v4.5.36: ACT 历史报告索引传参修复.

  1) `web/act_offline_import.html` 修复离线导入历史加载索引。
     历史按钮和 `loadHistory()` API 调用现在统一使用有限非负整数索引,
     避免 `NaN`、`Infinity` 或负数进入历史加载接口。

  2) `web/act_report_export.html` 修复报告历史载入/删除索引。
     历史行、bridge payload 以及直接 pywebview 调用都会通过
     `safeHistoryIndex()`, 保证载入/删除操作传递稳定索引。

## v4.5.35: ACT Web 动态样式渲染修复.

  1) `web/act_timeline_vcr.html` 修复事件 topic class 渲染。
     时间线事件不再把原始 topic 文本直接拼成 CSS class,
     改用安全分类 helper, 并让 damage/death 类事件稳定使用红色强调样式。

  2) `web/act_aggregate.html` 修复聚合条形宽度渲染。
     section row 与 graph preview 的 bar width 现在通过 `pctWidth()`
     夹到 0..100%, 避免负数、NaN 或异常 ratio 造成条形溢出/消失。

## v4.5.34: ACT Drilldown 数值渲染夹取修复.

  1) `web/act_combatant_drilldown.html` 修复技能条宽度计算。
     技能 amount 现在先转成有限非负数, bar 宽度会夹在 0..100%,
     避免异常/负数/NaN 数据让技能条溢出或不显示。

  2) `web/act_combatant_drilldown.html` 与 `web/act_skill_drilldown.html`
     修复百分比标签格式化。
     crit/share 等百分比现在会先转有限数并夹到 0..1,
     避免显示 `NaN%` 或异常大百分比。

## v4.5.33: BossRaid 菜单数字渲染与转义修复.

  1) `web/menu.html` 增强 `_escHtml()`。
     该 helper 现在会先转字符串并转义引号, 避免数字字段调用时报错,
     也避免被复用于 input/value 或 data 属性时漏掉引号转义。

  2) `web/menu.html` 修复 BossRaid 本地/云端卡片与编辑器数字字段渲染。
     HP、Enrage、阶段/时间线计数以及阶段/时间线 number input 的 value
     现在会先做有限数字规范化和属性转义, 避免导入或云端异常数据破坏卡片和输入框。

## v4.5.32: Raid Editor 配置渲染转义修复.

  1) `web/raid_editor.html` 修复阶段触发文本渲染。
     trigger type/value 现在会在进入 phase card 前 HTML escape,
     避免导入或异常配置让阶段列表内容截断或注入额外标签。

  2) `web/raid_editor.html` 修复机制卡颜色渲染。
     机制 `color` 现在必须通过十六进制 CSS color 白名单,
     非法或异常值会回退到默认蓝色, 避免 inline style 被污染导致卡片渲染异常。

## v4.5.31: Commander Web 渲染安全与技能槽显示修复.

  1) `web/commander.html` 修复队伍成员与副本信息的 HTML 拼接。
     member `data-uid`、等级、战力和 Dungeon ID 现在都会先格式化/转义,
     避免异常字段导致属性截断、乱码或内容渲染不全。

  2) `web/commander.html` 修复技能 CD 槽状态、索引和剩余时间渲染。
     slot state 现在限定为 `ready/cooldown/active`, CD 百分比会夹到 0..1,
     index/time/title 统一转义, 避免非法状态类名、NaN 时间或异常文本破坏格子。

## v4.5.30: Web 64-bit 实体 ID 保真修复.

  1) `web/raid_editor.html` 修复实体 role 切换的 UUID 传参。
     `toggleRole` 现在使用字符串 UUID 比较和调用 `set_entity_role`,
     不再把 64-bit UUID 作为 JS 数字传递导致精度损失或目标错位。

  2) `web/dps.html` 修复 DPS 列表/详情的 UID 处理。
     列表 `data-uid`、详情点击、实时详情缓存、命中特效行定位和 `get_entity_detail`
     统一使用字符串 UID key, 避免大 UID 被 `Number(...)` 截断。

## v4.5.29: ACT Web 管理面板点击参数转义修复.

  1) `web/plugin_manager.html` 修复插件卡片 action 按钮的 `plugin.id` 传参。
     `enable/disable/reload/pin/uninstall` 的 inline `onclick` 现在使用 HTML-escaped JSON 参数,
     插件 ID 中包含引号、反斜杠或实体文本时不再破坏按钮动作。

  2) `web/trigger_timer_manager.html` 修复触发/计时规则卡片按钮的 `rule.id` 传参。
     `enable/disable/test` 的 inline `onclick` 现在同样先转义 JSON 字符串,
     避免自定义规则 ID 导致点击无效或参数截断。

## v4.5.28: Web 菜单属性转义与排行榜渲染修复.

  1) `web/menu.html` 移除重复的弱版 `_escAttr()` 定义。
     AutoKey/BossRaid profile 点击参数和 Boss↔AutoKey 联动输入框现在共用同一个完整属性转义 helper,
     避免后定义覆盖导致 `&quot;` 等实体文本再次破坏 inline handler 参数。

  2) `web/menu.html` 修复排行榜行的 rank、level 和统计值渲染。
     这些字段现在先生成安全 label 再 HTML escape, 避免异常排行榜数据把行内容截断或注入到 `innerHTML`。

## v4.5.27: Web 菜单 Profile 点击参数转义修复.

  1) `web/menu.html` 修复 AutoKey 本地配置卡与云端下载按钮的 inline `onclick` 参数拼接。
     profile id 现在统一通过 `_jsAttrArg()` 做 JSON 编码和 HTML 属性转义,
     避免包含引号、反斜杠或特殊字符时编辑、启用、复制、导出、删除和下载动作失效。

  2) `web/menu.html` 修复 BossRaid 本地配置卡与云端下载按钮的 profile id 传递。
     本地选择/激活/编辑和云端下载都使用同一套安全参数编码, 保持复杂 ID 的完整传递。

## v4.5.26: ACT Web 点击参数转义修复.

  1) `web/act_combatant_drilldown.html` 修复技能行 `onclick` 的 `skill_id` 拼接。
     字符串技能 ID 即使包含引号或反斜杠也会通过 `jsArg()` 完整传给
     `open_skill_drilldown`, 避免点击技能钻取失效或参数截断。

  2) `web/act_graph_timeseries.html` 修复图表点 `onclick` 的 topic 拼接和点列表文本转义。
     自定义 topic/label 出现引号时不再破坏 inline handler, 点列表也统一 HTML escape。

## v4.5.25: ACT 面板可见字段刷新修复.

  1) `gui_modules/sao_gui_action_log.py` 补全 Action Log 渲染签名。
     分组名称、类型、UID 数、时间范围、地牢、展开明细、RAW 行的 actor/target/source
     以及页码/错误徽标等可见状态变化都会触发重绘, 避免同 key/count/value 时界面停在旧内容。

  2) `gui_modules/sao_gui_death_recap.py` 补全 Death Recap 渲染签名。
     summary 指标、死亡对象、窗口范围、错误徽标、行 actor/target/label 与展开 payload
     变化时都会刷新列表和指标卡, 避免死亡回放显示不完整或旧数据残留。

## v4.5.24: Selftest 直接运行路径修复.

  1) `tools/_bootstrap.py` 补齐顶层 `tools/*_selftest.py` 的直接运行路径。
     不再需要手动设置 `PYTHONPATH=.` 才能导入 `act_platform`、`engines`、
     `gui_modules` 或 `config`, 避免批量自测误报 ModuleNotFoundError。

  2) `tools/tablekit/_bootstrap.py` 补齐嵌套 `tools/tablekit/*_selftest.py` 的直接运行路径。
     `live_name_crossref_selftest.py` 可从 repo 根直接运行并导入 `tools.tablekit.*`。

## v4.5.23: Web 编辑器乱码与 shim 重复 API 修复.

  1) `web/autokey_editor.html` 和 `web/raid_editor.html` 清理用户可见 mojibake。
     AutoKey 编辑器的 Burst/Recording/状态栏标签恢复为有效 HTML,
     Raid 编辑器阶段按钮、实体分隔符和触发条件比较符恢复可读文本。

  2) `web/pywebview-shim.js` 移除重复的 `export_last_report` 定义。
     避免后定义覆盖前定义导致 normalize 行为丢失, 并新增 shim API 唯一性自测。

## v4.5.22: ACT Action Log 兜底名称与退出清理修复.

  1) `act_platform/runtime.py` 修复 Action Log 未解析怪物/目标的显示兜底。
     未知怪物现在恢复为 `怪物#完整UID`, 方便复制搜索和跨面板定位,
     同时仍不会写入 `name_resolution` 作为真实名字解析证据。

  2) `gui_modules/sao_gui_lifecycle_mixin.py` 修复关闭流程对可选面板属性的裸访问。
     当 Mem Scope 或部分 ACT 子面板从未初始化时, 退出清理不再因 AttributeError 中断,
     GPU 子窗口销毁、退出叠层清理和 root.quit 会按顺序完成。

## v4.5.21: WebView2 BossRaid 云端上传桥接修复.

  1) `web/pywebview-shim.js` 补齐最后一个动态 BossRaid 缺口:
     `upload_boss_raid_profile(id)` 转发到 C# `bossraid.cloud.upload`。
     当菜单只传 profile id 时, C# 会读取本地 active/指定 profile, 自动刷新或复用
     内存上传 token, 上传成功后持久化 `source=uploaded` 与 `remote_id`,
     并返回完整 BossRaid 菜单状态。

## v4.5.20: WebView2 BossRaid 云端搜索与上传凭证桥接修复.

  1) `web/pywebview-shim.js` 补齐 `search_boss_raid_remote(query)`,
     转发到 C# `bossraid.cloud.search`。
     C# 搜索响应现在兼容服务端 `results/items` 两种字段, 返回顶层 `results`,
     并回填完整 BossRaid 菜单状态以刷新云端结果列表。

  2) `web/pywebview-shim.js` 补齐 `refresh_boss_raid_upload_auth(force)`,
     C# `BossRaidCloudBridge` 新增 `bossraid.cloud.refresh_upload_auth`。
     刷新上传凭证现在使用当前角色 identity 调 issue-token, token 仅留在 bridge 内存,
     UI 状态只返回 masked token、过期时间、mode 和 identity。

## v4.5.19: WebView2 BossRaid 导出与远端下载桥接修复.

  1) `web/pywebview-shim.js` 补齐 `export_boss_raid_profile(id)`,
     C# `MenuStateBridge` 注册 `bossraid.export`。
     BossRaid 本地配置现在能导出到 `exports/boss_raids`, 并返回导出路径给菜单提示。

  2) `web/pywebview-shim.js` 补齐 `download_boss_raid_remote(id)`,
     C# `BossRaidCloudBridge` 新增 `bossraid.cloud.download`。
     云端下载会调用远端 get, normalize 为 `downloaded` profile, 处理重复 ID,
     持久化到本地配置并返回完整菜单状态。

## v4.5.18: WebView2 BossRaid 运行启动/停止桥接修复.

  1) `web/pywebview-shim.js` 补齐 `boss_raid_start()`,
     C# `BossRaidRuntimeBridge` 新增 `bossraid.start`。
     WebView2 BossRaid 开始按钮现在会读取 active profile, 自动启用 BossRaid 配置,
     启动真实 `BossRaidEngine`, 并把 `runtime.state=running` 回填到菜单状态。

  2) `web/pywebview-shim.js` 补齐 `boss_raid_stop()`,
     C# `BossRaidRuntimeBridge` 新增 `bossraid.stop`。
     停止按钮现在会停止真实运行态，并返回 `runtime.state=idle` 供前端恢复按钮文案。

## v4.5.17: WebView2 BossRaid 本地配置保存与删除修复.

  1) `web/pywebview-shim.js` 补齐 `save_boss_raid_profile(profile)`,
     C# `MenuStateBridge` 新增 `bossraid.profile.save`。
     BossRaid 编辑器保存现在会 normalize 并持久化 profile, 保留既有 `created_at`,
     并返回完整菜单状态。

  2) `web/pywebview-shim.js` 补齐 `delete_boss_raid_profile(id)`,
     C# `MenuStateBridge` 新增 `bossraid.profile.delete`。
     删除当前 active profile 时会按现有存储逻辑回落到剩余 profile。

## v4.5.16: WebView2 BossRaid 本地配置创建与激活修复.

  1) `web/pywebview-shim.js` 补齐 `activate_boss_raid_profile(id)`,
     C# `MenuStateBridge` 新增 `bossraid.profile.set_active`。
     BossRaid 本地配置卡片点击现在会真实持久化 active profile 并返回完整菜单状态。

  2) `web/pywebview-shim.js` 补齐 `create_boss_raid_profile()`,
     C# `MenuStateBridge` 新增 `bossraid.profile.create`。
     BossRaid 新建按钮现在会创建并激活默认 profile, 修复 WebView2 菜单无反馈的问题。

## v4.5.15: WebView2 BossRaid 导入入口与启用开关修复.

  1) `web/pywebview-shim.js` 补齐 `start_boss_raid_import_picker()`,
     C# `FilePickerBridge` 新增 `bossraid.import_picker.start`。
     BossRaid 菜单导入按钮现在能打开文件选择器, 并把后续 `select_file`
     正确路由为 BossRaid profile 导入。

  2) `web/pywebview-shim.js` 补齐 `set_boss_raid_enabled(enabled)`,
     C# `MenuStateBridge` 新增 `bossraid.set_enabled`。
     BossRaid 启用开关现在会持久化 `boss_raid.enabled`, 并返回完整菜单状态。

## v4.5.14: WebView2 BossRaid 实体角色与菜单阶段按钮修复.

  1) C# `BossRaidEngine` 补齐 Python 对齐的 `SetEntityRole(uuid, role)`,
     WebView2 `set_entity_role()` 现在会真正切换 tracked entity 的 boss/enemy
     角色, 并在指定新 boss 时降级旧 boss。

  2) `web/pywebview-shim.js` 补齐菜单动态调用使用的
     `boss_raid_next_phase()` / `boss_raid_reset()` 兼容别名。
     `menu.html` 的 BossRaid NEXT PHASE / RESET 按钮不再因为字符串 API 名不匹配失效。

## v4.5.13: WebView2 Raid Editor 阶段推进与重置桥接修复.

  1) `web/pywebview-shim.js` 补齐 `raid_next_phase()` 并新增
     `bossraid.runtime.next_phase` C# 桥接。
     Raid Editor 按钮现在会调用真实 `BossRaidEngine.NextPhase()`, idle 状态保持
     no-op 成功语义, 不再因为缺 API 静默失效。

  2) `web/pywebview-shim.js` 补齐 `raid_reset()` 并新增
     `bossraid.runtime.reset` C# 桥接。
     Raid Editor Reset 现在调用真实 `BossRaidEngine.Stop()`, 并返回当前 runtime 状态。

## v4.5.12: WebView2 本地文件导入与 AutoKey 录制动作保存修复.

  1) `web/pywebview-shim.js` 补齐 `select_file(path)` 并新增
     `file.select_file` C# 桥接。
     WebView2 自带文件浏览器选中文件后现在会按 `consumer` 真实导入 AutoKey/BossRaid
     profile, 持久化 settings, 并返回刷新后的菜单状态, 修复选择文件后无后续处理的问题。

  2) `web/pywebview-shim.js` 补齐 `save_autokey_actions(actions_json)` 并新增
     `autokey.actions.save` C# 桥接。
     录制面板动作现在会写入 `autokey_burst_actions`; C# 返回
     `live_reconfigured:false`, 不伪造运行中 runtime 已热更新。

## v4.5.11: WebView2 编辑器切换按钮兼容修复.

  1) `web/pywebview-shim.js` 补齐 `toggle_autokey_editor()`。
     WebView2 路径下优先打开 `menu.html` 内置 AutoKey Editor tab, 并继续上报
     `ui.menu_action` 便于 native 侧后续接真 overlay, 修复按钮无反馈的问题。

  2) `web/pywebview-shim.js` 补齐 `toggle_raid_editor()`。
     WebView2 路径下优先打开内置 BossRaid Editor tab, 同时保留 `ui.menu_action`
     观测事件, 修复可视化面板按钮静默失效的问题。

## v4.5.10: WebView2 AutoKey/BossRaid 菜单状态桥接修复.

  1) `web/pywebview-shim.js` 补齐 `get_auto_key_state()` 并新增
     `autokey.state.get` C# 桥接。
     修复 WebView2 路径下 AutoKey 菜单/编辑器启动时拿不到完整状态,
     导致 profiles_full、active_profile、identity、upload_auth 与远端搜索缓存无法同步的问题。

  2) `web/pywebview-shim.js` 补齐 `get_boss_raid_state()` 并新增
     `bossraid.state.get` C# 桥接。
     修复 BossRaid 菜单/编辑器启动状态缺失问题, 并沿用手写 JSON 序列化保留
     `time_s` 等 Python 兼容字段, 避免时间轴渲染字段漂移。

## v4.5.9: 内存冷定位全面 Cython 化 + 共享类索引 + TCP 锚点提速.

  目标: 把所有"冷定位"扫堆大计算下放 Cython, 给定位结果加缓存与快速重读,
  并用 TCP 已知语义值缩小扫描范围, 显著降低混合/内存模式下的 CPU 占用。功能不减。

  1) 新增 Cython 内核 (`mem_probe/_sao_cy_memscan.pyx`, 已重编 .pyd):
     - `collect_aligned_u64_in_range(buf, lo, hi)`: nogil 范围指针收集+去重, 取代
       GA 类索引构建里的 numpy/逐 8 字节 Python 预过滤。
     - `decode_i32_kv_pairs(buf, vmin, vmax)`: nogil 解码 ~200 万条 (i32,i32) 对,
       取代字符串本地化池 `indexes_` 的 2M 次 Python `unpack_from` 循环。
     - `cy_memscan.py` 在扩展缺失时改为醒目告警 (避免静默退化成纯 Python 全堆扫)。

  2) 新增进程级共享类索引 `mem_probe/il2cpp/klass_index.py`:
     - 按 `(pid, ga_base)` 单例, 一遍 GA 扫服务所有 reader (合并 wanted 类名),
       消除"每个 reader 各扫一遍 GA 镜像"(同会话最多 5+ 遍 → 1 遍)。
     - 按 `game_key`(GA 首 1MB sha256) 持久化已解析 RVA: 下次启动用 1 次
       `read_u64(ga+rva)`+按名校验即命中, 不再全镜像扫; 版本变了校验不过自动重扫
       重存 (与偏移 bundle 同款自愈)。负缓存: 缺失类一会话只扫一次。
     - 接入 LiveFieldResolver / StaticResolver / EntityMgr / DamageReader /
       StringPool / ConfigTable / Camera 全部 klass 解析入口。

  3) 冷定位扫堆全部走 Cython + 零拷贝 scratch + region hint + 负结果退避:
     - `mem_entity_mgr._scan_for_mgr`: 原纯 Python `bytes.find`+逐命中 Python 解码
       (本探针最重的 Python 全堆扫) → `find_aligned_u64` AVX2 + `read_bytes_into`
       复用缓冲; 命中 region 记为暖提示; 定位失败按 1→8s 指数退避 (取代加载界面/
       无实体场景每 1s tick 重扫整堆)。
     - `mem_damage_reader.locate`: 同款退避+scratch+暖提示; 去掉 >64MB region 整段
       跳过 (改分块, 不再漏扫大区), 战斗外不再每 tick 重扫。
     - `static_resolver.find_instances` / `mem_camera_reader._locate` /
       `mem_map_name_reader._locate_scene_cfg`: 统一 scratch 零拷贝分块; camera 改
       只扫 private 区 (klass 实例不在 image/mapped); scene_cfg 改 Cython 值扫+暖提示。

  4) 缓存命中后的快速读取 (每-tick 系统调用骤减):
     - EntityMgr/Damage 的 .NET/ZDictionary 枚举从"每槽 3 笔单读 RPM"改为 entries[]
       整块一次 `read_bytes` + 本地 `struct` 解码 (每 tick ~1.5k 次系统调用 → 1 次)。
     - `mem_state_anchor._read_skill_matches_from_attr`: 技能元素 (SkillLevelId,
       Duration) 改一次 `read_u32_many` nogil 批量读 (2N 单读 → 1 批)。
     - `mem_string_pool.field_off`: FieldInfo 表整块 16KB 一次读后本地解析。

  5) TCP 语义锚点缩小扫描:
     - `mem_state_bridge` 把 TCP/provider 已知的当前场景 id 作为 `hint_scene_id`
       传入 `current_scene_id`, 让 SceneConfigMgr 值扫从 N 路集合扫变单值精确扫。

  验证: memscan 单测 12 绿; 新增 `test_klass_index`(3) / `test_damage_block_decode`(2)
  全绿; `test_mem_state_anchor`(4, 修了缺 `_init_layout` 的 fixture) / `test_auto_offsets`
  (48) / `test_cache_invalidation`(5) / `test_name_attr_decode` / `test_table_array_decode`
  / `test_table_columns` / `mem_entity_combat_selftest` / `mem_access_selftest`(19) /
  `mem_attr_selftest` / `mem_relocation_selftest` 全绿。

## v4.5.8: WebView2 更新跳过与排行榜兼容桥接修复.

  1) `web/pywebview-shim.js` 补齐 `skip_update()` 并新增 `updater.skip` C# 桥接。
     修复 WebView2 路径下更新面板 Skip 按钮缺少 API 的问题; C# 现在会把当前
     latest version 写入 `update_skipped_version`, 后续 updater tick 会跳过同版本。

  2) `web/pywebview-shim.js` 补齐 `fetch_leaderboard(sort)` 并新增
     `ui.fetch_leaderboard` C# ack。
     排行榜功能已移除, 因此保持 no-op 语义, 修复 WebView2 路径下切换排行榜 tab
     调用缺失 API 的交互问题。



## v4.5.7: WebView2 数据源设置桥接修复.

  1) `web/pywebview-shim.js` 补齐 `set_data_source(mode)` 并新增
     `settings.set_data_source` C# 桥接。
     修复 WebView2 路径下数据源模式按钮只更新页面摘要, 没有把 `mem_data_source`
     写入 settings 的问题; C# 返回 `live_reconfigured:false`, 不伪造运行中重启数据源。

  2) `web/pywebview-shim.js` 补齐 `set_component_source(component, mode)` 并新增
     `settings.set_component_source` C# ack。
     保持 Python 版 legacy no-op 语义, 修复 WebView2 路径下旧组件数据源按钮调用缺失 API
     的问题, 同时保留 component/mode 参数传递用于诊断。



## v4.5.6: WebView2 文件夹选择与 Boss HP 命中区域桥接修复.

  1) `web/pywebview-shim.js` 补齐 `select_folder(path)` 并新增
     `file.select_folder` C# 桥接。
     修复 WebView2 路径下文件选择器“使用此文件夹”按钮调用缺失 API 的问题;
     返回结构保持 Python 版 `{ok:false,message}` 行为, 不误触发未实现的文件夹导入。

  2) `web/pywebview-shim.js` 补齐 `boss_hp_hit_regions(regions)` 并新增
     `ui.boss_hp_hit_regions` C# ack。
     修复 WebView2 路径下 Boss HP 页面无法上报显示命中区域的问题, 保持 Boss HP
     当前全 click-through 行为, 同时为后续命中区域接线保留参数传递。



## v4.5.5: WebView2 AutoKey 导入选择器浏览桥接修复.

  1) `web/pywebview-shim.js` 补齐 `browse_dir(path)` 并新增
     `file.browse_dir` C# 桥接。
     修复 WebView2 路径下文件选择器点击目录时没有把路径传入后端、目录列表无法导航的问题。

  2) `web/pywebview-shim.js` 补齐 `start_auto_key_import_picker(path)` 并新增
     `autokey.import_picker.start` C# 桥接。
     修复 WebView2 路径下 Auto Key 导入按钮因为缺少 pywebview API 而无法打开文件选择器的问题;
     返回结构保持 Python 版 `{ok,browser}` 形状, 避免菜单渲染缺字段。



## v4.5.4: WebView2 云端服务器地址桥接修复.

  1) `web/pywebview-shim.js` 补齐 `set_auto_key_server_url(url)` 并新增
     `autokey.cloud.set_server_url` C# 桥接。
     修复 WebView2 路径下 Auto Key 云端服务器地址保存按钮没有把 URL 写入
     `auto_key.server_url` 的问题; 后续云端请求会从 settings 重新创建 client,
     立即使用新地址。

  2) `web/pywebview-shim.js` 补齐 `set_boss_raid_server_url(url)` 并新增
     `bossraid.cloud.set_server_url` C# 桥接。
     修复 WebView2 路径下 Boss Raid 云端服务器地址保存按钮没有把 URL 写入
     `boss_raid.server_url` 的问题; 后续云端请求同样会立即使用新地址。



## v4.5.3: WebView2 Boss 血条与 DPS 淡出设置桥接修复.

  1) `web/pywebview-shim.js` 补齐 `set_boss_bar_mode(mode)` 并新增
     `settings.set_boss_bar_mode` C# 桥接。
     修复 WebView2 路径下菜单 Boss 血条模式按钮只更新前端高亮, 没有保存
     `boss_bar_mode` 设置的问题。

  2) `web/pywebview-shim.js` 补齐 `set_dps_fade_timeout(seconds)` 并新增
     `settings.set_dps_fade_timeout` C# 桥接。
     修复 WebView2 路径下 DPS 淡出秒数没有写入 `dps_fade_timeout_s` 的问题。
     同时 C# DPS overlay pump 会读取该设置, 并修复 idle finalize 后无法发出
     `fade_out` 事件导致面板不淡出的渲染问题。



## v4.5.2: WebView2 菜单音效设置桥接修复.

  1) `web/pywebview-shim.js` 补齐 `set_sound_enabled(enabled)` 并新增
     `sound.set_enabled` C# 桥接。
     修复 WebView2 路径下菜单音效开关只更新页面控件, 没有即时更新播放器状态或保存
     `sound_enabled` 设置的问题。

  2) `web/pywebview-shim.js` 补齐 `set_sound_volume(volume)` 并新增
     `sound.set_volume` C# 桥接。
     修复 WebView2 路径下音量滑杆的数值没有传到播放器和 `sound_volume` 设置的问题。
     C# 启动音效桥时也会读取已保存的音效开关和音量。



## v4.5.1: WebView2 菜单 HUD 设置桥接修复.

  1) `web/pywebview-shim.js` 补齐 `set_watched_slots(slots)` 并新增
     `settings.set_watched_slots` C# 桥接。
     修复 WebView2 路径下技能槽位复选框只更新页面状态, 没有把 slots 数组保存到
     `watched_skill_slots` 设置的问题。

  2) `web/pywebview-shim.js` 补齐 `set_burst_enabled(enabled)` 并新增
     `settings.set_burst_enabled` C# 桥接。
     修复 WebView2 路径下 Burst 开关的布尔参数没有写入 `burst_enabled` 设置的问题。



## v4.5.0: Boss Raid 示例数据更新.

  1) 插入提交 `b851259` 更新 `assets/boss_raids/13023_噩梦P3_机制示例.json`
     并将 `APP_VERSION` 推进到 `4.5.0`。



## v4.4.58: WebView2 Alert 与 DPS 详情桥接修复.

  1) `web/pywebview-shim.js` 补齐 `alert_ok` 兼容入口。
     修复 WebView2 路径下菜单告警 OK 按钮关闭弹窗后因为 shim 缺少 no-op API
     而抛出 `TypeError` 的交互问题。

  2) `C#/src/SaoAuto.App/WebBridge/DpsBridge.cs` 与 `web/pywebview-shim.js`
     补齐 `get_entity_detail(uid)` 到 `dps.entity_detail` 的桥接。
     修复 Web DPS 详情模式无法把选中实体 UID 传到 live `DpsTracker`、技能拆分长期显示为空的问题。
     同时 `web/dps.html` 支持渲染 C# 返回的 `name` 技能名字段。



## v4.4.57: WebView2 BuffMon 与更新器 shim 桥接修复.

  1) `web/pywebview-shim.js` 补齐 `set_buffmon_enabled` / `get_buffmon_enabled`。
     修复 WebView2 路径下菜单里的 Buff Monitor 开关因为 API 缺失而无法把启停参数传到
     `buffmon.set_enabled` / `buffmon.get_enabled` 桥接命令的问题。

  2) `web/pywebview-shim.js` 补齐 `download_update` / `apply_update`。
     修复 WebView2 路径下更新下载与应用按钮因为 API 缺失而无法调用
     `updater.download` / `updater.apply` 桥接命令的问题。



## v4.4.56: Web Trigger Timer 与 DPS shim 桥接修复.

  1) `web/trigger_timer_manager.html` 现在与 Tk 面板一致, 会合并 `triggers`
     与 `timers` 后渲染并去重。修复只有 timer 行时, Web 面板列表显示为空的问题。

  2) `web/pywebview-shim.js` 补齐 DPS 相关 WebView2 兼容 API:
     `show_last_dps_report`、`show_last_report`、`reset_dps`、`set_dps_enabled`、
     `get_dps_enabled`、`request_live_snapshot`、`list_history`、`export_last_report`。
     修复 WebView2 路径下 DPS 菜单/面板按钮认为 API 不存在或参数没有传到桥接命令的问题。



## v4.4.55: Web 插件管理与插件表格滚动修复.

  1) `web/plugin_manager.html` 的右侧 SDK Quick Start 侧栏现在可滚动。
     修复插件说明内容较长时, 因 `overflow:hidden` 裁掉下半部分帮助文本的问题。

  2) `web/plugin_layer.js` 为插件 UI 表格容器补齐横向滚动样式。
     修复插件声明式面板渲染宽表格时, 列内容可能被外层面板裁掉且无法横向查看的问题。
     扩展 Web layout/shim selftest 覆盖这两个回归点。



## v4.4.54: 全局快捷键组合键与冲突守卫修复.

  1) 全局快捷键支持 `CTRL/ALT/SHIFT + F1-F12` 组合键规范化与匹配。
     修复插件快捷键和内置快捷键在不同监听路径下解析不一致的问题。

  2) 插件快捷键现在会避让内置占用键并清理已被遮蔽的旧覆盖。
     修复插件键位可能覆盖主程序快捷键, 或因冲突导致用户按键无效的问题。



## v4.4.53: ACT Web 观测侧栏滚动修复.

  1) `web/data_source_health.html` 的右侧 Runtime Summary/Diagnostics 栏现在可滚动。
     修复诊断项过多时, 侧栏因为 `overflow:hidden` 裁掉下半部分内容的问题。

  2) `web/trigger_timer_manager.html` 的右侧 Runtime/Recent/Rule Source 栏现在可滚动。
     修复近期事件或说明内容过多时, 侧栏内容被裁剪且无法查看的问题。扩展
     `tools/web_act_layout_selftest.py` 覆盖这两个 Web 面板。



## v4.4.52: ACT Tk Data Source Health 渲染签名补全.

  1) `gui_modules/sao_gui_data_source_health.py` 的 source 渲染签名现在包含
     `_format_source()` 已显示的 `requested_mode`。修复数据源请求模式变化时,
     source 卡片可能因为旧签名命中而不重绘的问题。

  2) 同一签名现在包含 `_format_source()` 已显示的 `uptime_s`。修复 uptime 变化时,
     source 卡片可能继续显示旧运行时长的问题。扩展 Data Source Health selftest
     覆盖这两个可见字段。



## v4.4.51: ACT Tk Data Source Health 缓存修复.

  1) `gui_modules/sao_gui_data_source_health.py` 的 350ms refresh 缓存现在区分
     `health` 与 `diagnose` 结果。修复点击 Diagnose 后立刻 Refresh 时,
     面板可能复用诊断结果、没有重新调用 health 状态的问题。

  2) `DataSourceHealthPanel.destroy()` 现在会清理 source/diagnostics 渲染签名。
     修复窗口销毁后重开时, 如果状态内容相同, 新建的空列表/诊断栏可能因为旧签名
     命中而跳过重渲染的问题。



## v4.4.50: ACT Tk Report/Timeline 刷新缓存参数修复.

  1) `gui_modules/sao_gui_report_export.py` 的 350ms refresh 缓存现在按
     `fmt` 建 key。修复快速切换预览/导出格式时, Report Export Tk 面板可能
     复用上一格式的报告预览结果的问题。导出、载入、删除、导入后也会清理 refresh key。

  2) `gui_modules/sao_gui_timeline_vcr.py` 的 350ms refresh 缓存现在按
     `query` 建 key。修复快速修改时间线过滤词时, Timeline VCR Tk 面板可能
     复用旧事件列表、没有把新查询传到后端的问题。播放/暂停/步进/seek/filter
     动作后也会清理 refresh key。



## v4.4.49: ACT Tk Death/Graph 刷新缓存参数修复.

  1) `gui_modules/sao_gui_death_recap.py` 的 350ms refresh 缓存现在按
     `entity_id/window_s` 建 key。修复快速切换死亡回放实体或时间窗口时,
     Death Recap Tk 面板可能显示上一组窗口结果的问题。

  2) `gui_modules/sao_gui_graph_timeseries.py` 的 350ms refresh 缓存现在按
     `metric/query/topic/time_range_ms` 建 key。修复快速切换指标、主题、搜索词或时间范围时,
     Graph Timeseries Tk 面板可能复用旧图表状态、没有把新参数传到后端的问题。



## v4.4.48: ACT Tk Drilldown 刷新缓存参数修复.

  1) `gui_modules/sao_gui_combatant_drilldown.py` 的 350ms refresh 缓存现在按
     `combatant_id/query/focus_target` 建 key。修复快速切换成员、搜索词或目标焦点时,
     Combatant Drilldown Tk 面板可能直接复用旧状态、没有把新参数传到后端的问题。

  2) `gui_modules/sao_gui_skill_drilldown.py` 的 350ms refresh 缓存现在按
     `combatant_id/skill_id/query` 建 key。修复快速切换技能或搜索词时,
     Skill Drilldown Tk 面板可能显示上一技能详情的问题。扩展 Combatant/Skill
     Drilldown selftest 覆盖相同参数复用与参数变化强制刷新。



## v4.4.47: ACT Tk 刷新缓存参数修复.

  1) `gui_modules/sao_gui_act_aggregate.py` 的 350ms refresh 缓存现在按
     `query/source/group_by/group_field` 建 key。修复快速切换搜索、来源或聚合维度时,
     Aggregate Tk 面板直接复用旧状态、没有把新参数传到后端的问题。

  2) `gui_modules/sao_gui_action_log.py` 的 350ms refresh 缓存现在按
     `query/topic/cursor/source/encounter/offset` 建 key。修复刷新按钮或来源/页码变化过快时,
     Action Log Tk 面板显示旧请求结果的问题。扩展 Aggregate/Action Log selftest
     覆盖相同参数复用与参数变化强制刷新。



## v4.4.46: ACT Web 钻取状态保留.

  1) `web/act_aggregate.html` 现在会保留主内容滚动位置, 并把 section
     折叠/展开状态保存到 JS 状态表。修复刷新、插件 hook 重绘或展开聚合行后,
     Aggregate Cockpit 回到顶部且折叠区全部重新展开的问题。

  2) `web/act_skill_drilldown.html` 现在会保留 timeline refs 的滚动位置与已展开
     payload。修复 Copy/Refresh 后用户正在查看的技能事件详情全部折叠、滚动位置丢失的问题。
     `tools/web_act_render_state_selftest.js` 同步覆盖 Aggregate 与 Skill Drilldown。



## v4.4.45: ACT Web 刷新状态保留.

  1) `web/act_action_log.html` 的主列表刷新现在会在普通 refresh、复制和 group
     展开/收起后保留滚动位置；搜索、过滤、跳转和翻页仍会回到新结果顶部。修复 live/history
     行为日志重绘后浏览位置跳回顶部、用户需要反复找回上下文的问题。

  2) `web/act_timeline_vcr.html` 的事件列表现在在 refresh/play/pause/step/seek
     后保留滚动位置, 并通过稳定 key 保留已展开事件 payload。修复 VCR 控制或刷新后
     事件详情全部折叠、列表跳顶的问题。新增 `tools/web_act_render_state_selftest.js`
     锁定 Web 渲染状态回归。



## v4.4.44: ACT Web 侧栏滚动补全.

  1) `web/act_report_export.html` 的 EXPORT SUMMARY/OFFLINE IMPORT/HISTORY
     侧栏改为内部滚动。修复固定高度 WebView 中导入控件、历史搜索和清空按钮可能被
     顶层 `overflow:hidden` 截断、只剩 history 子区域可滚的问题。

  2) `web/act_death_recap.html` 的 SUMMARY 侧栏补齐 `overflow:auto`。
     修复小高度窗口下 Copy JSON 与死亡摘要统计溢出到外层 hidden shell 后不可达的问题。
     `tools/web_act_layout_selftest.py` 同步扩展到 Action Log、Report Export 与
     Death Recap 的侧栏滚动检查。



## v4.4.43: ACT Web 面板裁剪修复.

  1) `web/act_graph_timeseries.html` 的最新点位列表现在为 6 行渲染预留足够高度,
     并改为内部滚动。修复底部 points 区仍渲染 6 行但 `110px + overflow:hidden`
     导致末尾点位被截断、无法完整查看的问题。

  2) `web/act_action_log.html` 的右侧 LOG CONTROL/STATUS 面板改为内部滚动。
     修复固定高度 WebView 内筛选控件和状态行较多时, 侧栏被 `overflow:hidden`
     截断、用户无法访问底部状态信息的问题。新增 `tools/web_act_layout_selftest.py`
     锁定 ACT Web 布局裁剪回归。



## v4.4.42: ACT Entity Skill/Aggregate 签名补全.

  1) `gui_modules/sao_gui_skill_drilldown.py` 的 Entity Skill Drilldown 签名加入
     `summary.name/kind/damage/heal` 等事实区字段。修复技能名称、类型或 damage/heal
     事实变化但 amount/casts/hits 未变时, 摘要卡和 SKILL FACTS 区保持旧渲染的问题。

  2) `gui_modules/sao_gui_act_aggregate.py` 的 Entity Aggregate 签名加入
     `overview.dps/hps/span/dungeon/mode`、source badges、group 标题/元数据/展开行
     与 graph preview 点位。修复聚合值未变但顶部概览、分组说明、展开 payload 或趋势预览
     改变时, 面板跳过重绘导致显示不全/旧数据的问题。扩展 Skill/Aggregate selftest。



## v4.4.41: ACT Entity 面板渲染签名补全.

  1) `gui_modules/sao_gui_graph_timeseries.py` 的 Entity Graph 面板渲染签名加入
     `row_count`、`encounter_id`、`filters.query/topic` 与错误列表。修复点位数据相同但
     筛选条件、行数或错误状态变化时, 指标区 badge/Rows/Error 信息跳过重绘的问题。

  2) `gui_modules/sao_gui_combatant_drilldown.py` 的 Entity Combatant Drilldown
     签名加入摘要 `name/dps/hps/crit_rate/damage_pct`、技能 `crit_rate`/关联 ID
     与侧栏 outgoing 行。修复伤害/治疗未变但 DPS、暴击、占比、技能暴击或目标侧栏变化时
     面板保持旧渲染的问题。扩展 Graph/Combatant selftest 覆盖这些字段。



## v4.4.40: Plugin UI builder 数值参数容错修复.

  1) `act_platform/ui_spec.py` 的 `UI.input(..., width=...)` 现在复用规范化器的
     宽度容错与夹取逻辑。修复插件作者传入 `"auto"`、空值或浮点字符串时, builder
     在渲染前直接抛 `ValueError`、导致插件面板无法显示的问题。

  2) `UI.canvas(width, height, ...)` 现在对宽高使用与 `normalize_ui_spec` 一致的
     默认值和最大尺寸夹取。修复 canvas 宽高为浮点字符串或非法值时, 规范化器尚未接手
     就抛异常的问题。新增 `tools/act_plugin_window_selftest.py` 覆盖宽松数值参数。



## v4.4.39: Combatant Drilldown 与 Trigger Timer bridge fallback 修复.

  1) `web/act_combatant_drilldown.html` 与 `web/pywebview-shim.js` 新增
     `act.skill.open` 命名 fallback, 点击成员技能行时会把 `combatant_id` 与
     `skill_id` 一起传给 native bridge。修复 WebView2 fallback 下把 combatant id
     误当 `ui.menu_action.action`、技能 ID 丢失, 导致无法打开指定技能钻取的问题。

  2) `web/trigger_timer_manager.html` 的关闭按钮现在复用 bridge-aware `apiCall`,
     并将 `toggle_trigger_timer_manager` 映射到 `ui.menu_action` 的命名 payload。
     修复无 pywebview shim 的 native WebView2 host 下 Trigger Timer 关闭按钮失效的问题。
     C# `ActBridge` 同步注册 `act.skill.open`, 并扩展 shim/C# 回归测试。



## v4.4.38: Data Source 与 ACT 命令注册 fallback 修复.

  1) `web/data_source_health.html` 的 WebView2 bridge fallback 现在会对
     health/diagnose/copy 发送空命名 payload, 对 toggle 发送 `action` payload。
     修复该页面与 pywebview shim/C# bridge payload 形状不一致、真实命令仍携带裸
     `args` 的问题。

  2) C# `ActBridge` 补齐 `act.offline_import.*`、`act.mini_parse.*` 与
     `act.selective_parsing.*` 命令注册。修复已命名化的 Offline Import、Mini Parse
     与 Selective Parsing Web fallback 在 native WebView2 host 下仍落到
     `unknown_command` 的问题。扩展 shim selftest 与 `Session194ActBridgeTests`。



## v4.4.37: Death Recap WebView2 fallback 修复.

  1) `web/act_death_recap.html` 的 fallback 现在会传递 `limit`、`window_s`
     与 `entity_id` 命名 payload。修复 Death Recap 在 WebView2 bridge 路径下
     时间窗口、目标实体和行数参数落入裸 `args` 后无法被后端读取的问题。

  2) C# `ActBridge` 新增 `act.death_recap.status` / `act.death_recap.copy`
     命令注册。修复 Death Recap 通过 native WebView2 host 调用时落到
     `unknown_command` 的问题。扩展 `tools/web_pywebview_shim_selftest.js`
     与 `Session194ActBridgeTests`。



## v4.4.36: ACT Timeline 与 Action Log fallback 参数修复.

  1) `web/act_timeline_vcr.html` 的 WebView2 bridge fallback 现在会把
     `limit`、`query`、`speed`、`delta_ms`、`cursor_ms` 等参数映射成命名 payload。
     修复无 pywebview 直连时 Timeline VCR 的筛选、步进、seek、速度参数落入裸
     `args` 而后端无法读取的问题。

  2) `web/act_action_log.html` 的 fallback 现在会传递 `limit`、`query`、`topic`、
     `cursor_ms`、`source`、`encounter_id` 与 `offset`。修复 Action Log 在 WebView2
     fallback 下 history/live 来源、翻页、跳转和筛选参数丢失的问题。扩展
     `tools/web_pywebview_shim_selftest.js` 覆盖这两个页面。



## v4.4.35: WebView2 插件桥接参数修复.

  1) `web/pywebview-shim.js` 与 `web/plugin_manager.html` 的插件 render/action fallback
     现在会保留对象 payload, 不再把对象参数转换成 `"[object Object]"`。修复 WebView2
     fallback 下插件面板 render payload 或 action payload 丢字段的问题。

  2) `web/plugin_layer.js` 的 bridge fallback 改为发送命名 payload 和真实
     `act.plugins.*` / `act.render.*` 命令。C# `ActBridge` 同步注册 render hook/overlay
     命令, 避免插件 overlay/action fallback 落到 `unknown_command`。更新
     `tools/web_pywebview_shim_selftest.js` 与 `Session194ActBridgeTests`。



## v4.4.34: Plugin Renderer 输入参数与主题签名修复.

  1) `gui_modules/sao_plugin_ui_render.py` 的 input 复用路径现在会同步
     `placeholder`、`input_type`、`width`、password show 与 number validate/pack 配置。
     修复插件面板输入框 id 不变但参数变化时, Entity Tk 渲染保持旧占位文本、旧密码显示
     或旧宽度的问题。

  2) `gui_modules/sao_gui_plugin_manager.py` 的 detached plugin panel 渲染缓存签名加入当前
     SAO panel theme。修复主题切换但 spec 不变时, detached 插件面板跳过 reconcile,
     内容颜色/画布继续停留在旧主题的问题。新增 `tools/plugin_renderer_compat_selftest.py`。



## v4.4.33: Panel UI 主题同步与控件重刷修复.

  1) `gui_modules/sao_panel_ui.py` 的面板主题常量同步不再只覆盖
     `gui_modules.sao_gui_*`。顶层 `sao_gui` 兼容导入和其它 SAO 相关模块持有
     `_SAO_PANEL_*` 常量时也会一起刷新, 修复主题切换后部分新建/重绘面板仍用旧色的问题。

  2) `_style_panel_descendants()` 现在会安全跳过已销毁控件, 并补齐
     Checkbutton/Radiobutton 的 bg/fg/select/active 样式。修复延迟重刷遇到销毁中面板时
     递归异常风险, 以及筛选开关控件在主题切换后颜色不完整的问题。新增
     `tools/panel_ui_theme_selftest.py`。



## v4.4.32: Player Panel 实时资料与 HP/STA 刷新修复.

  1) `gui_modules/sao_player_panel.py` 新增 `update_vitals()` 并让状态同步路径通过它
     更新 HP/STA。修复等级/EXP 没变化时, `_sta_hp`/`_sta_sta` 只被写字段但不触发
     玩家面板重绘, 导致顶部 HP/STA 显示停留在旧值的问题。

  2) 新增 `update_profile()` 并让 GameState/profile dialog 同步路径通过它更新已存在的
     玩家面板。修复用户名或职业变化时只更新主 GUI 状态/菜单标题, 左侧 Player Panel
     继续显示旧用户名的问题。新增 `tools/player_panel_selftest.py`。



## v4.4.31: Session Players 强制刷新与战力签名修复.

  1) `gui_modules/sao_session_players_panel.py` 的行签名在 Cython 基础签名外叠加
     实际渲染的 `fight_power` 文本。修复 rows_provider 只更新显示战力字符串,
     或未提供 `fight_power_value` 时, 玩家列表跳过刷新并显示旧战力的问题。

  2) `update_rows(..., force=True)` 不再因已有相同签名提前返回。强制打开/刷新会
     重置 lazy render 状态、滚动起点与 GPU repaint/drain, 修复强制刷新被缓存吞掉的问题。
     新增 `tools/session_players_panel_selftest.py`。



## v4.4.30: Mem Scope 实时签名刷新修复.

  1) `gui_modules/sao_gui_mem_scope.py` 的渲染签名现在覆盖 catalog、自身状态、
     实体 HP/百分比、伤害总表、搜索结果地址/解码 hint 等实际渲染字段。修复实体数量、
     伤害 key 数或搜索 count 不变但数值变化时, Mem Scope 仍显示旧数据的问题。

  2) Mem Scope 在窗口销毁/重建时会清空 `_last_sig`, 避免新 `_rows` 容器因为命中
     旧签名而跳过首屏渲染。新增 `tools/mem_scope_panel_selftest.py`。



## v4.4.29: Offline Import 渲染与 fallback 参数修复.

  1) `gui_modules/sao_gui_offline_import.py` 的行签名现在覆盖实际渲染的导入预览字段
     和前 20 条 history 行内容, 并在窗口销毁/重建时清空缓存。修复历史数量不变但
     伤害/完成时间变化、导入预览 encounter/importer/persisted 变化, 或重建窗口时
     面板不刷新的问题。

  2) `web/act_offline_import.html` 的 `window.bridge.cmd` fallback 不再发送裸
     `{args:[...]}`。history_limit、导入 path/persist/show、history index/show
     和关闭菜单 action 现在按命名字段传给 bridge。扩展 Python/JS 回归测试。



## v4.4.28: Report Export 渲染与 fallback 参数修复.

  1) `gui_modules/sao_gui_report_export.py` 在窗口销毁/重建时会清空 preview/history
     渲染签名。修复生命周期销毁后用相同报告状态重新打开时, 新容器可能因为命中旧签名
     而不渲染预览或历史列表的问题。

  2) `web/act_report_export.html` 的 `window.bridge.cmd` fallback 不再发送裸
     `{args:[...]}`。报告格式、history index、show 标志、离线导入 path/persist/show
     和 mini-parse formatter 现在按命名字段传给 `act.report.*`、`act.history.*`、
     `act.offline_import.*` 等命令。扩展 Python/JS 回归测试。



## v4.4.27: Entity Timeline VCR 签名刷新修复.

  1) `gui_modules/sao_gui_timeline_vcr.py` 的列表渲染签名现在包含 source、
     payload、事件总数、展开状态以及 cursor/speed/playing/errors 等 VCR 状态。
     修复事件主体未变但来源、展开 payload、播放速度或第 80 条后的数量变化时,
     面板指标和展开内容不刷新的问题。

  2) Timeline VCR 的窗口销毁/重建会重置 `_last_events_sig`。
     修复生命周期销毁后用相同 status 重新打开时, 新 `_events` 容器可能因为命中旧签名
     而跳过首屏渲染的问题。新增 `tools/timeline_vcr_panel_selftest.py`。



## v4.4.26: Entity 触发/计时面板渲染修复.

  1) `gui_modules/sao_gui_trigger_timer_manager.py` 增加列表渲染签名。
     相同触发器/近期事件状态重复刷新时不再销毁并重建全部 Tk 子控件,
     避免滚动位置、按钮焦点和点击节奏被手动刷新/重载打断。

  2) Entity 触发/计时面板现在会合并 `triggers` 与 `timers` 行并按 rule id 去重。
     修复 status 只提供独立 `timers` 列表时摘要显示有计时器、列表却进入空态的问题。
     新增 `tools/trigger_timer_panel_selftest.py` 覆盖 timer-only 与重复刷新场景。



## v4.4.25: ACT 下钻页长列表滚动修复.

  1) `web/act_combatant_drilldown.html` 的技能列表/侧栏 section 不再
     `overflow:hidden` 裁掉长内容。主 layout 现在按视口高度约束, section 内部滚动,
     窄屏堆叠时限制单 section 高度。

  2) `web/act_skill_drilldown.html` 的 timeline refs / facts section 同步改为
     可滚动布局, 修复长时间线、展开 payload 或 facts 较多时底部内容不可见的问题。
     两个 selftest 增加 HTML CSS 回归断言。



## v4.4.24: Mem Scope 与面板主题 WebView2 shim 补齐.

  1) `pywebview-shim.js` 补齐 `get_mem_scope_status`、`mem_search`、
     `mem_search_status`、`mem_narrow`、`mem_search_cancel`、`mem_attr_map` 和
     `toggle_mem_scope`。C# `ActBridge` 同步注册 `act.mem_scope.*` 命令, 修复
     WebView2 shim 路径下 Mem Scope 直接显示 `NO API`、搜索/收敛/关闭不可用的问题。

  2) 补齐 `get_panel_themes/set_panel_theme` shim 和 C# `LegacyUiBridge` 命令。
     `get_panel_themes` 在无原生设置后端时返回页面可直接消费的默认 dark 主题对象,
     避免主题初始化/切换在 WebView2 shim 路径下静默失效。扩展 JS/C# 回归测试。



## v4.4.23: 插件面板轮询增量渲染.

  1) `web/plugin_manager.html` 的 Panels 标签不再每秒清空 `panels-grid` 并重建
     所有卡片。面板卡片、标题和 action callback 现在按 panel id 复用, spec 未变化时
     跳过 `PluginHooks.renderSpec`, 避免输入焦点/滚动/按钮状态被轮询刷新打断。

  2) `web/menu.html` 的 detached plugin panel 不再每 800ms 清空 `pd-body`。
     每个插件面板按 id 保留 DOM entry 并用 spec 签名跳过未变化渲染, 同时保留异步序号
     防止过期 render 结果写回。扩展 `tools/web_pywebview_shim_selftest.js` 加回归守卫。



## v4.4.22: WebView2 核心 UI shim + 插件/触发器 fallback 参数修复.

  1) `pywebview-shim.js` 补齐 `exit_app`、`toggle_menu`、`context_action`、
     `set_ctx_menu_active`、`window_drag`、`close_panel`、`panel_action` 等核心
     UI 别名, 并让 C# `LegacyUiBridge` 注册对应 `ui.*` 命令。修复 WebView2 shim
     路径下 HP/Menu/Panel 页面按钮可能 TypeError、unknown_command 或无反馈的问题。

  2) 插件管理页和触发器管理页的 `window.bridge.cmd` fallback 不再发送裸
     `{args:[...]}`。插件 ID、触发器 rule_id、pin 标志、插件 UI panel/action
     payload 现在按命名字段透传。新增/扩展 JS 与 C# bridge 回归测试。



## v4.4.21: WebView2 shim 参数透传 + ACT 下钻 fallback 修复.

  1) `pywebview-shim.js` 的 `get_aggregate_status` 补传 `group_by/group_field`。
     修复 WebView2 shim 路径下 ACT 聚合驾驶舱切换维度/自定义字段时参数被截断,
     后端始终按默认 skill 聚合的问题。

  2) 补 `show_action_log_at` shim, 并把 Graph/Combatant/Skill 三个 ACT 面板的
     `window.bridge.cmd` fallback 从裸数组改成命名 payload。无 pywebview.api 包装或
     shim 方法缺失时, C#/bridge handler 仍能读到 `cursor_ms`、`combatant_id`、
     `skill_id`、`query` 等字段。新增 `tools/web_pywebview_shim_selftest.js` 覆盖。



## v4.4.20: Web 插件层稳态渲染 + settings 新目录可靠保存.

  1) WebView 插件层增加 spec/overlay/override 签名闸: 800ms 轮询拿到相同内容时不再
     清空并重建 DOM, 减少插件 overlay 空转开销, 避免输入焦点/滚动/点击状态被重复重建
     打扰。空 spec 现在与 Entity 渲染器一致显示 muted "(empty)", 只有标题的 takeover
     spec 也不会被误判为空而直接移除。

  2) SettingsManager.save 先创建目标目录, 并初始化/清理临时文件路径后再 fallback 直写。
     修复新配置目录尚不存在时保存失败且被吞掉的问题, 防止首次运行或自定义 settings 路径
     下用户改动丢失。smoke 测试新增新目录保存回归覆盖。



## v4.4.17: hybrid 内存补充化 — 干掉 mem 每 tick O(N) 读, 人群/20人本不再卡.
  实测 hybrid 下 mem DPS/Boss HP 仍卡; 定向: hybrid 走 TCP 为主, mem 只补
  名字 / TCP 不发的基址。调查确认 DPS 行+Boss HP 本就 TCP 为主(dps_tracker
  _mem_primary=False; Boss HP 仅 boss_hp_source∈none/memory/estimate 时由 mem 补)。
  卡的根因 = MemStateBridge._entity_loop(1Hz)每 tick 两次 O(N) 持 GIL 内存读:
  prov.snapshot()(读每个可见实体, 随人群涨)+ _poll_mem_damage()(整张伤害表+top6,
  随队伍涨)。修法(只动 mem_state_bridge.py):
  1) 慢化采集: 非 memory 模式(hybrid/auto)下 _entity_loop 在「定位到 boss 且
     base_acquired」后退到 4s(冷启动/boss 消失自动回到 1s, 不延迟 boss 定位/破防切源);
     memory 模式恒 1s 全量(零变化, mem 仍主源)。
  2) O(1) Boss 路: _boss_cast_loop(~12.5Hz)本就用缓存 obj 调 read_combat(已返回
     cur/max/hp_pct/breaking/extinction), 之前只取 cast_skill_id; 现额外节流~3.5Hz
     重算 last_boss_break + push Boss HP(自门控同慢循环, 仅 TCP 未占条时写 memory),
     Boss 血条/破防反而更新(12.5Hz>1Hz)。
  3) 防陈旧守卫: 信任缓存 boss obj 前用 read_u64(obj+uuid 偏移)复核仍是同一 uuid,
     防池化复用把回收对象 HP 当 boss(把窗口从≤4s 收到一个快 tick~80ms)。
  净效果: hybrid 每秒 mem 开销 O(N)→O(1); TCP 仍权威实时源; mem 只补名字/基址/
     破防湮灭/max_hp 引导。无进程单测 17 绿(节奏门控/快推/守卫/节流), boss 全套 42 绿。

## v4.4.16: Boss 反应编辑器 — 名字映射 + 聚合视图 + 开页不卡.
  1) 三处 ID 都映射成真名(读时, 走权威名表 names.*, 历史记录一并修复):
     场景 names.dungeon、Boss names.boss/monster、技能 names.skill→boss_skill→
     boss_mechanic_skill→… 级联。实测截图里的 #500116→「友方木人掉血buff」、
     #121→「友方木桩」。场景无 dungeon 名时回退「场景#id」(木桩区本就无副本名,
     真副本由 TCP dungeon_name 落库)。
  2) 聚合视图: build_boss_reactions_state 新出 boss_detail — 技能/Buff(可绑反应)、
     机制/状态(徽章)、时间线(按出招时刻排序)、Boss 单位摘要(技能N·机制M·血线K·
     时长~Tms)。Tk 与 Web raid_editor 1:1 同款渲染。
  3) 开页卡顿修复: 编辑器契约改用 get_status(include_entities=False) — 不再在锁内
     建 O(N) 实体表; 观测只取选中 Boss(非场景内全部 Boss); 250ms 轮询仅 Entities
     tab 才建实体表; 每 tick 的 _push_game_state_locked 也停建实体表(后台同样省)。
     contract 多传 boss_base_id, 选 Boss 服务端重算分组(双端 reload 对称)。

## v4.4.15: hybrid 多人(20人本/拥挤场景)卡顿修复 — mem 每 tick O(N) buff 读取.
  BossActionTracker.update 对快照里 EVERY casting entity 都 read_buffs(BuffComp
  的 Python 逐项 RPM 循环, 持 GIL)来取施法时长/baseline, 但桥只用 boss 那条记录,
  其余全丢弃。人多/拥挤场景下这是 O(N) 持锁内存读, 拖垮主线程(TCP 路无此问题,
  故纯 TCP 20 人不卡)。改为 buff 读取仅对 boss(is_boss 门控); 非 boss 仍走廉价的
  快照 skill-id/actor-state 边沿检测(零内存读)。顺带: detect_buff_skill overlay
  的时长加 [200,10000]ms 合理区间校验, 不把 480s 狂暴计时类长 buff 当施法时长。
  新增 perf 回归测试(30 个施法非 boss + 1 boss → buff 读取 <=20)。

## v4.4.14: hybrid Boss 反应修复.
  1) hybrid 下编辑器一直提示「切 hybrid」且看不到记录: build_boss_reactions_state
     用 gs.data_source 判 mem_available, 但 GameState 根本没这字段(写入被静默丢弃)→
     永远 False → 编辑器短路成横幅, 不渲染已记录技能(记录本身是通的, mem bridge
     实时读 boss_raid_engine, set_boss_raid_engine 透传到活 bridge)。改为按用户选的
     数据源模式(settings mem_data_source/data_source)判定; tcp 与 hybrid/memory 双路
     都记录, 故识别运行即可用。TCP 为主仲裁(MEM_PRIORITY_WINDOW)不动。
  2) 反应编辑器抽成共享 _BossReactionsEditorMixin: 快捷面板(BossRaidPanel)与详细
     面板(BossRaidDetailPanel)1:1 复用, 详细面板也能浏览场景→Boss→观测技能并编辑
     反应(独立子帧重绘, 不丢 profile 编辑)。

## v4.4.13: HUD 三个小修.
  1) Boss 计时只在用户开了 bossraid profile (STATE_RUNNING) 时显示；自由战斗 /
     自带狂暴的 boss 不再把左下角身份牌的时钟换成 boss 时间 (boss 血条保留)。
  2) Boss 计时字号放大到与时钟一致 (Tk 18→30px, Web 13→30px)。
  3) 等级 base 硬上限 60 (GameState 收口)：内存路 RoleLevel.Level 带回赛季合算值
     时不再显示成 Lv.93，正确显示 Lv.60(+93)，赛季走 (+XX)。

## v4.4.0: Boss 技能聚合 — 按地图/场景/Boss 持久记录出招, 详细编辑面板标记特殊技能.
  1) Boss skills/mechanics are read from the BuffComp buff list (a new transient
     buff base_id = a cast); fixed a ZList<T> offset bug (items_@0x18/size_@0x20,
     a recyclePooledObj_ bool precedes items_) that made every buff read empty.
     base_id + Duration are memory-authoritative; the offline name table is only
     a hint. Symmetric TCP path: _tcp_detect_boss_skills_locked diffs
     monster.buff_list (BuffInfoSync) for new base_ids; a memory-priority gate
     (MEM_PRIORITY_WINDOW=3s) keeps memory authoritative in hybrid, TCP in pure-TCP.
  2) New persisted aggregate (engines/boss_skill_store.py, atomic-write like
     TcpNameCache): scene → boss → observation. Three kinds — skill (cast),
     mechanic (breaking/shield/super_armor/fracture/death/body_part from
     on_boss_event), state (enrage/invincible ONSET with HP%/elapsed). Tagged
     with concurrent state; hp_line / time cues derived from spread. Keyed by the
     live scene (GameState dungeon_scene_id/dungeon_id/dungeon_name); survives
     restarts. Saved on stop/reset + throttled in the run loop.
  3) Reactions editor is now scene-aware (build_boss_reactions_state scene_key):
     scene selector → scene-scoped bosses → observed skills/mechanics with colored
     type badges (施法/狂暴/无敌/护盾/霸体/破防/碎裂/眩晕/血线/定时/死亡/部位).
     Tk (sao_gui_bossraid) + WebView (raid_editor.html) render 1:1; skills get the
     inline reaction editor, mechanics/states render as marked info rows.

## v4.3.0: 内存驱动 Boss 反应 — bossraid 读内存 boss 动作/技能 → autokey 自动躲技能/自动操作.
  1) New mem boss-action feed (mem_probe/il2cpp/mem_boss_action_reader.py):
     BossActionTracker edge-detects cast_skill_id (attr 100) per tick for any
     boss/monster + breaking/overdrive/stun/hp; a 10-20Hz boss-cast fast poll in
     mem_state_bridge gives <100ms cast-start latency. Best-effort cast DURATION
     via BossDurationProbe (BuffComp.BuffItem.Duration, offsets verified vs dump
     fdc7111b) with a learned-EMA fallback. Exposed read-only via MemAccess
     boss_actions()/boss_action() + act_mem_boss_actions.
  2) bossraid engine on_mem_boss_action: updates cast state, records observed
     skills per boss (auto-discovery), fires boss_skill phase triggers from the
     mem skill_id, pushes boss_cast_* to GameState, implements the missing
     on_self_dead_change gate, forwards to the linkage with rising-edge flags.
  3) BossAutoKeyLinkage upgraded from alert-text to skill_id-accurate: new
     trigger types boss_cast / boss_breaking / boss_overdrive / boss_stun, new
     on_boss_action entry, per-mapping skill_id/boss_base_id scope + delay_ms/
     lead_ms/sequence dispatch (auto-dodge + offensive combos). Back-compat.
  4) New Boss 反应 visual editor in the BossRaid panel (Tk sao_gui_bossraid tab
     + WebView raid_editor.html tab, contract build_boss_reactions_state): pick
     a boss, see memory-discovered observed skills, assign reaction keys/timing.
     One optional autokey condition boss_casting_skill_is. PacketBridge now
     forwards boss_raid_engine into the hybrid mem source.

## v4.2.0: Mem Scope — memory-scan explorer + plugin mem API.
  1) New read-only MemAccess facade (mem_probe/mem_access.py) exposes every
     memory-scan-readable resource (self/entities/boss/damage/skill-damage/
     attr_map/name-resolve/raw-read) plus an async value-search job manager
     (decode-hint per hit: u32/i32/f32/utf16/cstr/ptr + module+offset + klass).
     Hybrid-gated and JSON-safe (addresses as hex, uuids as strings); never
     writes process memory.
  2) Plugin SDK: ctx.mem facade + act_mem_* runtime actions
     (ctx.call_runtime("mem_*")); fixed ctx.get_engine("memory_bridge") to
     resolve the live bridge nested under _packet_engine._mem_source in hybrid.
  3) Declarative UI gains an `input` text-field leaf (Tk + WebView 1:1) with
     value round-trip into payload["inputs"] and focus/text preservation
     across timed redraws — usable by any plugin panel.
  4) New first-class Mem Scope panel (Entity sao_gui_mem_scope.py + WebView
     mem_scope.html, contract act_mem_scope_status): browse the resource
     catalog with per-item hints, view live self/entities/damage, and run a
     manual value search with decoded hints + narrow convergence. Added a new
     `mem_scope` render surface for plugin hooks/overlays.

## v4.0.0: ACT platform + self-contained name-table pipeline (major release).
  Aggregates 3 days / 109 commits (6f837b4..) since v3.2.22 into a major
  version. The release turns the 3.x ACT work into a first-class platform
  and cuts the name-table pipeline loose from the external neighbour repo.
  1) ACT platform: new act_platform/ package (EventBus, parser adapters,
     process-isolated parser worker, mini-parse, selective parsing, trusted
     plugin engine + sample plugins). ACT XML report import/export + a
     compressed roundtrip, SQLite + JSONL history archive, local report API,
     action-log paging analytics, an offline import wizard, death recap
     (entity + WebView), live parser-adapter selection + health, and a 1:1
     dual-UI aggregate cockpit redesign.
  2) ACT coverage: 幻想技能 (ultimate) buff coverage% + trigger count in the
     buff overlay, self buff/debuff uptime tracking, per-target/element/
     min-max breakdown with CN names, encounter-driven aggregation window,
     and hybrid-memory policy gates.
  3) Name tables: self-contained classifier + full rebuild (no neighbour
     repo), 20+ semantic assets/name_tables/*.json, smart resolution,
     taxonomy/kind split, fallback routing, tcp name cache + deferred mem
     startup, and runtime tables aligned to our in-memory parse over stale
     neighbour data.
  4) GPU/UI: LinkStart GPU startup-animation polish with continuous phase +
     camera transitions, GPU-required entity panels, a centered map-name
     banner on scene change, an editor-window-as-game guard, and packet mem
     startup hardening.

## v3.2.22: ACT replay, memory-probe, and live name-table tooling refresh.
  Add the read-only mem_probe/IL2CPP runtime inspection pipeline, Cython
  memscan accelerator, live localization/name-table extraction diagnostics,
  and ACT replay/history helper modules so table and combat-data research can
  be validated without touching packet capture or packaged release steps.

## v3.2.21: Boss-HP teammate-leak fix.
  User reported: "怪物血量会把队友血量算进去" — the boss HP bar
  showed a 56.26M max while the real boss on screen only had 1.17M.
  Root cause: _is_self_combat_target only checked "is known friendly"
  to filter out party heals; but party members who joined the session
  milliseconds before their first heal event weren't yet in the
  friendly cache → their UUID leaked into _bb_recent_targets → the
  boss bar's sort-by-max_hp picked the teammate instead of the boss.
  Added a player-suffix hard guard ((uuid & 0xFFFF) == 640 → never
  eligible as boss target, regardless of cache state) at both the
  write site (sao_gui_packet_callbacks_mixin.py) and the read site
  (sao_gui_state_mixin.py) for defense-in-depth.

## v3.2.20: HOTFIX #2 — found the REAL cause of empty left panel.
  Four files still imported the GPU painter modules from top-level
  (e.g. `from sao_left_info_gpu import ...`) but those modules had
  been moved to gui_modules/ in an earlier refactor. The lazy
  try/except around those imports silently swallowed
  ModuleNotFoundError → _gpu_managed=False → Tk widgets stay at
  chroma-key bg with no GPU painter ever painting them →
  completely invisible panel. Fixed in:
    sao_theme.py (3x: menu_bar_gpu / left_info_gpu / child_bar_gpu)
    gui_modules/sao_player_panel.py
    gui_modules/sao_session_players_panel.py
  Verified: gpu_player_panel_enabled() now returns True instead of
  silently False; panel._gpu_managed=True; _PPGP_cls resolves.

## v3.2.19: HOTFIX — entity-mode left-panel + session-panel restored;
  pynput Thread-3 traceback silenced; popup error path no longer
  swallows widget-constructor exceptions silently.

  Bug 1+2 (one root cause): SAOPlayerPanel.__init__ called Animator()
  without an import. Because ui_gpu/popup.py wraps left_widget_factory
  in try/except (silently nulling _left_widget on any error), the user
  saw an EMPTY left column with NO traceback — both player panel AND
  session-players panel disappeared (because SAOMenuLeftStack.__init__
  builds player_panel first and crashes before session_panel is ever
  created). Fix: lazy `from sao_theme import Animator` inside __init__
  to avoid the circular import (sao_theme → ui_gpu.popup →
  sao_player_panel chain). Also added Optional, Tuple to typing import.

  Bug 3: pynput 1.8.1 on Python 3.11 has a ctypes-validation bug in
  its internal _PeekMessage call ("expected LP__PUMP_MSG instance
  instead of pointer to MSG"). The Listener thread dies on first
  message-loop tick and prints a noisy 9-line traceback to stderr.
  Our SAOHotkeyManager already has a try/except around start(), so
  the rest of the GUI runs fine. Fix: install threading.excepthook
  that swallows ONLY this specific ctypes.ArgumentError.

  Defensive: ui_gpu/popup.py:left_widget_factory now PRINTS a single
  diagnostic line (type+message) when the factory raises — previously
  the bare `except Exception: pass` made it impossible to spot
  widget-constructor bugs like this Animator one without manually
  running the factory in isolation.

## v3.2.18: runtime-crash fix — 15 missing imports across 7 mixins
  (round 80 of /loop). User ran `python main.py` and hit two real
  NameError crashes (APP_VERSION_LABEL @ status_updater_mixin:228 and
  ease_out @ link_animation_mixin:310). Re-ran AST audit with an
  aggressive predicate (round 73's audit had skipped lowercase names
  to keep false-positive noise down, which missed all the helper
  functions: ease_out, lerp, play_sound, time, math.*, np.*, etc.).
  Added time/APP_VERSION_LABEL/SAODialog to status_updater_mixin;
  math/numpy/PIL/get_sao_font/ease_out/ease_in_out/lerp to
  link_animation_mixin; time/play_sound/ease_out/ease_in_out to
  lifecycle_mixin; _CY_UI/ease_out to float_hp_mixin; time to
  float_handlers_mixin; threading/tk/gpu_capture helpers to
  fisheye_mixin; List/_CY_UI/get_skill_slot_rects/play_sound to
  misc_mixin. Verified with 25-second `python main.py` run — all
  overlays initialized cleanly, no NameError in trace.

## v3.2.17: attribute audit + dead code + import hygiene
  (rounds 76-77 of /loop). The final cleanup pass.
  Round 76: attribute contract AST audit caught self._drag reads
    in float_handlers_mixin that were never assigned. Investigation:
    the 3 reading methods (_float_click / _float_drag / _float_release,
    35 lines) are dead code dating back to pre-refactor. The float
    button's <Button-1> is bound directly to _toggle_sao_menu in
    _create_floating_widget — these 3 handlers were never wired to
    a Tk event. Confirmed via git blame on pre-refactor commit
    33f7fdd. Deleted the 3 methods + 3 docstring mentions.
  Round 77: unused-import audit on sao_gui.py via AST. Found 110
    candidates of 140 top-level imports. Conservative cleanup of
    the 5 truly inert ones (replaced with explanatory comments,
    net line change zero):
      from PIL import Image, ImageDraw, ImageTk, ImageFilter, ImageFont
        — all used to be by _get_hp_pil_font (deleted round 71).
      import numpy as np — same.
      from render_capture_sync import wait_until_capture_idle — caller
        moved to mixin.
      from perf_probe import probe + phase + gauge — used by 7 mixins
        each with its own import; sao_gui scope unused.
      import _sao_cy_uihelpers as _CY_UI — same pattern, 10 mixins.
    The 96 remaining "unused" imports stay: standard lib (sys, math,
    typing) are tiny + harmless; engine/overlay/panel class imports
    (AutoKeyEngine, DpsOverlay etc.) might benefit PyInstaller's
    static analysis even when sao_gui.py itself doesn't reference
    them; aggressive removal risks PyInstaller bundling regressions
    for marginal cosmetic gain.
  sao_gui.py: 507 lines unchanged this cadence (comments replaced
  the dead imports).
  Cumulative refactor: 9682 -> 507 = -9175 = -94.8%.
  Post-refactor bug-fix tally across rounds 71 + 73 + 76:
    - 1 _set_process_app_id NameError (live; would crash __init__)
    - 9 latent NameErrors for missing mixin imports
    - 3 dead methods deleted (would AttributeError on call, but never called)
    Total 11 latent + 4 dead-code purges. The refactor is now
    genuinely complete.

## v3.2.16: AST audit + 9 NameError bug fixes (rounds 73-74 of /loop).
  With the structural extraction done, ran an AST-based audit across
  all 19 SAOPlayerGUI mixins looking for the bug pattern that round 71
  caught one instance of: bare-name function calls inside extracted
  method bodies where the function wasn't carried over into the
  mixin's import block. The audit caught 9 more:
    sao_gui_float_hp_mixin.py: _perf_gauge (from perf_probe).
    sao_gui_fisheye_mixin.py: _perf_gauge + _phase_trace (perf_probe).
    sao_gui_link_animation_mixin.py: _disable_native_window_shadow
      (from sao_panel_ui) + SAOLinkStart (from sao_theme).
    sao_gui_status_updater_mixin.py: SAOButton + SAOProgressBar +
      SAOStatusPill + _sao_close_dialog (all from sao_theme; the
      last aliased from _close_alert).
  Each one was a NameError waiting to happen at first call. The
  bigger mixins (status-updater 857 lines, link-animation 911,
  fisheye 1421) had method bodies with helper calls that the
  manual extraction missed when adding imports.
  Round 74 (verification): bulk py_compile.compile across all
  41 gui_modules/*.py + sao_gui + sao_webview + sao_theme + main +
  XiaoACTUI.spec = ZERO failures. Runtime construction smoke
  blocked by an unrelated pynput/Python 3.11 ctypes
  incompatibility (library issue, not refactor regression).
  No line-count change to sao_gui.py — pure bug-fix work on the
  mixins. Cumulative refactor remains 9682 -> 507 = -94.8%, with
  9 latent NameError landmines now defused.

## v3.2.15: mem_probe cleanup + _set_process_app_id NameError fix
  (rounds 70-71 of /loop). Pivot from structural extraction to
  cleanup: close out the long-deferred mem_probe directory deletion
  + fix a latent runtime bug that round 68's MiscMixin extraction
  introduced.
  Round 70: mem_probe directory cleanup.
    - 22 mem_probe/* file deletions staged (carried forward in the
      git working tree since round 26 when the directory was deleted
      on disk).
    - XiaoACTUI.spec cleaned: MEM_PROBE_HIDDENIMPORTS (returned []
      anyway), MEM_PROBE_BINARIES (glob returned []), the
      ('mem_probe', 'mem_probe') data entry, and the
      binaries=... + MEM_PROBE_BINARIES / hiddenimports=... +
      MEM_PROBE_HIDDENIMPORTS arg pieces all removed.
    - build_cython_ext.py: the always-False if-exists wrapper around
      the mem_probe._sao_cy_memscan Extension removed.
    - packet_bridge.py intentionally NOT changed — its lazy import
      in _start_memory_source is only resolved when data_source is
      'memory'/'hybrid'/'auto' (default 'tcp' never touches it), and
      the existing failure path is already graceful (strict 'memory'
      errors with a clear message; 'hybrid'/'auto' fall back to TCP).
  Round 71: _set_process_app_id NameError fix + dead-code delete.
    - Latent bug: round 68 extracted _set_icon into MiscMixin but
      _set_icon references _set_process_app_id as a bare name without
      importing it. SAOPlayerGUI.__init__ calls self._set_icon() on
      every startup → NameError at app launch.
    - Fix: moved _set_process_app_id to gui_modules.sao_panel_ui
      (alongside the other Win32 helpers from round 59).
    - Deduplicated: sao_gui.py and sao_webview.py each had identical
      copies of the same shell32 call; both now `from gui_modules.
      sao_panel_ui import _set_process_app_id`. sao_gui.py also
      lists it in the panel_ui block re-import.
    - MiscMixin gained the import alongside _apply_window_icon.
    - Dead code: _get_hp_pil_font in sao_gui.py was defined but
      never called (repo-wide grep zero hits). 14 lines removed.
    - sao_gui.py: 524 -> 507.
  Cumulative refactor: 9682 -> 507 = -9175 = -94.8%. sao_gui.py
  stands at 5.2% of its original size, 8493 lines below the user's
  9000-line target.
  The refactor has reached its natural floor — what remains is
  class skeleton + entry-point glue. Continued extractions would
  yield diminishing returns + significant risk.

## v3.2.14: damage events + misc helpers mixins (rounds 67-68 of /loop).
  sao_gui.py crosses below 525 lines; cumulative reduction reaches
  94.6%. MRO depth grows to 19 mixin layers.
  Round 67: SAOPlayerGUIDamageEventsMixin (341 lines mixin / 286 net
    out). 5 methods covering damage-event normalization + pending-
    combat-reset gate:
      _maybe_apply_pending_combat_reset (88), _current_player_uid_int
      (16), _is_known_friendly_uid (42), _normalize_damage_event_for_self
      (47), _normalize_damage_event_target_for_entity (94).
    Mixin imports: time only.
    sao_gui.py: 1015 -> 729.
  Round 68: SAOPlayerGUIMiscMixin (261 lines mixin / 205 net out).
    14-method grab-bag of small helpers ≤51 lines each:
      _set_icon (5), _create_hp_alpha_strip_windows (5),
      _render_hp_strip_image (3), _sync_hp_alpha_strip_windows (4),
      _render_hp_shell (4), _render_hp_dynamic (4),
      _get_skillfx_layout (51 — biggest; SkillFX layout picker used
        by State mixin),
      _get_game_window_rect (25), _get_game_window_context (26),
      _format_level_text (3), _fade_panel_in (30), _fade_panel_out (39),
      _switch_to_old_ui (4), _show_leaderboard (4).
    Mixin imports: time + _apply_window_icon from sao_panel_ui.
    sao_gui.py: 729 -> 524.
  SAOPlayerGUI MRO now has 19 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
     FloatHp, FloatHandlers, Lifecycle, PanelFx, LinkAnimation,
     DamageEvents, Misc, State, Session). No name conflicts.
  Cumulative refactor: 9682 -> 524 = -9158 = -94.6%. sao_gui.py is
  8476 lines below the user's 9000-line target and now stands at
  just 5.4% of its original size. gui_modules/ holds 40 .py /
  ~30270 lines. **The refactor has reached its natural floor —
  what remains in sao_gui.py is class skeleton + entry point glue
  (__init__, run(), _set_setting/_get_setting, module imports +
  helpers, class declaration, __main__).**

## v3.2.13: panel-fx scheduler + link animations (rounds 64-65 of /loop).
  sao_gui.py crosses below 1100 lines; cumulative reduction reaches
  89.5%. MRO depth grows to 17 mixin layers.
  Round 64: SAOPlayerGUIPanelFxMixin (210 lines mixin / 158 net out).
    2 methods + 2 class attrs + 1 module-level helper:
      _make_sao_panel_hud (22, module-level Canvas factory),
      _attach_sao_panel_fx (62, register panel + sig cache + auto-
        unregister on destroy + start shared tick),
      _sao_fx_shared_tick (75, @_probe-decorated staticmethod;
        90ms shared tick driving all panels via _CY_UI.sao_fx_coords).
    Class attrs _sao_fx_panels and _sao_fx_after_id were declared on
    FloatHpMixin since round 56 (vestigial — they had originally been
    on SAOPlayerGUI proper and got carried along incidentally).
    Round 64 relocates them to their proper home alongside the
    methods that use them.
    SAOPlayerGUI._sao_fx_* class-attr refs in the methods rewritten
    to type(self)._sao_fx_* / type(self_ref)._sao_fx_* (semantics
    identical via class-attr lookup; avoids the not-yet-defined
    SAOPlayerGUI import cycle).
    sao_gui.py: 2023 -> 1865.
  Round 65: SAOPlayerGUILinkAnimationMixin (911 lines mixin / 850
    net out). The single biggest extraction since round 41 (Fisheye,
    1078). The full-screen SAO link-start/link-end overlay animations
    + moderngl GPU draw helpers + exit-window enumerator.
    12 methods (in source order):
      _play_link_start (68 — top-level entry animation),
      _init_entry_boot_gl (101 — moderngl context + GLSL shaders),
      _draw_entry_boot_gl (31), _create_entry_overlay (30),
      _draw_entry_overlay (107), _run_entry_animation (66),
      _init_exit_pulse_gl (107), _draw_exit_pulse_gl (31),
      _get_exit_banner (17), _create_exit_overlay (42),
      _draw_exit_overlay (141 — biggest method in this cluster;
        exit animation main loop),
      _collect_exit_windows (110 — enumerates every visible Tk
        Toplevel + ULW window for the exit fade).
    Mixin imports: just time + tkinter at module level. moderngl,
      PIL, gpu_overlay_window are inline-imported (matches pattern).
    sao_gui.py: 1865 -> 1015.
  SAOPlayerGUI MRO now has 17 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
     FloatHp, FloatHandlers, Lifecycle, PanelFx, LinkAnimation,
     State, Session). No name conflicts.
  Cumulative refactor: 9682 -> 1015 = -8667 = -89.5%. sao_gui.py is
  7985 lines below the user's 9000-line target. gui_modules/ now
  holds 38 .py / ~29664 lines. **The refactor is approaching its
  natural floor — most extractable cohesive clusters are out.**

## v3.2.12: menu widget reorg + lifecycle mixin (rounds 61-62 of /loop).
  sao_gui.py crosses below 2100 lines; cumulative reduction reaches
  79.1%. MRO depth grows to 15 mixin layers.
  Round 61 (user-driven): the remaining 4 standalone "sao_*" GPU
    widget files at the repo root moved into gui_modules/:
      sao_menu_hud.py (1505 lines, the renderer hub),
      sao_menu_bar_gpu.py (430), sao_left_info_gpu.py (892),
      sao_child_bar_gpu.py (550). Total 3377 lines relocated.
    Updates: 8 importer lines (sao_theme + 4 cross-imports within
      moved files + 3 ui_gpu/*) and 4 new hiddenimports in
      XiaoACTUI.spec (replacing the 1 old sao_menu_hud entry).
    sao_menu_hud.py's `_BASE = os.path.dirname(os.path.abspath(__file__))`
      fallback was rewritten to dirname(dirname(...)) since __file__
      now resolves one level deeper.
    Circular-import fix: the move exposed an existing latent cycle —
      sao_theme → gui_modules.sao_menu_hud → gui_modules/__init__ →
      sao_gui_menu_mixin → sao_theme.SAOPopUpMenu. Resolved by removing
      the SAOPlayerGUI mixin re-exports from gui_modules/__init__.py.
      The mixins are SAOPlayerGUI-internal — they're only imported via
      full dotted path in sao_gui.py, so the re-exports were unused
      convenience. The standalone helper classes (5 of them) that are
      imported as `from gui_modules import X` stay re-exported.
  Round 62: SAOPlayerGUILifecycleMixin (398 lines mixin / 324 net out).
    7 methods covering ordered teardown + restore-on-startup:
      _destroy_hp_alpha_strip_windows (9), _restore_panels (13),
      _cleanup_entry_overlay (23), _cleanup_exit_overlay (24),
      _finalize_close (128 — ordered destroy sequence: stop loops,
        cancel after IDs, remove listeners, unbind hotkeys, stop
        fisheye + boss-HP worker, close menu, stop engines, persist
        cache, destroy overlays, quit mainloop),
      _run_exit_animation (124 — confirm + fade-out + scheduled
        _finalize_close after the animation),
      _on_close (3 — top-level handler).
    `SAOPlayerGUI._sao_fx_after_id` class-attr references in the
    extracted block were rewritten to `type(self)._sao_fx_after_id`
    so the mixin doesn't need to import the not-yet-defined class
    (semantics identical via MRO/class-attr lookup).
    sao_gui.py: 2347 -> 2023.
  SAOPlayerGUI MRO now has 15 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
     FloatHp, FloatHandlers, Lifecycle, State, Session). No name
    conflicts.
  Cumulative refactor: 9682 -> 2023 = -7659 = -79.1%. sao_gui.py is
  6977 lines below the user's 9000-line target. gui_modules/ now
  holds 36 .py / ~28543 lines.

## v3.2.11: float-handlers mixin + Win32 helpers move (rounds 58-59 of
  /loop). sao_gui.py crosses below 2400 lines; cumulative reduction
  reaches 75.8%. MRO depth grows to 14 mixin layers.
  Round 58: SAOPlayerGUIFloatHandlersMixin (193 lines mixin / 137 net
    out). 9 methods covering remaining float-button handlers + misc:
      _float_click, _float_drag, _float_release, _float_enter,
      _float_leave, _lift_float_loop, _raise_panel_window,
      _arm_pending_combat_reset, _setup_hotkeys.
    _create_floating_widget DEFERRED to round 59 (needs Win32 helpers
    moved first to avoid circular import).
    sao_gui.py: 2674 -> 2537.
  Round 59 phase 1: 4 module-level Win32 helpers (63 lines total)
    moved from sao_gui.py to gui_modules/sao_panel_ui.py — natural
    cluster with the existing _apply_panel_style:
      _get_icon_path, _apply_window_icon (iconbitmap + WM_SETICON),
      _set_clickthrough_style (WS_EX_TRANSPARENT),
      _disable_native_window_shadow (DWMNCRP_DISABLED).
    sao_panel_ui.py grew from 195 -> ~280 lines.
    sao_gui.py keeps a single re-import line so callers that still
    reference the unqualified names work transparently.
  Round 59 phase 2: _create_floating_widget (131 lines, the float
    anchor Toplevel constructor) appended to FloatHandlersMixin.
    Mixin imports expanded to include tkinter, ctypes, the 2 Win32
    helpers, and get_cjk_font; mixin declares its own module-level
    _user32 = ctypes.windll.user32 handle (ctypes.windll caches DLL
    handles, so this is identical to sao_gui's _user32).
    sao_gui.py: 2537 -> 2347.
  SAOPlayerGUI MRO now has 14 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
     FloatHp, FloatHandlers, State, Session). No name conflicts.
  Cumulative refactor: 9682 -> 2347 = -7335 = -75.8%. sao_gui.py is
  6653 lines below the user's 9000-line target. gui_modules/ now
  holds 31 .py / ~25600 lines.

## v3.2.10: two more SAOPlayerGUI mixins extracted (rounds 55-56 of /loop).
  sao_gui.py crosses below 2700 lines; cumulative reduction reaches
  72.4%. MRO depth grows to 13 mixin layers.
  Round 55: SAOPlayerGUIPacketCallbacksMixin (420 lines mixin / 357
    net out). 9 methods covering the packet event callback surface:
      _send_linked_key (29) — boss-raid alert → Win32 SendInput
        keypress via auto_key_engine VK_NAME_MAP.
      _on_packet_damage (66) — damage tick callback; updates the
        boss-HP target lock + last-damage timestamp.
      _is_dead_state (10, cython predicate),
      _bump_boss_hp_target_hold (20),
      _boss_monster_usable (18, cython + revive side effect),
      _sync_boss_hp_revive_hold (7).
      _on_monster_update (66) — monster update callback.
      _on_boss_event (6) — delegates to boss_raid_engine.
      _on_scene_change (136, biggest here) — scene transition;
        arm pending combat reset + clear caches.
    Cross-mixin refs still work: _boss_monster_usable used by
    State mixin's _compute_boss_hp_delta; _send_linked_key used by
    EngineLifecycle's _start_recognition; both resolved via MRO.
    sao_gui.py: 3439 -> 3082.
  Round 56: SAOPlayerGUIFloatHpMixin (469 lines mixin / 408 net out).
    20 methods bundling HP overlay context handlers + float button +
    breath animations + the 146-line motion blur effect:
      HP overlay (6): _refresh_hp_layered, _reset_sta_offline_state,
        _should_show_sta_offline, _hp_overlay_on_click,
        _hp_overlay_restore_position, _hp_overlay_hide.
      Float button + animations (14): _build_float_hud_items,
        _set_float_alpha, _animate_float_hud, _start_float_breath,
        _breath_step, _stop_float_breath, _attach_panel_float,
        _panel_float_shared_tick (69, with @_probe.decorate),
        _update_float_display/status/fname/title, _animate_float_to,
        _play_motion_blur (146 — radial blur on menu open/close,
        background thread screen grab + radial blur + main-thread fade).
    Mixin needed `from perf_probe import probe as _probe` for the
    @_probe.decorate('ui.panel_float.tick') on shared_tick (caught
    by initial NameError, fixed before commit).
    sao_gui.py: 3082 -> 2674.
  SAOPlayerGUI MRO now has 13 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
     FloatHp, State, Session). No name conflicts.
  Cumulative refactor: 9682 -> 2674 = -7008 = -72.4%. sao_gui.py is
  6326 lines below the user's 9000-line target. gui_modules/ now
  holds 30 .py / ~25252 lines.

## v3.2.9: two more SAOPlayerGUI mixins extracted (rounds 52-53 of /loop).
  sao_gui.py crosses below 3500 lines; cumulative reduction reaches
  64.5%. MRO depth grows to 11 mixin layers.
  Round 52: SAOPlayerGUIDialogsMixin (268 lines mixin / 196 net out).
    7-method grab-bag of UI-construction helpers:
      _make_player_panel (34) — factory for SAO menu's left widget
        (SAOMenuLeftStack: player panel + session-players panel).
      _hp_overlay_on_menu (41) — right-click HP context menu.
      _show_welcome_then_menu (14) — first-launch flow.
      _show_entity_alert (13) — convenience around alert overlay
        (referenced by Panels / EngineToggles / DpsTheme /
        EngineLifecycle mixins; resolves via MRO).
      _switch_to_webview_ui (22) — confirm + exit + hot restart.
      _show_about (18) — about dialog with updater state hint.
      _edit_profile (55) — profile editor with anti-double-open guard.
    sao_gui.py: 3984 -> 3788.
  Round 53: SAOPlayerGUIEngineLifecycleMixin (441 lines mixin / 349
    net out). The heaviest single remaining cluster:
      _stop_recognition_engines (32) — stops AutoKey/BossRaid/HideSeek/
        packet/vision; clears refs.
      _reconfigure_data_engines (80) — restarts packet+vision engines
        for current mem_data_source setting; loads skill_names.json.
      _start_recognition (204 lines — the biggest method here) — full
        bring-up: GameStateManager + state + 30s cache thread + 7
        overlay windows + AutoKey + BossRaid + linkage.
      _persist_cached_identity_state (34) — write identity to game_cache.
    __file__ fix: _reconfigure_data_engines's inline
      `os.path.dirname(os.path.abspath(__file__))` for the
      skill_names.json bundle was replaced with
      resource_path('assets', 'skill_names.json'). __file__ now
      resolves to gui_modules/ instead of the project root, so the
      BUNDLE/BASE_DIR-aware resource_path() helper is the right path.
    Mixin imports: os, json, AutoKeyEngine, BossAutoKeyLinkage,
      BossRaidEngine, DpsTracker, play_sound, resource_path, + 6
      overlay classes from gui_modules.
    sao_gui.py: 3788 -> 3439.
  SAOPlayerGUI MRO now has 11 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, Dialogs, EngineLifecycle, State, Session). No
    name conflicts across 11 layers.
  Cumulative refactor: 9682 -> 3439 = -6243 = -64.5%. sao_gui.py is
  5561 lines below the user's 9000-line target. gui_modules/ now
  holds 28 .py / ~24363 lines.

## v3.2.8: panel-UI helpers extracted + Status+Updater mixin (rounds 49-50
  of /loop). sao_gui.py crosses below 4000 lines; cumulative reduction
  reaches 58.9%.
  Round 49: gui_modules/sao_panel_ui.py (195 lines) — pulls 10
    module-level _SAO_PANEL_* color constants + 9 helper funcs out of
    sao_gui.py into a focused utility module:
      _apply_panel_style (DWM round-corner), _hex_rgba,
      _make_panel_close_button (+ _close_btn_photo_cache),
      _sao_panel_header, _bind_panel_drag, _sao_panel_body,
      _sao_panel_hud_canvas, _sao_row, _sao_pill.
    sao_gui.py keeps a single re-import line so all still-resident
    panel handlers keep their unqualified usages.
    This unblocks _toggle_status_panel and other panel handlers that
    previously could not move out (would have been a circular import).
    sao_gui.py: 4890 -> 4759. Cumulative refactor crosses -50.0%.
  Round 50: gui_modules/sao_gui_status_updater_mixin.py (857 lines mixin
    / 775 net out). 20 methods: 2 status-panel handlers + 18 updater
    event-chain methods.
    Status panel: _toggle_status_panel (85 lines build/destroy),
      _update_status_panel (21 lines refresh-from-snapshot).
    Updater event chain: _get_update_snapshot, _get_update_view
      (139 lines — biggest single method here; snapshot → display tuple
      formatter), _ensure_updater_listener, _on_update_snapshot
      (root.after dispatch), _mark_update_popup_ready,
      _build_update_popup_payload (58), _maybe_show_update_popup,
      _start_update_download, _start_update_check, _skip_update_version,
      _apply_downloaded_update, _resolve_update_action,
      _set_update_button, _close_update_panel, _open_update_panel
      (111 lines panel UI builder), _refresh_update_panel,
      _check_for_updates_interactive (84), _prompt_update_available.
    sao_gui.py: 4759 -> 3984.
  SAOPlayerGUI MRO now has 9 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     StatusUpdater, State, Session). No method conflicts.
  Cumulative refactor: 9682 -> 3984 = -5698 = -58.9%. sao_gui.py is
  5016 lines below the user's 9000-line target. gui_modules/ now
  holds 26 .py / ~23654 lines.

## v3.2.7: two more SAOPlayerGUI mixins extracted (rounds 46-47 of /loop).
  sao_gui.py crosses below 4900 lines; cumulative reduction reaches
  49.5%. MRO depth grows to 8 mixin layers.
  Round 46: SAOPlayerGUIDpsThemeMixin (286 lines mixin / 226 net out).
    16 DPS methods + 3 Theme methods bound by menu-refresh + overlay-
    update pattern.
    DPS surface (timeouts, snapshots, report availability, overlay show
      paths, reset + toggle): _combat_damage_timeout_s,
      _boss_hp_hold_timeout_s, _cancel_dps_idle_reset_after,
      _schedule_dps_idle_reset_after_fade, _empty_dps_snapshot,
      _get_dps_last_report_available, _sync_dps_report_availability,
      _request_dps_live_snapshot, _get_dps_last_report,
      _request_dps_last_report, _request_dps_entity_detail,
      _show_dps_live_snapshot, _show_dps_last_report,
      _reset_dps_tracker, _show_last_dps_report_menu, _toggle_dps_enabled.
    Theme switcher: _toggle_panel_theme, _set_all_themes,
      _apply_theme_to_overlay. References self._THEME_OVERLAY_MAP
      (class attr on SAOPlayerGUI, resolved via MRO).
    sao_gui.py: 5303 -> 5077.
  Round 47: SAOPlayerGUIPanelsMixin (255 lines mixin / 187 net out).
    11 methods covering Commander, panel visibility, small settings:
    Commander: _toggle_commander_panel, _push_commander_data (48-line
      packet-driven snapshot builder, GUI-level sig-cached).
    Panel visibility: _toggle_hide_all_panels (51-line
      snapshot+restore across 7 panel handles).
    Recognition / sound / buffmon / boss-bar / mem-source / topmost:
      _toggle_recognition_menu, _toggle_sound_enabled,
      _adj_sound_volume, _toggle_buffmon_enabled, _cycle_boss_bar_mode,
      _get_mem_data_source, _cycle_mem_data_source, _toggle_topmost.
    Mixin imports: perf_probe.probe (for @_probe.decorate) +
      CommanderPanel from the already-relocated gui_modules.
    _toggle_status_panel deferred — references 10+ module-level
    sao_gui UI helpers (_SAO_PANEL_HEADER_BG, _sao_panel_header,
    _bind_panel_drag, etc.); moving it requires either pulling those
    helpers along or introducing a circular import.
    sao_gui.py: 5077 -> 4890.
  SAOPlayerGUI MRO now has 8 mixin layers (in extraction order):
    (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
     State, Session). No method conflicts across the chain.
  Cumulative refactor: 9682 -> 4890 = -4792 = -49.5%. sao_gui.py is
  4110 lines below the user's 9000-line target. gui_modules/ now
  holds 24 .py / 22602 lines.

## v3.2.6: two more SAOPlayerGUI mixins extracted (rounds 43-44 of /loop).
  sao_gui.py crosses below 5400 lines; cumulative reduction exceeds 45%.
  Round 43: SAOPlayerGUIActionsMixin (214 lines mixin / 152 net out).
    Bundles AutoKey + BossRaid clusters because they share the same
    shape: toggle_panel + toggle_detail_panel + toggle_engine +
    config_loader + config_saver + author_snapshot.
    AutoKey (9 methods): _toggle_autokey_panel,
      _toggle_autokey_detail_panel, _toggle_auto_script,
      _auto_key_settings_ref, _auto_key_author_snapshot,
      _load_auto_key_config, _save_auto_key_config,
      _load_autokey_burst_actions, _save_autokey_burst_actions.
    BossRaid (8 methods): _toggle_bossraid_panel,
      _toggle_bossraid_detail_panel, _toggle_boss_raid,
      _boss_raid_next_phase, _boss_raid_settings_ref,
      _boss_raid_author_snapshot, _load_boss_raid_config,
      _save_boss_raid_config.
    sao_gui.py: 5582 -> 5430.
  Round 44: SAOPlayerGUIEngineTogglesMixin (175 lines mixin / 127 net
    out). Bundles HideSeek + Burst — two distinct engine on/off
    clusters but the same toggle + refresh pattern.
    HideSeek (6 methods): _toggle_hide_seek, _start_hide_seek,
      _stop_hide_seek (instantiates HideSeekEngine + WindowLocator,
      AlertOverlay persistent UI), _on_hide_seek_status,
      _schedule_hide_seek_alert_refresh (50s refresh tick),
      _refresh_hide_seek_alert.
    Burst (5 methods): _pick_burst_trigger_slot (cython delegate),
      _normalize_watched_skill_slots (cython), _reset_burst_tracking_state,
      _toggle_burst_enabled, _toggle_burst_slot (preserves ≥1 slot
      invariant).
    sao_gui.py: 5430 -> 5303.
  SAOPlayerGUI now has 6 mixin layers via MRO:
    (MenuMixin, FisheyeMixin, ActionsMixin, EngineTogglesMixin,
     StateMixin, SessionMixin).
  Cumulative refactor: 9682 -> 5303 = -4379 = -45.2%. sao_gui.py is
  3697 lines below the user's 9000-line target. gui_modules/ now
  holds 22 .py / 22061 lines.

## v3.2.5: two more SAOPlayerGUI mixins extracted (rounds 40-41 of /loop).
  sao_gui.py crosses below 5600 lines — cumulative reduction now
  exceeds 42%.
  Round 40: SAOPlayerGUIMenuMixin (566 lines) — the full SAO PopUpMenu
    lifecycle. 17 methods, ~488 lines net out of sao_gui.py:
      - construction: _setup_sao_menu, _build_menu_children (139),
        _build_update_menu_label
      - open/close: _toggle_sao_menu, _close_sao_menu_from_background,
        _clear_sao_menu_close_pending
      - hooks: _on_sao_menu_open, _on_sao_menu_close,
        _dismiss_sao_menu_for_panel
      - refresh: _refresh_menu_if_open (debounced), _refresh_menu_immediate,
        _apply_menu_refresh_if_open
      - sig caching: _compute_menu_refresh_signature (200 ms-cached, 78
        lines), _get_menu_children_cached, _cancel_pending_menu_refresh
      - persistence: _persist_entity_menu_state, _restore_entity_menu_state
    sao_gui.py: 7431 -> 6943.
  Round 41: SAOPlayerGUIFisheyeMixin (1421 lines) — the persistent GPU
    fisheye overlay. 9 methods, 1362 lines net out, including the
    SINGLE BIGGEST method in all of SAOPlayerGUI:
      - _start_fisheye_overlay (1078 lines) — builds the full GPU
        rendering pipeline (PIL capture -> numpy/GPU distortion ->
        BGRA bytes -> GpuOverlayWindow + BgraPresenter @60fps) plus
        the transparent Tk hit layer and Win32 z-order management
      - _stop_fisheye_overlay (74) — daemon shutdown + GPU release
      - _run_fisheye_entry (103) — entry-animation flow
      - _fisheye_close_suppressed, _release_fisheye_input_zorder,
        _destroy_fisheye_hit_layer, _start_fisheye_with_retry,
        _any_panel_open, _maybe_stop_fisheye (smaller helpers)
    sao_gui.py: 6943 -> 5582.
  SAOPlayerGUI now inherits from 4 mixins via MRO:
    (MenuMixin, FisheyeMixin, StateMixin, SessionMixin).
  Cumulative refactor: 9682 -> 5582 = -4100 = -42.3%. sao_gui.py is
  3418 lines below the user's 9000-line target. gui_modules/ now
  holds 20 .py / 21672 lines.

## v3.2.4: file reorganization — ALL sao_gui_*.py satellite modules moved
  into gui_modules/ (rounds 37-38 of /loop). Addresses the user's
  "用文件夹来把所有的文件归类，包括以前的文件" objective.
  Round 37 (3 small files): sao_gui_menu_hud (347 lines),
    sao_gui_alert (457), sao_gui_commander (363) via git mv. Imports
    updated in sao_gui.py + sao_theme.py + XiaoACTUI.spec. Total:
    1167 lines moved.
  Round 38 (8 files atomic, hub module dependency forced this):
    sao_gui_dps.py (3188; hub — imported by 5 other overlay files),
    sao_gui_hp.py (3418), sao_gui_bosshp.py (3074),
    sao_gui_skillfx.py (1847), sao_gui_buffmon.py (1188),
    sao_gui_profile_editors.py (1231), sao_gui_autokey.py (547),
    sao_gui_bossraid.py (544). Total: 15037 lines moved.
    5 cross-imports updated within gui_modules/ (the hub module
    reference in hp / bosshp / skillfx / buffmon / alert).
    6 importer lines updated in sao_gui.py.
    4 lurking old-path imports caught + fixed:
      - gui_modules/sao_gui_hp.py: `import sao_gui_hp as _mod`
        self-import for theme setattr (would have ImportError at
        runtime on theme-switch toggle)
      - gui_modules/sao_gui_skillfx.py: same pattern
      - tools/bench_compose.py, tools/spike_skillfx_ab.py (dev tools)
  XiaoACTUI.spec hiddenimports: 11 entries updated to gui_modules.X
  dotted paths, plus added GUI_MODULES_HIDDENIMPORTS =
  collect_submodules('gui_modules') as a catch-all for future
  additions.
  Final state: 0 sao_gui_*.py files at repo root. gui_modules/ now
  holds 19 .py files / 18885 lines covering the full SAO HUD /
  overlay / panel layer + the 7 mixin/helper extractions from
  earlier rounds. sao_gui.py: 7375 (unchanged — it's the importer,
  not the source). Cumulative refactor still 9682 → 7375 = -2307
  (-23.8%).

## v3.2.3: COMBAT-LAG FIX — boss-HP compute moved off the Tk main thread
  (rounds 34-35 of /loop). Addresses the user's "重战斗卡顿" complaint.
  Round 34 (same-thread refactor):
    - Extracted the 260-line boss-HP inline compute from
      _push_packet_overlays into a new mixin method
      _compute_boss_hp_delta(gs, _pp_now). The helper returns the
      overlay update dict or None; touches no Tk widget so it's
      safe to call from any thread. Pure refactor, no behavior change.
  Round 35 (the actual fix):
    - Added a daemon worker thread (sao-boss-hp-worker) that consumes
      (gs, now) snapshots from a single-slot latest-wins mailbox and
      computes the overlay payload off the Tk main thread.
    - The 5 Hz recognition loop now only does: enqueue snapshot (O(1))
      + consume previous tick's payload + a single overlay.update()
      call. The ~260 lines of bridge.get_monster() / sort / dict-build
      / sig-hash compute happen on the worker thread.
    - Latency tradeoff: boss-HP overlay payload is from PREVIOUS tick
      (at most 200 ms behind), well below human visual detection
      threshold for HP-bar changes.
    - Five new mixin methods: _ensure_boss_hp_worker,
      _boss_hp_worker_loop, _enqueue_boss_hp_compute,
      _consume_boss_hp_payload, _stop_boss_hp_worker. Shutdown hook
      added to SAOPlayerGUI._finalize_close before overlay teardown.
    - Out-of-process smoke test verifies start / enqueue / consume /
      stop semantics.
  sao_gui.py: 7368 → 7375 (+7, just the _stop_boss_hp_worker hook in
  _finalize_close). State mixin: 794 → 913 lines (+119, worker
  scaffold). Cumulative refactor: 9682 → 7375 = -2307 = -23.8%.

## v3.2.2: sao_gui refactor reaches the mixin stage (rounds 31-32 of /loop).
  The first two SAOPlayerGUI mixins are extracted — this is the
  structural turning point: instead of pulling helper classes out
  around SAOPlayerGUI, the class itself is now being sliced.
    - gui_modules/sao_gui_session_mixin.py (208 lines) — 8 in-session
      player roster helpers (_merge_session_player,
      _sync_session_players_cache, _refresh_session_players_panel,
      _toggle_session_players_panel, plus 4 smaller utilities). They
      move as a unit because they call each other through self.
    - gui_modules/sao_gui_state_mixin.py (770 lines) — the four
      methods at the heart of the user's combat-lag complaint:
      _on_game_state_update, _apply_fast_state_update,
      _push_packet_overlays (HOT PATH; per-tick DPS/Boss/HP/SkillFX
      push), and _recognition_loop (200 ms Tk-after-driven loop).
      Isolating this cluster sets up the actual combat-lag fix:
      round 34+ will introduce a daemon worker that consumes packet
      snapshots off-main and re-enters Tk via root.after(0, ...).
  SAOPlayerGUI now inherits from (SAOPlayerGUIStateMixin,
  SAOPlayerGUISessionMixin); MRO resolves the 12 extracted methods
  transparently. py_compile + import sao_gui both clean.
  sao_gui.py shrinks 8388 -> 7368 (-1020 net since v3.2.1; cumulative
  refactor: 9682 -> 7368 = -2314 = -23.9%). gui_modules/ now holds
  978 lines of mixin code on top of the earlier 1338 lines of
  extracted helper classes.

## v3.2.1: sao_gui refactor continues (rounds 27-post / 28 / 29).
  Three more extractions to gui_modules/ + a dead-code purge:
    - SAOPlayerPanel (471 lines) -> gui_modules/sao_player_panel.py.
      This pushed sao_gui.py below the user's 9000-line target.
    - SAOMenuLeftStack (117 lines) -> gui_modules/sao_menu_left_stack.py.
    - SettingsManager (53 lines) -> gui_modules/settings_manager.py;
      CONFIG_FILE resolved via parent-of-parent so dev-mode path
      (sao_auto/settings.json) is preserved.
  Dead-code purge: _update_layered_win (Win32 layered-window helper,
  ~58 lines) + its 4 ctypes structs (_BLENDFUNCTION / _ULW_SIZE /
  _ULW_POINT / _BITMAPINFOHEADER). Confirmed unused via repo-wide grep;
  other ULW consumers all have their own copies. _user32 / _gdi32
  signature setup stays since 32+ call sites in sao_gui still need it.
  sao_gui.py shrinks 9091 -> 8388 lines (-703 net since v3.2.0;
  cumulative refactor: 9682 -> 8388 = -1294 = -13.4%).

## v3.2.0: sao_gui structural refactor begins (rounds 25-26 of /loop).
  Minor bump signals a visible structural change: sao_gui.py monolith
  (9682 lines) is being split into gui_modules/* subpackage. First two
  extractions land in this commit:
    - gui_modules/sao_hotkey_manager.py (75 lines) — SAOHotkeyManager
      moved out; sao_gui re-exports it for backward compatibility.
    - gui_modules/sao_session_players_panel.py (598 lines) — the
      SESSION PLAYERS left-stack panel + its private wheel-routing
      registry helpers (_SESSION_WHEEL_ROOTS, _dispatch_session_wheel)
      all moved together so the module is self-contained.
  sao_gui.py shrinks 9682 -> 9091 lines (-591 net). Public class names
  (`SAOHotkeyManager`, `SAOSessionPlayersPanel`) still importable from
  sao_gui so existing call sites and external consumers unaffected.

## v3.1.9: sao_gui main-thread polish (rounds 21-post / 22 / 23 of /loop).
  - _refresh_session_players_panel: inner _sync_session_players_cache
    call now also passes min_interval=0.25 (matches the outer call from
    _push_packet_overlays). The double-sync when menu was visible is
    gone; both call sites share the 4 Hz cadence.
  - _lift_float_loop: cadence bumped 150 ms -> 250 ms (~40% fewer
    per-sec SetWindowPos calls when the SAO menu is open).
  - _on_game_state_update: identity-compare gs.self_buffs against the
    last cached reference to skip ov.update_buffs() when the bridge
    hasn't pushed a fresh self_buffs list. Works because round-13's
    shallow-copy snapshot keeps list refs stable across non-buff
    state updates. Saves ~10-20 us per skipped call across 30-60 Hz.

## v3.1.8: sao_gui main-thread reduction (rounds 19-20 of a new targeted /loop).
  - sao_gui._recognition_loop: character_profile.save_profile() (sync
    settings.json read+write, 5-50 ms on slow disks) moved to a daemon
    thread on first identity arrival. Main loop no longer pays the
    one-frame hitch.
  - sao_gui._stop_fisheye_overlay: worker_thread.join(2.0) + GPU window
    destroy + presenter release moved to a daemon thread. Main thread
    does only the light Tk-bound work + flips running[0]. Legacy Tk
    Toplevel destroy re-dispatched back via root.after(0, ...) so
    thread-affine widgets stay on the main loop.
  - sao_gui._push_packet_overlays: bumped _sync_session_players_cache
    min_interval from 0.05 -> 0.25 (effective throttle from never-firing
    to 4 Hz). Cuts ~20-50 us/call * ~5 calls/sec main-thread work.

## v3.1.7: signature trim + throttle + deepcopy (rounds 15-post / 16 / 17 of perf /loop).
  - recognition._row_independent_pct: dropped unused `hue` and
    `fill_hue_ref` placeholder args (left over from round 4 cython
    migration). Single caller updated.
  - boss_raid_engine.BossRaidEngine.on_damage_event: replaced the
    per-event _fire_entity_update_locked() (rebuilds full entity dict
    each call) with a dirty-bit flip. _run_loop (4 Hz) drains the bit
    via _maybe_flush_entity_update_locked() honouring a 10 Hz UI cap.
    Saves 2.5-15 ms/sec of damage-handler time under heavy combat.
  - auto_key_engine.AutoKeyEngine.get_status: deepcopy -> dict() shallow
    copy on a flat 7-field primitive dict (~38x faster: 2605 -> 68 ns).

## v3.1.6: structural caching + cleanup (rounds 12-post / 13 / 14 of perf /loop).
  - dps_tracker: dropped the now-dead _compute_damage_id and
    _resolve_skill_key Python wrappers (rounds 1 and 10b inlined them
    into _CY_COMBAT direct calls; no external callers in the repo).
  - game_state.GameStateManager.update + load_cache: replaced the
    dict-comp + dataclass __init__ snapshot path (47 fields, ~3.5 us)
    with copy.copy(self._state) — ~1.0 us/call, 3.4x faster. Snapshot
    independence + shallow-copy list semantics preserved.
  - packet_bridge._publish_player_update: cached the 5 distinct
    _use_packet_source() results at function entry, replacing 9
    repeated calls. Also cached the lock-guarded self._state_mgr.state
    (3 accesses -> 1). ~2-2.5 us + 2 lock acquires saved per publish.

## v3.1.5: DPS micro-opts + auto_key engine cache (rounds 10-11 of perf /loop).
  - dps_tracker._build_snapshot_locked: hit_fx shallow-copy via dict()
    replaces copy.deepcopy (flat dict of primitives, ~10x faster on
    every UI poll).
  - dps_tracker._process_event: inlined _CY_COMBAT.resolve_skill_key
    call with a cached module-level skill-effect table; saves one
    Python wrapper call per damage event (~1.3 us/event end-to-end).
  - auto_key_engine.AutoKeyEngine._tick: cached normalize_auto_key_config
    result keyed by raw-dict id + player identity tuple. The 20 Hz
    engine tick now skips re-normalisation when settings + identity
    unchanged. ~75x speedup on cache hits (17.4 us -> 231 ns/tick).

## v3.1.4: Cython per-bar numerics (rounds 7-8 of comprehensive perf /loop).
  - _sao_cy_pixels.box_convolve5_same_f32: 5-wide moving-average
    smoothing in nogil, replaces both np.convolve calls in
    _detect_bar_pct (~3x faster).
  - _sao_cy_pixels.find_last_above_threshold_f32: rightmost-above-
    threshold scan with the redundant single-pixel-fill repair
    mathematically eliminated; replaces the >=/any/where/max chain
    in _detect_bar_pct (~27x faster).
  - _sao_cy_pixels.compute_bar_col_score_f32: folds the 8-op
    hue_delta / hue_bonus / col_score chain into one nogil pass,
    eliminating ~7 temp float32 arrays per bar (~7x faster).
  - End-to-end _detect_bar_pct: ~0.33 ms/call (down from ~0.9 baseline).

## v3.1.3: Cython per-bar recognition migration (rounds 4-5 of perf /loop).
  - recognition._detect_stamina_pct now uses _sao_cy_pixels.
    bgr_color_match_column_ratio — single nogil pass with squared-distance
    test, no sqrt, ~14x faster than the numpy reference path.
  - recognition._row_independent_pct now uses _sao_cy_pixels.
    row_independent_fill_pct — per-row score + 3-wide convolution +
    sub-pixel + quickselect median in one nogil block, ~15x faster.
  - recognition._gradient_edge_pct now uses _sao_cy_pixels.gradient_edge_pct
    — diff + 7-wide convolution + argmin + mid-score crossing in one nogil
    block, ~4.3x faster with parity-exact output (delta=0 on 7/7 cases).

## v3.1.2: Cython hot-path migration (rounds 1-2 of comprehensive perf /loop).
  - dps_tracker._compute_damage_id + skill_key fallback moved into
    _sao_cy_combat.compute_damage_id / resolve_skill_key (~2-3x faster per
    damage event, parity-verified across 8 cases).
  - packet_parser._parse_dirty_stream sub-field header parsing collapsed
    into _sao_cy_packet.parse_dirty_subfield_header (8 branches refactored,
    ~53% faster per branch, ~50 lines of boilerplate removed).

## v2.2.12 — SAO menu HUD now drives a per-pixel-alpha layered window
(UpdateLayeredWindow) composed off-thread on the heavy render lane,
replacing the legacy chroma-key Toplevel + per-tick `geometry()` move
(which forced un-vsync'd DWM region recomposites and was the dominant
tearing source). Set `SAO_GPU_MENU_HUD=0` to fall back to the legacy
canvas-native path for diagnostics.

## v2.3.0 (2026-04 fix): The whole GLFW-backed GPU overlay family
(menu bar fisheye painter, left info painter, menu HUD GPU window,
child bar painter, skillfx GPU pump) defaults to GPU when the shared
GLFW/ModernGL backend is available. Do not gate this with environment
variables; startup animations such as LinkStart rely on the GPU window
being created by default.

## v3.0.3
  Fix DPS overlay click-through after the second fade cycle. The
  v3.0.2 fix marshalled the whole click-through toggle to the GLFW
  pump as fire-and-forget commands; rapid fade_in / fade_out turns
  queued up there and the second hide pulse landed visible-but-faded
  while still grabbing clicks. v3.0.3 splits the toggle: the Win32
  ex-style flip (WS_EX_TRANSPARENT) is applied synchronously from
  the calling Tk thread (SetWindowLongPtrW is thread-safe), and only
  the GLFW_MOUSE_PASSTHROUGH attribute mirror is queued to the pump.

## v3.0.2
  Fix DPS overlay click-through while idle / faded out. The GPU
  panel now flips GLFW_MOUSE_PASSTHROUGH (in addition to the Win32
  ex-style) on the pump thread when fade_out() runs, and the tick
  loop re-asserts pass-through every idle frame so a focus/activation
  event can no longer leave the invisible panel quietly intercepting
  clicks meant for the game window.

## v3.0.1
  Minor bug fixes and performance improvements. Set up buff monitor.

## v3.0.0
  Added a new memory mode for combat data, which is more direct and has
  lower latency than the previous implementation. This mode is enabled by default,
  but can be disabled via the saomenu.

## v2.5.25:
    • Prevent BossHP raid/packet state from showing party members or stale
      non-damaged units when no live self-damage target is locked.

## v2.5.24:
    • Tighten Entity BossHP target gating: monster updates no longer adopt
      untargeted NPCs/party units, packet BossHP stays visible only for a
      self-damaged tracked target, and normal timeout/stable-hide can hide it
      again after combat ends.

## v2.5.23:
    • Keep Entity BossHP visible while packet boss data is valid instead of
      hiding it after the startup/scene grace window; stable-hide no longer
      suppresses a live packet-backed BossHP snapshot.

## v2.5.22:
    • Add packet-capture self-healing for long idle stalls: bridge timeout
      now clears the capture endpoint lock for re-detection and can request a
      pcap handle restart when raw frames stop, matching SRDPS-style idle
      reconnect behavior. Also align deferred DPS/BossHP reset with
      resonance semantics and route wipe buff 510072 into soft restart.

## v2.5.21:
    • Harden Entity DPS/BossHP recovery across map switches and mid-session
      startup: replay early self dirty packets after UID confirmation, keep a
      scene/startup damage grace window, force-show the first live DPS
      snapshot, and allow packet BossHP data to wake during that grace.

## v2.5.20:
    • Fix Entity DPS/BossHP recovery after map switches and mid-session
      startup by keeping the post-restart damage callback alive and canceling
      stale DPS idle-reset timers when fresh self combat damage arrives.

## v2.5.19:
    • Restore close motion blur and move fisheye backdrop input off the GPU
      fisheye window: the GPU backdrop is now click-through/render-only,
      while a transparent Tk hit layer handles backdrop click/drag/wheel;
      keep the popup GPU window and Tk shell raised above that hit layer.

## v2.5.18:
    • Do not stack the fullscreen motion-blur close overlay on top of the
      fisheye backdrop's own fade-out when closing by clicking the fisheye
      background; ID/HP/menu closes still keep their normal close blur.

## v2.5.17:
    • Preserve the fisheye backdrop window lifecycle during background-click
      close: demote/click-through only, keep the GPU window visible for its
      normal fade-out, and prevent stop-requested fade-out from rebounding
      into fade-in so the backdrop can reopen cleanly without black flashes.

## v2.5.16:
    • On fisheye-background menu close, synchronously hide the fisheye HWND,
      demote it below topmost, and make it click-through before the popup
      close animation runs, preventing background-click-only z-order steals.

## v2.5.15:
    • Restore automatic DPS idle fade-out and live-damage reset after the
      fade completes while keeping the last report, without regressing the
      post-scene fresh-damage path that reopens DPS/BossHP after map switches.

## v2.5.14:
    • Suppress fisheye restart/fade-in while a backdrop click is closing the
      SAO menu, so stale menu-visible state cannot flash the black fisheye
      background back to top or steal z-order during close.

## v2.5.12:
    • Release SAO popup/fisheye input and z-order immediately when closing
      by demoting their HWNDs below topmost and enabling click-through before
      fade/worker teardown, preventing closing overlays from stealing clicks.

## v2.5.11:
    • Keep BossHP visible during invincible / lock-HP boss mechanics while
      DPS still has recent live damage, and protect self/team/known player
      UIDs from the post-scene combat-target grace so teammates never become
      fake boss-bar targets.

## v2.5.10:
    • Keep DPS/BossHP recoverable after hard map switches by preserving the
      DPS overlay window, re-opening live DPS on fresh damage, and adding a
      short post-scene self-damage grace path for stale target classification.
    • Prevent SAO menu backdrop/fisheye clicks from re-raising the popup while
      closing, and strengthen DPS panel contrast, row glow, scanlines, and
      list masking so combat text remains readable over bright game scenes.

## v2.5.9:
    • Parse EnterScene payloads during map/server transitions so scene keys
      and self entity UUID refresh correctly, and prevent delayed scene-hide
      callbacks from suppressing fresh DPS/BossHP live updates after切图.

## v2.5.8:
    • Move packet byte decoding, GUI layout/signature math, entity-menu
      hit testing/animation helpers, and HP/DPS/BossHP pixel/formatting
      hot paths into mandatory Cython helpers to reduce Python-frame
      overhead without lowering overlay effects or frame cadence.

## v2.5.7:
    • Align DPS/BossHP scene and dungeon transition handling with
      StarResonanceDps / resonance-logs-cn, including deferred combat resets,
      dungeon dirty target progress, EnterScene scene IDs, and richer damage
      fields for stable all-map routing.
    • Expand skill_names coverage from the upstream merged skill table and
      switch DPS/HPS per-skill aggregation to resonance-logs-cn style
      SkillFightLevelTable-backed damage IDs.

## v2.5.6:
    • BossHP main-target picker uses the highest MAX_HP unit (the actual
      boss) instead of the most-full hp_pct, so a 100 % overworld trash
      next to a 28 % real boss no longer hijacks the bar.
    • boss_hp_hold_timeout_s floor dropped from 180 s → 1 s; user-set
      fade values now actually take effect when no damage is happening.
    • Hard scene resets push BossHP / DPS hide() immediately instead of
      deferring 120 ms behind a token check that stray packets could bump.

## v2.5.5:
  Restore UID/POWER Session Players wheel and click-drag scrolling through
  the fisheye backdrop, align target classification with StarResonanceDps
  / resonance-logs-cn so BOSS HP / DPS keep routing across map changes,
  and tighten BossHP fade behavior:
    • Fisheye GpuOverlayWindow now forwards wheel + cursor + button events
      to the Tk session_panel underneath (so wheel scroll and touch-style
      drag both work even though the Tk shell is chroma-keyed transparent).
    • Damage target classification (parser AoiSyncDelta SkillEffect path
      and the GUI _normalize_damage_event_target_for_entity fallback) now
      trusts UUID encoding over stale _monsters / _team_members / _players
      caches, mirroring SRDPS `IsUuidPlayerRaw` / SRLOGS `EEntityType`.

## v2.5.4:
  Route Entity SAOMenu backdrop clicks through the normal close animation:
  popup empty-area clicks and full-screen fisheye-background clicks now
  trigger the same menu-close/motion-blur/fisheye fade-out path instead of
  leaking to the game or only being swallowed silently.

## v2.5.3:
  Restore UID/POWER Session Players wheel input while the GPU panel is
  visible, keep the fisheye backdrop as a background layer so popup buttons
  remain the only interactive menu targets, swallow popup empty-area clicks,
  and move self-damage unknown-target fallback into the parser so repeated
  dungeon entries do not lose DPS/BossHP before GUI fallback can run.

## v2.5.0:
  Make the Entity SAOMenu fisheye backdrop non-click-through so clicks on
  the blurred background no longer leak to the game. Also make combat panel
  reveal decisions more robust: DPS can open from an existing live snapshot
  even if the dirty flag was already consumed, and BossHP no longer suppresses
  the first confirmed self-damage target as a hidden baseline.

## v2.4.40:
  Stop Entity SAOMenu GPU windows from click-through leaking to the game or
  panels behind them. Interactive GpuOverlayWindow instances now explicitly
  clear inherited Win32 WS_EX_TRANSPARENT / WS_EX_NOACTIVATE styles after
  GLFW window creation, complementing the existing sticky GLFW hint reset.

## v2.3.0: GPU SDF shader pipeline for SkillFX (ring + beam + glow as a
single fragment-shader pass). Replaces the old PIL/numpy compose that
cost 60-90 ms per frame on the render worker; the GPU path runs the
whole layer set in ~2-5 ms on the integrated GPU. Caption sprites
still PIL (cached statically). Falls back to the CPU path on any
pipeline failure. Set `SAO_SKILLFX_GPU=0` to force the legacy CPU
path for diagnostics.

## v2.4.37:
  Fix Entity HP overlay border/STA clipping caused by using SetWindowRgn
  as an input hit region; keep the full window visible and use dynamic
  click-through outside HP/ID hit zones instead. Also harden packet int32
  varint decoding against over-wide combat AOI attr encodings.

## v2.4.36:
  Fix Bosshp targets.

## v2.4.33:
  Second pass of cython acceleration on the recognition loop and the
  protobuf parser hot helpers, removing the remaining Python-side
  "calculation stalls" reported on slower CPUs.
    - `_sao_cy_uihelpers` adds `breath_offsets()` (60-fps float HUD sin
      offsets) and `compute_skillfx_layout()` (window/viewport/slot rect
      geometry for the BurstReady overlay).
    - `_sao_cy_packet` adds `varint_to_int64`, `varint_to_int32`,
      `decode_string_from_raw`, `decode_dirty_energy_value`,
      `is_sane_attr_stamina_max`, `normalize_season_medal_level`,
      `level_extra_source_priority`, `attrs_match_monster_hint`. These are
      per-packet hot paths in `packet_parser.py`.
    - `sao_gui._breath_step` and `_get_skillfx_layout` are now thin entries
      that gather inputs and delegate; same for the corresponding parser
      helpers.
  Microbench (cp311, x64): breath_offsets ≈ 0.06 µs / call,
  compute_skillfx_layout(9 slots) ≈ 5.2 µs / call,
  decode_dirty_energy_value ≈ 0.10 µs / call.

## v2.4.32:
  `dev_publish.py` / `dev_publish_gui.py` now smart-detect the recent
  cython refactor pattern (added/edited/removed `_sao_cy_*.pyx` and the
  matching `.pyd` artefacts). Highlights:
    - `git_changed_files_with_status()` separates added / modified /
      deleted, so deletes feed `manifest.removed_files` instead of being
      silently dropped.
    - `auto_rebuild_cython_if_stale()` detects a `.pyx` whose `.pyd` is
      missing or older and runs `build_cython_ext.py build_ext --inplace`
      in-place, then folds the freshly built `.pyd` into the change set.
    - New `.pyx` sources unregistered in `build_cython_ext.py` raise a
      visible warning during diagnose.
    - Deleted `.py` / `.pyx` / `.pyd` / data files become orphan-cleanup
      hints (`runtime/<x>.pyc`, `runtime/<x>.<EXT_SUFFIX>`, raw rel-paths)
      embedded both in `manifest.removed_files` and a zip-level
      `__remove_files__.json` sidecar.
    - Pure-delete publishes still produce a `runtime-delta` zip with the
      sidecar so the client can clean up without a body of new files.
  `update_apply.py` reads either signal, backs each orphan into the
  per-version `backup/__removed__/` tree before deletion, and refuses
  paths that escape `base/` or target the launcher/update exe.

## v2.4.31:
  New `_sao_cy_uihelpers` extension. The recognition-loop / panel-float
  pure-logic helpers in `sao_gui.py` (`_pick_burst_trigger_slot`,
  `_panel_float_shared_tick` sin offsets, `_format_level_text`,
  `_normalize_watched_skill_slots`, `_is_dead_state`, `_boss_monster_usable`,
  `_session_int`, `_format_session_power`) now route through cython.
  Side-effecting parts (Tk/PIL widget calls, monster.is_dead revive flip)
  stay in Python; the cython side returns intent flags only.

## v2.4.30:
  Move `dps_tracker.SkillStats` / `EntityStats` and the per-tick snapshot
  builder into `_sao_cy_combat`. `add_damage`, `add_heal`, `add_taken`,
  `to_dict`, `build_entity_snapshot`, and big-hit FX tier classification now
  run as Cython `cdef class` methods with C-typed fields. The Python module
  re-exports the names so external imports stay stable.

## v2.4.29:
  Fix DPS/BossHP not displaying stably and counting phantom HP/monsters after
  map switches. SyncNearEntities Disappear of non-Dead types (FAR_AWAY,
  REGION, TELEPORT, ENTER_VEHICLE, ENTER_RIDE) now evicts monsters from the
  parser cache instead of letting them linger. Soft scene transitions and
  restarts purge monsters whose `last_update` is older than 15 / 30 s, and
  Entity + WebView soft-scene paths clear `_bb_recent_targets` /
  `_bb_last_target_uuid` so the next damage event repopulates the boss bar
  with the live target.

## v2.4.27:
  Make Cython accelerators mandatory instead of optional: packet/combat,
  pixel premultiply/alpha, packet capture frame parsing, and SkillFX math
  kernels now fail fast if the matching _sao_cy*.pyd is missing. Runtime
  Python/NumPy/Numba fallbacks were removed from those hot paths.

## v2.4.26:
  Force Entity player UID/POWER panels onto the GPU overlay path, prewarm
  their GLFW windows asynchronously, and avoid Tk fallback redraws that made
  the first SAO menu open and large Session Players scrolls stutter.

## v2.4.24:
  Keep the Session Players GPU panel attached to the SAO menu shell,
  make Entity BossHP fixed-position/click-through, and treat the first
  BossHP target sample as a hidden baseline so HP-stable targets do not
  briefly pop before the auto-hide rule applies.

## v2.4.23:
  Default the Entity Session Players panel to the GPU painter when the
  shared GPU overlay gate is available, keep DPS/BossHP fade-out on exit,
  and classify service-declared or already-registered monsters before the
  player-like UUID fallback so repeated-instance and overworld targets keep
  driving DPS/BossHP panels.

## v2.4.22:
  Smooth the Entity Session Players panel by opening it from a collapsed
  height only on first reveal. Repeated panel/menu button clicks now keep the
  already-visible Session Players panel steady and only refresh row data,
  matching the player info panel behavior in Entity and WebView.

## v2.4.21:
  Align Session Players open timing with the player info panel in both
  WebView and Entity UI. The right-side WebView panel now uses the same
  one-second reveal cadence as leftInfo, and Entity starts the Session
  Players animation alongside the player panel instead of racing ahead.

## v2.4.20:
  Restore the Session Players open animation after making the panel
  persistent. WebView now restarts the right-side panel animation whenever it
  is shown/refreshed, instead of relying on the first `show` class transition.

## v2.4.19:
  Make Session Players a persistent show/refresh panel instead of a toggle in
  both Entity and WebView Saomenu, so it opens together with player info and
  cannot disappear on repeated clicks. Session Players column headers now use
  the normal UI font to avoid the small SAO-font stroke artifact near NAME.

## v2.4.18:
  Keep WebView Session Players on its right-side Saomenu layout and reveal it
  whenever menu data syncs while the menu is open. Entity Session Players now
  defaults to the embedded panel below player info and keeps the live panel
  reference so refreshes cannot early-return before painting.

## v2.4.17:
  Restore the Saomenu Session Players panel in Entity and WebView UI, while
  keeping the v2.4.16 GPU/lazy rendering optimizations. WebView -> Entity
  switching now launches a fresh entity process after saving ui_mode/game_cache
  so stale WebView/.NET window state cannot corrupt or block the new Saomenu.

## v2.4.15:
  Restore Saomenu fisheye enter/exit dynamics in both Entity and WebView:
  Entity now requests the GPU fade-out state instead of destroying the
  overlay immediately, while WebView keeps its WebGL loop alive through the
  close transition and animates blur/scale/distortion strength. Session
  player panels now follow the menu motion with lightweight panel/row
  animations.

## v2.4.14:
  Fix Saomenu/session-player scalability and overlay input regressions.
  Entity and WebView session-player panels now render large login-session
  player lists lazily, with light version signatures so unchanged player
  data no longer forces full sorting/DOM/Tk rebuilds when the menu is open.
  Entity HP auto-hide now clips the native input region to the ID plate
  instead of toggling whole-window click-through, keeping the ID panel
  visible/clickable while hidden HP/STA pixels pass mouse input through.
  Entity Saomenu fisheye now performs distortion/HUD shading in the final
  GLFW GPU window and avoids the old per-frame FBO readback/CPU composite
  path; normal screenshots can include the effect when the game-window
  DXGI capture source is available.

## v2.4.13:
  Add in-panel DPS detail mode for both Entity and WebView UI. The detail
  view reuses live/report per-entity skill breakdowns, supports returning to
  the compact list, and persists the resizable detailed panel size.
  Saomenu now exposes an in-session player list sourced from the active
  packet session, with WebView right-side placement and Entity menu-column
  parity. BossHP/DPS packet display stability was hardened around revive /
  server-switch edge cases, hidden HP panels stop intercepting clicks, and
  Entity BossHP now mirrors the WebView main/secondary panel split.
  Packet parsing gained a _sao_cy_packet helper for stable byte-level
  decode/scan hotspots (made mandatory in v2.4.27), and dev_publish now
  makes smarter full-package vs incremental-package decisions when the spec
  changes. Skill names were refreshed from current SRDPS/SRLOGS Chinese
  short-name tables so DPS skill breakdowns no longer show stale placeholders.

## v2.4.11:
  Clean ABI-sensitive runtime dependency folders before applying full/runtime
  refresh updates so stale NumPy/OpenCV files cannot make cv2 reject ndarray.

## v2.4.10:
  Harden packaged Hide & Seek OpenCV calls with array diagnostics/fallbacks
  while investigating onedir-only cv2/numpy runtime mismatches.

## v2.4.9:
  Guard self-identity updates behind server-confirmed UID ownership so nearby
  players cannot overwrite the cached/player-panel UID, name, level, or job.

## v2.4.7:
  Fix packaged onedir Hide & Seek click execution. The hide_seek worker now
  sets the same per-thread PerMonitorV2 DPI context as recognition, and mouse
  clicks move the visible cursor before sending separate down/up packets so
  frozen builds reliably click the matched screen coordinate.

## v2.4.6:
  Align encounter reset behavior with upstream counters: same-dungeon
  restarts now defer DPS/BossHP reset until the next real self damage, and
  idle report generation no longer clears live totals during long mechanics.

## v2.4.5:
  Preserve live DPS/BossHP during same-dungeon map/layer transitions and
  long boss mechanic gaps. Parser now distinguishes hard scene resets from
  soft in-instance transitions, while combat panels keep a longer idle
  window before fading/resetting.

## v2.4.4:
  Add Cython combat helpers for UUID classification, damage fallback,
  self-attacker detection, and DPS target gating. As of v2.4.27 the compiled
  helpers are mandatory so ABI mismatches fail fast instead of silently
  returning to Python hot paths.

## v2.4.3:
  Fix overworld / city-edge combat targets that only report HP loss or use
  non-standard entity suffixes: DPS now counts player damage to non-player
  combat targets, while BossHP only displays once usable packet HP exists.
  Keep the packet hot path allocation-light and verify the existing Cython
  pixel accelerator build stays healthy.

## v2.4.2:
  Follow upstream DPS-counter behavior for broad-map combat targets:
  player damage to any non-player target now counts for DPS, and non-player
  entities carrying monster HP / break / hate attrs are tracked for BossHP.

## v2.4.1:
  Add more Cython annotations and optimizations to the hotspots,
  further reducing CPU usage and improving frame stability,
  especially on lower-end machines.
  Fix entity DPS/BossHP scene reset parity and dungeon sub-map detection:
  same-scene retries and instanced mini-map layer changes now clear stale
  BossHP/DPS state before accepting the next damage event.

## v2.4.0:
  New Cython style for CPU optimized hotspots.

## v2.3.22:
  Same-scene retry fixes for DPS/BossHP and HP hidden-click region parity.

## v2.3.20:
  Entity menu / HP / DPS / BossHP GPU-overlay performance pass.

## v2.3.18:
  General performance improvements and bug fixes.

## v2.3.17:
  Fisheye worker: retry up to 3× (2 ms each) to acquire WGL lock, preventing worker starvation.

## v2.3.16:
  Minor bug fixes and performance improvements.

## v2.3.15:
  Entity GUI General fix.

## v2.3.14:
  Entity mode HUD panels decoupling, try to make render FPS
  more stable by isolating the heavy works in a separate lane.

## v2.3.13:
  Fiseye now use DXGI screenshot instead of mss.

## v2.3.10+:
  HUD improvements.

## v2.3.9:
  Packet capture reliability improvements.

## v2.3.8:
  Fix SkillFX silently falling back to CPU/PIL in onedir packaged build.
  Root cause: XiaoACTUI.spec 从未将 ``shaders/`` 目录加入 datas 清单,
  所以打包后 ``shaders/skillfx.frag`` 不存在. 首个调用 SkillFX 的
  渲染线程调 ``get_skillfx_pipeline`` → ``_load_fragment`` 抛
  FileNotFoundError → ``_tls.failed = True`` (永久标记) → 后续所有
  compose_frame 都走 PIL fallback. 开发环境下 __file__/项目根下存在
  shaders/ 所以看不出问题 — 仅冻结后才现.
  Fix:
    1) XiaoACTUI.spec 加 ('shaders', 'shaders') 进 datas.
    2) skillfx_pipeline._resolve_shader_path 增加 PyInstaller 感知 —
       依次检查 HERE/, sys._MEIPASS/, exe 同级、exe/_internal/.
    3) get_skillfx_pipeline 单独捕获 FileNotFoundError, 打印出期望
       路径, 下次丢包能从 stdout 直接看出是资源问题还是 GL 问题.

## v2.3.7:
  Continuation of v2.3.6: BossHP 反复刷/最后消失 (重连路径误识).
  v2.3.6 关住了跨 addr 服务器切换路径, 但同服重连路径仍然接受
  `_try_identify` (含松散 c3SB) 或 `_looks_like_frame_start`
  (4 字节 BE 头 ∈ [6, 999999]) 作为重连签名. 后者误中率约
  0.023%/包, 繁忙连接上每秒就能误触发, 每次都重置 _next_seq=-1
  并调用 _on_server_change → 清掉 BossHP 目标. 修复:
    1) 重连路径改为仅接受 _identify_strict (FrameDown 嵌套 c3SB
       或 LoginReturn 0x62), 丢弃松散 c3SB 和 帧头启发式判定.
    2) 增加 3 秒冷却窗口 — 真重连是单次事件, N 秒内重复触发
       一律视为误识, 避免任何残留误识路径造成刷屏循环.

## v2.3.6:
  Fix BossHP overlay rapidly flickering / popping then disappearing.
  Root cause: the cross-addr server-switch detector at packet_capture
  line 350 reused the LOOSE _try_identify (which returns True for any
  payload containing the 4-byte literal 'c3SB'). Any non-game TCP
  stream from the client (chat, social, voice, CDN) whose payload
  happened to contain those bytes hijacked _server_addr -> fired
  _on_scene_change -> wiped _bb_last_target_uuid -> BossHP hidden.
  The next real game packet then had addr != _server_addr again ->
  flipped back -> ping-pong, eventually stuck on a non-game socket
  ('过一会不出来了'). Now _try_identify is split into _identify_strict
  (FrameDown[type=6] nested c3SB or LoginReturn[0x62/type=3]) and
  _identify_loose (c3SB literal). Server switch path requires strict;
  initial identification still uses loose (no anchor exists yet);
  same-server reconnect keeps loose since v2.3.4's _seq_anomalous
  gate already rules out mid-stream segments.
  Also: surface SkillFX compose path on first frame (GPU vs CPU/PIL
  fallback) so '是不是返回CPU了' can be verified from stdout.

## v2.3.5:
  Fix updater modal hard-crashing the app on rapid clicks (especially
  in onedir packaged mode). The 立即更新/重启应用/稍后/跳过 buttons in
  the menu webview updater banner had no debounce: a fast double-click
  could fire multiple concurrent pywebview JS-bridge calls into the
  EdgeWebView2 COM apartment while the first call was still importing
  sao_updater (cold import in onedir takes 200-500ms), occasionally
  crashing the WebView2 process. Added a hard JS-side busy-lock with
  pointer-events:none + disabled flags + 1.5s safety timeout; lock is
  released either by the next state push from Python or the timeout.

## v2.3.4:
  Fix BossHP overlay randomly disappearing mid-fight. The same-server
  reconnect detector accepted any out-of-order TCP packet whose payload
  contained the 4-byte 'c3SB' literal (common in ZSTD'd game data /
  names / buff IDs) as a 'reconnect', triggering scene-change cleanup
  that hides BossHP and resets _bb_last_target_uuid. Now require the
  strong seq-anomaly signal (>1MB both directions = guaranteed new
  ISN) for ALL reconnect paths; mid-stream reorder packets within the
  TCP window can never falsely trigger again.

## v2.3.3:
  Fix GPU SAO popup menu hard crash (PyEval_RestoreThread NULL tstate
  fast-fail) on the second click. Tk's Tcl mainloop on Windows runs an
  implicit PeekMessage(NULL,...)+DispatchMessage pump that captured
  GLFW window messages and dispatched them to GLFW's WndProc, firing
  our mouse callback in a re-entrant context where touching any Tk API
  (root.after_idle) corrupts Tcl interpreter state mid-dispatch and
  crashes the next mainloop checkpoint. Cb now only enqueues hits to
  a deque; a polled drainer on a top-level Tk after() callback runs
  the actual handlers safely.
  Fix infinite same-server-reconnect loop that locked DPS/HP at zero.
  Replay window after reconnect now filters out pre-reconnect packets
  (old TCP ISN seqs) which previously polluted _next_seq and made the
  next live packet trigger another reconnect. Replay only packets
  within ±1MB of the new ISN.

## v2.3.2:
  Fix Gil compound deadlock when the GPU render thread tries to acquire the GIL,
  while the main thread is waiting for the render thread to join during shutdown.
  Fix packet reconnection logic that could cause the DPS and HP won't update in
  same dungeon.

## v2.3.0:
  GPU-accelerated rendering pipeline for all ULW overlays, replacing the old
  PIL-based CPU rendering + DirectX upload path.
  This should significantly reduce CPU usage and eliminate stutter on slower machines,
  especially for the more complex BossHP overlay.

## v2.2.16:
  Combat-CPU + SkillFX framerate. Two changes:
  1. CPU-affinity pinning of render lanes is now opt-in via
     SAO_RENDER_AFFINITY=1 (default OFF). On hybrid CPUs (12th-gen+ /
     14900HX P-core+E-core) the always-on pin parked SkillFX on a
     2.5 GHz E-core and capped its compose at ~30 fps; letting Windows
     scheduler migrate it to a P-core under turbo restores 60 fps.
  2. Scheduler combat-load tier: while SkillFX (or any heavy panel)
     is active, idle entity panels throttle from ≈10 Hz to ≈6 Hz so
     the burst animation and the menu open animation get the spare
     CPU/render-lane bandwidth. Animating panels still tick every
     frame.
  Note: the floating menu button tearing during fisheye is structural
  to Tk widgets on a chroma-key transparentcolor Toplevel (DWM does
  not vsync those composites). A real fix requires moving the menu
  buttons into the ULW HUD as PIL sprites — deferred (would lose Tk
  focus / IME / native click).

## v2.2.15:
  Fix HP/DPS clock + NErVGear pulse + ELAPSED counter freezing on idle.
  v2.2.14's per-tick `_idle_committed` short-circuit assumed every panel
  stops drawing once tweens settle, but HP renders a system clock and
  id-pulse continuously and DPS renders an elapsed counter every second.
  The scheduler's idle downsampling (~10–20 Hz when `_is_animating()` is
  False) keeps the CPU savings; only BossHP — which truly is static at
  full HP — keeps the per-tick gate.

## v2.2.14:
  Idle CPU reduction (target webview parity ~2-3% on i9-14900HX).
  - Fix BossHP._is_animating() (was hard-coded `return True`, forcing 60 Hz
    compose+commit on a steady boss bar at full HP — biggest single drain).
  - Add per-panel idle short-circuit in HP / DPS / BossHP _tick(): once a
    steady frame is committed and nothing is animating, skip compose+submit
    until state changes again. Combat / fades / tweens unaffected.
  - Scheduler: lower idle-downsample threshold 70% → 30% of frame budget AND
    unconditionally throttle non-animating panels to ~10–20 Hz regardless of
    CPU headroom. Animating panels keep full 60 Hz.
  No visual effects removed.

## v2.2.13:
  Add HP pannel and BossHP to GPU-accelerated.

## v2.2.12:
  Fully GPU-accelerated SAO Menu HUD via per-pixel-alpha layered window.
  Eliminates fullscreen chroma-key recomposites and tearing on the
  floating menu. Off-thread compose on the heavy render lane keeps the
  Tk main thread free of HUD draw work.

## v2.2.11:
  Fully GPU-accelerated rendering pipeline for ULW overlays, replacing the old PIL-based CPU
  rendering + DirectX upload path.
  This should significantly reduce CPU usage and eliminate stutter on slower machines,
  especially for the more complex BossHP overlay.

## v2.2.10:
  Fix pannel rendering not respecting the render FPS target, causing stutter on slower machines.
  Create sub-pixel paste / bar-width helpers for ULW overlays.

## v2.2.9:
  Profiled compose_frame on a worker thread
  — it averaged 33 ms steady, 80+ ms during ENTER, with cold-start spikes to 200 ms.
  That maps directly to the user's "only 3 frames" symptom on slower machines.

## v2.2.8:
  使用显示器刷新率而非固定 60 Hz 作为调度器默认频率, 让高刷显示器的动画更流畅。

## v2.2.7:
  Fix GL Cache caused upside-down rendering in skillfx.

## v2.2.6:
  Fix GPU Cache caused upside-down rendering in menu pannels.

## v2.2.5:
  GPU Cache masks/overlay GPU rendering.

## v2.2.4:
  修正阴影残留。

## v2.2.3:
  [entity HP 右侧外观修正] 把 HP 条右侧壳层改回接近 webview 的结构:
  xt_right 不再整块实心铺满，而是左半实体、右半渐隐到底层 cover；
  同时恢复 number_xt 独立数值底板，避免右侧视觉发闷、发厚。
  [HP / BossHP 底板统一] 把 webview / entity 的 ID plate、HP cover、
  BossHP cover 底板统一到同一套冷灰白层级；普通 HP 去掉残留灰绿色，
  BossHP 下调纯白度并拉开 cover / box 层次，避免整片白成一体。

## v2.2.2:
  1) [菜单白板统一] 把 webview / entity 主菜单的圆形按钮、左侧信息板、
     子菜单卡片，以及 commander / autokey / boss raid 编辑器的灰绿底板
     统一收敛到 sao_alert 那套冷白 + 轻灰层次，降低纯白刺眼感，并把
     子菜单 hover 从整块金色改成更克制的浅金 / 冷青过渡。
  2) [entity HP fade 修复] 恢复 HP 组件隐藏时两侧 XT 外框壳层的
     fadeout。此前 group fade 只覆盖名义 48px HP box，number_xt 壳层
     底部超出 box_rect，隐藏时会留下外框残影；现在改为覆盖整个 XT shell.

## v2.2.1:
  修复 entity 模式下 HP/BossHP/DPS overlay 隐藏后阴影残留:
    _apply_panel_style() 在设置面板里用 SetClassLongW(CS_DROPSHADOW)
    修改了整个 Tk 进程的窗口类, 导致同进程所有 Toplevel (包括 ULW
    overlay) 都被 DWM 加上系统阴影矩形. ULW bitmap 淡出到透明后窗口
    尚未 destroy, DWM 阴影仍可见. 修复: HpOverlay / BossHpOverlay /
    DpsOverlay.show() 创建窗口后立即调用
    DwmSetWindowAttribute(DWMWA_NCRENDERING_POLICY, DWMNCRP_DISABLED)
    让 DWM 对这三个 overlay 窗口不渲染非客户区 (含阴影).

## v2.2.0:
  1) [继续修 Hide & Seek] webview 下持续 alert 之前会被普通 identity 通知
     或 9s auto-dismiss 计时器误关 → "过一会就消失". 现在 hide_seek
     alert 用 alert_kind='hide_seek' 标记, _hide_identity_alert_window
     在引擎仍 active 时直接拒绝关闭, _sync_identity_alert 也不再用
     identity 推送覆盖 hide_seek alert.
  2) [全面 UI 重设计] 把所有面板的 "灰绿" 配色 (rgb(207,208,197) /
     rgb(60,62,50) / rgb(188,190,178) ...) 全部换成 SAO Alert 的
     "纯白 + 略灰" 扁平高科技配色 (rgba(255,255,255,X) / rgb(100,99,100) /
     rgb(140,135,138)). 透明度 (alpha) 一律保留原面板设置, 没动.
     影响:
       - web/menu.html, dps.html, boss_hp.html, hp.html, commander.html,
         autokey_editor.html, raid_editor.html
       - sao_gui_dps.py, sao_gui_bosshp.py, sao_gui_hp.py (entity ULW 面板)
     DPS/BossHP 面板新增青色切角外框 (基于参考图):
       - DPS: 右上 + 左下 22px 切角, 顶部青色高亮 + 底部 cyan→amber 渐变线
       - BossHP: 八边形切角 (四角各切 14px), 主条 + 附属单位统一风格
     其他面板暂保留圆角, 后续轮次按反馈微调.

## v2.1.20:
  修复"自动躲猫猫"两个回归 (检测算法本身一行未动):
    1) [entity / webview 共同] HideSeekEngine._assets_dir 之前用
       os.path.dirname(__file__) 拼接 'assets', 在 PyInstaller onedir
       打包 (runtime/ 子目录) 下永远落到 runtime/assets/ — 此目录在
       build_release.bat 把 assets/ 提升到 exe 顶层后并不存在, 导致
       5 个 template (1.png ~ 5.png) 全部 cv2.imread 失败 → 引擎线程
       正常运行但 _match_template 永远没结果, 表现为 "启动了不会有效果".
       改为优先 config.BASE_DIR/assets, 再 fallback 模块同级 assets,
       兼容源码 / onedir / 旧 onefile 三种布局.
    2) [webview] JS 桥 toggle_hide_seek 之前会再 spawn 一个 daemon
       thread 去跑 _toggle_hide_seek, 而 pywebview 的 JS callback 本身
       就在 worker thread; 嵌套两层非主线程后, _show_identity_alert_window
       内部的 alert_win.show() / pythonnet form.Invoke 与 evaluate_js
       会在两个不同的非 GUI 线程并发触达 WebView2 → 部分机器上 native
       crash, 表现为 "启动一下会自己闪退". 改为直接同步调用, engine
       自己的后台线程不变.

## v2.1.19:
  同 v2.1.18, 版本号补丁升级.

## v2.1.18:
  1) [核心] 修复切换场景服务器后 DPS / boss 血条 / 全量同步全部失效的根因:
     a) packet_parser.reset_scene 之前保留了 _current_uuid (旧场景的 entity
        UUID), 但游戏在新场景里给玩家分配的是新 UUID, 导致后续 SCDeltaInfo 里
        attacker_uuid != _current_uuid → attacker_is_self 永远 False →
        DPS tracker 把自己的伤害全部当成"别人的", 自己的条不出, boss bar
        target 也永远不会被采纳. 现在 reset_scene 会清零 _current_uuid,
        由下一个 SyncToMeDeltaInfo 自然重新填充;
     b) packet_capture 之前在 server-change / 同服重连的瞬间会把切换前后
        几个 TCP 段直接丢弃 (旧 addr 的被短路过滤, 新 addr 的在 _try_identify
        成功之前也被过滤), 这正好把关键的 SyncContainerData / SyncToMeDelta
        首包丢掉, 导致 "切场景/重新上线触发不了 full sync". 现在维护一个
        24-pkt 环形缓冲, 任何 server-change / reconnect / 首次识别成功后
        都会按 seq 升序回放属于该 addr 的缓存包.
  2) 修复 v2.1.17 webview→entity 持久化仍失效的根因: SettingsManager 在
     sao_webview 内同时存在两个独立实例 (self.settings 与 _cfg_settings_ref),
     各自持有不同的内存快照. v2.1.17 的预存逻辑先用 self.settings 写入
     ui_mode='entity' 后, 紧接着 _persist_cached_identity_state 又通过
     _cfg_settings_ref.save() 把 stale 的 ui_mode='webview' 覆写回磁盘.
     现在统一通过 _cfg_settings_ref 写 ui_mode + game_cache, 一次性 save();
  3) _persist_cached_identity_state 改为优先读取 GameState 上的实时字段
     (gs.player_name / profession_name / level_base / hp_* / stamina_*),
     实例变量仅作兜底. 之前实例变量在菜单未打开/recognition 未跑完时是
     stale 的, 导致即使 webview 内 GameState 已经收到 SyncContainerData,
     切到 entity 时仍然只能写出空名/0 级;
  4) entity (sao_gui) 必须先 subscribe(_on_game_state_update) 再 load_cache,
     否则 GameState.load_cache 内部对订阅者的初始通知会被丢弃,
     entity 面板启动时无法显示 webview 切过来时持久化的角色名/等级/HP.

## v2.1.17:
  1) 修复 webview 模式下 sao_alert 弹窗"跳两次"的视觉故障:
     _show_identity_alert_window 出于冷启动 WebView2 竞态考虑会立即 +
     350ms 各 push 一次, 但 alert.html 的 showAlert 每次都会重新触发
     show 动画. 现在 JS 端基于 (title, body) 签名去重, 同一条 alert
     仅播放一次入场动画;
  2) 修复 boss raid 告警声音播放两次的问题: BossRaidEngine 通过
     on_sound("boss_alert") 已经播了一次 Popup.SAO.Alert.mp3, 而
     _show_identity_alert_window 默认还会再播 'alert' (同一个文件).
     现在 _on_boss_alert_with_linkage 调用时显式 play_sound=False;
  3) 修复 onedir 冷启动时 BossHP / DPS 面板不出现的问题: 在
     _on_webview_started 内追加 4s/10s/16s 的延迟兜底, 若 boss_hp_win
     仍未可见就重新 show 并补做 click-through 设置, 同时 DPS 重新
     套用穿透样式;
  4) 修复 webview→entity 热切换时角色信息丢失 + 退出后菜单模式没写回
     的问题: _transition_with_animation 在销毁 webview 之前先把
     ui_mode 同步到磁盘 (切换到 entity 时立刻写 'entity'), 同时调用
     _persist_cached_identity_state(save_now=True) 把 player_name /
     level / profession / fight_point 立刻持久化, entity 启动后能直接
     读取到, 即使后续 entity __init__ 因任何原因没保存 ui_mode 也能
     在下次启动正确进入 entity 菜单.

## v2.1.16:
  1) 多核渲染优化: overlay_render_worker.py 提高高核心系统的渲染通道数
     (8 核以上系统从 4 通道提升至最多 6 通道), 多面板 (DPS+BossHP+Burst+
     menu) 可真正并行 compose 而非排队;
  2) Windows 线程亲和: 渲染通道线程 + CPU 任务池线程通过
     SetThreadAffinityMask 固定到 cores 2.., 减少 context-switch 抖动,
     改善 L1/L2 cache 命中, 让 Tk 主线程独占核心 0/1;
  3) 调度器自适应限速: overlay_scheduler.py 在 avg_frame_ms ≥ 13ms 时
     把空闲面板的 tick 频率从 20 Hz 降到 10 Hz (IDLE_EVERY_N_OVERLOADED=6),
     保护动画中的面板 60 Hz 预算;
  4) 调度器新增可选 visibility_fn (向下兼容): 隐藏面板可直接被跳过,
     避免 winfo 检查与 GIL 抢占;
  5) 进程优先级: main.py 启动时把进程提升到 ABOVE_NORMAL,
     防止重战斗时被后台程序抢占时间片导致掉帧;
  6) dev_publish 工具: 本地发布改为可选 (CLI --no-local-publish + GUI 复选框),
     并修复 GUI 全量包构建未刷新 release 布局导致 SHA256 与上版相同的问题.

## v2.1.13:
  1) 抓包层回退至 v1.3.1 基底并补齐三处关键 TCP 重组缺陷：
     a) 识别首包不喂入 TCP 流 → SyncContainerData / SyncNearEntities 丢失；
     b) 无 TCP 重传段过滤 → 已消费 seq 重入缓存, 加速 300 条溢出；
     c) 无缺段跳跃 → pcap 丢失一段后 _next_seq 卡住, 所有后续段堆满缓存
        触发溢出, DPS 伤害事件与全量角色同步数据全部丢失;
  2) 新增 GAP_SKIP_SEC=2.0 缺段超时跳跃 (参考 C# SRDPS ForceResyncTo),
     缓存有段但 2 秒未消费时自动跳过间隙恢复后续数据；
  3) _extract_frames 新增帧对齐修复扫描, gap-skip 后自动重定位帧边界；
  4) 诊断行新增 gap_skip= / overflow= 计数便于排查 pcap 丢包；
  5) 本版以 full-package + force_update + minimum_version=2.1.13 推送。

## v2.1.11:
  1) 抓包层回退到 v1.3.1 的源地址识别策略: 移除 _infer_server_endpoint
     方向推断 (私网/临时端口/小端口 启发式), 改为始终用包的源地址
     (src_ip:sport) 标识游戏服务器. 修复 VPN/加速器等双私网环境下方向
     推断失败→ _server_addr 被设为客户端地址→所有下行包被丢弃的问题;
  2) 保留 v2.1.8+ 的候选帧缓冲回放、空闲重识别、game-frame 回退识别;
  3) 本版以 full-package + force_update + minimum_version=2.1.11 推送。

## v2.1.10:
  1) TCP 重组层修复重传段缓存泄漏：seq 已消费的段不再入 cache，
     杜绝 "TCP cache overflow (301), reset" 导致关键同步包丢失；
  2) Alert 弹框从 4× _push (即时+0.12s+0.32s+0.72s) 缩减为 2×
     (即时+0.35s)，修复弹框重复显示三次的视觉问题；
  3) 本版以 full-package + force_update + minimum_version=2.1.10 推送。

## v2.1.9:
  1) parser 在 self UID 未确认前缓存并回放早到的 0x16 / 0x2E 自身同步包，
     修复 EnterGame 较晚时角色名 / 基础等级 / 赛季等级 / 技能 CD 与 slot
     长期缺失或显示 unknown；
  2) 本版以 full-package + force_update + minimum_version=2.1.9 推送，
     强制所有旧版本升级。

## v2.1.8:
  1) 抓包层在识别游戏服务器前先缓冲并回放候选下行首包，修复首个
     0x15/0x16 身份同步被丢弃后角色名 / 基础等级 / 赛季等级长期为空；
  2) 本版以 full-package + force_update + minimum_version=2.1.8 推送，
     强制所有旧版本升级。

## v2.1.7:
  1) SyncContainerData 在 pb2 缺失 / 解析失败时回退 mini 解码，恢复
     UID / 角色名 / 等级 / 职业等关键身份字段；
  2) webview HUD 改为跟随游戏窗口所在显示器的 DPI / 几何，修复高 DPI /
     多显示器下 STA / HP 区域漂移；
  3) _set_dpi_aware 统一复用早期 PerMonitorV2 提升逻辑；
  4) 本版以 full-package + force_update + minimum_version=2.1.7 推送，
     强制所有旧版本升级。

## v2.1.6:
  1) 识别线程设置 per-thread PerMonitorV2 DPI，修复 onedir/webview 下
     GetClientRect 返回逻辑像素导致 STA 裁剪坐标偏移、始终 OFFLINE；
  2) update.exe.new 替换增加 5 次重试 + 0.5s 间隔，处理目标被瞬时锁定
     的 PermissionError，并在失败后清理 .promoting 残留文件；
  3) PacketParser 启动时从 player_cache.json 预填充角色名/等级/职业，
     修复中途启动（未经过登录/换图）时名字 UID 等级长期为空的问题；
  4) _set_dpi_aware 回退从 SystemDpiAware(1) 改为 PerMonitorDpiAware(2)；
  5) 本版以 full-package + force_update + minimum_version=2.1.6 推送，
     强制所有旧版本升级。

## v2.1.2-m: 修复 sao_alert 同条 alert 4s 内重复触发只续展不重弹;
          webview _maybe_show_update_popup 同步 sao_gui 的 downloading
          静音 + alert 可见时跳过非 error 提示;
          本版以 full-package + force_update + minimum_version=2.1.2-l
          推送, 强制清理积压问题。

## v2.1.2-h: main.py bootstrap 把 EXE-dir 加入 sys.path → 修复 onedir 下
          `from proto import star_resonance_pb2` ImportError (proto/ 被
          build_release.bat 提升出 runtime/, 旧 sys.path 找不到);
          同时 main.py 最早调用 promote_runtime_update_exe() 解决新
          update.exe 不替换的问题; spec 显式 hiddenimport 抓包链路。

## v2.1.2-f: 彻底去掉 entity 识别循环的 _recognition_active 闸门 + 修复模块化布局下资源路径 (skill_names.json / fonts) + dev_publish 自动重建 update.exe 并注入增量包

## v2.1.2-e: entity 识别循环外层 recognition_ok/packet_active 总闸门完全去除，BurstReady / HP overlay / commander / identity 仅依赖自身数据检查

## v2.1.2-d: DPS/Boss HP 推送完全脱离 recognition gate (_push_packet_overlays); update.exe 无边框 + 圆角 + 60FPS 动画

## v2.1.2-c: packet_active 在识别到服务器后立即置 True，修复 DPS/Boss HP 不弹出

## v2.1.2-b: STA 识别只依赖 vision；entity 更新弹窗中文不再为方框；多个 alert 不再重叠

## v2.1.2-a: 同步当前源码整理并重新发布后缀版本增量包，延续 2.1.2 更新链路

## v2.1.2: entity SAO menu 常驻 60Hz HUD 调度; child-menu 刷新去重与状态合帧; updater 版本后缀比较修复

## v2.1.1-a: entity SAO menu 常驻 60Hz HUD 调度; child-menu 刷新去重与状态合帧

## v2.1.1: webview 更新提示不再被身份提示循环瞬时关闭; entity 更新面板中文字体修正; STA offline 状态同步修复

## v2.1.0: 远程更新链路、独立 update.exe、模块化 onedir 布局、发布工具、entity/webview 更新提示修正

## v2.0.1: entity 面板 webview 对齐、Overlay 异步渲染、Burst Ready 平滑度与透明线修复

## v2.0.0: entity/webview 双 UI、SAO 菜单与 HUD 新版打包/发布整理

## v1.3.1: 躲猫猫引擎：失败回退检测所有前置步骤; 线程崩溃自动恢复(resume); alert 持久显示修复

## v1.2.26: Commander 面板; 菜单滚动; 退出流程修复

## v1.2.25: 移除 level_adjust 模块依赖

## v1.2.24: 副本重开死亡单位重置; 升级时赛季等级优先级修复; 移除 level_adjust 模块

## v1.2.23: 深眠心相仪等级解析 (field 102); full CharSerialize dump on login; level_adjust override module
