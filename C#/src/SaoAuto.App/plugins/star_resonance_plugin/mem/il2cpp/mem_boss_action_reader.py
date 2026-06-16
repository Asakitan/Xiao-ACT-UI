# -*- coding: utf-8 -*-
"""mem_boss_action_reader - memory-driven boss action/skill feed (read-only).

Turns the per-tick combat attrs (already decoded by EntityCombatReader) into a
structured "boss action" stream: which skill an entity is CASTING (edge-detected
from attr 100 / A_SKILL_ID flipping 0 -> positive), its breaking/overdrive/stun/
hp state, and a best-effort cast DURATION so a downstream auto-dodge can be timed.

Cast duration is genuinely hard (no per-entity float in the dump; ZActionAnimInfo
.GetTotalTime is a method over Unity clips). Two layered sources, both honest:
  - learned: measure observed cast length (wall clock between the start/end edges)
    and EMA it per skill_id; available from the 2nd cast of a skill. The floor.
  - buff:    read BuffComp -> BuffItem.Duration (a real ms field, offsets verified
    vs dump fdc7111b) for a buff that appears at cast-start; the precise upgrade.

Read-only. Never blocks the hot loop (the buff probe runs on the 1Hz path only,
is bounded, and degrades to None). All offsets are IL2CPP field offsets (stable
across base/ASLR).
"""
from __future__ import annotations

import threading
import time
from typing import Any, Callable, Dict, List, Optional

# ── verified offsets (dump fdc7111b) ──────────────────────────────────────────
ENT_STATEMACHINE_OFF = 0x70   # ZEntity.stateMachine_ -> ZStateMachine
ENT_BUFFCOMP_OFF = 0x98       # ZEntity.buffComp_ -> BuffComp
SM_CURSTATE_OFF = 0x20        # ZStateMachine.currentState_ (EActorState int)
ACTOR_STATE_SINGING = 1       # EActorState.ActorStateSinging (casting/channel)
ACTOR_STATE_SKILL = 2         # EActorState.ActorStateSkill (instant skill/counterattack)
# An entity is "performing a skill action" in these states. Verified live: a
# counterattacking dummy flips into Skill(2) ~1/sec WITHOUT setting cast_skill_id
# (attr 100), so actor_state — not attr 100 alone — is the reliable action signal.
_ACTIVE_ACTOR_STATES = (ACTOR_STATE_SINGING, ACTOR_STATE_SKILL)

BUFFCOMP_LIST_OFF = 0x30      # BuffComp.buffList_ -> ZList<BuffItem>
# ZList<T> layout: header(0x10) + recyclePooledObj_ bool(0x10) -> items_ @ 0x18,
# size_ @ 0x20 (NOT 0x10/0x18 — there is a bool field before items_).
ZLIST_ITEMS_OFF = 0x18        # ZList.items_ (T[])
ZLIST_SIZE_OFF = 0x20         # ZList.size_ (int)
ARR_LEN_OFF = 0x18            # T[].Length
ARR_ELEMS_OFF = 0x20          # T[] first element
BUFFITEM_UUID_OFF = 0x10      # BuffItem.BuffUuid (int)
BUFFITEM_BASEID_OFF = 0x14    # BuffItem.BuffBaseId (int)
BUFFITEM_CREATE_OFF = 0x30    # BuffItem.CreateTime (long ms)
BUFFITEM_DURATION_OFF = 0x38  # BuffItem.Duration (long ms)

_PTR_LO = 0x10000
_PTR_HI = 0x7FFFFFFFFFFF
_MAX_BUFFS = 64
_DUR_MIN_MS = 200
_DUR_MAX_MS = 10_000
_LEARN_ALPHA = 0.4            # EMA weight for newly-observed cast lengths


def _plaus(p: Any) -> bool:
    try:
        return _PTR_LO <= int(p) <= _PTR_HI
    except Exception:
        return False


