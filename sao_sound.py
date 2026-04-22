# -*- coding: utf-8 -*-
"""
SAO 音效管理器 + 升级特效
使用 winsound (Windows) 或 pygame 播放 SAO-UI 原版音效
升级用经典 8-bit 音效 (procedurally generated via winsound.Beep)
"""

import os
import threading
import math
import time
import tempfile
import subprocess
import base64
import wave
from array import array
import sys
import tkinter as tk
from typing import Optional
from config import FONTS_DIR, SOUNDS_DIR

_SOUNDS = SOUNDS_DIR
_FONTS = FONTS_DIR

# ═══════════════════════════════════════════════
#  SAO 音效
# ═══════════════════════════════════════════════

# 预定义音效路径
SAO_SOUNDS = {
    'click':       os.path.join(_SOUNDS, 'Feedback.SAO.Click.mp3'),
    'menu_open':   os.path.join(_SOUNDS, 'Popup.SAO.Launcher.mp3'),
    'menu_close':  os.path.join(_SOUNDS, 'Dismiss.SAO.Launcher.mp3'),
    'panel':       os.path.join(_SOUNDS, 'Popup.SAO.Panel.mp3'),
    'submenu':     os.path.join(_SOUNDS, 'Popup.SAO.Menu.mp3'),
    'alert':       os.path.join(_SOUNDS, 'Popup.SAO.Alert.mp3'),
    'alert_close': os.path.join(_SOUNDS, 'Dismiss.SAO.Message.mp3'),
    'welcome':     os.path.join(_SOUNDS, 'Popup.SAO.Welcome.mp3'),
    'alo_welcome': os.path.join(_SOUNDS, 'Popup.ALO.Welcome.mp3'),
    'link_start':  os.path.join(_SOUNDS, 'LinkStart.SAO.Kirito.mp3'),
    'nervegear':   os.path.join(_SOUNDS, 'Startup.SAO.NerveGear.mp3'),
    'burst_ready': os.path.join(_SOUNDS, 'Popup.SAO.Alert.mp3'),  # Burst Mode Ready SFX
    'boss_alert':  os.path.join(_SOUNDS, 'Popup.SAO.Alert.mp3'),  # Boss timeline alert
    'boss_phase':  os.path.join(_SOUNDS, 'Popup.SAO.Panel.mp3'),  # Boss phase transition
}

# ═══ Global sound settings ═══
_sound_enabled = True
_sound_volume = 0.7   # 0.0 ~ 1.0

# 缓存 pygame 状态
_pygame_inited = False
_has_pygame = None
_burst_tts_lock = threading.Lock()
_burst_tts_ready = None
_burst_tts_fail = False

# ─── Single dedicated player thread ──────────────────────────────
# pygame.mixer is NOT thread-safe: concurrent Sound() construction
# and play() from multiple daemon threads (one per click) randomly
# corrupts mixer state and triggers `PyEval_RestoreThread NULL
# tstate` crashes when the user rapid-clicks SAO menu / panel
# buttons. Funnel everything through one worker thread + a small
# queue, and cache the Sound objects so we don't re-read the file
# on every click.
import queue as _queue_mod
_play_queue: '_queue_mod.Queue' = _queue_mod.Queue(maxsize=32)
_play_thread = None
_play_thread_lock = threading.Lock()
_sound_cache_lock = threading.Lock()
_sound_cache: dict = {}
_winsound_fallback_lock = threading.Lock()


def _ensure_player_thread():
    global _play_thread
    if _play_thread is not None and _play_thread.is_alive():
        return
    with _play_thread_lock:
        if _play_thread is not None and _play_thread.is_alive():
            return
        t = threading.Thread(target=_player_loop, name='sao-sound-player',
                             daemon=True)
        t.start()
        _play_thread = t


def _player_loop():
    while True:
        try:
            item = _play_queue.get()
        except Exception:
            return
        if item is None:
            return
        path, volume = item
        try:
            _play_one_sync(path, volume)
        except Exception:
            pass


