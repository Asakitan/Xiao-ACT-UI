# -*- coding: utf-8 -*-
"""Read-only memory-access facade for the plugin platform and the Mem Scope panel.

Exposes every memory-scan-readable resource through one stable, hybrid-gated API
plus an asynchronous manual value search. Borrows the readers the live
``MemStateBridge`` already built on its background threads, so it never opens a
second process handle and never blocks the UI thread for the heavy first scans.

Invariants:
  - READ ONLY. The process handle is ``PROCESS_VM_READ`` only (see
    ``mem_probe.process.StarProcess``); no write path is imported or reachable.
  - Every public method returns a JSON-safe dict and never raises. When memory is
    inactive it returns ``{"ok": False, "reason": <code>, "hint": <human text>}``.
  - 64-bit addresses are emitted as hex strings and uuids/large words as strings
    so JS Number precision (2**53) can never corrupt them.

Live engine reachability (resolved fresh on every call, mode can flip at runtime):
  owner._packet_engine._mem_source._bridge   (hybrid / auto / memory-with-TCP)
  owner._mem_bridge                          (memory-only without TCP)
from the bridge:
  ._entity_provider._pm / ._damage_reader.pm  -> shared StarProcess (already open)
  ._entity_provider._ecr                       -> EntityCombatReader
  ._provider._src.sr.pm                        -> force-open path (worker thread only)
"""
from __future__ import annotations

import math
import threading
import time
from typing import Any, Optional, Sequence

# Heap pointer plausibility (matches the readers / cy_memscan bounds).
_PTR_LO = 0x10000
_PTR_HI = 0x7FFFFFFFFFFF
_CLASS_NAME_OFF = 0x10          # Il2CppClass.name pointer offset
_JS_SAFE = (1 << 53) - 1

_VALID_DTYPES = ("i32", "u32", "i64", "u64", "f32", "f64", "utf16")
_INT_DTYPES = ("i32", "u32", "i64", "u64")


# ───────────────────────── small helpers ─────────────────────────

def _plaus(addr: Any) -> bool:
    try:
        return _PTR_LO <= int(addr) <= _PTR_HI
    except Exception:
        return False


def _num(v: Any) -> Any:
    """JSON-safe number: stringify ints that exceed JS 2**53 precision."""
    if isinstance(v, bool):
        return v
    if isinstance(v, int) and abs(v) > _JS_SAFE:
        return str(v)
    return v


def _hex(addr: Any) -> str:
    try:
        return hex(int(addr) & 0xFFFFFFFFFFFFFFFF)
    except Exception:
        return "0x0"


def _printable(text: str) -> bool:
    if not text:
        return False
    good = sum(1 for ch in text if 0x20 <= ord(ch) < 0x7F or ord(ch) > 0xA0)
    return good >= max(2, int(len(text) * 0.7))


def _err(reason: str, hint: str = "") -> dict:
    return {"ok": False, "reason": reason, "hint": hint}


# ───────────────────────── engine resolution ─────────────────────────

def resolve_bridge(owner: Any) -> Optional[Any]:
    """Return the live ``MemStateBridge`` wherever it currently lives, else None."""
    if owner is None:
        return None
    pe = getattr(owner, "_packet_engine", None) or getattr(owner, "_packet_bridge", None)
    if pe is not None:
        ms = getattr(pe, "_mem_source", None)            # UnifiedDataSource | None
        br = getattr(ms, "_bridge", None) if ms is not None else None
        if br is not None:
            return br
    return getattr(owner, "_mem_bridge", None)


def _bridge_kind(owner: Any, bridge: Any) -> str:
    if bridge is None:
        return "none"
    pe = getattr(owner, "_packet_engine", None) or getattr(owner, "_packet_bridge", None)
    ms = getattr(pe, "_mem_source", None) if pe is not None else None
    if ms is not None and getattr(ms, "_bridge", None) is bridge:
        return "hybrid"
    if getattr(owner, "_mem_bridge", None) is bridge:
        return "memory_only"
    return "unknown"


