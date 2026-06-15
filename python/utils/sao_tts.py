# -*- coding: utf-8 -*-
"""通用 TTS 播报：SAPI 离线合成 + 文件缓存 + 预合成 + 优先级打断。

战斗路径设计为纯缓存命中：机制文案在档案加载/保存时预合成到
temp/tts_cache/，speak_text 只做一次 dict/磁盘查找后丢给 sao_sound
的专用 TTS 通道播放（高优先级自然打断低优先级）。
"""

import base64
import hashlib
import os
import queue
import subprocess
import threading
import time
import wave
from collections import deque
from typing import Any, Callable, Dict, List, Optional

from config import TEMP_DIR
from utils import sao_sound

CACHE_DIR = os.path.join(TEMP_DIR, "tts_cache")

_RATE = -2          # SAPI 语速 (与 burst_ready 一致)
_STALE_AFTER_S = 4.0   # 合成完成时若请求已过期则不播 (迟到的播报比没有更糟)

_tts_enabled = True
_tts_volume = 0.8   # 0.0 ~ 1.0

_state_lock = threading.Lock()
_current_priority = ""        # 正在播放的优先级 ('' = idle)
_current_ends_at = 0.0
_pending: deque = deque(maxlen=2)   # 排队的 normal 播报 (text, volume, expires_at)

_synth_queue: "queue.Queue" = queue.Queue(maxsize=64)
_synth_thread: Optional[threading.Thread] = None
_synth_thread_lock = threading.Lock()
_synth_inflight: set = set()
_synth_inflight_lock = threading.Lock()

_zh_voice_available: Optional[bool] = None
_zh_voice_lock = threading.Lock()


def set_tts_enabled(enabled: bool):
    global _tts_enabled
    _tts_enabled = bool(enabled)


def get_tts_enabled() -> bool:
    return _tts_enabled


def set_tts_volume(volume_pct: int):
    global _tts_volume
    _tts_volume = max(0.0, min(1.0, int(volume_pct or 0) / 100.0))


def get_tts_volume() -> int:
    return int(round(_tts_volume * 100))


def _has_cjk(text: str) -> bool:
    return any("一" <= ch <= "鿿" for ch in text)


def _culture_for(text: str) -> str:
    return "zh-CN" if _has_cjk(text) else "en-US"


def _cache_path(text: str) -> str:
    culture = _culture_for(text)
    key = hashlib.sha1(f"{culture}|{_RATE}|{text}".encode("utf-8")).hexdigest()[:16]
    return os.path.join(CACHE_DIR, f"{key}.wav")


def _cached(path: str) -> bool:
    try:
        return os.path.exists(path) and os.path.getsize(path) > 1024
    except Exception:
        return False


def _encoded_command(script: str) -> str:
    return base64.b64encode(script.encode("utf-16le")).decode("ascii")


def _synthesize(text: str, output_path: str) -> bool:
    """SAPI 合成到 wav；中文文本请求 zh-CN 声音 (失败回系统默认声音)。"""
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    culture = _culture_for(text)
    safe_output = output_path.replace("'", "''")
    safe_text = text.replace("'", "''")
    script = (
        "Add-Type -AssemblyName System.Speech;"
        f"$culture=[System.Globalization.CultureInfo]::GetCultureInfo('{culture}');"
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        "try{$s.SelectVoiceByHints([System.Speech.Synthesis.VoiceGender]::Female,"
        "[System.Speech.Synthesis.VoiceAge]::Adult,0,$culture)}catch{};"
        f"$s.Rate={_RATE};"
        "$s.Volume=100;"
        f"$s.SetOutputToWaveFile('{safe_output}');"
        "$p=New-Object System.Speech.Synthesis.PromptBuilder($culture);"
        f"$p.AppendText('{safe_text}');"
        "$s.Speak($p);"
        "$s.Dispose();"
    )
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-EncodedCommand", _encoded_command(script)],
            capture_output=True, timeout=20, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return (completed.returncode == 0 and _cached(output_path))
    except Exception:
        return False


def _probe_zh_voice() -> bool:
    script = (
        "Add-Type -AssemblyName System.Speech;"
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        "$v=$s.GetInstalledVoices()|ForEach-Object{$_.VoiceInfo.Culture.Name};"
        "$s.Dispose();"
        "if($v -contains 'zh-CN'){Write-Output 'YES'}else{Write-Output 'NO'}"
    )
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-EncodedCommand", _encoded_command(script)],
            capture_output=True, timeout=15, check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return b"YES" in (completed.stdout or b"")
    except Exception:
        return False


def get_tts_health() -> Dict[str, Any]:
    """编辑器健康检查：zh_voice=None 表示尚未探测 (首次调用会同步探测)。"""
    global _zh_voice_available
    if _zh_voice_available is None:
        with _zh_voice_lock:
            if _zh_voice_available is None:
                _zh_voice_available = _probe_zh_voice()
    return {"zh_voice": bool(_zh_voice_available), "enabled": _tts_enabled,
            "volume": get_tts_volume()}


