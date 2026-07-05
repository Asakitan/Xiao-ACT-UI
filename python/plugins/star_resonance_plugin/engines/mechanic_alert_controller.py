# -*- coding: utf-8 -*-
# 机制提醒协调器：引擎 on_mechanic 事件 → TTS 播报 + 顶部横幅。
#
# 每个 UI 进程持有一个实例 (Tk 端横幅走 MechBannerOverlay, Web 端走
# 'SAO MechBanner' 窗口推送)。躲避按键不经此处 —— 引擎的 forward 直接进
# BossAutoKeyLinkage。本模块还提供编辑器的试发入口 (战斗外干跑)。

import threading
import time
from typing import Any, Callable, Dict, Iterable, Optional

from utils import sao_tts


def _s(v: Any) -> str:
    return str(v or "").strip()


class MechanicAlertController:
    # on_mechanic(evt) 消费者: 按事件的 alert_type 分发 TTS 与横幅。

    def __init__(self,
                 on_banner: Optional[Callable[[Dict[str, Any]], None]] = None,
                 speak: Optional[Callable[..., Any]] = None,
                 banner_enabled_fn: Optional[Callable[[], bool]] = None):
        # Args:
        # on_banner: callback(entry) → 顶部横幅推一行 (Tk overlay / Web push)
        # speak: TTS 入口, 默认 sao_tts.speak_text
        # banner_enabled_fn: 返回横幅总开关 (None = 恒开)
        self._on_banner = on_banner
        self._speak = speak or sao_tts.speak_text
        self._banner_enabled_fn = banner_enabled_fn

    # ── runtime path ──

    def on_mechanic(self, evt: Dict[str, Any]):
        # 引擎机制事件 (已在引擎锁外的线程上)。
        if not isinstance(evt, dict) or not evt.get("alert_enabled", True):
            return
        alert_type = _s(evt.get("alert_type")) or "both"
        if alert_type in ("sound", "both"):
            text = _s(evt.get("tts_text")) or _s(evt.get("name"))
            if text:
                priority = "high" if (evt.get("dodge_enabled")
                                      or evt.get("source") == "enrage") else "normal"
                try:
                    self._speak(text, priority=priority)
                except Exception:
                    pass
        if alert_type in ("visual", "both"):
            self._push_banner(evt)

    def _push_banner(self, evt: Dict[str, Any]):
        if self._on_banner is None:
            return
        if self._banner_enabled_fn is not None:
            try:
                if not self._banner_enabled_fn():
                    return
            except Exception:
                pass
        countdown_s = float(evt.get("countdown_s") or 0.0)
        entry = {
            "id": _s(evt.get("mechanic_id")) or f"evt{int(time.time() * 1000)}",
            "name": _s(evt.get("name")) or "机制",
            "text": _s(evt.get("banner_text")) or _s(evt.get("name")),
            "color": _s(evt.get("color")),
            "countdown_ms": int(countdown_s * 1000),
            "remaining_ms": int(countdown_s * 1000),
            "pre_warn_ms": int(float(evt.get("pre_warn_s") or 0.0) * 1000),
            "urgency": "normal",
        }
        try:
            self._on_banner(entry)
        except Exception:
            pass

    # ── editor test path (战斗外干跑) ──

    def test_mechanic(self, mech: Dict[str, Any],
                      kinds: Iterable[str] = ("tts", "banner")) -> Dict[str, Any]:
        # 试发一个机制配置的提醒部分; 按键试发走 linkage.fire_mapping_test。
        result: Dict[str, Any] = {"ok": True}
        if not isinstance(mech, dict):
            return {"ok": False, "error": "bad mechanic"}
        alert = mech.get("alert") or {}
        name = _s(mech.get("name")) or "机制"
        kinds = set(kinds or ())
        if "tts" in kinds:
            text = _s(alert.get("tts_text")) or name
            try:
                result["tts"] = self._speak(text, priority="high")
            except Exception:
                result["tts"] = "error"
        if "banner" in kinds:
            evt = {
                "mechanic_id": _s(mech.get("id")) or "test",
                "name": name,
                "banner_text": _s(alert.get("banner_text")) or name,
                "color": _s(mech.get("color")),
                "countdown_s": float(alert.get("countdown_s") or 0.0),
                "pre_warn_s": float(alert.get("pre_warn_s") or 0.0),
                "alert_enabled": True,
                "alert_type": "visual",
            }
            self._push_banner(evt)
            result["banner"] = "shown"
        return result

    def presynthesize_profile(self, profile: Dict[str, Any]):
        # 档案加载/保存后台预合成全部 TTS 文案 (战斗路径纯缓存命中)。
        try:
            texts = sao_tts.collect_profile_tts_texts(profile)
            if texts:
                threading.Thread(target=sao_tts.presynthesize, args=(texts,),
                                 daemon=True).start()
        except Exception:
            pass