class BossDurationProbe:
    """Best-effort cast-duration decoder. Isolated so a bad read never breaks the
    reliable edge feed. Reads are bounded; failure returns None/empty."""

    def __init__(self, pm: Any, *, resolver=None):
        self.pm = pm
        # auto-offset: ZEntity.stateMachine_/buffComp_, ZStateMachine.currentState_,
        # BuffComp.buffList_ by name from the dump (literal fallback). BuffItem/ZList
        # stay literal (BuffItem not in the curated bundle; ZList is an open generic).
        from plugins.star_resonance_plugin.mem.il2cpp import auto_offsets as _ao
        ent = _ao.resolve(resolver, "Panda.ZGame.ZEntity", {
            "off_statemachine": ("stateMachine_", ENT_STATEMACHINE_OFF),
            "off_buffcomp": ("buffComp_", ENT_BUFFCOMP_OFF),
        })
        self.off_statemachine = ent["off_statemachine"]
        self.off_buffcomp = ent["off_buffcomp"]
        self.off_sm_curstate = _ao.resolve(resolver, "Panda.ZGame.ZStateMachine", {
            "a": ("currentState_", SM_CURSTATE_OFF)})["a"]
        self.off_buffcomp_list = _ao.resolve(resolver, "Panda.ZGame.BuffComp", {
            "a": ("buffList_", BUFFCOMP_LIST_OFF)})["a"]
        bi = _ao.resolve(resolver, "Panda.ZGame.BuffItem", {
            "off_bi_uuid": ("BuffUuid", BUFFITEM_UUID_OFF),
            "off_bi_baseid": ("BuffBaseId", BUFFITEM_BASEID_OFF),
            "off_bi_create": ("CreateTime", BUFFITEM_CREATE_OFF),
            "off_bi_duration": ("Duration", BUFFITEM_DURATION_OFF),
        })
        for _k, _v in bi.items():
            setattr(self, _k, _v)

    def read_actor_state(self, ent_addr: int) -> Optional[int]:
        """ZEntity.stateMachine_.currentState_ (EActorState). None on bad read."""
        if not _plaus(ent_addr):
            return None
        sm = self.pm.read_u64(ent_addr + self.off_statemachine)
        if not _plaus(sm):
            return None
        return self.pm.read_i32(sm + self.off_sm_curstate)

    def read_buffs(self, ent_addr: int) -> List[dict]:
        """Read BuffComp -> [{uuid, base_id, create_ms, duration_ms}, ...]. Bounded."""
        if not _plaus(ent_addr):
            return []
        comp = self.pm.read_u64(ent_addr + self.off_buffcomp)
        if not _plaus(comp):
            return []
        zlist = self.pm.read_u64(comp + self.off_buffcomp_list)
        if not _plaus(zlist):
            return []
        size = self.pm.read_u32(zlist + ZLIST_SIZE_OFF) or 0
        if size <= 0:
            return []
        size = min(int(size), _MAX_BUFFS)
        items = self.pm.read_u64(zlist + ZLIST_ITEMS_OFF)
        if not _plaus(items):
            return []
        arr_len = self.pm.read_u32(items + ARR_LEN_OFF) or 0
        n = min(size, int(arr_len))
        out: List[dict] = []
        for i in range(n):
            bi = self.pm.read_u64(items + ARR_ELEMS_OFF + i * 8)
            if not _plaus(bi):
                continue
            try:
                out.append({
                    "uuid": int(self.pm.read_u32(bi + self.off_bi_uuid) or 0),
                    "base_id": int(self.pm.read_u32(bi + self.off_bi_baseid) or 0),
                    "create_ms": int(self.pm.read_i64(bi + self.off_bi_create) or 0),
                    "duration_ms": int(self.pm.read_i64(bi + self.off_bi_duration) or 0),
                })
            except Exception:
                continue
        return out

    def pick_cast_buff(self, buffs: List[dict], baseline_uuids: set) -> Optional[int]:
        """Choose the cast-channel buff: one that appeared since `baseline_uuids`
        with a plausible duration. Returns its duration_ms or None."""
        cands = [b for b in buffs
                 if b["uuid"] not in baseline_uuids
                 and _DUR_MIN_MS <= b["duration_ms"] <= _DUR_MAX_MS]
        if not cands:
            return None
        # most-recently created wins the tie (the cast just started)
        cands.sort(key=lambda b: -b["create_ms"])
        return int(cands[0]["duration_ms"])