def resolve_pm(bridge: Any) -> Optional[Any]:
    """Non-blocking StarProcess: reuse a handle the background loop already opened."""
    if bridge is None:
        return None
    ep = getattr(bridge, "_entity_provider", None)
    pm = getattr(ep, "_pm", None) if ep is not None else None
    if pm is not None:
        return pm
    dr = getattr(bridge, "_damage_reader", None)
    pm = getattr(dr, "pm", None) if dr is not None else None
    if pm is not None:
        return pm
    prov = getattr(bridge, "_provider", None)
    src = getattr(prov, "_src", None) if prov is not None else None
    sr = getattr(src, "_sr", None) if src is not None else None   # private: no lazy open
    return getattr(sr, "pm", None) if sr is not None else None


def resolve_pm_blocking(bridge: Any) -> Optional[Any]:
    """StarProcess, forcing the resolver open if needed. Worker-thread only."""
    pm = resolve_pm(bridge)
    if pm is not None:
        return pm
    try:
        prov = getattr(bridge, "_provider", None)
        src = getattr(prov, "_src", None) if prov is not None else None
        if src is not None:
            sr = src.sr                                   # may open the process / block
            return getattr(sr, "pm", None)
    except Exception:
        return None
    return None


# ───────────────────────── address hint decoder ─────────────────────────

def _ga_base(pm: Any, modules) -> int:
    for m in modules or ():
        if str(getattr(m, "name", "")).lower() == "gameassembly.dll":
            return int(getattr(m, "base", 0) or 0)
    return 0


def _maybe_klass_name(pm: Any, klass_ptr: int, ga_base: int, ga_size: int) -> str:
    if not (ga_base and ga_size and ga_base <= klass_ptr < ga_base + ga_size):
        return ""
    try:
        np = pm.read_u64(klass_ptr + _CLASS_NAME_OFF)
        if not _plaus(np):
            return ""
        name = pm.read_cstr(np, 48)
        return name if name and _printable(name) else ""
    except Exception:
        return ""


def _locate(addr: int, modules) -> dict:
    for m in modules or ():
        base = int(getattr(m, "base", 0) or 0)
        size = int(getattr(m, "size", 0) or 0)
        if base and base <= addr < base + size:
            return {"module": getattr(m, "name", ""), "offset": _hex(addr - base)}
    return {"region": "heap"}


def decode_hint(pm: Any, addr: int, modules=None, *, ga: tuple = (0, 0)) -> dict:
    """Decode one address many ways and locate it. Never raises. JSON-safe."""
    out: dict = {"addr": _hex(addr)}
    if not _plaus(addr):
        out["valid"] = False
        return out
    vals: dict = {}
    try:
        u32 = pm.read_u32(addr)
        if u32 is not None:
            vals["u32"] = u32
            vals["i32"] = u32 - 0x100000000 if u32 >= 0x80000000 else u32
        u64 = pm.read_u64(addr)
        if u64 is not None:
            vals["u64"] = _num(u64)
            i64 = u64 - 0x10000000000000000 if u64 >= 0x8000000000000000 else u64
            vals["i64"] = _num(i64)
            if _plaus(u64):
                vals["ptr"] = _hex(u64)
        f = pm.read_f32(addr)
        if f is not None and math.isfinite(f) and abs(f) < 1e12 and (f == 0.0 or abs(f) > 1e-9):
            vals["f32"] = round(f, 4)
        w = pm.read_utf16(addr, 32)
        if w and len(w) >= 2 and _printable(w):
            vals["utf16"] = w
        c = pm.read_cstr(addr, 48)
        if c and len(c) >= 2 and _printable(c):
            vals["cstr"] = c
    except Exception:
        pass
    out["as"] = vals
    out["in"] = _locate(addr, modules)
    try:
        ga_base, ga_size = int(ga[0] or 0), int(ga[1] or 0)
        u64 = vals.get("u64")
        cand = int(u64) if isinstance(u64, int) else (int(u64) if isinstance(u64, str) and u64.isdigit() else 0)
        kn = _maybe_klass_name(pm, cand, ga_base, ga_size) if cand else ""
        if kn:
            out["klass_hint"] = kn
    except Exception:
        pass
    return out


# ───────────────────────── async search jobs ─────────────────────────

