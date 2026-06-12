# SAO Auto

面向《星痕共鸣》的 Windows 外部 HUD / ACT 战斗分析工具。提供 SAO 风格的透明覆盖界面、DPS/HPS 统计、Boss 战辅助与自动按键，数据以 TCP 抓包为主、视觉识别与只读内存为辅。

- 当前版本：`4.6.110`（以 `config.py` 的 `APP_VERSION` 为准）
- 运行平台：Windows 10 / 11
- 两套 UI：`webview`（pywebview + WebView2）与 `entity`（Tk / 原生窗口）

## 功能概览

**HUD 与战斗数据**
- 透明 SAO 风格 HUD，`webview` 与 `entity` 双 UI 1:1 同款渲染。
- HP / 等级 / 身份 / 技能状态通过 TCP 同步；体力（STA）通过视觉识图，避免与抓包状态互相抢写。
- 独立 DPS / HPS 面板：排行榜、单人明细、技能拆分、上次战斗与历史报告。
- 独立 Boss HP 覆盖条，处理 break / extinction / revive hold 等状态。
- Burst Ready、SkillFX、菜单粒子、扫描线、浮动面板等视觉效果。

**Boss Raid 与自动按键**
- Boss Raid 阶段计时、时间轴提醒、狂暴倒计时，可由 Boss 事件联动触发自动按键。
- 自动按键 profile：动作顺序 / 按键 / 次数 / 延迟 / 重触发间隔、结构化触发条件、JSON 导入导出、云端脚本搜索与上传下载。

**ACT 平台**（`act_platform/`）
- 事件总线 + 解析适配器 + 进程隔离 parser worker + 选择性解析 + 可信插件引擎。
- 双 UI 管理面板：Triggers/Timers、Data Source Health、Report/Export、History Browser、Timeline/VCR、Action Log、Graph/Timeseries、Combatant/Skill Drilldown。
- 幻想技能 buff 覆盖率、self buff/debuff 持续时间追踪、按目标 / 元素 / min-max 拆分。

**辅助与运维**
- Commander 队伍面板、BuffMon、Hide & Seek、置顶 / 隐藏面板。
- 远程更新检查、`runtime-delta` / `full-package` 下载、独立 `update.exe` 应用更新并重启。

## 运行要求

- Windows 10 / 11，Python 3.11 + `pip`。
- Microsoft Edge WebView2 Runtime（webview 模式）。
- Npcap（抓包模式建议安装）。
- 主线依赖见 `requirements.txt`：`pywebview`、`numpy`、`Pillow`、`mss`、`windows-capture`、`pythonnet`、`moderngl`、`glfw`、`skia-python`、`Cython`、`opencv-python`、`pynput`、`pygame`、`zstandard`、`protobuf` 等。

> `Cython` 为构建期必需依赖：`_sao_cy_*.pyx` / `.pyd` 用于加速 packet、memscan、像素、战斗与 UI helper 热路径。

## 快速开始

在 `sao_auto` 目录下：

```powershell
pip install -r requirements.txt
python main.py            # 按 settings.json 的 ui_mode 启动 HUD
```

或使用批处理入口 `.\Start.bat`。常用模式：

```powershell
python main.py --test      # 单次窗口定位 + 截图识别测试
python main.py --headless  # 无 HUD，终端输出识别状态
```

`ui_mode` 取值：`webview`（默认）、`entity`（Tk）、`sao`（历史别名，映射为 `entity`）。首选 UI 启动失败时会依次回退到另一套 UI，最后回退 headless。

## 数据源与热键

`settings.json` 默认采用混合数据源：

| 数据 | 默认来源 |
| --- | --- |
| `hp` / `level` / `identity` / `skills` | `packet` |
| `stamina` | `vision` |

只读内存（`mem_data_source`）仅在 hybrid / memory 模式下补齐 TCP 未下发的字段（如名字、基址、破防）；TCP 始终是权威实时源。

默认热键来自 `config.DEFAULT_HOTKEYS`，可在 `settings.json` 覆盖：

| 热键 | 功能 |
| --- | --- |
| `F5` | 开始 / 停止识别 |
| `F6` | 开始 / 停止自动按键 |
| `F7` | 开始 / 停止 Boss Raid |
| `F8` | Boss Raid 下一阶段 |
| `F9` | 切换置顶 |
| `F10` | 隐藏 / 恢复面板 |
| `F11` | 切换 Hide & Seek |

默认游戏窗口匹配：标题含 `Star` / `星痕共鸣`，进程名 `star.exe`。

## 项目结构

```text
sao_auto/
├─ main.py                  # 程序入口与 CLI 参数
├─ config.py                # 版本、路径、默认热键 / 数据源、SettingsManager
├─ settings.json            # 本地运行配置与 UI 状态缓存
├─ sao_gui.py               # Entity/Tk HUD 入口（实现拆到 gui_modules/）
├─ sao_webview.py           # WebView HUD 入口
├─ act_platform/            # ACT 事件总线 / 解析适配器 / 插件引擎 / 选择性解析
├─ act_replay/              # 离线 ACT/TCP replay 合同自检
├─ engines/                 # GameState、DPS、Boss Raid、AutoKey、ACT、Hide & Seek
├─ gui_modules/             # Entity HUD 拆分模块、面板、mixin、热键、弹窗
├─ net/                     # packet_capture / packet_bridge
├─ packet_parser/           # TCP 协议解析器包
├─ render/  ui_gpu/         # GPU overlay、render worker、SkillFX、菜单 / 弹窗布局
├─ vision/                  # 截图识别、STA / 技能识别、vision 加速
├─ mem_probe/  tools/       # 只读内存探针、IL2CPP 研究工具、tablekit、基准
├─ web/  assets/  proto/    # WebView 资源、字体 / 音效 / 名称表、protobuf
├─ server/                  # 脚本仓库 FastAPI 服务
├─ update_host/  updater/   # 更新服务端与客户端更新调度
└─ build_*.py / *.bat       # Cython / delta / full-package / 发布脚本
```

