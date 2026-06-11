# SAO Auto 版本历史

逐版本变更记录, 最新在前。本文件由 config.py 内联的历史注释迁出。

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