def _play_one_sync(path: str, volume: float) -> None:
    if _init_pygame():
        try:
            import pygame
            with _sound_cache_lock:
                snd = _sound_cache.get(path)
                if snd is None:
                    snd = pygame.mixer.Sound(path)
                    _sound_cache[path] = snd
            snd.set_volume(float(volume))
            snd.play()
            return
        except Exception:
            pass
    # 回退: winsound (仅支持 .wav, mp3 不支持)
    try:
        import winsound
        if path.lower().endswith('.wav'):
            with _winsound_fallback_lock:
                winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC)
    except Exception:
        pass

_BURST_TTS_TEXT = 'Burst Mode Ready'
_BURST_TTS_DIR = os.path.join(tempfile.gettempdir(), 'sao_auto_tts')
_BURST_TTS_RAW = os.path.join(_BURST_TTS_DIR, 'burst_ready_raw.wav')
_BURST_TTS_PROC = os.path.join(_BURST_TTS_DIR, 'burst_ready_scifi.wav')


def _init_pygame():
    """懒初始化 pygame mixer"""
    global _pygame_inited, _has_pygame
    if _has_pygame is not None:
        return _has_pygame
    try:
        import pygame
        pygame.mixer.init(frequency=44100, size=-16, channels=2, buffer=1024)
        _pygame_inited = True
        _has_pygame = True
    except Exception:
        _has_pygame = False
    return _has_pygame


def set_sound_enabled(enabled: bool):
    """Set global sound enabled/disabled."""
    global _sound_enabled
    _sound_enabled = bool(enabled)


def set_sound_volume(volume_pct: int):
    """Set global volume (0-100)."""
    global _sound_volume
    _sound_volume = max(0.0, min(1.0, volume_pct / 100.0))


def get_sound_enabled() -> bool:
    return _sound_enabled


def get_sound_volume() -> int:
    return int(round(_sound_volume * 100))


def _build_powershell_encoded(script: str) -> str:
    return base64.b64encode(script.encode('utf-16le')).decode('ascii')


def _synthesize_tts_to_wav(text: str, output_path: str) -> bool:
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    safe_output = output_path.replace("'", "''")
    safe_text = text.replace("'", "''")
    script = (
        "Add-Type -AssemblyName System.Speech;"
        "$culture=[System.Globalization.CultureInfo]::GetCultureInfo('en-US');"
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        "try{$s.SelectVoiceByHints([System.Speech.Synthesis.VoiceGender]::Female,"
        "[System.Speech.Synthesis.VoiceAge]::Adult,0,$culture)}catch{};"
        "$s.Rate=-2;"
        "$s.Volume=100;"
        f"$s.SetOutputToWaveFile('{safe_output}');"
        "$p=New-Object System.Speech.Synthesis.PromptBuilder($culture);"
        f"$p.AppendText('{safe_text}');"
        "$s.Speak($p);"
        "$s.Dispose();"
    )
    try:
        encoded = _build_powershell_encoded(script)
        completed = subprocess.run(
            [
                'powershell',
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-EncodedCommand', encoded,
            ],
            capture_output=True,
            timeout=20,
            check=False,
        )
        return completed.returncode == 0 and os.path.exists(output_path) and os.path.getsize(output_path) > 1024
    except Exception:
        return False


