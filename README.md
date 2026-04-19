# SAO Auto

`SAO Auto` 是一个面向《星痕共鸣》的外部 HUD / 自动化项目，当前版本同时维护 `entity` 与 `webview` 两套 UI 入口，并补齐了远程更新、独立 updater、模块化发布目录、脚本仓库与更新服务端的完整发布链路。

当前版本：`2.1.2`

## 2.1.2 概览

- entity SAO Menu 打开后保持 60Hz HUD 调度，并继续保留 child menu 去重与状态合帧优化
- 修复 updater 版本比较，客户端现在会把 `2.1.1-a` / `2.1.2` 这类后续发布正确识别为新版本
- 为后续自动更新链路切回纯数字版本，避免旧客户端把带后缀版本误判为不需要更新

## 2.1.1-a 概览

- entity SAO Menu 打开后改为持续走 60Hz HUD 调度，不再在短暂强制阶段后退回到 20Hz idle tick
- 为 entity 菜单状态刷新补了签名缓存与 idle 合帧，同一帧内重复状态变更只会刷新一轮子菜单
- 为 child menu 注册与可见菜单重建补了内容去重，未变化时不再重复触发布局/HUD 重算

## 2.1.1 概览

- 修复 webview 更新提示复用全局 alert 宿主时，被身份提示同步循环立即关闭的问题
- 修复 entity 更新面板 `meta` 行中文使用 SAO 字体导致的方框字形
- 修复 webview / entity 共用状态下，STA 会被 packet bridge 误判为 `offline` 的问题

## 2.1.0 概览

- 新增客户端远程更新链路，`entity / webview` 两套 UI 都会展示发现更新、下载中、下载完成三种 SAO 状态提示
- 引入独立 `update.exe` 与 `update_apply.py`，主程序退出后由 updater 接管文件替换与重启
- 客户端发布目录切换为模块化 `onedir` 布局：`XiaoACTUI.exe + update.exe + web/ + assets/ + proto/ + runtime/`
- 新增 `update_host/` 与本地发布工具链，支持差量/全量包构建和远端上传
- 冻结版资源路径统一改为 `config.py` 的运行时解析，顶层 `web / assets / proto / icon.ico` 可被源码端与打包端共用
- entity 侧补齐 `AutoKey / BossRaid` 详情编辑器、DPS 上次战斗报告与明细交互，并继续和 webview 行为对齐

## 功能列表

- Windows 平台透明 HUD
- `HP / 等级 / 身份 / 技能状态` 抓包同步
- `STA` 识图识别与离线检测
- 独立 `Boss HP` 覆盖条
- 独立 `DPS / HPS` 面板、上次战斗报告与技能拆分
- `Burst Ready` 视觉与音效提示
- `Boss Raid` 阶段计时、时间轴提醒、狂暴倒计时
- `Boss ↔ AutoKey` 联动触发
- 自动按键本地配置、Quick Panel / Detail Editor、导入导出、云端脚本库
- `Commander` 指挥官面板与队伍概览
- 远程更新检查、下载、独立更新器接管与重启应用

## 运行要求

- Windows 10 / 11
- Python 3.11 及 `pip`
- `Microsoft Edge WebView2 Runtime`
- 抓包模式建议安装 `Npcap`

补充说明：

- `requirements.txt` 包含 `pywebview`、`pythonnet`、`pygame`、`opencv-python`、`pynput`、`moderngl` 等主线依赖。
- 若系统未安装 `Npcap`，程序会在需要抓包时尝试触发安装流程。
- 运行源码时，配置写入当前目录；打包后写入 exe 同目录。

## 快速开始

在 `sao_auto` 目录下执行：

```bash
pip install -r requirements.txt
python main.py
```

常用模式：

- 普通 HUD：`python main.py`
- 单次识别测试：`python main.py --test`
- 无界面终端模式：`python main.py --headless`

默认情况下，程序会根据 `settings.json` 中的 `ui_mode` 启动对应 UI：

- `webview`
- `entity`

## 默认数据源

当前默认是混合数据源：

- `hp`：`packet`
- `level`：`packet`
- `identity`：`packet`
- `skills`：`packet`
- `stamina`：`vision`

## 默认快捷键

- `F5`：开始 / 停止识别
- `F6`：开始 / 停止自动按键
- `F7`：开始 / 停止 Boss Raid
- `F8`：Boss Raid 下一阶段

保留但默认未绑定实际动作：

- `F9`
- `F10`

## 自动按键

自动按键支持：

- 本地 `profile` 管理
- 动作顺序、按键、次数、延迟、重触发间隔
- `Boss Raid` 事件联动触发
- 结构化条件判断
- `JSON` 导入 / 导出
- 云端搜索、下载、上传脚本

