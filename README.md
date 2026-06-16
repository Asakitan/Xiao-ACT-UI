# SAO ACT UI

通用 ACT（Advanced Combat Tracker）平台，提供 SAO 风格透明覆盖界面、插件化游戏适配、DPS/HPS 统计框架与自动化引擎。平台本身不绑定任何游戏——游戏支持通过插件提供。

- 当前版本：`5.0.0`（以 `python/config.py` 的 `APP_VERSION` 为准）
- 运行平台：Windows 10 / 11
- 两套 UI：`webview`（pywebview + WebView2）与 `entity`（Tk / 原生窗口）

## 架构

```
sao_auto/
├── README.md / CHANGELOG.md / AGENTS.md / LICENSE
├── C#/                         # C# 客户端（WPF / Overlay / Proto）
└── python/                     # Python 平台 + 插件
    ├── act_platform/           # 插件框架、事件总线、渲染钩子、UI spec
    ├── engines/                # 平台引擎（DPS tracker / Encounter / Trigger / AutoKey）
    ├── gui_modules/            # 平台 UI mixin（菜单 / 鱼眼 / NerveGear 按钮 / 面板）
    ├── render/  ui_gpu/        # GPU overlay 渲染、HUD 布局
    ├── sao_theme/              # SAO UI 组件 + LinkStart 动画 + 字体 / 音效
    ├── mem_probe/              # 通用内存扫描基础设施
    ├── server/ updater/        # 脚本仓库 + 远程更新
    └── plugins/                # 游戏适配插件
        ├── star_resonance_plugin/  ★ 星痕共鸣适配
        ├── hide_seek_plugin/       躲猫猫自动化
        └── midi_piano_plugin/      MIDI 钢琴演奏
```

### 平台层

平台提供通用能力，不依赖任何插件：

- **NerveGear 按钮**：64px 圆形入口（dark/light 主题，可拖动），左键打开 SAO 菜单
- **SAO 菜单**：平台固定 5 分类（控制/ACT/插件/皮肤/关于）+ 插件动态贡献分类
- **鱼眼背景**：菜单打开时的 GPU 桶形畸变效果，支持桌面截图/自选图片/纯色三种源
- **LinkStart 动画**：NerveGear 风格启动动画
- **ACT 框架**：事件总线 + DPS/HPS 追踪 + 战斗管理 + 触发器引擎
- **插件 SDK**：`register_ui_panel` / `register_render_hook` / `register_menu_category` / `register_engine` / `register_data_source` / `subscribe` / `emit` / 热键 / 定时器 / 通知

### 插件层

游戏支持通过插件实现。插件通过 SDK 注册数据源、引擎、面板和菜单分类。

**星痕共鸣插件**（`star_resonance_plugin`）提供：
- TCP 抓包 + IL2CPP 只读内存双数据源
- DPS/BossHP/HP/Alert/SkillFX/BuffMon 覆盖面板
- AutoKey 自动按键 + BossRaid 阶段计时
- Boss 机制提醒（TTS + 横幅 + 定向躲避）
- 技能 CD 监视 + 地图横幅 + 死亡回放
- 菜单贡献：自动/Boss/Burst/面板 四个分类

## 运行要求

- Windows 10 / 11，Python 3.11 + `pip`
- Microsoft Edge WebView2 Runtime（webview 模式）
- Npcap（抓包模式建议安装）
- 主线依赖见 `python/requirements.txt`

## 快速开始

```powershell
cd python
pip install -r requirements.txt
python main.py
```

或使用 `python/Start.bat`。`ui_mode` 取值：`webview`（默认）、`entity`（Tk）。首选 UI 启动失败时依次回退。

## 插件开发

插件放在 `python/plugins/<plugin_id>/` 下，包含 `plugin.json` 清单和 `plugin.py` 入口。

```python
def on_load(ctx):
    ctx.register_menu_category('MyGame', 'G', build_menu_items)
    ctx.register_engine('game_state', my_game_state)
    ctx.register_data_source('my_tcp', {...}, start_fn, stop_fn)
    ctx.register_ui_panel('my_dps', {...}, render=my_render)
    ctx.subscribe('damage', on_damage)

def on_enable(): ...
def on_disable(): ...
def on_unload(): ...
```

完整 SDK 文档见 `python/act_platform/plugins.py` 中的 `PluginContext` 类。

## 热键

默认热键（`config.DEFAULT_HOTKEYS`，可在 `settings.json` 覆盖）：

| 热键 | 功能 |
| --- | --- |
| `F5` | 开始 / 停止识别 |
| `F9` | 切换置顶 |
| `F10` | 隐藏 / 恢复面板 |

游戏专属热键由各插件注册（如星痕共鸣插件注册 F6=AutoKey、F7=BossRaid）。

## 打包与发布

```powershell
cd python
.\build_release.bat       # Cython + PyInstaller + 客户端目录
python build_delta.py     # 增量更新包
python build_full_package.py  # 完整安装包
```

## 更新记录

逐版本变更见 [CHANGELOG.md](CHANGELOG.md)。

## License

- `sao_auto` 目录下未特别注明的原创代码：MIT
- 第三方项目 / 资源 / 生成产物：以各自原始许可证为准
