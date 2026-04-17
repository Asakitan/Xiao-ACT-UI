# SAO Auto

`SAO Auto` 是一个面向《星痕共鸣》的外部 HUD / 自动化项目，当前版本同时维护 `entity` 与 `webview` 两套 UI 入口，围绕抓包、识图、Boss Raid、DPS 统计、自动按键和云端脚本仓库持续迭代。

当前版本：`2.0.0`

## 2.0.0 概览

- 同时支持 `entity` 与 `webview` 两种 UI 模式
- SAO Menu、ID / HP / BossHP / DPS / Burst Ready / Commander 面板保持并行维护
- 默认采用 `packet + vision` 混合数据源
- 自带 Boss Raid / AutoKey 云端仓库服务端
- 打包脚本已覆盖动态导入模块、运行时资源与发布目录初始化

## 功能列表

- Windows 平台透明 HUD
- `HP / 等级 / 身份 / 技能状态` 抓包同步
- `STA` 识图识别与离线检测
- 独立 `Boss HP` 覆盖条
- 独立 `DPS / HPS` 面板与技能拆分
- `Burst Ready` 视觉与音效提示
- `Boss Raid` 阶段计时、时间轴提醒、狂暴倒计时
- `Boss ↔ AutoKey` 联动触发
- 自动按键本地配置、导入导出、云端脚本库
- `Commander` 指挥官面板与队伍概览

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
- 战斗重置
- 空闲自动淡出

## 自建服务端

仓库内自带 `FastAPI` 服务端，默认监听 `9320` 端口。

在 `sao_auto` 目录下执行：

```bash
python server/app.py
```

服务端提供两类数据：

- 自动按键脚本：`/api/scripts`
- Boss Raid 配置：`/api/boss-raids`

本地数据库默认位于：

- `server/data/scripts.db`

## 打包

客户端打包：

```bash
pyinstaller --clean --noconfirm XiaoACTUI.spec
```

服务端打包：

```bash
pyinstaller --clean --noconfirm server/AutoKeyServer.spec
```

一键发布目录：

```bat
build_release.bat
```

当前打包脚本会：

- 显式包含 `main.py` 中动态导入的 `entity` / `webview` UI 模块
- 打包 `web/`、`assets/`、`proto/` 等运行时资源
- 保留 `install_npcap` 相关逻辑，避免抓包安装链路在发布包中缺失
- 生成 `dist/release/XiaoACTUI` 与 `dist/release/AutoKeyServer`
- 预建 `exports/auto_keys`、`exports/boss_raids`、`temp`、`server/data` 等运行目录
- 复制 `README.md`、`LICENSE`、`Start.bat` 等发布说明文件

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
├─ sao_webview.py          # WebView HUD 主进程
├─ sao_gui.py              # Entity HUD 主进程
├─ sao_theme.py            # SAO 菜单与视觉主题
├─ sao_sound.py            # 音效、字体与 Burst Ready 语音
├─ build_release.bat       # 发布目录打包脚本
├─ web/                    # WebView 页面资源
├─ assets/                 # 字体、音效、技能名表、贴图资源
├─ proto/                  # Protobuf 与协议相关文件
└─ server/
   └─ app.py               # FastAPI 脚本 / BossRaid 服务端
```

## 更新记录

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