当前常用条件包括：

- `hp_pct_gte`
- `hp_pct_lte`
- `sta_pct_gte`
- `burst_ready_is`
- `slot_state_is`
- `profession_is`
- `player_name_is`

## Boss Raid 与 DPS

`Boss Raid` 当前支持：

- 本地 profile 新建、保存、删除
- profile 导入 / 导出
- 云端搜索 / 下载 / 上传
- 阶段推进
- 时间轴提醒
- 狂暴倒计时
- Boss 条显示模式切换

`DPS` 面板当前支持：

- 伤害 / 治疗分页
- 玩家排行
- 单人明细面板
- 技能伤害拆分
- 上次战斗报告
- 战斗重置
- 空闲自动淡出

## 自建服务端

仓库内自带两类 `FastAPI` 服务端：

- 脚本仓库服务端：默认监听 `9320`
- 更新服务端：默认监听 `9330`

脚本仓库服务在 `sao_auto` 目录下执行：

```bash
python server/app.py
```

脚本仓库服务端提供两类数据：

- 自动按键脚本：`/api/scripts`
- Boss Raid 配置：`/api/boss-raids`

本地数据库默认位于：

- `server/data/scripts.db`

更新服务端在 `sao_auto` 目录下执行：

```bash
python update_host/app.py
```

更新服务端提供：

- 更新清单：`/api/update/latest`
- 更新摘要：`/api/update/summary`
- 更新包下载：`/downloads/*`

更新服务端的部署与发布细节见 `update_host/README.md`。

## 远程更新

- 客户端启动后会后台检查 `settings.json` 中的 `update_host`；为空时回退到 `config.DEFAULT_UPDATE_HOST`
- `entity / webview` 两套 UI 都会在 HUD 启动完成后再弹出全局 SAO 更新提示，不依赖隐藏菜单窗口
- 下载完成后不会直接覆盖运行中的主程序，而是由同目录 `update.exe` 在主进程退出后应用 `runtime-delta` 或 `full-package`
- `staging/`、`backup/`、`update_state.json` 和 `update_apply.log` 都属于本地运行时产物，不参与源码版本管理

## 打包

客户端打包：

```bash
pyinstaller --clean --noconfirm XiaoACTUI.spec
```

独立 updater 打包：

```bash
pyinstaller --clean --noconfirm update.spec
```

服务端打包：

```bash
pyinstaller --clean --noconfirm server/AutoKeyServer.spec
```

更新服务端打包：

```bash
pyinstaller --clean --noconfirm update_host/UpdateHost.spec
```

一键发布目录：

```bat
build_release.bat
```

更新服务端部署包：

```bat
build_update_host_package.bat
```

差量 / 全量更新包：

```bash
python build_delta.py --version 2.1.0 --files runtime/sao_gui.py web/menu.html
python build_full_package.py --version 2.1.0
```

当前打包脚本会：

- 构建 `XiaoACTUI.exe`、`update.exe` 与 `AutoKeyServer.exe`
- 显式包含 `main.py` 中动态导入的 `entity / webview` UI 模块与更新模块
- 生成模块化发布目录 `dist/release/XiaoACTUI`，并将 `web/`、`assets/`、`proto/`、`icon.ico` 提升到客户端顶层
- `runtime/` 仅保留 Python 解释器、依赖 DLL、`.py` / `.pyc` 与运行时模块
- 预建 `exports/auto_keys`、`exports/boss_raids`、`temp`、`server/data` 等运行目录
- 支持后续将变更内容打成 `runtime-delta` 或 `full-package` 更新包

## 目录概览

```text
sao_auto/
├─ main.py                 # 程序入口
├─ config.py               # 全局配置、版本号、默认热键、数据源设置
├─ game_state.py           # 统一状态模型与缓存
├─ packet_capture.py       # Npcap 抓包
├─ packet_parser.py        # 协议解析
├─ packet_bridge.py        # packet -> GameState / DPS / BossRaid
├─ recognition.py          # STA / ROI 识图
├─ dps_tracker.py          # DPS / HPS 统计
├─ boss_raid_engine.py     # Boss Raid 阶段、时间轴、状态机
├─ boss_autokey_linkage.py # Boss 事件与自动按键联动
├─ auto_key_engine.py      # 自动按键引擎与云端客户端
├─ sao_updater.py          # 客户端更新检查 / 下载 / 状态管理
├─ sao_webview.py          # WebView HUD 主进程
├─ sao_gui.py              # Entity HUD 主进程
├─ sao_theme.py            # SAO 菜单与视觉主题
├─ sao_sound.py            # 音效、字体与 Burst Ready 语音
├─ update_apply.py         # 外部更新应用器
├─ build_delta.py          # runtime-delta 包构建脚本
├─ build_full_package.py   # full-package 包构建脚本
├─ build_release.bat       # 发布目录打包脚本
├─ build_update_host_package.bat # 更新服务端部署包脚本
├─ web/                    # WebView 页面资源
├─ assets/                 # 字体、音效、技能名表、贴图资源
├─ proto/                  # Protobuf 与协议相关文件
├─ update_host/            # FastAPI 更新服务端与发布脚本
└─ server/
   └─ app.py               # FastAPI 脚本 / BossRaid 服务端
```

