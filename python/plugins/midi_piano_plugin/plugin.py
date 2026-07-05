# -*- coding: utf-8 -*-
# MIDI 钢琴演奏家
#
# 职责：引导外置依赖(mido) → 装配引擎(mp_player/mp_api) → 注册主面板 + 多个子面板
# (钢琴键盘/音符卷帘/MIDI分析/设置) + 可改键热键 → 把按钮动作翻译成引擎调用
# → 原生资源管理器选曲 + 前台门控 + 配置持久化。
#
# 真正的弹琴逻辑全在 engine/mp_*.py。本文件不含演奏算法，
# 只做编排与展示。子面板既注册为平台独立面板（在“插件面板”里可单独唤出），
# 主面板顶部也带「视图切换」按钮可就地唤出。

from __future__ import annotations

import math
import os
import threading

_ctx = None
_plugin_dir = os.path.dirname(os.path.abspath(__file__))

_boot = None
_boot_record = None
_mpp = None      # mp_player 模块
_api = None      # mp_api 模块
_dlg = None      # mp_dialog 模块
_player = None   # MidiPlayer 实例
_audition = None # MidiAudition 实例（本机试听）

_orig_press = None
_locator = None

PANEL_ID = "midi_piano"
# 子面板：各自独立注册，主面板顶部的按钮点一下各弹一个独立窗口（ctx.open_window）。
SUBPANELS = [
    ("midi_kbd", "🎹 钢琴键盘", "kbd"),
    ("midi_roll", "🎼 音符卷帘", "roll"),
    ("midi_analysis", "📊 MIDI 分析", "analysis"),
    ("midi_settings", "⚙ 设置", "settings"),
]
# 每个面板自己声明的窗口大小（host 据此开独立窗口；未声明的面板用 host 默认）。
PANEL_SIZE = {
    PANEL_ID: (660, 860),
    "midi_kbd": (860, 460),       # 钢琴键盘可视化：宽
    "midi_roll": (760, 680),      # 音符卷帘可视化：高
    "midi_analysis": (500, 620),
    "midi_settings": (480, 620),
}

_S = {
    "file": "", "file_name": "", "lib": [], "page": 0,
    "deps": "", "channels": [], "redraw_token": None, "last_err": "",
    "view": "main",       # 主面板当前视图: main|kbd|roll|analysis|settings
    "roll_page": 0,       # 音符卷帘分页
    "dialog_open": False, # 防重复弹对话框
}


# ── 设置 ────────────────────────────────────────────────────────────────────
def _defaults():
    return {
        "auto_detect": True, "require_foreground": True, "legato_overlap": False,
        "page_size": 8, "extra_midi_dir": "", "last_file": "",
        "speed": 1.0, "mode_system": "auto", "transpose": 0,
    }


def _get(key):
    d = _defaults()
    return _ctx.get_setting(key, d.get(key)) if _ctx else d.get(key)


# ── 前台门控 ────────────────────────────────────────────────────────────────
def _game_in_foreground() -> bool:
    global _locator
    try:
        import ctypes
        hwnd = ctypes.windll.user32.GetForegroundWindow()
        if not hwnd:
            return False
        from utils.window_locator import WindowLocator
        if _locator is None:
            _locator = WindowLocator()
        found = _locator.find_target_window()
        return bool(found and int(found[0]) == int(hwnd))
    except Exception:
        return True


def _install_foreground_gate():
    global _orig_press
    if _mpp is None or _orig_press is not None:
        return
    kb = _mpp.keyboard
    _orig_press = kb.press

    def _gated_press(name):
        if _get("require_foreground") and not _game_in_foreground():
            return False
        return _orig_press(name)

    kb.press = _gated_press


def _restore_foreground_gate():
    global _orig_press
    if _mpp is not None and _orig_press is not None:
        try:
            _mpp.keyboard.press = _orig_press
        except Exception:
            pass
    _orig_press = None


# ── 曲库 ────────────────────────────────────────────────────────────────────
def _midi_dirs():
    dirs = [os.path.join(_plugin_dir, "assets", "midi")]
    extra = (_get("extra_midi_dir") or "").strip()
    if extra:
        dirs.append(extra)
    return dirs


def _refresh_library():
    _S["lib"] = _api.list_midi_files(_midi_dirs())
    ps = max(1, int(_get("page_size") or 8))
    npages = max(1, math.ceil(len(_S["lib"]) / ps))
    _S["page"] = max(0, min(_S["page"], npages - 1))