class _SearchJob:
    __slots__ = ("job_id", "dtype", "align", "state", "progress", "hits", "count",
                 "error", "created_at", "updated_at", "last_value", "_stop", "_thread")

    def __init__(self, job_id: str, dtype: str, align: int, now: float):
        self.job_id = job_id
        self.dtype = dtype
        self.align = align
        self.state = "running"
        self.progress = 0.0
        self.hits: list = []
        self.count = 0
        self.error = ""
        self.created_at = now
        self.updated_at = now
        self.last_value: Any = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None


class MemSearchManager:
    """Background value-scan / narrow jobs over the live process. Never blocks UI.

    One per owner (stashed as ``owner._mem_search_mgr``) so the per-call facade and
    a cached ``ctx.mem`` share one job registry. The shared StarProcess handle is
    read-only and ReadProcessMemory is thread-safe per call, so the worker reads
    concurrently with the entity loop without a lock (it never calls the heavy
    snapshot path that ``_sr_lock`` guards)."""

    MAX_CONCURRENT = 2
    MAX_TOTAL_HITS = 200_000
    RETURN_CAP = 200
    JOB_TTL_S = 300.0
    MAX_REGION_SIZE = 256 * 1024 * 1024

    def __init__(self, owner: Any):
        self._owner = owner
        self._jobs: dict[str, _SearchJob] = {}
        self._lock = threading.Lock()
        self._seq = 0

    # ---- public ----
    def search(self, value, dtype: str, *, align: int = 0) -> dict:
        dtype = str(dtype or "").lower()
        if dtype not in _VALID_DTYPES:
            return _err("bad_arg", f"未知类型 {dtype!r}; 可用: {','.join(_VALID_DTYPES)}")
        coerced = self._coerce(value, dtype)
        if coerced is None:
            return _err("bad_arg", f"值 {value!r} 无法编码为 {dtype}")
        bridge = resolve_bridge(self._owner)
        if bridge is None:
            return _err("no_bridge", "内存引擎未就绪")
        self._reap()
        with self._lock:
            running = sum(1 for j in self._jobs.values() if j.state == "running")
            if running >= self.MAX_CONCURRENT:
                return _err("busy", f"已有 {running} 个搜索在跑 (上限 {self.MAX_CONCURRENT})")
            self._seq += 1
            job_id = f"s{self._seq}"
            job = _SearchJob(job_id, dtype, int(align or 0), time.time())
            job.last_value = coerced
            self._jobs[job_id] = job
        t = threading.Thread(target=self._run_search, args=(job, bridge, coerced),
                             name=f"mem-search-{job_id}", daemon=True)
        job._thread = t
        t.start()
        return {"ok": True, "job_id": job_id, "state": "running"}

    def narrow(self, job_id: str, value) -> dict:
        with self._lock:
            job = self._jobs.get(str(job_id or ""))
        if job is None:
            return _err("not_found", "搜索任务不存在或已过期")
        if job.state == "running":
            return _err("busy", "上一轮搜索还在进行")
        coerced = self._coerce(value, job.dtype)
        if coerced is None:
            return _err("bad_arg", f"值 {value!r} 无法编码为 {job.dtype}")
        bridge = resolve_bridge(self._owner)
        if bridge is None:
            return _err("no_bridge", "内存引擎未就绪")
        job.state = "running"
        job.error = ""
        job.last_value = coerced
        job._stop.clear()
        t = threading.Thread(target=self._run_narrow, args=(job, bridge, coerced),
                             name=f"mem-narrow-{job_id}", daemon=True)
        job._thread = t
        t.start()
        return {"ok": True, "job_id": job_id, "state": "running"}

    def status(self, job_id: str) -> dict:
        with self._lock:
            job = self._jobs.get(str(job_id or ""))
        if job is None:
            return _err("not_found", "搜索任务不存在或已过期")
        job.updated_at = time.time()
        out = {
            "ok": True,
            "job_id": job.job_id,
            "state": job.state,
            "done": job.state in ("done", "error"),
            "progress": round(float(job.progress), 3),
            "dtype": job.dtype,
            "count": int(job.count),
            "truncated": int(job.count) > self.RETURN_CAP,
            "error": job.error,
        }
        if job.state in ("done", "error") and job.hits:
            bridge = resolve_bridge(self._owner)
            pm = resolve_pm(bridge)
            if pm is not None:
                try:
                    modules = pm.list_modules()
                except Exception:
                    modules = []
                ga = (_ga_base(pm, modules), 0)
                if ga[0]:
                    for m in modules:
                        if int(getattr(m, "base", 0) or 0) == ga[0]:
                            ga = (ga[0], int(getattr(m, "size", 0) or 0))
                            break
                out["results"] = [decode_hint(pm, a, modules, ga=ga)
                                  for a in job.hits[:self.RETURN_CAP]]
            else:
                out["results"] = [{"addr": _hex(a)} for a in job.hits[:self.RETURN_CAP]]
        else:
            out["results"] = []
        return out

    def cancel(self, job_id: str) -> dict:
        with self._lock:
            job = self._jobs.get(str(job_id or ""))
        if job is None:
            return _err("not_found", "搜索任务不存在或已过期")
        job._stop.set()
        if job.state == "running":
            job.state = "cancelled"
        return {"ok": True, "job_id": job_id, "state": job.state}

    def list_jobs(self) -> dict:
        self._reap()
        now = time.time()
        with self._lock:
            jobs = [{"job_id": j.job_id, "state": j.state, "dtype": j.dtype,
                     "count": int(j.count), "age_s": round(now - j.created_at, 1)}
                    for j in self._jobs.values()]
        return {"ok": True, "jobs": jobs}

    # ---- internals ----
    @staticmethod
    def _coerce(value, dtype: str):
        from mem_probe import scanner
        try:
            if dtype == "utf16":
                v = str(value)
            elif dtype in ("f32", "f64"):
                v = float(value)
            else:
                v = int(str(value).strip(), 0) if isinstance(value, str) else int(value)
            scanner.encode_value(v, dtype)         # validate encodable
            return v
        except Exception:
            return None

    def _run_search(self, job: _SearchJob, bridge: Any, value) -> None:
        pm = resolve_pm_blocking(bridge)
        if pm is None:
            job.state = "error"
            job.error = "not_armed"
            return
        try:
            hits = self._scan(pm, value, job)
            if job._stop.is_set():
                job.state = "cancelled"
            else:
                job.hits = hits
                job.count = len(hits)
                job.progress = 1.0
                job.state = "done"
        except Exception as exc:
            job.state = "error"
            job.error = f"process_gone: {exc}"

    def _run_narrow(self, job: _SearchJob, bridge: Any, value) -> None:
        pm = resolve_pm_blocking(bridge)
        if pm is None:
            job.state = "error"
            job.error = "not_armed"
            return
        try:
            from mem_probe import scanner
            hits = scanner.narrow(pm, job.hits, value, job.dtype)
            job.hits = hits
            job.count = len(hits)
            job.progress = 1.0
            job.state = "done"
        except Exception as exc:
            job.state = "error"
            job.error = f"process_gone: {exc}"

    def _scan(self, pm: Any, value, job: _SearchJob) -> list:
        """Region-by-region scan with real progress + cooperative cancel."""
        from mem_probe import scanner
        from mem_probe import cy_memscan as _cy
        dtype = job.dtype
        needle = scanner.encode_value(value, dtype)
        align = job.align or scanner._default_align(dtype)
        use_cy = (isinstance(value, int) and dtype in _INT_DTYPES
                  and align == scanner._default_align(dtype))
        width = 8 if dtype in ("i64", "u64") else 4
        if use_cy:
            v = value & (0xFFFFFFFFFFFFFFFF if width == 8 else 0xFFFFFFFF)
            find_fn = _cy.find_aligned_u64 if width == 8 else _cy.find_aligned_u32
        regions = list(pm.iter_regions())
        total = len(regions) or 1
        hits: list = []
        for i, region in enumerate(regions):
            if job._stop.is_set():
                break
            job.progress = i / total
            if region.size > self.MAX_REGION_SIZE:
                continue
            buf = pm.read_bytes(region.base, region.size)
            if buf is None:
                continue
            remaining = self.MAX_TOTAL_HITS - len(hits)
            if remaining <= 0:
                break
            if use_cy:
                for off in find_fn(buf, v, max_hits=remaining):
                    hits.append(region.base + off)
            else:
                for off in scanner._find_all_in_chunk(buf, needle, align=align):
                    hits.append(region.base + off)
                    if len(hits) >= self.MAX_TOTAL_HITS:
                        break
            if len(hits) >= self.MAX_TOTAL_HITS:
                break
        return hits

    def _reap(self) -> None:
        now = time.time()
        with self._lock:
            dead = [jid for jid, j in self._jobs.items()
                    if j.state != "running" and (now - j.updated_at) > self.JOB_TTL_S]
            for jid in dead:
                self._jobs.pop(jid, None)