def _electrify_wave(input_path: str, output_path: str) -> bool:
    try:
        with wave.open(input_path, 'rb') as src:
            channels = src.getnchannels()
            sample_width = src.getsampwidth()
            frame_rate = src.getframerate()
            frame_count = src.getnframes()
            raw = src.readframes(frame_count)

        if sample_width != 2 or frame_count <= 0:
            return False

        samples = array('h')
        samples.frombytes(raw)
        total = len(samples)
        if total <= 0:
            return False

        bitcrush_step = 96
        downsample = 3
        echo_a = int(frame_rate * 0.045) * max(1, channels)
        echo_b = int(frame_rate * 0.095) * max(1, channels)
        out = array('h', [0]) * total

        for idx in range(total):
            base_idx = idx - (idx % downsample)
            crushed = int(round(samples[base_idx] / bitcrush_step)) * bitcrush_step
            t = (idx // max(1, channels)) / float(max(1, frame_rate))
            mod = 0.62 + 0.38 * math.sin(2.0 * math.pi * 27.0 * t)
            value = crushed * mod
            if idx >= echo_a:
                value += out[idx - echo_a] * 0.28
            if idx >= echo_b:
                value += out[idx - echo_b] * 0.18
            value = samples[idx] * 0.20 + value * 0.86
            out[idx] = max(-32768, min(32767, int(value)))

        with wave.open(output_path, 'wb') as dst:
            dst.setnchannels(channels)
            dst.setsampwidth(sample_width)
            dst.setframerate(frame_rate)
            dst.writeframes(out.tobytes())
        return True
    except Exception:
        return False


def _ensure_burst_ready_tts() -> Optional[str]:
    global _burst_tts_ready, _burst_tts_fail
    if _burst_tts_ready and os.path.exists(_burst_tts_ready):
        return _burst_tts_ready
    if _burst_tts_fail:
        return None

    with _burst_tts_lock:
        if _burst_tts_ready and os.path.exists(_burst_tts_ready):
            return _burst_tts_ready
        os.makedirs(_BURST_TTS_DIR, exist_ok=True)
        if os.path.exists(_BURST_TTS_PROC) and os.path.getsize(_BURST_TTS_PROC) > 1024:
            _burst_tts_ready = _BURST_TTS_PROC
            return _burst_tts_ready
        ok = _synthesize_tts_to_wav(_BURST_TTS_TEXT, _BURST_TTS_RAW)
        if ok:
            ok = _electrify_wave(_BURST_TTS_RAW, _BURST_TTS_PROC)
        if ok and os.path.exists(_BURST_TTS_PROC):
            _burst_tts_ready = _BURST_TTS_PROC
            return _burst_tts_ready
        _burst_tts_fail = True
        return None


def play_sound(name: str, volume: float = 0.7):
    """
    播放 SAO 音效 (非阻塞)
    
    Args:
        name: 音效名 (见 SAO_SOUNDS)
        volume: 0.0~1.0 音量 (会被全局音量和开关覆盖)
    """
    if not _sound_enabled:
        return
    path = SAO_SOUNDS.get(name, '')
    if name == 'burst_ready':
        path = _ensure_burst_ready_tts() or path
    if not path or not os.path.exists(path):
        return
    # Apply global volume
    effective_volume = min(volume, _sound_volume)
    _ensure_player_thread()
    try:
        _play_queue.put_nowait((path, effective_volume))
    except _queue_mod.Full:
        # Queue saturated under storms of clicks: drop the request
        # rather than block the caller (we are usually on the Tk
        # main thread or a GLFW after_idle deferred handler).
        pass


# ═══════════════════════════════════════════════
#  8-bit 升级音效 (using winsound.Beep)
# ═══════════════════════════════════════════════

def play_levelup_sfx():
    """播放经典 8-bit 升级音效 (winsound.Beep 合成)"""
    def _beep_sequence():
        try:
            import winsound
            # 经典升级旋律: C5 → E5 → G5 → C6 (快速上行)
            notes = [
                (523, 80),   # C5
                (587, 60),   # D5
                (659, 80),   # E5
                (784, 60),   # G5
                (880, 80),   # A5
                (1047, 120), # C6
                (1319, 150), # E6
                (1568, 200), # G6 (hold)
            ]
            for freq, dur in notes:
                winsound.Beep(freq, dur)
        except Exception:
            pass

    threading.Thread(target=_beep_sequence, daemon=True).start()


# ═══════════════════════════════════════════════
#  升级特效 (全屏闪光 + 文字)
# ═══════════════════════════════════════════════

class LevelUpEffect:
    """
    SAO 风格升级特效
    - 全屏金色闪光 (flash)  
    - "LEVEL UP!" 文字从中心放大弹出
    - 等级数字显示
    - 经典 8-bit 升级音效
    """

    @staticmethod
    def show(root: tk.Tk, old_level: int, new_level: int):
        """显示升级特效"""
        # 播放升级音效
        play_levelup_sfx()

        ov = tk.Toplevel(root)
        ov.overrideredirect(True)
        ov.attributes('-topmost', True)
        ov.attributes('-alpha', 0.0)

        sw = root.winfo_screenwidth()
        sh = root.winfo_screenheight()
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#000000')

        # 使用 Canvas 绘制
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#000000',
                       highlightthickness=0)
        cv.pack()

        # 文字元素 (初始隐藏)
        cx, cy = sw // 2, sh // 2
        
        # "LEVEL UP!" 大字
        title_id = cv.create_text(cx, cy - 40, text='LEVEL UP!',
                                   font=('Segoe UI', 1, 'bold'),
                                   fill='#f3af12')
        # 等级数字
        level_id = cv.create_text(cx, cy + 40, text=f'Lv.{new_level}',
                                   font=('Segoe UI', 1),
                                   fill='#ffffff')

        # 动画
        t0 = time.time()
        total_dur = 2.5  # 总时长

        def _tick():
            if not ov.winfo_exists():
                return
            elapsed = time.time() - t0
            t = elapsed / total_dur

            if t >= 1.0:
                ov.destroy()
                return

            # 阶段 1: 金色闪光 (0~0.3)
            if t < 0.15:
                # 快速闪入
                alpha = min(0.85, t / 0.15 * 0.85)
                ov.attributes('-alpha', alpha)
                # 背景从黑到金色
                g = int(t / 0.15 * 243)
                b_val = int(t / 0.15 * 175)
                cv.configure(bg=f'#{g:02x}{b_val:02x}12')
            elif t < 0.3:
                # 闪白
                ov.attributes('-alpha', 0.85)
                wt = (t - 0.15) / 0.15
                gv = int(255 - 12 * wt)
                cv.configure(bg=f'#{gv:02x}{gv:02x}{gv:02x}')

            # 阶段 2: 文字放大弹入 (0.2~0.6)
            if 0.2 <= t < 0.6:
                text_t = (t - 0.2) / 0.4
                # ease-out elastic
                et = 1 - (1 - text_t) ** 3
                size = max(1, int(48 * et))
                cv.itemconfigure(title_id, font=('Segoe UI', size, 'bold'))
                lv_size = max(1, int(28 * et))
                cv.itemconfigure(level_id, font=('Segoe UI', lv_size))
                # 背景渐暗为半透明黑
                bg_alpha = 0.85 - 0.5 * text_t
                ov.attributes('-alpha', max(0.35, bg_alpha))
                cv.configure(bg='#1a1a1a')

            # 阶段 3: 保持 (0.6~0.8)
            elif 0.6 <= t < 0.8:
                ov.attributes('-alpha', 0.35)

            # 阶段 4: 淡出 (0.8~1.0)
            elif t >= 0.8:
                fade_t = (t - 0.8) / 0.2
                ov.attributes('-alpha', 0.35 * (1 - fade_t))

            ov.after(16, _tick)

        ov.after(50, _tick)


