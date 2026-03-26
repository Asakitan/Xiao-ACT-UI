# SAO Auto — 游戏 HUD 与自动化

## 功能

- **HP 面板** — 左下角 SAO 风格透明悬浮窗，覆盖原始人物名字区域
  - 显示等级 `60(+XX)`、角色名、玩家 ID
  - SAO 风格 HP 条 (实时颜色变化)
- **体力覆盖层** — 中央下方不透明覆盖，覆盖原始 HP/体力条位置
  - 实时获取体力条数据
- **屏幕识别** — OCR + 像素/条形检测
  - 自动定位游戏窗口
  - 按比例 ROI 适配 16:9 分辨率
- **自动化框架** — 热键、状态采集、可扩展动作接口

## 架构

```
sao_auto/
├── main.py                 # 入口 (WebView / headless / test)
├── config.py               # 配置、ROI、设置持久化
├── game_state.py           # 统一状态模型
├── window_locator.py       # 游戏窗口定位 (Win32)
├── recognition.py          # 截图 + OCR + 条形检测
├── webview_controller.py   # WebView 透明 HUD 控制器
├── automation.py           # 自动化核心 (采集/热键)
├── sao_sound.py            # 音效与字体
├── requirements.txt
├── web/
│   ├── hp.html             # HP HUD 面板 (HTML/CSS/JS)
│   ├── stamina.html        # 体力覆盖层
│   └── fonts/              # SAO 字体
└── assets/
    ├── fonts/              # 系统字体
    └── sounds/             # SAO 音效
```

## 使用

```bash
# 安装依赖
pip install -r requirements.txt

# 默认模式: WebView HUD
python main.py

# 单次识别测试
python main.py --test

# 无 HUD 终端模式
python main.py --headless
```

## 数据流

```
游戏窗口 → 截图 → ROI 裁切 → 识别 (OCR / 像素) → GameState → HUD 更新
                                                          ↓
                                                    自动化动作 (预留)
```

## 识别策略

| 字段   | 方法              | 频率     |
|--------|-------------------|----------|
| HP 条  | 像素颜色/长度识别 | 5 fps    |
| 体力条 | 像素颜色/长度识别 | 5 fps    |
| HP 数值 | OCR              | 每 2 秒  |
| 体力数值 | OCR            | 每 2 秒  |
| 等级   | OCR               | 每 2 秒  |
| 名字   | OCR               | 每 2 秒  |
| 玩家 ID | OCR              | 每 2 秒  |

## 热键

| 按键 | 功能         |
|------|-------------|
| F9   | 切换 HUD    |
| F10  | 调试面板    |
| F11  | 自动功能    |