## 开发与自检

离线 ACT replay 自检不进游戏即可验证 parser → encounter → render_spec 链路（归一化事件、dungeon/scene 元信息、skill lifecycle、Boss HP render spec、DPS snapshot 等）。双 UI 共享 `render_spec`，因此一次验证覆盖两端。

```powershell
python -m act_replay.selftest                      # 主要自检
python -m act_replay.run_fixture demo_events       # 复放单个 JSONL fixture
```

常用校验组合：

```powershell
python -m py_compile main.py config.py sao_gui.py sao_webview.py
python -m act_replay.selftest
git diff --check
```

注意事项：
- 不要用仓库级 `compileall` 当唯一验证门——历史工具目录可能存在不适合 Python 3.11 直接编译的旧脚本。
- 新增模块归入对应子包；需保留旧顶层导入时在 `__init__.py` re-export。
- 拆分动态导入模块时同步检查 `XiaoACTUI.spec` 的 `hiddenimports` / `collect_submodules`。
- 性能改动关注重战斗路径，不要只看 idle 状态。
- `mem_probe/` 与 `tools/tablekit/` 默认只读诊断，不进发布包。

可选 ACT 触发规则通过 `settings.json` 的 `act_trigger_rules` 配置（未配置则不产生 alert）：

```json
{
  "act_trigger_rules": [
    {"id": "boss-low-hp", "type": "boss_hp_pct_below", "threshold": 35, "message": "Boss HP below 35%"},
    {"id": "party-damage", "type": "damage_total", "threshold": 1000000, "message": "Party damage 1M"}
  ]
}
```

## 自建服务端

仓库内含两个互相独立的 FastAPI 服务。

**脚本仓库服务端**（默认端口 `9983`）——自动按键脚本与 Boss Raid 配置仓库：

```powershell
python server/app.py
# 或：python -m uvicorn server.app:app --host 0.0.0.0 --port 9983
```

数据库默认位于 `server/data/scripts.db`；上传签名优先读 `SAO_UPLOAD_SECRET`，否则在 `server/data/upload_secret.txt` 生成本地 secret。主要端点：`/health`、`/api/scripts`、`/api/scripts/{id}`、`/api/scripts/{id}/export`、`/api/upload-token/issue`、`/api/boss-raids`。

**更新服务端**（默认端口 `9973`）——客户端远程更新。客户端默认访问 `config.DEFAULT_UPDATE_HOST`，可由 `settings.json` 的 `update_host` 覆盖：

```powershell
cd update_host
pip install fastapi uvicorn
$env:UPDATE_HOST_RELEASE_DIR = "$pwd\releases"
python -m uvicorn app:app --host 0.0.0.0 --port 9973
```

主要端点：`/api/health`、`/api/update/latest`、`/api/update/summary`、`/api/update/anchor`、`/api/update/publish`、`/downloads/<channel>/<target>/<file>`。更多细节见 `update_host/README.md`。

## 打包与发布

> 打包 / 发布会触发 Cython、PyInstaller 或远端上传，普通代码修复不应自动运行这些命令。

客户端发布目录构建：

```powershell
.\build_release.bat
```

该脚本依次构建 Cython 扩展、`XiaoACTUI.exe`、独立 `update.exe`、`AutoKeyServer.exe`，生成 `dist/release/` 目录，并将 `web/`、`assets/`、`proto/`、`icon.ico` 提升到客户端顶层以便增量更新。

更新包构建：

```powershell
python build_delta.py --version 4.6.94 --files runtime/sao_gui.py web/menu.html
python build_full_package.py --version 4.6.94
python dev_publish.py --dry-run      # 发布到更新服务端时用 --upload
```

远程更新机制：
- 客户端启动后后台检查 `update_host`（为空回退 `config.DEFAULT_UPDATE_HOST`）。
- `runtime-delta` 路径与客户端 `BASE_DIR` 对齐，不覆盖启动器 exe；改 `XiaoACTUI.exe`、Python runtime、DLL 或启动 spec 时用 `full-package`。
- `update.exe` 在主程序退出后应用更新；`staging/`、`backup/`、`update_state.json` 为本地运行产物。

## 更新记录

逐版本变更见 [CHANGELOG.md](CHANGELOG.md)（最新在前）。更早的追溯可查 Git 提交记录、`rollout_summaries/` 与仓库记忆。

## 致谢与 License

开发过程参考了开源项目 `StarResonanceDps` 与 `StarResonanceDamageCounter`；其中第三方代码 / 资源仍遵循各自原始许可证。

- `sao_auto` 目录下未特别注明的原创代码：MIT。
- 第三方项目 / 资源 / 生成产物：以各自原始许可证为准。
