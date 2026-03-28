# SAO Auto

`SAO Auto` 是一个面向《星痕共鸣》的外部 HUD / 自动化实验项目。当前主线版本已经切到 `pywebview` 透明覆盖层方案，围绕抓包、窗口识图、Boss Raid、DPS 统计和自动按键工作流持续迭代。

当前版本：`1.1.8`

## 当前版本支持内容

- Windows 平台透明 WebView HUD
- 抓包驱动的 `HP / 等级 / 身份 / 技能状态` 同步
- 固定 ROI 识图驱动的 `STA` 百分比识别
- 独立 `Boss HP` 覆盖条
- 独立 `DPS / HPS` 实时统计面板
- `Burst Ready` 视觉与音效提示
- `Boss Raid` 阶段计时、时间轴提醒、狂暴倒计时
- `Boss ↔ AutoKey` 联动触发
- 自动按键本地配置、导入导出、云端脚本库

## 当前版本说明

- 当前主线 UI 入口只有 `WebView` 模式，旧的 tkinter 壳不再是主线运行方式。
- 本版本继续走“外部工具”路线：抓包、窗口识图、WebView HUD、模拟按键。
- 不建议继续尝试内存注入或类似内存修改方案。
- 当前实现里，`STA` 仍然使用识图；`skills` 仍然固定使用 `packet`；`hp / level / identity` 默认使用 `packet`。

## 运行环境

- Windows 环境
- 已安装 Python 与 `pip`
- 已安装 `Microsoft Edge WebView2 Runtime`
- 抓包模式建议安装 `Npcap`

补充说明：

- `requirements.txt` 中包含 `pywebview`、`pythonnet`、`pygame`、`opencv-python`、`pynput` 等当前主线依赖。
- 若系统中未安装 `Npcap`，抓包桥接在启动时会尝试自动安装。
- 运行源码时，配置文件默认写入当前目录下的 `settings.json`；打包后则写在 exe 同目录。

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

## 当前默认数据源

当前版本默认是混合数据源：

- `hp`：`packet`
- `level`：`packet`
- `identity`：`packet`
- `skills`：`packet`
- `stamina`：`vision`

这套默认值来自当前代码主线配置，和菜单里的显示保持一致。

## 默认快捷键

当前 WebView 主线默认启用：

- `F5`：开始 / 停止识别
- `F6`：开始 / 停止自动按键
- `F7`：开始 / 停止 Boss Raid
- `F8`：Boss Raid 下一阶段

保留但当前主线未实际绑定功能：

- `F9`
- `F10`

## 自动按键

自动按键当前支持：

- 本地 `profile` 管理
- 动作顺序、按键、次数、延迟、重触发间隔
- `Boss Raid` 事件联动触发
- 结构化条件判断
- 外部 `JSON` 导入 / 导出
- 云端搜索、下载、上传脚本

当前内置条件包括：

- `hp_pct_gte`
- `hp_pct_lte`
- `sta_pct_gte`
- `burst_ready_is`
- `slot_state_is`
- `profession_is`
- `player_name_is`

## Boss Raid / DPS

当前版本中的 `Boss Raid` 支持：

- 本地 profile 新建、保存、删除
- profile 导入 / 导出
- 云端搜索 / 下载 / 上传
- 阶段推进
- 时间轴提醒
- 狂暴倒计时
- Boss 条显示模式切换

当前版本中的 `DPS` 面板支持：

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

当前服务端提供两类数据：

- 自动按键脚本：`/api/scripts`
- Boss Raid 配置：`/api/boss-raids`

本地数据库默认位于：

- `server/data/scripts.db`

## 目录概览

```text
sao_auto/
├─ main.py                 # 程序入口
├─ config.py               # 全局配置、默认热键、数据源设置
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
├─ sao_sound.py            # 音效、Burst Ready TTS 与字体加载
├─ web/
│  ├─ hp.html              # 主 HUD
│  ├─ menu.html            # 全屏菜单
│  ├─ skillfx.html         # Burst / 特效面板
│  ├─ boss_hp.html         # Boss HP 覆盖条
│  ├─ dps.html             # DPS 面板
│  ├─ panel.html           # 通用子面板
│  ├─ alert.html           # 弹窗
│  └─ stamina.html         # 体力相关展示
└─ server/
   └─ app.py               # FastAPI 脚本 / BossRaid 服务端
```

## 注意事项

- 当前项目默认通过窗口标题关键字 `Star` / `星痕共鸣` 与进程名 `star.exe` 查找游戏窗口。
- 抓包链路依赖当前协议解析逻辑，游戏协议变化后可能需要同步调整解析器。
- 当前版本仍然以 Windows 运行体验为目标，跨平台不在主线支持范围内。

## 致谢

这个项目在开发过程中直接受益于以下开源仓库和作者公开的源码、协议整理、思路与工程经验。非常感谢他们的分享：

- [`StarResonanceDamageCounter`](../StarResonanceDamageCounter/README.md)
- [`StarResonanceDps`](../StarResonanceDps/README.md)

补充说明：

- 本项目里引用、参考、学习到的思路和实现非常多。
- 若某些第三方代码、资源或生成产物来自上游项目，则仍应遵循其原始许可证。
- 本仓库 `sao_auto` 目录下未特别注明的原创部分，按 MIT License 发布。

## License

- `sao_auto` 目录下未特别注明的原创代码：`MIT`
- 第三方项目、第三方资源、第三方生成产物：以各自原始许可证为准

