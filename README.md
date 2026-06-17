# SAO ACT UI

通用 NervGear ACT 平台——SAO 风格透明覆盖界面，适用于任何游戏。通过插件适配不同游戏，提供 DPS/HPS 统计、Boss 机制提醒、自动化引擎等功能。

## 特色

- **SAO 风格界面** — NerveGear 按钮、鱼眼菜单、LinkStart 动画、透明 HUD 覆盖
- **插件化架构** — 平台不绑定任何游戏，游戏支持全部由插件提供
- **双 UI 模式** — WebView（推荐，基于 Edge WebView2）与 Tk 原生窗口两套界面
- **DPS/HPS 框架** — 实时伤害/治疗统计、战斗回放、Boss 血条与破防追踪
- **AI 编辑器** — 内置多模型 LLM IDE，支持 OpenAI / Claude / DeepSeek / Ollama，Tool Calling、MCP、自定义 Agent 和 Workflow
- **内存扫描** — 通用内存读取基础设施，插件可用于读取游戏运行时数据
- **自动更新** — 增量更新 + 完整包更新双通道

## 系统要求

| 要求 | 说明 |
|------|------|
| 操作系统 | Windows 10 / 11 |
| 运行时 | Microsoft Edge WebView2 Runtime（WebView 模式需要） |
| 网络 | Npcap（TCP 抓包模式建议安装） |
| 权限 | 内存扫描功能需要管理员权限 |

## 安装

从发布页面下载最新版本压缩包，解压后运行 `XiaoACTUI.exe`。

首次启动会显示 NerveGear 按钮（屏幕左上角圆形图标），点击打开 SAO 菜单。

## 快速上手

1. **启动** — 运行 `XiaoACTUI.exe`，NerveGear 按钮出现在屏幕上
2. **菜单** — 左键点击按钮打开 SAO 菜单
3. **插件** — 在菜单 → 插件 中管理已安装的游戏适配插件
4. **ACT** — 在菜单 → ACT 中打开 DPS 面板、Boss 面板等
5. **设置** — 右键 NerveGear 按钮或通过菜单进入设置

## UI 模式

| 模式 | 说明 |
|------|------|
| `webview` | 基于 Edge WebView2 的现代界面（推荐） |
| `entity` | Tk 原生窗口，兼容性更好 |

在设置中切换 UI 模式。如果选择的模式启动失败，会自动回退到另一种。

## 热键

| 热键 | 功能 |
|------|------|
| `F5` | 开始 / 停止识别 |
| `F9` | 切换窗口置顶 |
| `F10` | 隐藏 / 恢复面板 |

游戏专属热键由各插件注册。所有热键可在设置中自定义。

## AI 编辑器

从 SAO 菜单 → ACT → "AI Editor (LLM)" 打开内置 AI 编辑器。

- 支持 OpenAI、Claude、DeepSeek、Ollama 等多种模型
- VSCode 风格布局，右侧边栏支持 CHAT / Claude Code / Codex 多 tab
- 12 个 Tool Calling 工具，可读写文件、搜索代码、运行终端
- 自定义 Agent（代码审查、调试、优化等）和多步 Workflow
- MCP 服务器集成，VSCode Marketplace 扩展浏览
- 自定义指令、三层 scope（系统/项目/插件）

详见 [AI 编辑器文档](python/docs/AI_EDITOR.md)。

## 插件开发

插件是独立目录，包含清单文件和入口脚本。通过 SDK 注册数据源、面板、菜单和引擎扩展。

```python
# plugins/my_game/plugin.py
def on_load(ctx):
    ctx.register_menu_category('MyGame', 'G', build_menu)
    ctx.register_data_source('tcp', config, start, stop)
    ctx.register_ui_panel('dps', spec, render=my_render)
    ctx.subscribe('damage', on_damage)
```

完整开发指南见 [插件 SDK 文档](python/docs/PLUGIN_SDK.md)。

## 已有插件

| 插件 | 说明 |
|------|------|
| 星痕共鸣 | TCP + 内存双数据源，DPS/Boss/HP/Buff 面板，自动按键，Boss 机制提醒 |
| 躲猫猫 | 躲猫猫游戏自动化 |
| MIDI 钢琴 | 游戏内 MIDI 钢琴演奏 |

## 文档

| 文档 | 内容 |
|------|------|
| [平台概览](python/docs/ACT_PLATFORM.md) | 平台功能和配置 |
| [插件 SDK](python/docs/PLUGIN_SDK.md) | 插件开发完整指南 |
| [数据源](python/docs/HYBRID_MEMORY_TCP.md) | TCP/内存混合数据源使用 |
| [游戏状态 API](python/docs/GAME_STATE_API.md) | 游戏数据访问接口 |
| [AI 编辑器](python/docs/AI_EDITOR.md) | AI 编辑器功能和配置 |
| [面板开发](python/docs/ACT_UI_PARITY_SDK.md) | 插件 UI 面板开发 |
| [更新记录](CHANGELOG.md) | 逐版本变更日志 |

## 许可

- 原创代码：MIT
- 第三方项目 / 资源 / 生成产物：以各自原始许可证为准