def get_search_manager(owner: Any) -> MemSearchManager:
    mgr = getattr(owner, "_mem_search_mgr", None)
    if not isinstance(mgr, MemSearchManager):
        mgr = MemSearchManager(owner)
        try:
            setattr(owner, "_mem_search_mgr", mgr)
        except Exception:
            pass
    return mgr


# ───────────────────────── the facade ─────────────────────────

class MemAccess:
    """Read-only, hybrid-gated facade over every memory-scan-readable resource."""

    def __init__(self, owner: Any):
        self._owner = owner

    # ---- resolution / gating ----
    def _mode(self) -> str:
        owner = self._owner
        fn = getattr(owner, "_get_mem_data_source", None)
        if callable(fn):
            try:
                m = str(fn() or "tcp").strip().lower()
                if m in ("tcp", "memory", "hybrid", "auto"):
                    return m
            except Exception:
                pass
        get = getattr(owner, "_get_setting", None)
        if callable(get):
            try:
                m = str(get("mem_data_source", "tcp") or "tcp").strip().lower()
                if m in ("tcp", "memory", "hybrid", "auto"):
                    return m
            except Exception:
                pass
        return "tcp"

    def _bridge(self) -> Optional[Any]:
        return resolve_bridge(self._owner)

    def _gate(self) -> Optional[dict]:
        if self._mode() == "tcp":
            return _err("mode_tcp", "数据源为 TCP-only；切到 hybrid / auto / memory 模式才能调用内存引擎")
        if self._bridge() is None:
            return _err("no_bridge", "内存引擎尚未就绪（未在 hybrid/memory 模式启动或仍在预热）")
        return None

    # ---- status / discovery (not gated: they report the gate state) ----
    def status(self) -> dict:
        mode = self._mode()
        bridge = self._bridge()
        pm = resolve_pm(bridge) if bridge is not None else None
        armed = pm is not None
        process = ""
        module_base = ""
        if armed:
            try:
                process = f"{pm.name}#{pm.pid}"
            except Exception:
                process = ""
            try:
                for m in pm.list_modules():
                    if str(getattr(m, "name", "")).lower() == "gameassembly.dll":
                        module_base = _hex(m.base)
                        break
            except Exception:
                module_base = ""
        return {
            "ok": True,
            "mode": mode,
            "active": bool(mode != "tcp" and bridge is not None),
            "bridge": _bridge_kind(self._owner, bridge),
            "armed": armed,
            "provider_mode": str(getattr(bridge, "mode", "") or "") if bridge is not None else "",
            "last_error": str(getattr(bridge, "last_error", "") or "") if bridge is not None else "",
            "process": process,
            "process_attached": armed,
            "module_base": module_base,
            "hint": "" if (mode != "tcp" and bridge is not None)
                    else "切换数据源到 hybrid 模式以启用内存浏览器",
        }

    def catalog(self) -> dict:
        bridge = self._bridge()
        ep = getattr(bridge, "_entity_provider", None) if bridge is not None else None
        dr = getattr(bridge, "_damage_reader", None) if bridge is not None else None
        pm = resolve_pm(bridge) if bridge is not None else None
        cats = [
            ("self_state", "自身状态 Self", "mem_self",
             "UID / HP / 职业 / 名字 / 技能CD数 / 资源 / 死亡 / 等级 / 战力 / 场景。来自缓存 last_* + 快照。",
             bridge is not None, {}),
            ("entities", "实体列表 Entities", "mem_entities",
             "全部可见实体实时 HP+战斗态（含 TCP 未上报的屏外/开怪前目标）：uuid/base_id/kind/cur_hp/max_hp/"
             "hp_pct/breaking_stage/overdrive/stun/cast_skill_id/name。",
             ep is not None, {"include_monsters": True, "max_per_dict": 128}),
            ("boss", "Boss", "mem_boss",
             "当前最高血量实体（bossDict 优先）的 HP 快照。", ep is not None, {}),
            ("damage_totals", "伤害总表 Damage", "mem_damage",
             "游戏自带 DamageDataMgr 的每玩家总伤/总治疗/总承伤（uuid→total）。",
             dr is not None, {"total_type": 1}),
            ("skill_damage", "逐技能伤害 Skill DPS", "mem_skill_damage",
             "某玩家 uuid 的每技能伤害明细（skill_id→value）。", dr is not None, {"uuid": "<uuid>"}),
            ("attr_map", "实体属性表 Attr Map", "mem_attr_map",
             "任意实体地址的全属性 {attr_id: value}（HP/破防/过载/眩晕/灭却/施法技能 等）。",
             ep is not None, {"ent_addr": "<0x..>"}),
            ("resolve_name", "名字解析 Name", "mem_resolve_name",
             "离线表把 id 解析成名字。kind: monster/dungeon/skill/monster_skill。",
             bridge is not None, {"kind": "monster", "id": "<id>"}),
            ("read_at", "读地址 Read", "mem_read",
             "读任意地址并多路解码（u32/i32/u64/f32/utf16/cstr/指针/所在模块）。",
             pm is not None, {"addr": "<0x..>", "dtype": "u64"}),
            ("search", "手动搜索 Search", "mem_search",
             "按值全堆扫描（异步 job），命中地址带多路解码提示；可 narrow 多帧收敛。",
             pm is not None, {"value": "<value>", "dtype": "i32"}),
        ]
        return {
            "ok": True,
            "categories": [
                {"id": cid, "name": name, "action": action, "hint": hint,
                 "available": bool(avail), "example": example}
                for (cid, name, action, hint, avail, example) in cats
            ],
        }

    # ---- cheap synchronous reads ----
    def self_state(self) -> dict:
        gate = self._gate()
        if gate:
            return gate
        b = self._bridge()
        out = {
            "ok": True,
            "uid": str(int(getattr(b, "last_uid", 0) or 0)),
            "hp": int(getattr(b, "last_hp", 0) or 0),
            "max_hp": int(getattr(b, "last_max_hp", 0) or 0),
            "profession_id": int(getattr(b, "last_profession_id", 0) or 0),
            "char_name": str(getattr(b, "last_char_name", "") or ""),
            "skill_cd_count": int(getattr(b, "last_skill_cd_count", 0) or 0),
            "is_dead": bool(getattr(b, "last_is_dead", False)),
            "resources": _json_safe(getattr(b, "last_resources", {}) or {}),
        }
        mx = out["max_hp"]
        out["hp_pct"] = round(out["hp"] / mx, 4) if mx > 0 else 0.0
        try:
            snap = b.snapshot()
        except Exception:
            snap = None
        for key in ("level", "level_base", "season_exp", "fight_point", "scene_map_id",
                    "stamina", "stamina_max", "origin_energy"):
            v = getattr(snap, key, None) if snap is not None else None
            if v is not None:
                out[key] = _num(v)
        return out

    def entities(self, *, include_monsters: bool = True, include_npcs: bool = False,
                 max_per_dict: int = 128) -> dict:
        gate = self._gate()
        if gate:
            return gate
        b = self._bridge()
        ep = getattr(b, "_entity_provider", None)
        rows = None
        cached = getattr(b, "last_entities", None)
        default_args = include_monsters and not include_npcs and max_per_dict == 128
        if default_args and cached:
            rows = cached
        elif ep is not None:
            try:
                rows = ep.snapshot(include_monsters=include_monsters,
                                   include_npcs=include_npcs,
                                   max_per_dict=max(1, min(int(max_per_dict or 128), 256)))
            except Exception as exc:
                return _err("process_gone", f"实体快照失败: {exc}")
        if rows is None:
            if ep is None:
                return _err("not_armed", "实体读取器尚未挂载（hybrid 预热中，约 1-2s）")
            rows = []
        return {"ok": True, "count": len(rows), "entities": [self._ent_row(e) for e in rows]}

    def boss(self) -> dict:
        gate = self._gate()
        if gate:
            return gate
        b = self._bridge()
        boss = getattr(b, "last_boss_mem", None)
        if boss is None:
            ep = getattr(b, "_entity_provider", None)
            if ep is None:
                return _err("not_armed", "实体读取器尚未挂载")
            try:
                boss = ep.boss()
            except Exception as exc:
                return _err("process_gone", f"boss 读取失败: {exc}")
        if not boss:
            return {"ok": True, "boss": None}
        return {"ok": True, "boss": self._ent_row(boss)}

    def damage_totals(self, *, total_type: int = 1) -> dict:
        gate = self._gate()
        if gate:
            return gate
        b = self._bridge()
        dr = getattr(b, "_damage_reader", None)
        if dr is None:
            cached = getattr(b, "last_mem_damage", None)
            if cached:
                return {"ok": True, "total_type": int(total_type),
                        "totals": {str(k): _num(v) for k, v in cached.items()}, "source": "cache"}
            return _err("not_armed", "伤害读取器尚未挂载")
        try:
            totals = dr.read_player_totals(int(total_type)) or {}
        except Exception as exc:
            return _err("process_gone", f"伤害表读取失败: {exc}")
        return {"ok": True, "total_type": int(total_type),
                "totals": {str(int(k)): _num(int(v)) for k, v in totals.items()}, "source": "live"}

    def skill_damage(self, uuid) -> dict:
        gate = self._gate()
        if gate:
            return gate
        try:
            uuid = int(str(uuid).strip(), 0) if isinstance(uuid, str) else int(uuid)
        except Exception:
            return _err("bad_arg", f"uuid 非法: {uuid!r}")
        b = self._bridge()
        dr = getattr(b, "_damage_reader", None)
        if dr is None:
            return _err("not_armed", "伤害读取器尚未挂载")
        try:
            skills = dr.read_player_skill_damage(uuid) or {}
        except Exception as exc:
            return _err("process_gone", f"逐技能伤害读取失败: {exc}")
        return {"ok": True, "uuid": str(uuid),
                "skills": {str(int(k)): _num(int(v)) for k, v in skills.items()}}

    def attr_map(self, ent_addr) -> dict:
        gate = self._gate()
        if gate:
            return gate
        addr = self._coerce_addr(ent_addr)
        if addr is None:
            return _err("bad_arg", f"地址非法: {ent_addr!r}")
        b = self._bridge()
        ep = getattr(b, "_entity_provider", None)
        ecr = getattr(ep, "_ecr", None) if ep is not None else None
        if ecr is None:
            pm = resolve_pm(b)
            if pm is None:
                return _err("not_armed", "属性读取器尚未挂载")
            try:
                from mem_probe.il2cpp.mem_entity_combat import EntityCombatReader
                ecr = EntityCombatReader(pm)
            except Exception as exc:
                return _err("process_gone", f"无法创建属性读取器: {exc}")
        try:
            amap = ecr.read_attr_map(addr) or {}
        except Exception as exc:
            return _err("process_gone", f"属性表读取失败: {exc}")
        return {"ok": True, "ent_addr": _hex(addr),
                "attrs": {str(int(k)): _num(v) for k, v in amap.items()}}

    def resolve_name(self, kind: str, id) -> dict:
        gate = self._gate()
        if gate:
            return gate
        kind = str(kind or "").strip().lower()
        try:
            ident = int(str(id).strip(), 0) if isinstance(id, str) else int(id)
        except Exception:
            return _err("bad_arg", f"id 非法: {id!r}")
        b = self._bridge()
        nr = None
        fn = getattr(b, "_name_resolver", None)
        if callable(fn):
            try:
                nr = fn()
            except Exception:
                nr = None
        if nr is None:
            return _err("not_armed", "名字表未加载")
        name = ""
        try:
            method = getattr(nr, kind, None) or getattr(nr, {"skill": "monster_skill"}.get(kind, ""), None)
            if callable(method):
                name = str(method(ident, default="") or "")
        except Exception:
            name = ""
        return {"ok": True, "kind": kind, "id": _num(ident), "name": name}

    # ---- cheap follow-up raw reads ----
    def read_at(self, addr, dtype: str = "u64") -> dict:
        gate = self._gate()
        if gate:
            return gate
        a = self._coerce_addr(addr)
        if a is None:
            return _err("bad_arg", f"地址非法: {addr!r}")
        pm = resolve_pm(self._bridge())
        if pm is None:
            return _err("not_armed", "进程句柄尚未就绪")
        try:
            modules = pm.list_modules()
        except Exception:
            modules = []
        ga = _ga_base(pm, modules)
        ga_t = (ga, 0)
        if ga:
            for m in modules:
                if int(getattr(m, "base", 0) or 0) == ga:
                    ga_t = (ga, int(getattr(m, "size", 0) or 0))
                    break
        return {"ok": True, "hint": decode_hint(pm, a, modules, ga=ga_t)}

    def read_many(self, addrs: Sequence, dtype: str = "u64") -> dict:
        gate = self._gate()
        if gate:
            return gate
        if not isinstance(addrs, (list, tuple)):
            return _err("bad_arg", "addrs 需为列表")
        pm = resolve_pm(self._bridge())
        if pm is None:
            return _err("not_armed", "进程句柄尚未就绪")
        try:
            modules = pm.list_modules()
        except Exception:
            modules = []
        ga = _ga_base(pm, modules)
        ga_t = (ga, 0)
        if ga:
            for m in modules:
                if int(getattr(m, "base", 0) or 0) == ga:
                    ga_t = (ga, int(getattr(m, "size", 0) or 0))
                    break
        out = []
        for a in list(addrs)[:256]:
            ca = self._coerce_addr(a)
            out.append(decode_hint(pm, ca, modules, ga=ga_t) if ca is not None
                       else {"addr": str(a), "valid": False})
        return {"ok": True, "count": len(out), "hints": out}

    # ---- manual search (delegated) ----
    def search(self, value, dtype: str, *, align: int = 0) -> dict:
        gate = self._gate()
        if gate:
            return gate
        return get_search_manager(self._owner).search(value, dtype, align=align)

    def search_status(self, job_id: str) -> dict:
        return get_search_manager(self._owner).status(job_id)

    def narrow(self, job_id: str, value) -> dict:
        gate = self._gate()
        if gate:
            return gate
        return get_search_manager(self._owner).narrow(job_id, value)

    def search_cancel(self, job_id: str) -> dict:
        return get_search_manager(self._owner).cancel(job_id)

    def search_list(self) -> dict:
        return get_search_manager(self._owner).list_jobs()

    # ---- internals ----
    @staticmethod
    def _coerce_addr(addr) -> Optional[int]:
        try:
            a = int(str(addr).strip(), 0) if isinstance(addr, str) else int(addr)
        except Exception:
            return None
        return a if _plaus(a) else None

    @staticmethod
    def _ent_row(e: dict) -> dict:
        e = e or {}
        return {
            "uuid": str(int(e.get("uuid", 0) or 0)),
            "config_uuid": str(int(e.get("config_uuid", 0) or 0)),
            "base_id": int(e.get("base_id", 0) or 0),
            "kind": str(e.get("kind", "") or ""),
            "name": str(e.get("name", "") or ""),
            "cur_hp": _num(e.get("cur_hp", 0)),
            "max_hp": _num(e.get("max_hp", 0)),
            "hp_pct": round(float(e.get("hp_pct", 0) or 0), 4),
            "obj": _hex(e.get("obj", 0) or 0),
            "breaking_stage": e.get("breaking_stage"),
            "overdrive": e.get("overdrive"),
            "stun": e.get("stun"),
            "cast_skill_id": e.get("cast_skill_id"),
        }


def _json_safe(obj: Any) -> Any:
    if isinstance(obj, dict):
        return {str(k): _json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_json_safe(v) for v in obj]
    if isinstance(obj, bool) or obj is None or isinstance(obj, (str, float)):
        return obj
    if isinstance(obj, int):
        return _num(obj)
    return str(obj)


__all__ = [
    "MemAccess",
    "MemSearchManager",
    "get_search_manager",
    "resolve_bridge",
    "resolve_pm",
    "decode_hint",
]
