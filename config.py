# -*- coding: utf-8 -*-
"""
SAO Auto — 配置与设置管理

包含:
  - 识别 ROI 配置 (供 recognition.py 使用)
  - Settings 持久化
"""

import os
import sys
import json
from typing import Any, Optional

# ═══════════════════════════════════════════════
#  路径
# ═══════════════════════════════════════════════
if getattr(sys, 'frozen', False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))

ASSETS_DIR = os.path.join(BASE_DIR, 'assets')
SOUNDS_DIR = os.path.join(ASSETS_DIR, 'sounds')
FONTS_DIR  = os.path.join(ASSETS_DIR, 'fonts')
WEB_DIR    = os.path.join(BASE_DIR, 'web')

# GUI 设置
WINDOW_TITLE = "SAO Auto — 游戏辅助 UI"
WINDOW_SIZE = "900x980"

# ═══════════════════════════════════════════════
#  默认 ROI (基于 16:9 参考分辨率 1920×1080)
#  所有值都是相对窗口尺寸的百分比 (0.0 ~ 1.0)
# ═══════════════════════════════════════════════
DEFAULT_ROI = {
    # 左下角 — 人物名字/等级/ID 区域
    'identity': {
        'x': 0.010,   # 左侧起始
        'y': 0.910,   # 底部偏上
        'w': 0.200,   # 宽度占屏幕 20%
        'h': 0.060,   # 高度占屏幕 6%
    },
    # 等级数字区域 (更精确的子区域)
    'level': {
        'x': 0.010,
        'y': 0.925,
        'w': 0.100,    # 加宽以捕获完整 "LV6" 或 "等级60(+12)"
        'h': 0.040,    # 加高容错
    },
    # 人物名字区域
    'name': {
        'x': 0.085,
        'y': 0.930,
        'w': 0.120,
        'h': 0.030,
    },
    # 中央下方 — HP 数值条 (生命值/406569/406569)
    'hp_bar': {
        'x': 0.330,
        'y': 0.932,
        'w': 0.340,
        'h': 0.036,
    },
    # HP 数字文本区域
    'hp_text': {
        'x': 0.380,
        'y': 0.940,
        'w': 0.240,
        'h': 0.028,
    },
    # 体力值条
    'stamina_bar': {
        'x': 0.330,
        'y': 0.957,    # 略微上移以捕获完整条
        'w': 0.340,
        'h': 0.036,    # 加高容错
    },
    # 体力数字文本
    'stamina_text': {
        'x': 0.530,
        'y': 0.968,
        'w': 0.130,
        'h': 0.018,
    },
    # 玩家编号区域 (底部状态栏)
    'player_id': {
        'x': 0.230,
        'y': 0.968,
        'w': 0.100,
        'h': 0.020,
    },
    # 技能栏区域 (底部中央, 通常 8~10 个技能格)
    'skill_bar': {
        'x': 0.300,
        'y': 0.900,
        'w': 0.400,
        'h': 0.070,
    },
}

# HP / 体力条颜色范围 (HSV)
# 游戏中 HP 条为绿色，体力条为橙黄色
BAR_COLORS = {
    'hp': {
        'h_min': 45, 'h_max': 160,    # 绿色色相范围 (宽容)
        's_min': 25, 's_max': 255,
        'v_min': 60, 'v_max': 255,
    },
    'stamina': {
        'h_min': 8, 'h_max': 50,      # 橙黄色色相范围 (加宽容错)
        's_min': 50, 's_max': 255,     # 降低饱和度阈值
        'v_min': 80, 'v_max': 255,     # 降低亮度阈值
    },
    'skill_cooldown': {
        # 冷却遮罩通常是半透明深色覆盖 (低饱和度 + 低亮度)
        'v_max_dark': 80,              # 暗于此值 → 视为冷却中
        's_max_gray': 40,              # 饱和度低 → 灰色遮罩
    },
}

# 默认全局快捷键
DEFAULT_HOTKEYS = {
    'toggle_recognition': 'F5',
    'toggle_topmost': 'F9',
    'hide_panels': 'F10',
}

# 配置文件路径
def _get_config_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(__file__)

CONFIG_FILE = os.path.join(_get_config_dir(), 'settings.json')

# 游戏窗口匹配
GAME_WINDOW_KEYWORDS = ['Star', '星痕共鸣']   # 标题关键词 (不区分大小写)
GAME_PROCESS_NAMES  = ['star.exe']              # 进程名匹配 (不区分大小写)

# 采集帧率
CAPTURE_FPS = 5            # 普通采集 5 fps
CAPTURE_FPS_FAST = 10      # 快速采集 10 fps

# ═══════════════════════════════════════════════
#  Settings 持久化
# ═══════════════════════════════════════════════
class SettingsManager:
    """JSON 配置文件读写"""

    def __init__(self, path: Optional[str] = None):
        self._path = path or os.path.join(BASE_DIR, 'settings.json')
        self._data: dict = {}
        self._load()

    def _load(self):
        try:
            if os.path.exists(self._path):
                with open(self._path, 'r', encoding='utf-8') as f:
                    self._data = json.load(f)
        except Exception:
            self._data = {}

    def get(self, key: str, default: Any = None) -> Any:
        return self._data.get(key, default)

    def set(self, key: str, value: Any):
        self._data[key] = value

    def save(self):
        try:
            with open(self._path, 'w', encoding='utf-8') as f:
                json.dump(self._data, f, indent=2, ensure_ascii=False)
        except Exception:
            pass

    def get_roi(self, name: str) -> dict:
        """获取 ROI 配置，优先用户自定义，回退默认"""
        custom = self._data.get('roi', {}).get(name)
        if custom:
            return custom
        return DEFAULT_ROI.get(name, {})

    def set_roi(self, name: str, roi: dict):
        if 'roi' not in self._data:
            self._data['roi'] = {}
        self._data['roi'][name] = roi