## 更新记录

### 2.1.1-a

- entity SAO Menu HUD 在菜单保持打开时持续按 60Hz 调度，扫描线 / 光点 / 呼吸位移不再掉回低帧率
- `sao_gui.py` 为菜单状态刷新增加签名缓存与 `after_idle` 合帧，避免多个状态变更连续重建整套 child menu
- `sao_theme.py` 为 child menu 注册和可见菜单切换增加内容签名去重，并减少 overlay geometry / HUD relayout 的无效调用

### 2.1.2

- 延续 entity SAO Menu 的 60Hz 调度与 child menu 去重优化
- 修复 `sao_updater.py` 版本比较逻辑，使 `2.1.1-a` 等后缀版本能被客户端正确判定为新版本
- 当前发布版本切换为纯数字 `2.1.2`，兼容旧版 `2.1.1` 客户端的自动更新判断

### 2.1.1

- 修复 webview 更新提示复用全局 alert 宿主时被 `_sync_identity_alert()` 立即收起，提示现在会按预期停留到超时或后续状态变化
- 修复 entity 更新面板 `meta_text` 中文显示为方框的问题，改为使用 CJK 字体渲染
- 修复 `PacketBridge` 状态轮询覆盖 `RecognitionEngine.recognition_ok`，导致 webview / entity 的 STA 长期显示 `OFFLINE`

### 2.1.0

- 新增 `sao_updater.py` / `update_apply.py` 远程更新链路，支持 `runtime-delta` 与 `full-package`
- 引入 `update.exe`、`build_delta.py`、`build_full_package.py`、`build_update_host_package.bat` 和 `update_host/` 发布 / 部署工具链
- 客户端发布目录改成模块化 `onedir` 布局，冻结版资源解析统一收口到 `config.py`
- webview / entity 更新提示改为启动完成后再弹出，并统一走全局 SAO alert 宿主
- entity 新增 `AutoKey / BossRaid` detail editor、DPS 上次战斗报告与实体明细交互
- `BossHP / HP / DPS / Burst Ready` 等 overlay 继续修正显示与交互细节

### 2.0.1

- entity 侧 `AutoKey / BossRaid / Commander` 面板按 webview 样式重写到原有文件中
- `HP / BossHP / DPS` overlay 清理了后加的杂乱横线，并修复多处透明线条穿透问题
- `BossHP` 修正名字阴影透明度、HP 条右端对齐以及 break / shield 相关视觉状态
- `Burst Ready` 恢复 GL 能量层、加入运动模糊，并调整 steady-state 采样与提交流程以减少帧步感
- 新增 `overlay_render_worker.py` 与 `render_capture_sync.py`，将重型合成移出 Tk 主线程并与捕获流程同步
- `recognition.py` 改为以 `PrintWindow` 为主的游戏层捕获路径，并补充缓存帧与耐力条识别稳定性处理
- `XiaoACTUI.spec` 与 README 同步更新，覆盖新增模块与当前发布内容

### 2.0.0

- 统一项目版本号到 `2.0.0`
- README 改为当前双 UI 架构说明
- 客户端 `PyInstaller` 规范补齐动态导入模块与必要运行时资源
- 发布脚本补齐客户端 / 服务端产物、说明文件和运行目录

## 注意事项

- 项目默认通过窗口标题关键字 `Star` / `星痕共鸣` 与进程名 `star.exe` 查找游戏窗口。
- 抓包链路依赖当前协议解析逻辑，游戏协议变化后可能需要同步调整解析器。
- 项目当前仍以 Windows 运行体验为目标，跨平台不在主线支持范围内。
- 不建议继续尝试内存注入或类似内存修改方案。

## 致谢

项目开发过程中直接受益于以下开源仓库和公开资料：

- [`StarResonanceDamageCounter`](../StarResonanceDamageCounter/README.md)
- [`StarResonanceDps`](../StarResonanceDps/README.md)

若某些第三方代码、资源或生成产物来自上游项目，则仍应遵循其原始许可证。

## License

- `sao_auto` 目录下未特别注明的原创代码：`MIT`
- 第三方项目、第三方资源、第三方生成产物：以各自原始许可证为准

