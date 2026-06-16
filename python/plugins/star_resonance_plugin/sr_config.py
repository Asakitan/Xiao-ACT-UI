# Star Resonance game-specific configuration constants
# Extracted from platform config.py during 5.0.0 restructure.
from typing import Any, Dict, List, Tuple

GAME_PROCESS_NAMES: list = ["star.exe"]
GAME_WINDOW_KEYWORDS: list = ["Star", "星痕共鸣"]
GAME_MAIN_MODULE: str = "GameAssembly.dll"

# GPU 渲染开关: GLFW/ModernGL 后端可用时默认全开 (LinkStart 开场动画依赖默认创建 GPU 窗口),
# 不要用环境变量门控; 后端不可用或窗口创建失败时自动回退到 ULW/CPU 路径。
USE_GPU_MENU_HUD = True
USE_GPU_OVERLAY = True
# ── Buff 监视器 ──
# True: 在主控台输出 buff 数据流诊断日志 (FIRST data / FIRST rows / 全部被过滤掉等),
# 用于排查 "buff 栏没出来" 类问题。生产环境建议 False。
BUFFMON_DEBUG = True
# 'ultimate': 仅显示 奥义/幻想/职业大招 buff (默认, 严格过滤)
# 'all'     : 显示所有自身 buff (无过滤, 用于诊断或纯展示)
BUFFMON_SELF_FILTER = 'ultimate'
# True: 走 GPU presenter (GpuOverlayWindow + AsyncFrameWorker + BgraPresenter),
#       同 sao_left_info_gpu / sao_gui_skillfx 的渲染路径; 后台合成 + GL 上传, 主线程零开销。
# False: 走 ULW (UpdateLayeredWindow) 兼容路径, 主线程 PIL 合成。
# 自动 fallback: 当 GLFW 不可用或 GPU 窗口创建失败时, 自动切到 ULW。
USE_GPU_BUFFMON = True
USE_GPU_SKILLFX = True

BASE_CLIENT_WIDTH = 1920.0
BASE_CLIENT_HEIGHT = 1080.0

VISUAL_RECT_SPECS: Dict[str, Dict[str, int]] = {
    "stamina_bar_visual": {"right": 1214, "bottom": 1050, "width": 250, "height": 10},
    "skill_slot_1": {"right": 720, "bottom": 1003, "width": 52, "height": 85},
    "skill_slot_2": {"right": 767, "bottom": 1002, "width": 47, "height": 83},
    "skill_slot_3": {"right": 816, "bottom": 1003, "width": 49, "height": 85},
    "skill_slot_4": {"right": 864, "bottom": 1003, "width": 49, "height": 90},
    "skill_slot_5": {"right": 911, "bottom": 1002, "width": 45, "height": 87},
    "skill_slot_6": {"right": 960, "bottom": 1003, "width": 49, "height": 89},
    "skill_slot_7": {"right": 1032, "bottom": 1009, "width": 72, "height": 119},
    "skill_slot_8": {"right": 1104, "bottom": 1012, "width": 73, "height": 124},
    "skill_slot_9": {"right": 1177, "bottom": 1007, "width": 74, "height": 119},
}

# Packet / watched slot numbers now match the on-screen boxes directly.
SKILL_SLOT_VISUAL_INDEX = {
    1: 1,
    2: 2,
    3: 3,
    4: 4,
    5: 5,
    6: 6,
    7: 7,
    8: 8,
    9: 9,
}


def get_visual_rect_spec(name: str) -> Dict[str, int]:
    return dict(VISUAL_RECT_SPECS.get(name, {}))


def get_skill_slot_visual_index(slot_index: int) -> int:
    try:
        slot_index = int(slot_index or 0)
    except Exception:
        return 0
    return int(SKILL_SLOT_VISUAL_INDEX.get(slot_index, slot_index))


def _spec_to_base_box(spec: Dict[str, int]) -> Tuple[float, float, float, float]:
    right = float(spec["right"])
    bottom = float(spec["bottom"])
    width = float(spec["width"])
    height = float(spec["height"])
    return (right - width, bottom - height, right, bottom)