class BossActionTracker:
    """Edge-detects boss/monster casts and assembles JSON-safe action records.

    Fed by the mem entity loop: ``update(snap, boss)`` per ~1Hz rich tick, and
    ``update_fast(uuid, skill_id, actor_state, ...)`` per ~10Hz boss-only tick for
    low-latency cast-start detection. Both share the per-uuid prev state under a
    lock so whichever sees the edge first wins; the other is a no-op edge."""

    def __init__(self, ecr: Any, *, pm: Any = None, name_resolver: Any = None,
                 duration_probe: Optional[BossDurationProbe] = None,
                 on_event: Optional[Callable[[dict], None]] = None):
        self._ecr = ecr
        self._pm = pm if pm is not None else getattr(ecr, "pm", None)
        self._names = name_resolver
        self._probe = duration_probe or (BossDurationProbe(self._pm) if self._pm else None)
        self._on_event = on_event
        self._lock = threading.RLock()
        self._prev: Dict[int, dict] = {}     # uuid -> state
        self._learned: Dict[int, int] = {}   # skill_id -> EMA cast length (ms)
        self._records: Dict[int, dict] = {}  # uuid -> latest action record

    # ── public ────────────────────────────────────────────────────────────────
    def update(self, snap: List[dict], boss: Optional[dict]) -> List[dict]:
        """Rich per-tick update over the full entity snapshot. Never raises."""
        # Read the boss's actor_state once so its instant skills (which don't set
        # cast_skill_id) are still detected on the rich path; non-boss entities use
        # the cheaper skill-id-only path.
        boss_uuid = int((boss or {}).get("uuid") or 0)
        boss_actor = None
        if boss and self._probe and (boss.get("obj")):
            try:
                boss_actor = self._probe.read_actor_state(int(boss["obj"]))
            except Exception:
                boss_actor = None
        records: List[dict] = []
        live = set()
        for e in (snap or []):
            try:
                uuid = int(e.get("uuid") or 0)
            except Exception:
                uuid = 0
            if not uuid:
                continue
            live.add(uuid)
            try:
                is_boss = (uuid == boss_uuid)
                rec = self._process_rich(e, boss_actor if is_boss else None, is_boss=is_boss)
                if rec is None:
                    continue
                # overlay the boss's specific named skill from BuffComp (real skill
                # id + name + cast duration); takes precedence over actor_state.
                if uuid == boss_uuid and e.get("obj"):
                    bs = self.detect_buff_skill(uuid, int(e["obj"]))
                    if bs is not None:
                        base_id, nm, dur, kind = bs
                        rec["skill_id"] = int(base_id)
                        rec["skill_name"] = nm
                        rec["skill_kind"] = kind
                        # only treat the buff Duration as a cast window if it is a
                        # plausible cast length — a long-lived buff (e.g. a 480s enrage
                        # timer) is a real skill but NOT a cast duration.
                        if dur and _DUR_MIN_MS <= dur <= _DUR_MAX_MS:
                            rec["cast_duration_ms"] = int(dur)
                            rec["cast_duration_src"] = "buff"
                        rec["cast_edge"] = "start"
                        rec["cast_active"] = True
                records.append(rec)
            except Exception:
                continue
        with self._lock:
            self._prune(live)
        return records

    def update_fast(self, uuid: int, skill_id: int, actor_state: Optional[int] = None,
                    *, obj: int = 0) -> Optional[dict]:
        """Low-latency boss-only update: edge-detect from skill_id alone, reusing
        the base_id/name cached by the rich path. Returns the record on an edge."""
        uuid = int(uuid or 0)
        if not uuid:
            return None
        skill = int(skill_id or 0)
        with self._lock:
            st = self._prev.get(uuid)
            if st is None:
                st = self._new_state(uuid)
                self._prev[uuid] = st
            if obj:
                st["obj"] = int(obj)
            edge = self._apply_action_locked(st, skill, actor_state)
            if edge == "none":
                return None
            rec = self._record_from_state(st, edge)
            self._records[uuid] = rec
        self._emit(rec)
        return rec

    def actions(self) -> List[dict]:
        with self._lock:
            return [dict(r) for r in self._records.values()]

    def boss_action(self, uuid: int) -> Optional[dict]:
        with self._lock:
            r = self._records.get(int(uuid or 0))
            return dict(r) if r else None

    # ── rich path ───────────────────────────────────────────────────────────--
    def _process_rich(self, e: dict, actor_state: Optional[int] = None,
                      is_boss: bool = True) -> Optional[dict]:
        uuid = int(e.get("uuid") or 0)
        skill = int(e.get("cast_skill_id") or 0)
        with self._lock:
            st = self._prev.get(uuid)
            if st is None:
                st = self._new_state(uuid)
                self._prev[uuid] = st
            # cache identity + state from the rich snapshot
            st["base_id"] = int(e.get("base_id") or st.get("base_id") or 0)
            nm = e.get("name")
            if nm:
                st["name"] = str(nm)
            st["obj"] = int(e.get("obj") or st.get("obj") or 0)
            st["breaking_stage"] = e.get("breaking_stage")
            st["overdrive"] = e.get("overdrive")
            st["stun"] = e.get("stun")
            st["extinction"] = e.get("extinction")
            st["hp"] = int(e.get("cur_hp") or 0)
            st["max_hp"] = int(e.get("max_hp") or 0)
            st["hp_pct"] = float(e.get("hp_pct") or 0.0)

            edge = self._apply_action_locked(st, skill, actor_state, is_boss)
            # while casting, try to upgrade duration via the BuffComp probe (boss-only,
            # 1Hz): reading buffs for EVERY casting entity in a crowd was the per-tick
            # O(N) GIL-holding hot path that made hybrid laggy with many players.
            if is_boss and st["skill_id"] and not st.get("duration_locked") \
                    and self._probe and st.get("obj"):
                self._try_buff_duration_locked(st)
            rec = self._record_from_state(st, edge)
            self._records[uuid] = rec
        if edge != "none":
            self._emit(rec)
        return rec

    # ── edge detection (call under lock) ──────────────────────────────────────
    def _apply_action_locked(self, st: dict, skill: int, actor_state: Optional[int],
                             is_boss: bool = True) -> str:
        """Detect a skill-action start/end. An action is 'active' when the entity is
        casting a skill (cast_skill_id>0) OR its actor_state is an active skill state
        (Singing/Skill) — the latter catches instant skills (e.g. a counterattack)
        that never populate cast_skill_id."""
        skill = int(skill or 0)
        if actor_state is not None:
            st["actor_state"] = int(actor_state)
        active = bool(skill) or (actor_state in _ACTIVE_ACTOR_STATES)
        prev_active = bool(st.get("active"))
        prev_skill = int(st.get("skill_id") or 0)
        # chained: a NEW named skill replaces a still-active named one
        chained = active and prev_active and skill and prev_skill and skill != prev_skill
        if active and (not prev_active or chained):
            if prev_active:
                self._learn_locked(prev_skill, st)
            st["active"] = True
            st["skill_id"] = skill
            st["skill_name"] = self._skill_name(skill) if skill else ""
            st["cast_started_at"] = time.monotonic()
            st["duration_locked"] = False
            st["duration_src"] = "none"
            # learned key includes base_id + skill so no-id actions (skill 0) learn
            # per boss and don't collide across bosses.
            learned = self._learned.get(self._learn_key(st, skill))
            if learned:
                st["cast_duration_ms"] = int(learned)
                st["duration_src"] = "learned"
            else:
                st["cast_duration_ms"] = None
            st["buff_baseline"] = self._buff_uuids(st.get("obj") or 0) if is_boss else set()
            return "start"
        if not active and prev_active:
            self._learn_locked(prev_skill, st)
            st["active"] = False
            st["skill_id"] = 0
            st["skill_name"] = ""
            return "end"
        # skill id appeared after the action already started — fill it in, no edge
        if active and skill and not prev_skill:
            st["skill_id"] = skill
            st["skill_name"] = self._skill_name(skill)
        return "none"

    def _try_buff_duration_locked(self, st: dict) -> None:
        try:
            buffs = self._probe.read_buffs(int(st["obj"]))
        except Exception:
            return
        dur = self._probe.pick_cast_buff(buffs, st.get("buff_baseline") or set())
        if dur:
            st["cast_duration_ms"] = int(dur)
            st["duration_src"] = "buff"
            st["duration_locked"] = True

    @staticmethod
    def _learn_key(st: dict, skill_id: int):
        return (int(st.get("base_id") or 0), int(skill_id or 0))

    def _learn_locked(self, skill_id: int, st: dict) -> None:
        started = st.get("cast_started_at")
        if not started:
            return
        observed = int(max(0.0, (time.monotonic() - started)) * 1000.0)
        if not (_DUR_MIN_MS <= observed <= _DUR_MAX_MS):
            return
        key = self._learn_key(st, skill_id)
        prev = self._learned.get(key)
        self._learned[key] = observed if prev is None else int(
            prev * (1 - _LEARN_ALPHA) + observed * _LEARN_ALPHA)

    # ── helpers ───────────────────────────────────────────────────────────────
    def _buff_uuids(self, obj: int) -> set:
        if not (self._probe and obj):
            return set()
        try:
            return {b["uuid"] for b in self._probe.read_buffs(int(obj))}
        except Exception:
            return set()

    # ── named boss-skill detection via the buff list ──────────────────────────
    _SKILL_PRIO = {"boss_mechanic_skill": 3, "boss_mechanic": 2, "boss_skill": 1}

    def _skill_name_hint(self, base_id: int):
        """Best-effort, NON-authoritative display name for a buff base_id. The id
        itself (read from memory) is the authority; this offline-table lookup may be
        version-offset, so it is only a UI hint and never used for matching/filtering."""
        nr = self._names
        if nr is None or base_id <= 0:
            return "", ""
        for kind in ("boss_mechanic_skill", "boss_skill", "boss_mechanic", "buff"):
            fn = getattr(nr, kind, None)
            if callable(fn):
                try:
                    nm = fn(int(base_id), default="")
                    if nm:
                        return str(nm), kind
                except Exception:
                    continue
        return "", ""

    def detect_buff_skill(self, uuid: int, obj: int):
        """Return (base_id, name_hint, duration_ms, kind) for a NEW buff that appeared
        on this entity since last call, else None. The boss's skills/mechanics surface
        as transient buffs in BuffComp; the base_id + duration are read from memory
        (authoritative). Persistent buffs (present from the baseline) never re-fire, so
        only real skill casts emit. Name is a best-effort hint only -- the player maps
        reactions by the memory base_id, so an offset name table can't break matching."""
        if not (self._probe and obj):
            return None
        with self._lock:
            st = self._prev.get(uuid)
            if st is None:
                st = self._new_state(uuid)
                self._prev[uuid] = st
            cur = {}
            try:
                for b in self._probe.read_buffs(int(obj)):
                    bid = int(b["base_id"])
                    if bid > 0:
                        cur[bid] = int(b["duration_ms"] or 0)
            except Exception:
                return None
            prev = st.get("buff_skills")
            st["buff_skills"] = set(cur)
            if prev is None:        # first sight -> seed baseline, don't fire
                return None
            new_ids = set(cur) - prev
            if not new_ids:
                return None
            # the longest-duration new buff is the most likely telegraphed cast
            best = max(new_ids, key=lambda bid: cur[bid])
            name, kind = self._skill_name_hint(best)
            return best, name, cur[best], kind

    def _new_state(self, uuid: int) -> dict:
        return {"uuid": uuid, "base_id": 0, "name": "", "obj": 0, "active": False,
                "buff_skills": None,   # None until first buff read (seeds baseline)
                "skill_id": 0, "skill_name": "", "cast_started_at": 0.0,
                "cast_duration_ms": None, "duration_src": "none", "duration_locked": False,
                "actor_state": None, "breaking_stage": None, "overdrive": None,
                "stun": None, "extinction": None, "hp": 0, "max_hp": 0, "hp_pct": 0.0,
                "buff_baseline": set()}

    def _record_from_state(self, st: dict, edge: str) -> dict:
        started = st.get("cast_started_at") or 0.0
        elapsed = int(max(0.0, (time.monotonic() - started)) * 1000.0) if (started and st.get("active")) else 0
        return {
            "boss_uuid": int(st["uuid"]),
            "boss_base_id": int(st.get("base_id") or 0),
            "boss_name": str(st.get("name") or ""),
            "skill_id": int(st.get("skill_id") or 0),
            "skill_name": str(st.get("skill_name") or ""),
            "cast_edge": edge,
            "actor_state": st.get("actor_state"),
            "cast_active": bool(st.get("active")),
            "cast_started_at": float(started),
            "cast_elapsed_ms": elapsed,
            "cast_duration_ms": st.get("cast_duration_ms"),
            "cast_duration_src": str(st.get("duration_src") or "none"),
            "breaking_stage": st.get("breaking_stage"),
            "overdrive": st.get("overdrive"),
            "stun": st.get("stun"),
            "extinction": st.get("extinction"),
            "hp": int(st.get("hp") or 0),
            "max_hp": int(st.get("max_hp") or 0),
            "hp_pct": float(st.get("hp_pct") or 0.0),
            "ts": time.time(),
        }

    def _skill_name(self, skill_id: int) -> str:
        nr = self._names
        if nr is None or not skill_id:
            return ""
        for meth in ("skill", "boss_skill", "monster_skill"):
            fn = getattr(nr, meth, None)
            if callable(fn):
                try:
                    nm = fn(int(skill_id), default="")
                    if nm:
                        return str(nm)
                except Exception:
                    continue
        return ""

    def _prune(self, live: set) -> None:
        if len(self._prev) <= 1024:
            dead = [u for u in self._prev if u not in live and not self._prev[u].get("skill_id")]
        else:
            dead = [u for u in self._prev if u not in live]
        for u in dead:
            self._prev.pop(u, None)
            self._records.pop(u, None)

    def _emit(self, rec: dict) -> None:
        if self._on_event:
            try:
                self._on_event(rec)
            except Exception:
                pass


__all__ = ["BossActionTracker", "BossDurationProbe"]