def _derive_channels():
    chans = {}
    parser = _player.parser if _player else None
    for n in getattr(parser, "notes", []) or []:
        chans[n.channel] = chans.get(n.channel, 0) + 1
    names = getattr(parser, "track_names", []) or []
    out = []
    for ch in sorted(chans):
        label = ""
        if 0 <= ch < len(names) and names[ch]:
            label = str(names[ch])[:18]
        out.append((ch, chans[ch], label or f"通道 {ch}"))
    _S["channels"] = out


# ── 载入曲目 ────────────────────────────────────────────────────────────────
def _load_file(path):
    if not path or not os.path.isfile(path):
        _S["last_err"] = "文件不存在"
        return False
    try:
        if _player.state.is_playing:
            _player.stop()
        ok = _player.load_midi(path)
    except Exception as exc:
        _S["last_err"] = f"载入失败: {exc}"
        return False
    if not ok:
        _S["last_err"] = "解析失败"
        return False
    _S["last_err"] = ""
    _S["file"] = path
    _S["file_name"] = os.path.basename(path)
    _S["roll_page"] = 0
    if _get("auto_detect"):
        _api.autodetect_mode_system(_player, apply=True)
    else:
        mode = _get("mode_system")
        if mode in ("classic", "extended"):
            _api.set_mode_system(_player, mode)
    try:
        _player.set_legato_overlap(bool(_get("legato_overlap")), save=False)
    except Exception:
        pass
    try:
        # 恢复用户手动移调（在自动八度之上的微调），保证重启/换曲后参数不丢。
        _player.set_transpose(int(_clampf(int(_get("transpose") or 0), -36, 36)))
    except Exception:
        pass
    _derive_channels()
    if _ctx:
        _ctx.set_setting("last_file", path)
    return True


# ── 原生文件对话框 ──────────────────────────────────────────────────────────
def _pick_midi_async():
    if _S.get("dialog_open"):
        return
    _S["dialog_open"] = True
    _S["last_err"] = "正在打开资源管理器…"

    def _work():
        try:
            init = ""
            if _S.get("file"):
                init = os.path.dirname(_S["file"])
            if not init:
                init = os.path.join(_plugin_dir, "assets", "midi")
            hwnd = _dlg.foreground_hwnd() if _dlg else 0
            path = _dlg.open_midi(initial_dir=init, hwnd_owner=hwnd) if _dlg else None
            if path:
                # 把所选文件所在目录并入曲库扫描，方便看同目录其它曲子
                d = os.path.dirname(path)
                if d and os.path.isdir(d):
                    _ctx.set_setting("extra_midi_dir", d)
                _load_file(path)
                _refresh_library()
                _S["view"] = "main"
            else:
                _S["last_err"] = ""
        except Exception as exc:
            _S["last_err"] = f"打开对话框失败: {exc}"
        finally:
            _S["dialog_open"] = False
            if _ctx:
                _ctx.request_redraw(PANEL_ID)

    threading.Thread(target=_work, daemon=True, name="mp_filedialog").start()


# ── 渲染：公共片段 ──────────────────────────────────────────────────────────
def _switcher(active):
    ui = _ctx.ui
    # 主控就地显示；其余按钮各弹一个独立窗口（每个子面板自己一个窗口）。
    # 用 win_ 前缀，避免和 open_file(资源管理器选曲) 撞前缀。
    btns = [ui.button("• 主控", "view_main", style=("primary" if active == "main" else "ghost"))]
    for (_pid, title, v) in SUBPANELS:
        btns.append(ui.button("⤢ " + title, f"win_{v}", style="ghost"))
    return ui.row(btns)


def _names(midi_notes):
    if not _player:
        return "-"
    nn = _player.mapper.note_to_name
    return " ".join(nn(n) for n in (midi_notes or [])[:4]) or "-"


