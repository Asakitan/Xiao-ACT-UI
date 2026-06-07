# MIDI 钢琴演奏家 — SAO-UI 插件

把 MIDI 自动弹成**游戏电子琴**：`mido` 解析 → 智能编排 → 60 键(CTRL/SHIFT) / 88 键(`<`>`) 键位映射 → ctypes 扫描码注入。
依赖**随包自带**：插件带自己的 `requirements.txt`，把依赖装进自己的目录、从自己目录 import，不污染主程序全局环境。

## 功能
- 播放 / 暂停 / 停止；速度 0.3–2.0×（±5%）；手动移调 ±36（在自动八度之上微调）。
- **60/88 键切换 + 自动识别**：载入曲目时按覆盖率自动在 60 键(classic) / 88 键(extended) 间择优，并自动算最佳移调；也可手动 `[60键] [88键]`，或随时 `[自动识别]`。
- 演奏引擎：Skyline 主旋律提取、低音脉冲伴奏、智能缩谱（≤2 键）、节拍量化、延音（按键时长 + MIDI 踏板加成）、连音重叠、力度/音区表情、弹性速度、乐句呼吸、熟练度模拟、防漂移模式切换状态机（CTRL/SHIFT 直切；`<`>`方向步进；累计 40 次防漂移重置）、同键防吞音、单键速率限制、八度折叠。
- 分通道：每通道启用/禁用 + 八度增减。
- **原生资源管理器选曲**：comdlg32 `GetOpenFileNameW`（不依赖 Tk root，Entity/WebView 两种模式都能弹出），所选目录自动并入曲库扫描。
- **子面板**：钢琴键盘(键位使用分布/音域/覆盖率)、音符卷帘(播放事件时间轴表)、MIDI 分析(BPM/音部/调号/60-88覆盖率)、设置。每个都 `register_ui_panel` 独立注册（host「插件面板」里可单独唤出），主面板顶部也有「视图切换」可就地唤出。
- 本机试听（不驱动游戏）：Windows 自带合成器(WinMCI) 优先，pygame 兜底；无需 SoundFont。
- folder-scan 曲库（`assets/midi`，可加 `extra_midi_dir`）；可改键热键（默认 F8 播放/暂停、F10 停止）；前台门控（仅游戏在前台时注入）。
- 面板在 Entity(Tk) 与 WebView 双端渲染（用平台声明式 `ctx.ui`）。

## 目录
```
plugin.json        清单（capabilities=ui_panels + settings_schema）
plugin.py          SDK 胶水：引导依赖→装配引擎→注册面板/热键/动作→渲染
bootstrap.py       外置依赖引导（见下）+ sys.path 注入/还原
requirements.txt   插件自己的外置依赖声明（mido>=1.3）
engine/            演奏引擎（mp_ 前缀，避开主程序 config 等同名模块）
  mp_config.py     键位 / 时序常量
  mp_mapper.py     60/88 映射、折叠、移调、覆盖率（全区间有单测）
  mp_parser.py     mido 解析 + 全量编排 + play_events
  mp_player.py     播放循环 + 模式机 + 延音 + 计时；落键走 mp_input
  mp_input.py      ctypes 扫描码注入后端（keyboard 库 API 兼容垫片 press/release）
  mp_audio.py      本机试听（WinMCI / pygame）
  mp_dialog.py     原生「打开文件」对话框（comdlg32，跨 UI 模式）
  mp_api.py        装配 facade + 自动选 60/88 + 状态摘要 + 曲库扫描
vendor/mido/       随包自带的纯 Python mido（离线/冻结态兜底）
libs/              运行时 pip --target 装外置依赖到此（插件本地；默认空）
assets/midi/       默认曲库
```

## 外置依赖引导
`bootstrap.ensure_requirements(plugin_dir, ctx)` 满足 `requirements.txt`，**全部落在插件目录内**：
1. `libs/` —— 开发态联网时 `pip install --target libs -r requirements.txt`（装进插件自己的目录）；
2. `vendor/` —— 随包自带的纯 Python 副本（冻结态 / 离线兜底）；
3. （仅当插件目录内都没有且本地安装失败，才最后退回主程序环境，记为 `site(fallback)`，不直接报错。）

把 `engine/ libs/ vendor/` 前插到 `sys.path` 最前，使插件本地副本优先于全局 site-packages；
`import` 后校验 `__file__` 确实在插件目录内，否则触发本地安装。`on_unload` 会还原 `sys.path`。

测试 pip 路径：删空 `vendor/mido` 后联网启用插件 → 会自动 `pip install` 到 `libs/`。

## 注入说明
扫描码（`KEYEVENTF_SCANCODE`）对游戏 DirectInput/RawInput 兼容性最好。`mp_input.press/release` 与 PyPI `keyboard` 库同名同义，
因此 `mp_player.py` 顶部 `import mp_input as keyboard` 后，全部落键调用零改动直通。
> 星痕共鸣/ACE 环境下注入有效性需以实测为准；扫描码已是最佳路径。

## 自检要点（已通过）
- `mp_mapper` 全 128 音 × classic/extended + suggest_transpose/coverage 输出稳定（有单测）。
- 端到端：解析 → 自动识别 → 播放循环 → 扫描码注入 → 停止；面板 spec 经平台校验器 `normalize_ui_spec` 合法（Tk/Web 双端）。
- 依赖默认从插件本地 `vendor/` import；pip→`libs/` 路径实测可装。
