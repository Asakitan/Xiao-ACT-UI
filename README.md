# SAO Auto

星痕共鸣 ACT。当前主模式是透明 `WebView` HUD，支持抓包 `packet` 与 OCR 双数据源，其中角色等级 `60(+XX)`、STA、职业、UID 等信息优先走 `packet`。

## 当前功能

- 透明 SAO 风格 HUD
  - 左下角色面板
  - HP 条、STA 条、职业、UID、等级 `60(+XX)`
  - 技能栏与右键菜单
- `packet` 优先的数据链路
  - 角色等级、赛季等级、HP、STA、职业、UID
  - 登录后会优先回显缓存，再等实时包刷新
- OCR / 像素兜底
  - 主要保留给非 packet 场景和少量补充识别
- 状态缓存
  - `game_cache` 会保存角色名、等级、赛季等级、HP/STA、职业、UID
  - 第二次登录会先显示缓存值，再被实时数据覆盖

## 目录

```text
sao_auto/
├── main.py                 # 入口
├── config.py               # 配置与 settings.json 读写
├── game_state.py           # HUD 统一状态模型与 game_cache
├── packet_capture.py       # Npcap 抓包
├── packet_parser.py        # 星痕共鸣包解析
├── packet_bridge.py        # packet -> GameState 桥接
├── recognition.py          # OCR / 像素识别
├── sao_webview.py          # WebView HUD 主控制器
├── sao_gui.py              # 旧 GUI 模式（仍保留部分兼容逻辑）
├── web/
│   ├── hp.html             # HP / STA / 技能栏 / 右键菜单
│   ├── menu.html           # 主菜单
│   └── panel.html          # 控制 / 状态面板
└── proto/
    └── star_resonance.proto
```

## 运行

```bash
pip install -r requirements.txt
python main.py
```

常用模式：

- 默认：`WebView HUD`
- 测试：`python main.py --test`
- 无 HUD：`python main.py --headless`

## 数据流

```text
packet / OCR
    ↓
GameStateManager
    ↓
sao_webview.py
    ↓
web/hp.html
```

当前默认思路：

- HP / STA / `(+XX)` 优先使用 `packet`
- WebView 前端只负责显示与点击区域回传
- 点击命中区域由前端实际可见 UI 矩形上报，Python 侧转换为窗口命中区域

## 缓存与设置

- 角色相关缓存保存在 `settings.json -> game_cache`
- HUD 位置、数据源、ROI 也保存在 `settings.json`

## 已知说明

- `packet` 模式依赖 Npcap
- 登录早期可能先显示缓存值，随后再被实时包修正
- 如果点击区域偶发异常，通常和 HUD 窗口完成定位前的命中区域注册时机有关；当前实现已在窗口落位后补做多次重注册