# ═══════════════════════════════════════════════
#  SAO 字体加载
# ═══════════════════════════════════════════════

_fonts_loaded = False

def load_sao_fonts():
    """加载 SAO-UI 字体 (Windows: AddFontResourceExW)"""
    global _fonts_loaded
    if _fonts_loaded:
        return
    _fonts_loaded = True
    
    try:
        import ctypes
        # 使用 0 而非 FR_PRIVATE 以让 tkinter 可见
        for fname in ['SAOUI.ttf', 'ZhuZiAYuanJWD.ttf']:
            fpath = os.path.join(_FONTS, fname)
            if os.path.exists(fpath):
                ctypes.windll.gdi32.AddFontResourceExW(fpath, 0, 0)
    except Exception:
        pass


def _font_available(name):
    """检查字体是否在 tkinter 可用"""
    try:
        import tkinter.font as tkfont
        return name in tkfont.families()
    except Exception:
        return False


def get_sao_font(size: int = 12, bold: bool = False):
    """获取 SAO 字体族名 (回退到 Segoe UI)"""
    load_sao_fonts()
    family = 'SAO UI' if _font_available('SAO UI') else (
             'SAOUI' if _font_available('SAOUI') else 'Segoe UI')
    weight = 'bold' if bold else ''
    return (family, size, weight) if weight else (family, size)


def get_cjk_font(size: int = 10, bold: bool = False):
    """获取中文圆体字体 (回退到 Microsoft YaHei UI)"""
    load_sao_fonts()
    for name in ['方正FW筑紫A圆 简 D', 'ZhuZiAYuanJWD', 'Microsoft YaHei UI']:
        if _font_available(name):
            family = name
            break
    else:
        family = 'Microsoft YaHei UI'
    weight = 'bold' if bold else ''
    return (family, size, weight) if weight else (family, size)