def peek_tts_health() -> Dict[str, Any]:
    """非阻塞版健康检查：zh_voice 未探测时返回 None 并触发后台探测。"""
    global _zh_voice_available
    if _zh_voice_available is None:
        def _bg():
            global _zh_voice_available
            with _zh_voice_lock:
                if _zh_voice_available is None:
                    _zh_voice_available = _probe_zh_voice()
        threading.Thread(target=_bg, daemon=True,
                         name="sao-tts-health-probe").start()
    return {"zh_voice": _zh_voice_available, "enabled": _tts_enabled,
            "volume": get_tts_volume()}


def _ensure_synth_thread():
    global _synth_thread
    if _synth_thread is not None and _synth_thread.is_alive():
        return
    with _synth_thread_lock:
        if _synth_thread is not None and _synth_thread.is_alive():
            return
        t = threading.Thread(target=_synth_loop, name="sao-tts-synth", daemon=True)
        t.start()
        _synth_thread = t


def _synth_loop():
    while True:
        try:
            item = _synth_queue.get()
        except Exception:
            return
        if item is None:
            return
        text, path, on_done = item
        try:
            ok = _cached(path) or _synthesize(text, path)
        except Exception:
            ok = False
        with _synth_inflight_lock:
            _synth_inflight.discard(path)
        if on_done:
            try:
                on_done(ok)
            except Exception:
                pass


def _enqueue_synth(text: str, path: str,
                   on_done: Optional[Callable[[bool], None]] = None) -> bool:
    with _synth_inflight_lock:
        if path in _synth_inflight:
            return True
        _synth_inflight.add(path)
    _ensure_synth_thread()
    try:
        _synth_queue.put_nowait((text, path, on_done))
        return True
    except queue.Full:
        with _synth_inflight_lock:
            _synth_inflight.discard(path)
        return False


def presynthesize(texts: List[str],
                  on_progress: Optional[Callable[[str, bool], None]] = None):
    """后台预合成一批文案 (档案加载/保存/raid start 时调用)，重复文案去重。"""
    seen = set()
    for text in texts or []:
        text = str(text or "").strip()
        if not text or text in seen:
            continue
        seen.add(text)
        path = _cache_path(text)
        if _cached(path):
            continue
        cb = (lambda ok, t=text: on_progress(t, ok)) if on_progress else None
        _enqueue_synth(text, path, cb)


def _wav_duration_s(path: str) -> float:
    try:
        with wave.open(path, "rb") as wf:
            frames = wf.getnframes()
            rate = wf.getframerate() or 1
            return frames / float(rate)
    except Exception:
        return 2.0


def _play_now(path: str, priority: str, volume: Optional[float]):
    global _current_priority, _current_ends_at
    effective = min(_tts_volume if volume is None else float(volume),
                    sao_sound.get_sound_volume() / 100.0)
    dur = _wav_duration_s(path)
    with _state_lock:
        _current_priority = priority
        _current_ends_at = time.time() + dur
    sao_sound.play_tts_file(path, effective)
    threading.Timer(dur + 0.05, _drain_pending).start()


def _drain_pending():
    item = None
    with _state_lock:
        global _current_priority
        if time.time() >= _current_ends_at:
            _current_priority = ""
        while _pending:
            text, volume, expires_at = _pending.popleft()
            if time.time() <= expires_at:
                item = (text, volume)
                break
    if item:
        speak_text(item[0], priority="normal", volume=item[1])


def speak_text(text: str, priority: str = "normal",
               volume: Optional[float] = None) -> str:
    """非阻塞播报。返回 'played' | 'queued' | 'synthesizing' | 'disabled' | 'error'。

    high 打断一切；normal 不打断正在播的 high (排队 ≤2 条、过期即弃)。
    """
    text = str(text or "").strip()
    if not text:
        return "error"
    if not _tts_enabled or not sao_sound.get_sound_enabled():
        return "disabled"
    path = _cache_path(text)
    if _cached(path):
        with _state_lock:
            busy_high = (_current_priority == "high"
                         and time.time() < _current_ends_at)
            if priority != "high" and busy_high:
                _pending.append((text, volume,
                                 time.time() + _STALE_AFTER_S))
                return "queued"
        _play_now(path, priority, volume)
        return "played"
    requested_at = time.time()

    def _after_synth(ok: bool):
        if not ok:
            return
        if time.time() - requested_at > _STALE_AFTER_S:
            return
        speak_text(text, priority=priority, volume=volume)

    if _enqueue_synth(text, path, _after_synth):
        return "synthesizing"
    return "error"


def collect_profile_tts_texts(profile: Dict[str, Any]) -> List[str]:
    """收集一个 boss raid 档案需要预合成的全部 TTS 文案 (机制 + 狂暴里程碑)。"""
    texts: List[str] = []
    if not isinstance(profile, dict):
        return texts
    for mech in profile.get("mechanics") or []:
        if not isinstance(mech, dict):
            continue
        alert = mech.get("alert") or {}
        texts.append(str(alert.get("tts_text") or mech.get("name") or "").strip())
    enrage = profile.get("enrage") if isinstance(profile.get("enrage"), dict) else {}
    for m in enrage.get("tts_milestones") or []:
        try:
            texts.append(f"距离狂暴还有{int(m)}秒")
        except Exception:
            continue
    return [t for t in texts if t]