def _union_base_boxes(spec_names: List[str]) -> Dict[str, float]:
    boxes = [_spec_to_base_box(VISUAL_RECT_SPECS[name]) for name in spec_names if name in VISUAL_RECT_SPECS]
    if not boxes:
        return {"x": 0.0, "y": 0.0, "w": 0.0, "h": 0.0}
    left = min(box[0] for box in boxes)
    top = min(box[1] for box in boxes)
    right = max(box[2] for box in boxes)
    bottom = max(box[3] for box in boxes)
    return {
        "x": left / BASE_CLIENT_WIDTH,
        "y": top / BASE_CLIENT_HEIGHT,
        "w": (right - left) / BASE_CLIENT_WIDTH,
        "h": (bottom - top) / BASE_CLIENT_HEIGHT,
    }


_SKILL_SLOT_NAMES = [f"skill_slot_{idx}" for idx in range(1, 10)]
_SKILL_BAR_ROI = _union_base_boxes(_SKILL_SLOT_NAMES)

# Skill-slot positions are pure functions of the game-client geometry, so we
# compute once per (client_rect / client_w x client_h) and reuse on every UI /
# vision tick. Bounded to a handful of entries because client geometry only
# changes when the game window is moved or resized.
_SKILL_SLOT_BBOX_CACHE: Dict[Tuple[int, int, int, int], List[Dict[str, Any]]] = {}
_SKILL_SLOT_CLIENT_CACHE: Dict[Tuple[int, int], List[Dict[str, Any]]] = {}

DEFAULT_ROI = {
    "identity": {"x": 0.010, "y": 0.910, "w": 0.200, "h": 0.060},
    "level": {"x": 0.010, "y": 0.925, "w": 0.100, "h": 0.040},
    "name": {"x": 0.085, "y": 0.930, "w": 0.120, "h": 0.030},
    "hp_bar": {"x": 0.330, "y": 0.932, "w": 0.340, "h": 0.036},
    "hp_text": {"x": 0.380, "y": 0.940, "w": 0.240, "h": 0.028},
    "stamina_bar": {"x": 0.330, "y": 0.957, "w": 0.340, "h": 0.036},
    "stamina_text": {"x": 0.530, "y": 0.968, "w": 0.130, "h": 0.018},
    "player_id": {"x": 0.230, "y": 0.968, "w": 0.100, "h": 0.020},
    "skill_bar": dict(_SKILL_BAR_ROI),
}

BAR_COLORS = {
    "hp": {"h_min": 45, "h_max": 160, "s_min": 25, "s_max": 255, "v_min": 60, "v_max": 255},
    "stamina": {"h_min": 8, "h_max": 50, "s_min": 50, "s_max": 255, "v_min": 80, "v_max": 255},
    "skill_cooldown": {"v_max_dark": 80, "s_max_gray": 40},
}
DEFAULT_PANEL_THEMES = {
    "dps": "dark",
    "hp": "dark",
    "bosshp": "dark",
    "skillfx": "dark",
    "alert": "dark",
    "act": "dark",
    "buffmon": "dark",
}


def normalize_panel_theme(theme: Any, default: str = "dark") -> str:
    fallback = "light" if str(default or "").strip().lower() == "light" else "dark"
    return "light" if str(theme or "").strip().lower() == "light" else fallback


def normalize_panel_themes(raw: Any) -> dict:
    themes = dict(DEFAULT_PANEL_THEMES)
    if isinstance(raw, dict):
        for key, value in raw.items():
            name = str(key or "").strip().lower()
            if name in themes:
                themes[name] = normalize_panel_theme(value, themes[name])
    return themes


DEFAULT_SETTINGS = {
    "mem_persist_names": True,
    "mem_per_field_authority": True,
    "mem_failure_backoff_max_s": 30.0,
    "mem_reprobe_interval_s": 1.0,
    "mem_overlay_flush_interval_s": 5.0,
    "mem_root_ptr_cache_enabled": True,
    "mem_enforce_o1_poll_contract": True,
    "panel_themes": dict(DEFAULT_PANEL_THEMES),
}


def normalize_source_mode(mode: Any, default: str = "packet") -> str:
    text = str(mode or "").strip().lower()
    if text in ("ocr", "vision", "screen", "screen_vision"):
        return "vision"
    if text in ("packet", "network", "network_capture"):
        return "packet"
    return default


def normalize_source_map(raw_map: Any, legacy_mode: Any = None) -> dict:
    legacy = normalize_source_mode(legacy_mode, "packet")
    normalized = {key: legacy for key in DATA_SOURCE_COMPONENTS}
    normalized.update(DEFAULT_DATA_SOURCE_MAP)
    if isinstance(raw_map, dict):
        for key, value in raw_map.items():
            if key == "stamina":
                normalized[key] = "vision"
            elif key == "skills":
                normalized[key] = "packet"
            elif key in normalized:
                normalized[key] = normalize_source_mode(value, normalized[key])
    normalized["stamina"] = "vision"
    normalized["skills"] = "packet"
    return normalized