# ── 渲染：主控 body ─────────────────────────────────────────────────────────
def _body_main():
    ui = _ctx.ui
    st = _api.build_status(_player) if _player else {}
    phase = st.get("phase", "stopped")
    badge = {"playing": ("● 播放中", "ok"), "paused": ("⏸ 暂停", "warn"),
             "stopped": ("■ 已停止", "muted")}.get(phase, ("■ 已停止", "muted"))
    classic = st.get("mode_system", "classic") == "classic"
    speed = float(st.get("speed", 1.0) or 1.0)
    user_t = int(st.get("user_transpose", 0) or 0)
    oct_off = int(st.get("octave_offset", 0) or 0)
    legato = bool(st.get("legato", False))
    fgate = bool(_get("require_foreground"))
    out = []

    out.append(ui.section("状态", [
        ui.row([ui.badge(badge[0], badge[1]), ui.kv("曲目", _S.get("file_name") or "—")]),
        ui.bar("进度", pct=st.get("pct", 0.0), color="cyan",
               caption=f"{_api.fmt_time(st.get('current', 0))} / {_api.fmt_time(st.get('total', 0))}"),
        ui.row([ui.kv("BPM", st.get("bpm", 0)), ui.kv("音符", st.get("notes", 0)),
                ui.kv("覆盖率", f"{st.get('coverage', 0):.0f}%")]),
        ui.row([ui.kv("键位", "60键 (CTRL/SHIFT)" if classic else "88键 (</>)"),
                ui.kv("自动八度", f"{oct_off:+d}"), ui.kv("手动移调", f"{user_t:+d}")]),
    ], accent="gold"))

    out.append(ui.section("控制", [
        ui.row([ui.button("▶ / ⏸  播放/暂停", "play_pause", style="primary"),
                ui.button("■ 停止", "stop", style="danger")]),
        ui.row([ui.button("速度 −", "speed_down"), ui.kv("速度", f"{speed:.2f}x"),
                ui.button("速度 +", "speed_up")]),
        ui.row([ui.button("移调 −", "transpose_down"), ui.kv("移调", f"{user_t:+d} 半音"),
                ui.button("移调 +", "transpose_up"), ui.button("自动移调", "transpose_auto")]),
        ui.row([ui.button(("● 60键" if classic else "60键"), "mode_classic",
                          style=("primary" if classic else "default")),
                ui.button(("● 88键" if not classic else "88键"), "mode_extended",
                          style=("primary" if not classic else "default")),
                ui.button("自动识别", "mode_auto", style="ghost")]),
        ui.row([ui.button(f"连音: {'开' if legato else '关'}", "toggle_legato"),
                ui.button(f"前台门控: {'开' if fgate else '关'}", "toggle_foreground_gate")]),
        ui.row([ui.button("🔊 本机试听", "preview_play", style="ghost"),
                ui.button("⏹ 停止试听", "preview_stop", style="ghost"),
                ui.badge(_audition.backend_name() if _audition else "试听不可用", "muted")]),
    ], accent="cyan"))

    out.append(_section_library())
    ch_sec = _section_channels()
    if ch_sec is not None:
        out.append(ch_sec)
    return out


def _section_library():
    ui = _ctx.ui
    lib = _S.get("lib", [])
    ps = max(1, int(_get("page_size") or 8))
    npages = max(1, math.ceil(len(lib) / ps))
    page = max(0, min(_S.get("page", 0), npages - 1))
    items = lib[page * ps:(page + 1) * ps]
    kids = [
        ui.row([ui.button("📂 打开资源管理器选 MIDI…", "open_file", style="primary"),
                ui.button("刷新", "lib_refresh", style="ghost")]),
        ui.row([ui.button("◀ 上一页", "lib_prev"), ui.kv("曲库", f"{len(lib)}首·{page + 1}/{npages}"),
                ui.button("下一页 ▶", "lib_next")]),
    ]
    if not items:
        kids.append(ui.text("点上方按钮选曲，或把 .mid 放进 assets/midi 后“刷新”", style="muted"))
    for name, path in items:
        sel = (path == _S.get("file"))
        kids.append(ui.button(("▶ " + name) if sel else ("♪ " + name), "select_file",
                              payload={"path": path}, style=("primary" if sel else "default")))
    return ui.section("曲库", kids, accent="cyan")


def _section_channels():
    ui = _ctx.ui
    chans = _S.get("channels", [])
    if len(chans) <= 1 or not _player:
        return None
    mapper = _player.mapper
    kids = []
    for ch, count, label in chans[:16]:
        enabled = mapper.is_channel_enabled(ch)
        ctr = mapper.channel_transpose.get(ch, 0)
        kids.append(ui.row([
            ui.kv(label, f"{count}音 / {ctr:+d}"),
            ui.button(("启用" if enabled else "禁用"), "channel_toggle",
                      payload={"ch": ch}, style=("default" if enabled else "ghost")),
            ui.button("八度 −", "channel_oct_down", payload={"ch": ch}),
            ui.button("八度 +", "channel_oct_up", payload={"ch": ch}),
        ]))
    return ui.section("分通道", kids, accent="cyan")


# ── 可视化（钢琴键盘 + 音符卷帘） ───────────────────────────────────────────
_WHITE_PC = {0, 2, 4, 5, 7, 9, 11}


def _is_white(m):
    return (m % 12) in _WHITE_PC


def _heat(shade, dark=False):
    # 使用频次热力色：白键 浅灰→金；黑键 深灰→金。
    a = (27, 31, 36) if dark else (243, 245, 247)
    b = (222, 166, 32)
    s = max(0.0, min(1.0, float(shade)))
    c = tuple(int(a[i] + (b[i] - a[i]) * s) for i in range(3))
    return "#%02x%02x%02x" % c