def anchored_rect_spec_to_pixels(
    spec: Dict[str, int], client_rect: Tuple[int, int, int, int]
) -> Optional[Tuple[int, int, int, int]]:
    if not spec or not client_rect:
        return None
    left, top, right, bottom = client_rect
    client_w = max(1, int(right - left))
    client_h = max(1, int(bottom - top))
    x2 = left + int(round(client_w * (float(spec["right"]) / BASE_CLIENT_WIDTH)))
    y2 = top + int(round(client_h * (float(spec["bottom"]) / BASE_CLIENT_HEIGHT)))
    width = max(1, int(round(client_w * (float(spec["width"]) / BASE_CLIENT_WIDTH))))
    height = max(1, int(round(client_h * (float(spec["height"]) / BASE_CLIENT_HEIGHT))))
    x1 = x2 - width
    y1 = y2 - height
    return (x1, y1, x2, y2)


def anchored_rect_spec_to_client_rect(
    spec: Dict[str, int], client_w: int, client_h: int
) -> Optional[Dict[str, int]]:
    if not spec or client_w <= 0 or client_h <= 0:
        return None
    x2 = int(round(client_w * (float(spec["right"]) / BASE_CLIENT_WIDTH)))
    y2 = int(round(client_h * (float(spec["bottom"]) / BASE_CLIENT_HEIGHT)))
    width = max(1, int(round(client_w * (float(spec["width"]) / BASE_CLIENT_WIDTH))))
    height = max(1, int(round(client_h * (float(spec["height"]) / BASE_CLIENT_HEIGHT))))
    return {"x": x2 - width, "y": y2 - height, "w": width, "h": height}


def get_visual_rect_bbox(name: str, client_rect: Tuple[int, int, int, int]):
    return anchored_rect_spec_to_pixels(VISUAL_RECT_SPECS.get(name, {}), client_rect)


def get_visual_rect_client_rect(name: str, client_w: int, client_h: int):
    return anchored_rect_spec_to_client_rect(VISUAL_RECT_SPECS.get(name, {}), client_w, client_h)


def get_skill_slot_rects(client_rect: Tuple[int, int, int, int]) -> List[Dict[str, Any]]:
    if not client_rect:
        return []
    cached = _SKILL_SLOT_BBOX_CACHE.get(tuple(client_rect))
    if cached is None:
        cached = _build_skill_slot_rects(client_rect)
        _SKILL_SLOT_BBOX_CACHE[tuple(client_rect)] = cached
    return [dict(item) for item in cached]


def _build_skill_slot_rects(client_rect: Tuple[int, int, int, int]) -> List[Dict[str, Any]]:
    rects: List[Dict[str, Any]] = []
    for idx in range(1, 10):
        visual_idx = get_skill_slot_visual_index(idx)
        name = f"skill_slot_{visual_idx}"
        bbox = get_visual_rect_bbox(name, client_rect)
        if not bbox:
            continue
        rects.append({
            "index": idx,
            "visual_index": visual_idx,
            "bbox": bbox,
            "spec": get_visual_rect_spec(name),
        })
    return rects


def get_skill_slot_client_rects(client_w: int, client_h: int) -> List[Dict[str, Any]]:
    cached = _SKILL_SLOT_CLIENT_CACHE.get((client_w, client_h))
    if cached is None:
        cached = _build_skill_slot_client_rects(client_w, client_h)
        _SKILL_SLOT_CLIENT_CACHE[(client_w, client_h)] = cached
    return [dict(item) for item in cached]


def _build_skill_slot_client_rects(client_w: int, client_h: int) -> List[Dict[str, Any]]:
    rects: List[Dict[str, Any]] = []
    for idx in range(1, 10):
        visual_idx = get_skill_slot_visual_index(idx)
        name = f"skill_slot_{visual_idx}"
        rect = get_visual_rect_client_rect(name, client_w, client_h)
        if not rect:
            continue
        rects.append({
            "index": idx,
            "visual_index": visual_idx,
            "rect": rect,
            "spec": get_visual_rect_spec(name),
        })
    return rects


def get_skill_bar_roi() -> Dict[str, float]:
    return dict(_SKILL_BAR_ROI)