def _playing_notes(events, ct):
    out = set()
    for e in events:
        if e.time <= ct <= e.time + e.duration:
            for m in e.midi_notes:
                out.add(m)
    return out


def _kbd_canvas(target_w=810, h=150):
    # 画一架钢琴：白/黑键，按使用频次着色，正在演奏的键高亮。
    ui = _ctx.ui
    p = _player.parser
    pitches = [n.note for n in (p.notes or [])]
    if not pitches:
        return None
    lo = max(21, (min(pitches) // 12) * 12)
    hi = min(108, ((max(pitches) // 12) + 1) * 12 - 1)
    usage = {}
    for m in pitches:
        if lo <= m <= hi:
            usage[m] = usage.get(m, 0) + 1
    mxu = max(usage.values()) if usage else 1
    st = _player.state
    playing = _playing_notes(p.get_play_events() or [], st.current_time) \
        if (st.is_playing and not st.is_paused) else set()
    nn = _player.mapper.note_to_name
    whites = [m for m in range(lo, hi + 1) if _is_white(m)]
    nW = max(1, len(whites))
    ww = max(10, min(26, target_w // nW))
    W = ww * nW
    bw = max(6, int(ww * 0.62))
    bh = int(h * 0.62)
    xof = {}
    ops = []
    for i, m in enumerate(whites):
        x = i * ww
        xof[m] = x
        fill = "accent" if m in playing else (_heat(usage[m] / mxu) if usage.get(m) else "white")
        ops.append(ui.rect(x, 0, ww, h, fill=fill, outline="border", width=1))
        if m % 12 == 0:
            ops.append(ui.ctext(x + ww // 2, h - 3, nn(m), fill="label", size=8, anchor="s"))
    for m in range(lo, hi + 1):
        if _is_white(m) or (m - 1) not in xof:
            continue
        x = xof[m - 1] + ww - bw // 2
        fill = "accent" if m in playing else (_heat(usage[m] / mxu, dark=True) if usage.get(m) else "black")
        ops.append(ui.rect(x, 0, bw, bh, fill=fill, outline="black", width=1))
    return ui.canvas(W, h, ops, bg="body")


def _roll_canvas(target_w=700, h=430):
    # 音符卷帘：横=时间(随播放滚动)，纵=音高，红线=播放头。
    ui = _ctx.ui
    p = _player.parser
    events = p.get_play_events() or []
    if not events:
        return None
    pitches = [m for e in events for m in e.midi_notes] or [60]
    lo, hi = min(pitches) - 1, max(pitches) + 1
    span = max(1, hi - lo)
    nn = _player.mapper.note_to_name
    st = _player.state
    ct = st.current_time if (st.is_playing or st.is_paused) else 0.0
    win = 8.0
    t0 = max(0.0, ct - 2.0)

    def xo(t):
        return int((t - t0) / win * target_w)

    def yo(m):
        return int(h - (m - lo) / span * h)

    ops = []
    for m in range(lo, hi + 1):
        if m % 12 == 0:
            y = yo(m)
            ops.append(ui.line(0, y, target_w, y, fill="sep", width=1))
            ops.append(ui.ctext(2, y - 1, nn(m), fill="label", size=8, anchor="w"))
    t1 = t0 + win
    for e in events:
        if e.time > t1 or (e.time + e.duration) < t0:
            continue
        x = xo(e.time)
        w = max(2, xo(e.time + e.duration) - x)
        live = e.time <= ct <= e.time + e.duration
        fill = "accent" if live else ("gold" if e.is_chord else "cyan")
        for m in e.midi_notes:
            if lo <= m <= hi:
                ops.append(ui.rect(x, yo(m) - 3, w, 6, fill=fill, outline=""))
    px = xo(ct)
    ops.append(ui.line(px, 0, px, h, fill="bad", width=2))
    return ui.canvas(target_w, h, ops, bg="body")


# ── 渲染：子面板 body ───────────────────────────────────────────────────────
def _body_keyboard():
    ui = _ctx.ui
    p = _player.parser if _player else None
    notes = [n for n in getattr(p, "notes", []) or []]
    if not notes:
        return [ui.text("未载入曲目。请在“主控/曲库”里选一首。", "muted")]
    mapper = _player.mapper
    nn = mapper.note_to_name
    remap = getattr(_player, "_note_remap", {}) or {}
    ut = getattr(_player, "_user_transpose", 0)
    low_keys = set("zxcvbnm12345")
    mid_keys = set("asdfghj67890")
    buckets = {"low": 0, "mid": 0, "high": 0, "out": 0}
    for n in notes:
        mnote = remap.get(n.note, n.note) + ut
        key = mapper.map_note(mnote)
        if key is None:
            buckets["out"] += 1
        elif key in low_keys:
            buckets["low"] += 1
        elif key in mid_keys:
            buckets["mid"] += 1
        else:
            buckets["high"] += 1
    mx = max(1, buckets["low"], buckets["mid"], buckets["high"])
    classic = mapper.mode_system == "classic"
    lo, hi = min(n.note for n in notes), max(n.note for n in notes)
    cov = _api.coverage_pct(_player)
    kb = _kbd_canvas()
    head = []
    if kb is not None:
        head = [ui.text("钢琴键盘（金=使用频次，亮蓝=正在演奏）", "label"), kb, ui.divider()]
    return head + [
        ui.row([ui.kv("键位", "60键 (CTRL/SHIFT)" if classic else "88键 (</>)"),
                ui.kv("音域", f"{nn(lo)} – {nn(hi)}")]),
        ui.bar("覆盖率", pct=cov / 100.0, color=("ok" if cov >= 95 else "warn"),
               caption=f"{cov:.0f}%"),
        ui.text("物理键位使用分布（映射后）", "label"),
        ui.bar("低音区 Z-M / 1-5", pct=buckets["low"] / mx, color="cyan", caption=str(buckets["low"])),
        ui.bar("中音区 A-J / 6-0", pct=buckets["mid"] / mx, color="gold", caption=str(buckets["mid"])),
        ui.bar("高音区 Q-U / IOP[]", pct=buckets["high"] / mx, color="accent", caption=str(buckets["high"])),
        ui.kv("超范围/未映射", str(buckets["out"])),
        ui.text("（CTRL/SHIFT 或 </> 模式键自动切换八度；超范围音符做八度折叠）", "muted"),
    ]


def _body_roll():
    ui = _ctx.ui
    if not _player or not getattr(_player.parser, "notes", None):
        return [ui.text("未载入曲目。", "muted")]
    events = _player.parser.get_play_events() or []
    per = 14
    npages = max(1, math.ceil(len(events) / per))
    page = max(0, min(_S.get("roll_page", 0), npages - 1))
    rows = []
    for e in events[page * per:(page + 1) * per]:
        rows.append({
            "t": _api.fmt_time(e.time),
            "note": _names(e.midi_notes),
            "key": (e.key or "-").upper(),
            "dur": f"{e.duration:.2f}s",
            "kind": "和弦" if e.is_chord else ("滑奏" if e.is_glissando else "音符"),
        })
    roll = _roll_canvas()
    head = []
    if roll is not None:
        head = [ui.text("音符卷帘（随播放滚动；红线=播放头，亮蓝=正在演奏，金=和弦）", "label"),
                roll, ui.divider()]
    return head + [
        ui.text(f"播放事件时间轴（共 {len(events)} 个）", "label"),
        ui.table(columns=[
            {"key": "t", "title": "时间", "align": "right"},
            {"key": "note", "title": "音符"},
            {"key": "key", "title": "键", "align": "center"},
            {"key": "dur", "title": "时长", "align": "right"},
            {"key": "kind", "title": "类型", "align": "center"},
        ], rows=rows),
        ui.row([ui.button("◀ 上一页", "roll_prev"), ui.kv("页", f"{page + 1}/{npages}"),
                ui.button("下一页 ▶", "roll_next")]),
    ]


def _body_analysis():
    ui = _ctx.ui
    if not _player or not getattr(_player.parser, "notes", None):
        return [ui.text("未载入曲目。", "muted")]
    p = _player.parser
    nn = _player.mapper.note_to_name
    ksig = "—"
    if getattr(p, "key_signatures", None):
        try:
            _t, k, mode = p.key_signatures[0]
            ksig = f"{nn((k % 12) + 60)[:-1]} {mode}"
        except Exception:
            ksig = "—"
    info = _api.autodetect_mode_system(_player, apply=False)
    nchan = len({n.channel for n in p.notes})
    return [
        ui.section("基本", [
            ui.row([ui.kv("BPM", f"{getattr(p, 'bpm', 0):.0f}"),
                    ui.kv("时长", _api.fmt_time(getattr(p, "total_time", 0)))]),
            ui.row([ui.kv("总音符", len(p.notes)), ui.kv("通道数", nchan),
                    ui.kv("踏板事件", len(getattr(p, "sustain_events", []) or []))]),
            ui.kv("Tempo 变化", len(getattr(p, "tempo_changes", []) or [])),
        ], accent="gold"),
        ui.section("音部", [
            ui.row([ui.kv("主旋律", len(getattr(p, "melody_notes", []) or [])),
                    ui.kv("低音部", len(getattr(p, "bass_notes", []) or []))]),
            ui.row([ui.kv("分割点", nn(getattr(p, "pitch_split_point", 60))),
                    ui.kv("调号", ksig)]),
        ], accent="cyan"),
        ui.section("可弹性", [
            ui.kv("当前模式", "60键 classic" if _player.get_mode_system() == "classic" else "88键 extended"),
            ui.bar("60键 覆盖率", pct=info["classic_cov"], color="cyan",
                   caption=f"{info['classic_cov']*100:.0f}%"),
            ui.bar("88键 覆盖率", pct=info["ext_cov"], color="accent",
                   caption=f"{info['ext_cov']*100:.0f}%"),
            ui.kv("自动八度", f"{getattr(_player, '_octave_offset', 0) * 12:+d}"),
            ui.text("（“自动识别”按覆盖率在 60/88 间择优，并算最佳移调）", "muted"),
        ], accent="accent"),
    ]


def _body_settings():
    ui = _ctx.ui
    auto = bool(_get("auto_detect"))
    fgate = bool(_get("require_foreground"))
    legato = bool(_player.get_legato_overlap()) if _player else bool(_get("legato_overlap"))
    ps = int(_get("page_size") or 8)
    extra = (_get("extra_midi_dir") or "").strip() or "—"
    return [
        ui.section("文件", [
            ui.row([ui.button("📂 打开资源管理器选 MIDI…", "open_file", style="primary")]),
            ui.kv("额外曲库目录", extra),
            ui.text("选文件后会把其所在目录并入曲库扫描。", "muted"),
        ], accent="gold"),
        ui.section("识别 / 演奏", [
            ui.row([ui.button(f"自动识别(60/88+移调): {'开' if auto else '关'}", "toggle_auto_detect")]),
            ui.row([ui.button(f"前台门控: {'开' if fgate else '关'}", "toggle_foreground_gate")]),
            ui.row([ui.button(f"连音重叠: {'开' if legato else '关'}", "toggle_legato")]),
            ui.row([ui.button("每页 −", "page_size_down"), ui.kv("曲库每页", str(ps)),
                    ui.button("每页 +", "page_size_up")]),
        ], accent="cyan"),
        ui.section("热键 / 信息", [
            ui.kv("播放/暂停", "默认 CTRL+F8（可在键位编辑器改键）"),
            ui.kv("停止", "默认 CTRL+F10（可在键位编辑器改键）"),
            ui.kv("依赖", _S.get("deps") or "—"),
            ui.text("试听用 WinMCI/pygame（本机扬声器，不驱动游戏）。", "muted"),
        ], accent="accent"),
    ]


_VIEW_BODY = {
    "main": _body_main, "kbd": _body_keyboard, "roll": _body_roll,
    "analysis": _body_analysis, "settings": _body_settings,
}


def _render(_payload=None):
    # 主面板：顶部视图切换 + 当前视图 body。
    ui = _ctx.ui
    view = _S.get("view", "main")
    children = [_switcher(view)]
    if view != "main":
        children.append(ui.button("← 返回主控", "view_main", style="ghost"))
    body_fn = _VIEW_BODY.get(view, _body_main)
    try:
        children.extend(body_fn())
    except Exception as exc:
        children.append(ui.text(f"渲染出错: {exc}", "bad"))
    if _S.get("last_err"):
        children.append(ui.text("⚠ " + _S["last_err"], "bad"))
    return ui.panel("MIDI 钢琴演奏家", children)


def _make_subpanel_render(view):
    # 子面板独立注册时的渲染（只画该子面板 body，不含切换器）。
    def _r(_payload=None):
        ui = _ctx.ui
        title = next((t for (_pid, t, v) in SUBPANELS if v == view), "MIDI")
        kids = []
        try:
            kids.extend(_VIEW_BODY[view]())
        except Exception as exc:
            kids.append(ui.text(f"渲染出错: {exc}", "bad"))
        return ui.panel(title, kids)
    return _r


# ── 动作 ────────────────────────────────────────────────────────────────────
def _clampf(v, lo, hi):
    return max(lo, min(hi, v))


def _on_action(action_id, payload=None):
    if _player is None:
        return _render()
    p = payload or {}
    try:
        # 子面板各弹一个独立窗口（win_ 前缀，勿与 open_file 撞）
        if action_id.startswith("win_"):
            view = action_id[len("win_"):]
            sub_pid = next((pid for (pid, _t, v) in SUBPANELS if v == view), "")
            if sub_pid:
                _ctx.open_window(sub_pid)
            return _render()
        # 视图切换（主控就地；保留以兼容）
        if action_id in ("view_main", "view_kbd", "view_roll", "view_analysis", "view_settings"):
            _S["view"] = action_id[len("view_"):]
        elif action_id == "play_pause":
            stt = _player.state
            if stt.is_playing and not stt.is_paused:
                _player.pause()
            elif not _S.get("file"):
                _S["last_err"] = "请先在曲库中选择一首曲目"
            else:
                _player.play()
        elif action_id == "stop":
            _player.stop()
        elif action_id == "speed_up":
            _player.set_speed(round(_clampf(_player.state.speed + 0.05, 0.3, 2.0), 2))
            _ctx.set_setting("speed", _player.state.speed)
        elif action_id == "speed_down":
            _player.set_speed(round(_clampf(_player.state.speed - 0.05, 0.3, 2.0), 2))
            _ctx.set_setting("speed", _player.state.speed)
        elif action_id == "transpose_up":
            _player.set_transpose(int(_clampf(_player._user_transpose + 1, -36, 36)))
            _ctx.set_setting("transpose", _player._user_transpose)
        elif action_id == "transpose_down":
            _player.set_transpose(int(_clampf(_player._user_transpose - 1, -36, 36)))
            _ctx.set_setting("transpose", _player._user_transpose)
        elif action_id == "transpose_auto":
            _player._user_transpose = 0
            if getattr(_player.parser, "notes", None):
                _player._analyze_and_setup_mapping()
            _ctx.set_setting("transpose", 0)
        elif action_id == "mode_classic":
            _api.set_mode_system(_player, "classic"); _ctx.set_setting("mode_system", "classic")
        elif action_id == "mode_extended":
            _api.set_mode_system(_player, "extended"); _ctx.set_setting("mode_system", "extended")
        elif action_id == "mode_auto":
            info = _api.autodetect_mode_system(_player, apply=True)
            _ctx.set_setting("mode_system", "auto")
            _ctx.log(f"[auto] 选 {info['system']} (60键{info['classic_cov']*100:.0f}% / 88键{info['ext_cov']*100:.0f}%)")
        elif action_id == "toggle_legato":
            new = not _player.get_legato_overlap()
            _player.set_legato_overlap(new, save=False); _ctx.set_setting("legato_overlap", new)
        elif action_id == "toggle_foreground_gate":
            _ctx.set_setting("require_foreground", not _get("require_foreground"))
        elif action_id == "toggle_auto_detect":
            _ctx.set_setting("auto_detect", not _get("auto_detect"))
        elif action_id == "page_size_up":
            _ctx.set_setting("page_size", int(_clampf(int(_get("page_size") or 8) + 2, 4, 20)))
            _refresh_library()
        elif action_id == "page_size_down":
            _ctx.set_setting("page_size", int(_clampf(int(_get("page_size") or 8) - 2, 4, 20)))
            _refresh_library()
        elif action_id == "lib_refresh":
            _refresh_library()
        elif action_id == "lib_prev":
            _S["page"] = max(0, _S.get("page", 0) - 1)
        elif action_id == "lib_next":
            _S["page"] = _S.get("page", 0) + 1
        elif action_id == "roll_prev":
            _S["roll_page"] = max(0, _S.get("roll_page", 0) - 1)
        elif action_id == "roll_next":
            _S["roll_page"] = _S.get("roll_page", 0) + 1
        elif action_id == "select_file":
            _load_file(p.get("path", ""))
        elif action_id == "open_file":
            _pick_midi_async()
        elif action_id == "channel_toggle":
            ch = int(p.get("ch", -1))
            if ch >= 0:
                _player.mapper.set_channel_enabled(ch, not _player.mapper.is_channel_enabled(ch))
        elif action_id == "channel_oct_up":
            ch = int(p.get("ch", -1))
            if ch >= 0:
                _player.mapper.set_channel_transpose(ch, _player.mapper.channel_transpose.get(ch, 0) + 12)
        elif action_id == "channel_oct_down":
            ch = int(p.get("ch", -1))
            if ch >= 0:
                _player.mapper.set_channel_transpose(ch, _player.mapper.channel_transpose.get(ch, 0) - 12)
        elif action_id == "preview_play":
            if _audition and _S.get("file"):
                _audition.play(_S["file"])
            elif not _S.get("file"):
                _S["last_err"] = "请先选择一首曲目再试听"
        elif action_id == "preview_stop":
            if _audition:
                _audition.stop()
    except Exception as exc:
        _S["last_err"] = f"{action_id}: {exc}"
        if _ctx:
            _ctx.log(_S["last_err"])
    # 动作可能影响所有面板，统一请求重绘
    if _ctx:
        _ctx.request_redraw()
    return _render()


# ── 热键 ────────────────────────────────────────────────────────────────────
def _hk_play_pause():
    _on_action("play_pause")


def _hk_stop():
    _on_action("stop")


# ── 进度刷新 ────────────────────────────────────────────────────────────────
def _tick_redraw():
    # 播放中实时刷新主控(进度条) + 可视化窗口(钢琴键盘 / 音符卷帘)。渲染器是**原地更新**
    # (不销毁控件)，所以主控进度条平滑走动、按钮始终可点、不闪烁。
    try:
        if _player and _player.state.is_playing and not _player.state.is_paused:
            _ctx.request_redraw("midi_piano")
            _ctx.request_redraw("midi_kbd")
            _ctx.request_redraw("midi_roll")
    except Exception:
        pass


# ── 生命周期 ────────────────────────────────────────────────────────────────
def on_load(ctx):
    global _ctx, _boot, _boot_record, _mpp, _api, _dlg, _player, _audition
    _ctx = ctx
    ctx.set_defaults(_defaults())

    _boot = ctx.load_local("bootstrap.py")
    _boot_record = _boot.ensure_requirements(_plugin_dir, ctx)
    _S["deps"] = ", ".join(f"{k}={v}" for k, v in (_boot_record.get("deps") or {}).items()) or "—"

    import importlib
    _mpp = importlib.import_module("mp_player")
    _api = importlib.import_module("mp_api")
    try:
        _dlg = importlib.import_module("mp_dialog")
    except Exception as exc:
        _dlg = None
        ctx.log(f"[dialog] 文件对话框不可用: {exc}")
    _player = _mpp.MidiPlayer()
    try:
        _audition = importlib.import_module("mp_audio").MidiAudition()
    except Exception as exc:
        _audition = None
        ctx.log(f"[audio] 试听后端不可用: {exc}")

    try:
        _player.set_speed(float(_get("speed") or 1.0))
        _player.set_legato_overlap(bool(_get("legato_overlap")), save=False)
    except Exception:
        pass

    _install_foreground_gate()
    _player.on_playback_end = lambda: ctx.request_redraw()

    _refresh_library()
    last = _get("last_file")
    if last and os.path.isfile(last):
        _load_file(last)

    # 主面板 + 子面板（每个面板自带窗口大小；子面板在 host“插件面板”里可单独弹窗）
    _mw, _mh = PANEL_SIZE[PANEL_ID]
    ctx.register_ui_panel(PANEL_ID,
                          {"title": "MIDI 钢琴", "description": "把 MIDI 自动弹成游戏电子琴 (60/88键自动识别)",
                           "width": _mw, "height": _mh, "primary": True},
                          render=_render, on_action=_on_action)
    for pid, title, view in SUBPANELS:
        _w, _h = PANEL_SIZE.get(pid, (460, 620))
        # hidden=True：不在「插件面板」列表里单列，只由主面板按钮呼出独立窗口。
        ctx.register_ui_panel(pid, {"title": title, "description": f"MIDI 子面板：{title}",
                                    "width": _w, "height": _h, "hidden": True},
                              render=_make_subpanel_render(view), on_action=_on_action)

    # CTRL+ 组合: 纯 F8/F10 已被主 UI 的 boss_raid_next_phase / hide_panels 占用。
    ctx.register_hotkey("play_pause", _hk_play_pause, default_key="CTRL+F8", label="MIDI 播放/暂停")
    ctx.register_hotkey("stop", _hk_stop, default_key="CTRL+F10", label="MIDI 停止")
    _S["redraw_token"] = ctx.set_interval(_tick_redraw, 0.3)

    ctx.log(f"midi_piano_plugin 已加载; deps={_S['deps']}; 曲库={len(_S['lib'])}首; 子面板={len(SUBPANELS)}")


def on_enable():
    if _ctx:
        _ctx.request_redraw()


def on_disable():
    if _player:
        try:
            _player.stop()
        except Exception:
            pass
    if _audition:
        try:
            _audition.stop()
        except Exception:
            pass


def on_unload():
    if _player:
        try:
            _player.stop()
        except Exception:
            pass
    if _audition:
        try:
            _audition.stop()
        except Exception:
            pass
    _restore_foreground_gate()
    if _boot and _boot_record:
        try:
            _boot.restore_paths(_boot_record)
        except Exception:
            pass
