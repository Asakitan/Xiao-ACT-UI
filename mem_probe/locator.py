"""SmartLocator — TCP-anchored self-object locator with persistent known_uid.

Two run modes share one entry point (`SmartLocator.locate`):

  warm_run (no TCP)
      Persisted `known_uid` exists  →  Cython scan klass_ptr  →  filter by
      CharSerialize.CharId == known_uid  →  return SELF.  ~1–3s on a 5 GB
      heap, zero PacketBridge / network activity.

  first_run (TCP-anchored)
      No `known_uid` (or `--force-first-run`) → start TcpSnapshotSource,
      wait for uid+hp+max_hp → run the same Cython klass scan + CharId filter
      using TCP uid as ground truth, then double-check by:
        (a) `mem_max_hp == tcp_max_hp` (strict)
        (b) `tcp.hp_in_window(mem_cur_hp, window_s=0.3)` — TCP is ~200ms
            behind memory, so a sliding-window check absorbs the lag.
      On success, persist `known_uid` so all subsequent launches go warm.

Persistence file: `mem_probe/anchors.json` (independent of the legacy
`tools/mem_probe/anchors.json` used by research CLIs). Schema:

    {
      "smart_locator": {
        "known_uid": 36668136,
        "known_uid_set_at": <ts>,
        "known_uid_set_via": "tcp_first_run",
        "last_pid": 40340,
        "last_self_obj":          "0x...",
        "last_user_fight_attr":   "0x...",
        "last_char_base":         "0x...",
        "last_role_level":        "0x...",
        "last_profession_list":   "0x...",
        "last_energy_item":       "0x...",
        "last_season_medal_info": "0x...",
        "last_located_at": <ts>,
        "last_located_via": "warm_scan"
      }
    }

Public API:
    locator = SmartLocator()
    refs    = locator.locate()                  # auto: cache → warm → first
    refs    = locator.locate(allow_tcp_fallback=False)  # warm-only (raise on miss)
    refs    = locator.locate(force_first_run=True)      # ignore known_uid

CLI:
    python -m mem_probe.locator first              # always do TCP first_run
    python -m mem_probe.locator warm               # warm-only (raise on miss)
    python -m mem_probe.locator                    # auto
    python -m mem_probe.locator status             # print persisted anchors
    python -m mem_probe.locator reset              # clear known_uid
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from tools.mem_probe.process import StarProcess, StarProcessError, is_admin
from tools.mem_probe.il2cpp.static_resolver import StaticResolver, open_resolver
from tools.mem_probe.il2cpp.bundle_loader import open_resolver_from_bundle
from tools.mem_probe.il2cpp.bundle_store import find_bundle_for_running_game


ANCHORS_PATH = os.path.join(_HERE, "anchors.json")
_DEFAULT_BUNDLE = os.path.join(
    _SAO_AUTO, "tools", "mem_probe", "il2cpp", "_cache", "bundle.json",
)
_DEFAULT_DUMP_ID = "ef9ef95a"

# IL2CPP class / field names anchored as the SELF identification path.
SELF_CLASS = "Zproto.CharSerialize"
SENTINEL_FIELD = "Attr"
SENTINEL_CLASS = "Zproto.UserFightAttr"
UID_FIELD = "CharId"
HP_FIELD = "CurHp"
MAX_HP_FIELD = "MaxHp"
# Substruct fields hung off CharSerialize that get persisted alongside SELF
# so downstream code (DPS / overlay / autokey) can directly subscript.
SUBSTRUCT_FIELDS: List[str] = [
    "CharBase", "RoleLevel", "ProfessionList", "EnergyItem", "SeasonMedalInfo",
]


# ───────────────────────── Result type ─────────────────────────

@dataclass
class SelfRefs:
    char_serialize: int = 0
    user_fight_attr: int = 0
    char_base: int = 0
    role_level: int = 0
    profession_list: int = 0
    energy_item: int = 0
    season_medal_info: int = 0
    char_id: int = 0
    cur_hp: int = 0
    max_hp: int = 0
    located_via: str = "unknown"
    # 'cache' | 'warm_scan' | 'tcp_first_run'  (dump path)
    # 'value_anchor_cache' | 'value_anchor_warm' | 'value_anchor_first_run'
    located_at: float = 0.0
    elapsed_s: float = 0.0
    # Field offsets discovered (or assumed) for the SELF object layout.
    # Persisted by _persist for warm-run reuse, especially in value-anchor
    # mode where dump.cs offsets cannot be trusted. -1 = unknown.
    uid_off: int = -1            # CharId offset within CharSerialize
    attr_slot_off: int = -1      # Attr-ptr offset within CharSerialize
    cur_hp_off: int = -1         # CurHp offset within UserFightAttr
    max_hp_off: int = -1         # MaxHp offset within UserFightAttr
    hp_width: int = -1           # CurHp/MaxHp width in bytes (4 or 8)


class SmartLocatorError(RuntimeError):
    pass


# ───────────────────────── Helpers ─────────────────────────

def _load_anchors(path: str) -> dict:
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def _save_anchors(path: str, data: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)


def _hex(v: int) -> str:
    return f"0x{int(v):X}"


def _parse_hex(s) -> int:
    if s is None:
        return 0
    if isinstance(s, int):
        return s
    return int(str(s), 16)


# ───────────────────────── SmartLocator ─────────────────────────

class SmartLocator:
    """TCP-anchored self locator with persistent known_uid."""

    def __init__(self, *, anchors_path: str = ANCHORS_PATH,
                 bundle_path: Optional[str] = None,
                 dump_id: str = _DEFAULT_DUMP_ID,
                 hp_window_s: float = 0.3,
                 force_value_anchor: bool = False) -> None:
        self.anchors_path = anchors_path
        self.bundle_path = bundle_path or _DEFAULT_BUNDLE
        self.dump_id = dump_id
        self.hp_window_s = float(hp_window_s)
        self.force_value_anchor = bool(force_value_anchor)
        self._pm: Optional[StarProcess] = None
        self._sr: Optional[StaticResolver] = None
        self._sr_unavailable_reason: Optional[str] = None

    @property
    def pm(self) -> StarProcess:
        """Direct StarProcess handle, available even when dump.cs is unusable."""
        if self._pm is None:
            self._pm = StarProcess()
        return self._pm

    @property
    def sr(self) -> StaticResolver:
        """StaticResolver (requires dump.cs). Raises SmartLocatorError on failure."""
        if self._sr is not None:
            return self._sr
        if self._sr_unavailable_reason:
            raise SmartLocatorError(
                f"StaticResolver unavailable: {self._sr_unavailable_reason}"
            )
        try:
            store_hit = find_bundle_for_running_game()
            if store_hit:
                bundle, key, _ga = store_hit
                self._sr = open_resolver_from_bundle(bundle)
            elif os.path.isfile(self.bundle_path):
                self._sr = open_resolver_from_bundle(self.bundle_path)
            else:
                self._sr = open_resolver(self.dump_id)
        except Exception as e:
            self._sr_unavailable_reason = f"{type(e).__name__}: {e}"
            raise SmartLocatorError(
                f"StaticResolver init failed: {self._sr_unavailable_reason}"
            ) from e
        return self._sr

    # ───────── public ─────────

    def locate(self, *, allow_tcp_fallback: bool = True,
               force_first_run: bool = False) -> SelfRefs:
        """Auto: cache → warm scan → first_run, with dump→value_anchor fallback.

        Strategy:
          1. Try dump-based path (cache → warm → tcp first_run).
          2. If dump path fails (StaticResolver unavailable, klass sanity fail,
             or stage-1/2 finds nothing), fall through to value-anchor path.
          3. value-anchor uses TCP truth values (UID + HP/MaxHP) directly,
             no dump.cs needed. Substruct refs (char_base etc.) cannot be
             populated — only char_serialize / user_fight_attr / hp / max_hp.
        """
        t0 = time.time()
        anchors = _load_anchors(self.anchors_path)
        sl = anchors.get("smart_locator", {})
        known_uid = 0 if force_first_run else int(sl.get("known_uid", 0) or 0)
        last_via = sl.get("last_located_via", "")
        # Skip dump path entirely if last successful run used value-anchor
        # — dump.cs is known stale for this game, no need to waste 5s on it.
        prefer_value_anchor = (
            self.force_value_anchor
            or last_via.startswith("value_anchor")
        )

        # ── Path 1: dump-based (skipped if value-anchor was last successful) ──
        if not prefer_value_anchor:
            try:
                refs = self._try_dump_path(sl, known_uid, allow_tcp_fallback,
                                           force_first_run)
                if refs is not None:
                    refs.located_at = time.time()
                    refs.elapsed_s = time.time() - t0
                    self._persist(refs, refs.char_id or known_uid)
                    return refs
            except SmartLocatorError as e:
                print(f"[locate] dump path failed: {e}")
                print(f"[locate] falling back to value-anchor path "
                      f"(no dump.cs required)")
            except Exception as e:
                print(f"[locate] dump path crashed unexpectedly ({type(e).__name__}: {e}); "
                      f"falling back to value-anchor.")

        # ── Path 2: value-anchor (no dump.cs) ──
        if known_uid:
            cached = self._try_cached_value_anchor(sl, known_uid)
            if cached is not None:
                cached.located_via = "value_anchor_cache"
                cached.located_at = time.time()
                cached.elapsed_s = time.time() - t0
                self._persist(cached, known_uid)
                return cached

            refs = self._warm_locate_value_anchor(sl, known_uid)
            if refs is not None:
                refs.located_via = "value_anchor_warm"
                refs.located_at = time.time()
                refs.elapsed_s = time.time() - t0
                self._persist(refs, known_uid)
                return refs

        if not allow_tcp_fallback:
            raise SmartLocatorError(
                f"both dump and value-anchor warm paths failed "
                f"(known_uid={known_uid}); TCP fallback disabled. "
                "Pass allow_tcp_fallback=True or run "
                "`python -m mem_probe.locator first`."
            )

        refs = self._value_anchor_first_run()
        refs.located_via = "value_anchor_first_run"
        refs.located_at = time.time()
        refs.elapsed_s = time.time() - t0
        self._persist(refs, refs.char_id)
        return refs

    def _try_dump_path(self, sl: dict, known_uid: int,
                       allow_tcp_fallback: bool,
                       force_first_run: bool) -> Optional[SelfRefs]:
        """Run the dump-based locate flow. Returns SelfRefs on success or None
        if a non-fatal miss; raises SmartLocatorError on dump-stale conditions."""
        if known_uid and not force_first_run:
            cached = self._try_cached(sl, known_uid)
            if cached is not None:
                cached.located_via = "cache"
                return cached
            refs = self._warm_locate(known_uid)
            if refs is not None:
                refs.located_via = "warm_scan"
                return refs

        if not allow_tcp_fallback:
            return None  # caller will check known_uid path next
        refs = self._first_run_with_tcp()
        refs.located_via = "tcp_first_run"
        return refs

    def reset_known_uid(self) -> None:
        anchors = _load_anchors(self.anchors_path)
        if "smart_locator" in anchors:
            anchors["smart_locator"].pop("known_uid", None)
            anchors["smart_locator"]["reset_at"] = time.time()
            _save_anchors(self.anchors_path, anchors)

    def status(self) -> dict:
        return _load_anchors(self.anchors_path).get("smart_locator", {})

    # ───────── strategies ─────────

    def _try_cached(self, sl: dict, known_uid: int) -> Optional[SelfRefs]:
        """O(1): validate persisted last_self_obj for the current pid."""
        last_pid = int(sl.get("last_pid", 0) or 0)
        if last_pid != self.sr.pm.pid:
            return None
        obj = _parse_hex(sl.get("last_self_obj"))
        if obj == 0:
            return None
        klass = self.sr.resolve_klass(SELF_CLASS)
        if not klass:
            return None
        if self.sr.pm.read_u64(obj) != klass:
            return None
        cid = self.sr.read_field(obj, SELF_CLASS, UID_FIELD)
        if cid != known_uid:
            return None
        return self._build_refs(obj, char_id=cid)

    def _warm_locate(self, known_uid: int) -> Optional[SelfRefs]:
        """Cython klass scan → CharId filter → SELF (no TCP)."""
        klass = self.sr.resolve_klass(SELF_CLASS)
        if not klass:
            raise SmartLocatorError(
                f"klass {SELF_CLASS!r} not resolvable — IL2CPP dump out of date?"
            )
        cands = self.sr.find_instances(klass, max_hits=4096)
        for obj in cands:
            cid = self.sr.read_field(obj, SELF_CLASS, UID_FIELD)
            if cid == known_uid:
                return self._build_refs(obj, char_id=cid)
        return None

    def _first_run_with_tcp(self) -> SelfRefs:
        """TCP-anchored: capture uid, scan, validate, persist known_uid.

        Validation strategy (per candidate):
          1. CharId matches TCP uid               (hard gate; uid is global unique)
          2. Attr ptr deref → UserFightAttr klass (sanity: object layout is right)
          3. one of:
              a) cur_hp present in TCP HP history within window_s, OR
              b) only 1 cid-matching candidate AND max_hp within ±5%
                 of any TCP-reported max_hp in last 2s
          4. max_hp ±5% match → soft, only logged if mismatch

        max_hp is NOT a hard gate because TCP can lag during scene/buff
        transitions (observed: TCP reports both 483921 and 387080 within
        the same first_run as game state shifted).
        """
        from .tcp_source import TcpSnapshotSource

        klass = self.sr.resolve_klass(SELF_CLASS)
        ufa_klass = self.sr.resolve_klass(SENTINEL_CLASS)
        if not klass or not ufa_klass:
            raise SmartLocatorError(
                f"klass {SELF_CLASS!r} / {SENTINEL_CLASS!r} not resolvable"
            )

        # Sanity check: a real IL2CPP klass pointer points into the GA module's
        # .data/.bss (i.e., must lie inside [GA.base, GA.base+GA.size)). If it
        # doesn't, the script.json RVA is stale — abort with a clear message
        # rather than wasting 10s on a futile heap scan.
        ga_base = self.sr.ga
        try:
            mods = self.sr.pm.list_modules()
            ga_mod = next((m for m in mods if m.name.lower() == "gameassembly.dll"), None)
            ga_size = ga_mod.size if ga_mod else 0
        except Exception:
            ga_size = 0
        ga_end = ga_base + ga_size if ga_size else (ga_base + 0x40000000)
        print(f"[first_run] GA: base=0x{ga_base:X} size=0x{ga_size:X} end=0x{ga_end:X}")
        print(f"[first_run] klass {SELF_CLASS}=0x{klass:X} | {SENTINEL_CLASS}=0x{ufa_klass:X}")
        bad = []
        for label, kp in ((SELF_CLASS, klass), (SENTINEL_CLASS, ufa_klass)):
            if kp < ga_base or kp >= ga_end:
                bad.append(f"{label}=0x{kp:X} not in GA range")
        if bad:
            raise SmartLocatorError(
                "IL2CPP klass pointers fail sanity check (not inside "
                "GameAssembly.dll module range): "
                + "; ".join(bad)
                + f". The dump (dump_id={self.dump_id!r}) is almost certainly "
                "STALE — script.json RVAs no longer match the current "
                "GameAssembly.dll. Regenerate the IL2CPP dump:\n"
                "  1. python -m tools.mem_probe.il2cpp.mem_dump_metadata\n"
                "  2. python -m tools.mem_probe.il2cpp.setup_dumper\n"
                "  3. python -m tools.mem_probe.il2cpp.metadata_builder\n"
                "Or pass --dump-id <new_id> if a fresh dump already exists."
            )

        with TcpSnapshotSource() as src:
            print("[first_run] waiting for TCP uid+hp+max_hp ...")
            ready = src.wait_ready(timeout=120.0)
            print(f"[first_run] TCP ready: uid={ready['uid']} "
                  f"hp={ready['hp']}/{ready['max_hp']} name={ready['name']!r}")
            tcp_uid = int(ready["uid"])
            tcp_max_hp = int(ready["max_hp"])
            time.sleep(1.0)  # accumulate ≥1s of HP samples

            print(f"[first_run] Cython scanning klass {SELF_CLASS} ...")
            t_scan = time.time()
            cands = self.sr.find_instances(klass, max_hits=4096)
            print(f"[first_run] {len(cands)} candidates ({time.time()-t_scan:.2f}s)")

            sf_off = self.sr.dci.field_offset(SELF_CLASS, SENTINEL_FIELD) or 0x88
            verbose = (len(cands) <= 8)

            # Stage 1: filter by CharId == tcp_uid (via dump.cs offset)
            cid_matches: List[Tuple[int, int]] = []  # [(obj, cid)]
            uid_off_via_dump = self.sr.dci.field_offset(SELF_CLASS, UID_FIELD)
            for obj in cands:
                cid = self.sr.read_field(obj, SELF_CLASS, UID_FIELD)
                if verbose:
                    print(f"  [cand 0x{obj:X}] CharId(via dump.cs +0x{uid_off_via_dump:X})={cid}")
                if cid == tcp_uid:
                    cid_matches.append((obj, int(cid)))

            # Stage 1 fallback: dump.cs offset is stale → auto-detect by scanning
            # the object body for i64/i32 == tcp_uid. If exactly one candidate
            # contains tcp_uid in its body, accept that candidate.
            if not cid_matches:
                cid_matches = self._stage1_autodetect(cands, tcp_uid)

            if not cid_matches:
                raise SmartLocatorError(
                    f"0/{len(cands)} klass candidates had CharId == {tcp_uid} "
                    f"(via dump.cs +0x{uid_off_via_dump:X} OR auto-detect scan ±0x200). "
                    f"check IL2CPP dump or game patch."
                )

            # Stage 2: validate Attr deref + read hp/max_hp (with offset fallback)
            verified: List[Tuple[int, int, int, int]] = []  # [(obj, attr, cur, max)]
            for obj, cid in cid_matches:
                attr, used_off = self._read_attr_with_fallback(obj, sf_off, ufa_klass)
                if not attr:
                    print(f"  [reject 0x{obj:X}] Attr deref failed at +0x{sf_off:X} "
                          f"and auto-detect found no slot whose deref klass == "
                          f"UserFightAttr 0x{ufa_klass:X}")
                    continue
                if used_off != sf_off:
                    print(f"  [autodetect 0x{obj:X}] Attr offset +0x{used_off:X} "
                          f"(dump.cs gave +0x{sf_off:X})")
                cur_hp = self.sr.read_field(attr, SENTINEL_CLASS, HP_FIELD)
                max_hp = self.sr.read_field(attr, SENTINEL_CLASS, MAX_HP_FIELD)
                if cur_hp is None or max_hp is None:
                    print(f"  [reject 0x{obj:X}] HP/MaxHP read failed at attr 0x{attr:X}")
                    continue
                verified.append((obj, attr, int(cur_hp), int(max_hp)))
                print(f"  [pass-attr 0x{obj:X}] attr=0x{attr:X} hp={cur_hp}/{max_hp}")

            if not verified:
                raise SmartLocatorError(
                    f"all {len(cid_matches)} CharId-matching candidates failed "
                    f"Attr deref. Likely IL2CPP layout drift (Attr offset {sf_off:#x})."
                )

            # Stage 3: pick best — single candidate is auto-accept (CharId is
            # already a strong unique signal); multi-candidate uses HP window.
            chosen: Optional[Tuple[int, int, int, int]] = None
            if len(verified) == 1:
                chosen = verified[0]
                obj, attr, cur_hp, max_hp = chosen
                in_window = src.hp_in_window(cur_hp, window_s=self.hp_window_s)
                hp_ratio = abs(max_hp - tcp_max_hp) / max(1, tcp_max_hp)
                hist = src.history_summary(window_s=max(self.hp_window_s, 2.0))
                print(f"[first_run] single candidate; auto-accept "
                      f"(cur_hp_in_window={in_window}, max_hp_diff={hp_ratio*100:.1f}%, "
                      f"tcp_history={hist['values']})")
                if hp_ratio > 0.10:
                    print(f"[warn] mem max_hp={max_hp} differs from TCP max_hp="
                          f"{tcp_max_hp} by {hp_ratio*100:.1f}% — could be lag or a real mismatch.")
            else:
                # Multi-candidate: use HP window to disambiguate
                in_window = [v for v in verified
                             if src.hp_in_window(v[2], window_s=self.hp_window_s)]
                if len(in_window) == 1:
                    chosen = in_window[0]
                    print(f"[first_run] {len(verified)} attr-valid; "
                          f"1 matches TCP HP window — picked {_hex(chosen[0])}")
                elif len(in_window) > 1:
                    raise SmartLocatorError(
                        f"ambiguous SELF: {len(in_window)} of {len(verified)} "
                        f"candidates match HP window. Addrs: "
                        f"{[_hex(v[0]) for v in in_window]}"
                    )
                else:
                    # Fallback: pick the one with closest max_hp to tcp_max_hp
                    sorted_by_diff = sorted(
                        verified, key=lambda v: abs(v[3] - tcp_max_hp))
                    if abs(sorted_by_diff[0][3] - tcp_max_hp) <= 0.05 * tcp_max_hp:
                        chosen = sorted_by_diff[0]
                        print(f"[first_run] {len(verified)} candidates; none in "
                              f"HP window — picked closest max_hp "
                              f"{_hex(chosen[0])} (max={chosen[3]} vs tcp={tcp_max_hp})")
                    else:
                        raise SmartLocatorError(
                            f"none of {len(verified)} candidates match HP window "
                            f"or max_hp ±5%. tcp_history={src.history_summary(window_s=2.0)}; "
                            f"verified=[{', '.join(f'(obj={_hex(v[0])}, hp={v[2]}/{v[3]})' for v in verified)}]"
                        )

            obj, attr, cur_hp, max_hp = chosen
            print(f"[first_run] SELF: char_serialize=0x{obj:X} attr=0x{attr:X} "
                  f"uid={tcp_uid} hp={cur_hp}/{max_hp}")
            return self._build_refs(obj, char_id=tcp_uid, attr=attr,
                                    cur_hp=cur_hp, max_hp=max_hp)

    # ───────── stage 1 auto-detect (game-patch resilience) ─────────

    def _stage1_autodetect(self, cands: List[int], tcp_uid: int,
                           *, scan_radius: int = 0x200) -> List[Tuple[int, int]]:
        """When dump.cs CharId offset is stale, scan each candidate object
        body for any i64/i32 that equals tcp_uid; if found uniquely in one
        candidate, accept it as SELF (and log the actual offset so the
        dump.cs index can be regenerated).
        """
        print(f"[stage1-fallback] dump.cs CharId offset gave 0 matches; "
              f"auto-scanning {len(cands)} candidate bodies for i64/i32 "
              f"== {tcp_uid} in +0..+0x{scan_radius:X} ...")
        results: List[Tuple[int, List[int], List[int]]] = []
        uid_lo32 = int(tcp_uid) & 0xFFFFFFFF
        for obj in cands:
            blob = self.sr.pm.read_bytes(obj, scan_radius)
            if not blob:
                continue
            i64_offs: List[int] = []
            for off in range(0, len(blob) - 8 + 1, 8):
                if int.from_bytes(blob[off:off + 8], "little") == tcp_uid:
                    i64_offs.append(off)
            i32_offs: List[int] = []
            for off in range(0, len(blob) - 4 + 1, 4):
                v32 = int.from_bytes(blob[off:off + 4], "little")
                if v32 != uid_lo32:
                    continue
                # Filter: this is i32 stored at a 4-byte slot; the high 32
                # bits of the surrounding i64 must be 0 (standard layout).
                if off + 8 <= len(blob):
                    v_hi = int.from_bytes(blob[off + 4:off + 8], "little")
                    if v_hi != 0:
                        continue
                i32_offs.append(off)
            if i64_offs or i32_offs:
                results.append((obj, i64_offs, i32_offs))
                print(f"  [autodetect 0x{obj:X}] i64 hits @ "
                      f"{[hex(o) for o in i64_offs]}, "
                      f"i32 hits @ {[hex(o) for o in i32_offs]}")
            else:
                # Dump first 0x80 bytes of i64 grid for forensics
                print(f"  [autodetect 0x{obj:X}] no match in +0..+0x{scan_radius:X}; "
                      f"i64 grid +0..+0x80:")
                for off in range(0, min(len(blob), 0x80), 8):
                    v = int.from_bytes(blob[off:off + 8], "little")
                    print(f"    +0x{off:03X}: u64=0x{v:016X}  i32_lo={v & 0xFFFFFFFF}")

        # Single candidate with at least one hit → accept
        if len(results) == 1:
            obj, i64_offs, i32_offs = results[0]
            picked_off = i64_offs[0] if i64_offs else i32_offs[0]
            picked_t = "i64" if i64_offs else "i32"
            print(f"[stage1-fallback] accepted single candidate 0x{obj:X}; "
                  f"actual CharId offset = +0x{picked_off:X} ({picked_t}) "
                  f"— dump.cs index likely needs regeneration.")
            return [(obj, tcp_uid)]
        if len(results) > 1:
            print(f"[stage1-fallback] {len(results)} candidates all contain "
                  f"tcp_uid={tcp_uid} in their body; ambiguous, deferring to "
                  f"stage 2 / 3 to disambiguate.")
            return [(obj, tcp_uid) for obj, _, _ in results]
        return []

    def _read_attr_with_fallback(self, obj: int, default_off: int,
                                 ufa_klass: int,
                                 *, scan_radius: int = 0x200) -> Tuple[int, int]:
        """Try the dump.cs Attr offset; if deref klass mismatch, scan obj
        body for any 8-byte aligned ptr whose *(ptr) == ufa_klass.

        Returns (attr_addr, offset_used). attr_addr=0 on total failure.
        """
        # Try the canonical offset first
        attr = self.sr.pm.read_u64(obj + default_off) or 0
        if attr and self.sr.pm.read_u64(attr) == ufa_klass:
            return attr, default_off
        # Auto-detect: scan all 8-byte slots
        blob = self.sr.pm.read_bytes(obj, scan_radius)
        if not blob:
            return 0, default_off
        for off in range(0, len(blob) - 8 + 1, 8):
            ptr = int.from_bytes(blob[off:off + 8], "little")
            if not ptr:
                continue
            # Pointer has to look like a userspace heap address
            if ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
                continue
            if self.sr.pm.read_u64(ptr) == ufa_klass:
                return ptr, off
        return 0, default_off

    # ───────── value-anchor strategies (no dump.cs needed) ─────────

    # Search radii for the value-anchor algorithm. char_serialize_obj is a
    # large struct (UID is somewhere in the first ~0x300 bytes); UserFightAttr
    # is a smaller struct (CurHp + MaxHp typically in first 0x80 bytes).
    _VA_CHAR_BODY_RADIUS = 0x300
    _VA_ATTR_BODY_RADIUS = 0x100
    # Possible offsets for UID inside CharSerialize (most-likely first)
    _VA_UID_OFF_TRIES = (0x10, 0x8, 0x18, 0x20, 0x40)
    # CurHp / MaxHp are usually adjacent (4 or 8 bytes apart)
    _VA_HP_DELTA_TRIES = (4, -4, 8, -8)

    def _try_cached_value_anchor(self, sl: dict, known_uid: int) -> Optional[SelfRefs]:
        """O(1) cache validation that doesn't depend on dump.cs.

        Reads (last_self_obj + last_uid_off) and verifies it equals known_uid.
        """
        from mem_probe import cy_memscan as _cy  # noqa: F401  (forces import)
        last_pid = int(sl.get("last_pid", 0) or 0)
        if last_pid != self.pm.pid:
            return None
        char_obj = _parse_hex(sl.get("last_self_obj"))
        attr_obj = _parse_hex(sl.get("last_user_fight_attr"))
        uid_off = int(sl.get("last_uid_off", -1) or -1)
        cur_off = int(sl.get("last_cur_hp_off", -1) or -1)
        max_off = int(sl.get("last_max_hp_off", -1) or -1)
        if char_obj == 0 or uid_off < 0:
            return None
        # Verify UID still at the persisted location
        uid_blob = self.pm.read_bytes(char_obj + uid_off, 8)
        if not uid_blob:
            return None
        cid = int.from_bytes(uid_blob, "little")
        if cid != known_uid:
            return None
        # Read HP if attr_obj + offsets are persisted (honor hp_width)
        cur_hp = max_hp = 0
        hp_width = int(sl.get("last_hp_width", 4) or 4)
        if attr_obj and cur_off >= 0 and max_off >= 0:
            for off, name in ((cur_off, "cur"), (max_off, "max")):
                blob = self.pm.read_bytes(attr_obj + off, hp_width)
                if blob:
                    val = int.from_bytes(blob, "little")
                    if name == "cur":
                        cur_hp = val
                    else:
                        max_hp = val
        return SelfRefs(
            char_serialize=char_obj,
            user_fight_attr=attr_obj,
            char_id=int(cid),
            cur_hp=int(cur_hp),
            max_hp=int(max_hp),
        )

    def _warm_locate_value_anchor(self, sl: dict,
                                  known_uid: int) -> Optional[SelfRefs]:
        """Re-scan UID from scratch, re-using persisted offsets to skip ptr
        scanning when possible. Falls back to full first_run-style search
        when offsets aren't persisted yet.
        """
        from mem_probe import cy_memscan as _cy

        # Stage A: Cython UID scan
        uid_addrs = self._va_scan_uid_full_heap(known_uid)
        if not uid_addrs:
            return None

        uid_off_hint = int(sl.get("last_uid_off", -1) or -1)
        attr_slot_off_hint = int(sl.get("last_attr_slot_off", -1) or -1)
        cur_off_hint = int(sl.get("last_cur_hp_off", -1) or -1)
        max_off_hint = int(sl.get("last_max_hp_off", -1) or -1)
        hp_width_hint = int(sl.get("last_hp_width", 4) or 4)
        last_max_hp = int(sl.get("last_max_hp", 0) or 0)

        # Try cached offsets first (super fast). Verify candidates in parallel
        # — each verify is 3 RPM (8 bytes each), 5ms × 3 × 500 candidates =
        # 7.5s sequential; 4 threads cut this to ~2s.
        if (uid_off_hint >= 0 and attr_slot_off_hint >= 0
                and cur_off_hint >= 0 and max_off_hint >= 0):
            import threading
            from concurrent.futures import ThreadPoolExecutor

            t_verify = time.time()
            result_box: List[Optional[SelfRefs]] = [None]
            stop_event = threading.Event()
            lock = threading.Lock()

            def verify_one(uid_addr):
                if stop_event.is_set():
                    return
                char_obj = uid_addr - uid_off_hint
                attr_blob = self.pm.read_bytes(char_obj + attr_slot_off_hint, 8)
                if not attr_blob:
                    return
                attr_obj = int.from_bytes(attr_blob, "little")
                if not (0x10000 <= attr_obj <= 0x7FFFFFFFFFFF):
                    return
                cur_hp_blob = self.pm.read_bytes(attr_obj + cur_off_hint, hp_width_hint)
                max_hp_blob = self.pm.read_bytes(attr_obj + max_off_hint, hp_width_hint)
                if not cur_hp_blob or not max_hp_blob:
                    return
                cur_hp = int.from_bytes(cur_hp_blob, "little")
                max_hp = int.from_bytes(max_hp_blob, "little")
                # Phase 0: max_hp can change drastically (gear/buff/debuff), so
                # don't validate against last_max_hp absolute value. Only sanity
                # range check + cur<=max invariant.
                if not (1000 < max_hp < 100_000_000):
                    return
                if cur_hp < 0 or cur_hp > max_hp + 1:
                    return
                refs = SelfRefs(
                    char_serialize=char_obj,
                    user_fight_attr=attr_obj,
                    char_id=known_uid,
                    cur_hp=cur_hp,
                    max_hp=max_hp,
                    uid_off=uid_off_hint,
                    attr_slot_off=attr_slot_off_hint,
                    cur_hp_off=cur_off_hint,
                    max_hp_off=max_off_hint,
                    hp_width=hp_width_hint,
                )
                with lock:
                    if result_box[0] is None:
                        result_box[0] = refs
                        stop_event.set()

            with ThreadPoolExecutor(max_workers=8) as ex:
                list(ex.map(verify_one, uid_addrs))

            print(f"[value-anchor warm] verified {len(uid_addrs)} candidates "
                  f"in {time.time()-t_verify:.2f}s "
                  f"(8 workers, hit={result_box[0] is not None})")
            if result_box[0] is not None:
                return result_box[0]

        # Cached offsets didn't pan out → fall through to full first_run-style
        # ptr-deref + HP-adjacency search using last_max_hp as the HP target.
        if last_max_hp <= 0:
            return None  # no max_hp to search for; need TCP first_run
        return self._va_resolve_self_offline(uid_addrs, known_uid, last_max_hp)

    def _value_anchor_first_run(self) -> SelfRefs:
        """Pure-value first_run: zero dump.cs dependency.

        1. TCP supplies (uid, cur_hp, max_hp).
        2. Cython scans UID across the heap → uid_addrs.
        3. For each uid_addr, try guesses for K1 = obj's UID-field offset.
        4. obj_base = uid_addr - K1; check obj_base+0 looks like a klass
           pointer (lives inside some loaded module).
        5. Within obj_base+8..+0x300 scan 8-byte aligned ptr slots; deref each.
        6. In the deref'd region (0..0x100) find an i32 == tcp_max_hp with an
           adjacent i32 in {tcp_cur_hp} ∪ TCP HP history within ±8 bytes.
        7. The unique passing tuple identifies (CharSerialize, UserFightAttr,
           UID offset, Attr-slot offset, CurHp/MaxHp offsets).
        """
        from .tcp_source import TcpSnapshotSource

        with TcpSnapshotSource() as src:
            print("[value-anchor] waiting for TCP uid+hp+max_hp ...")
            ready = src.wait_ready(timeout=120.0)
            print(f"[value-anchor] TCP ready: uid={ready['uid']} "
                  f"hp={ready['hp']}/{ready['max_hp']} name={ready['name']!r}")
            tcp_uid = int(ready["uid"])
            # Give the bridge a couple more seconds so SyncContainerData /
            # SyncSelfState etc. can populate the all_seen_* sets with both
            # the "first reported" HP (e.g. base 483921) and the
            # post-effect "current" HP (e.g. effective 387080).
            time.sleep(2.0)
            # Use all_seen_*: rolling 1s window can drop the very first
            # SyncContainerData value before we get to scan.
            max_hp_candidates = src.all_seen_max_hp()
            hp_values = src.all_seen_hp()
            if int(ready["max_hp"]) > 0:
                max_hp_candidates.add(int(ready["max_hp"]))
            if int(ready["hp"]) > 0:
                hp_values.add(int(ready["hp"]))
            if not max_hp_candidates:
                raise SmartLocatorError(
                    "value-anchor: TCP never reported a non-zero max_hp; "
                    "wait longer or trigger a SyncContainerData (relog / map change)."
                )
            print(f"[value-anchor] all_seen_max_hp ({len(max_hp_candidates)}): "
                  f"{sorted(max_hp_candidates)}")
            print(f"[value-anchor] all_seen_hp     ({len(hp_values)}): "
                  f"{sorted(hp_values)}")

            uid_addrs = self._va_scan_uid_full_heap(tcp_uid)
            if not uid_addrs:
                raise SmartLocatorError(
                    f"value-anchor: no i64 == {tcp_uid} found in private heap. "
                    "TCP UID may be reported in a non-standard encoding, or game "
                    "writes UID with a different width. Cannot proceed."
                )

            # ── Strategy 1: UID-forward (scan UID, look for nearby HP) ──
            print(f"[value-anchor v1] forward search ({len(uid_addrs)} UID candidates) ...")
            verified = self._va_verify_with_hp(
                uid_addrs, max_hp_candidates, hp_values, src,
            )

            # ── Strategy 2: HP-reverse (when v1 finds nothing) ──
            # HP fields are much rarer in memory than UID values, so reverse
            # search has higher signal-to-noise. Cost: 2-3 extra full-heap scans.
            if not verified:
                print(f"[value-anchor v1] 0 hits; trying v2 (HP-reverse) ...")
                verified = self._va_verify_via_hp_reverse(
                    tcp_uid, max_hp_candidates, hp_values,
                )

            if not verified:
                raise SmartLocatorError(
                    f"value-anchor: both v1 (UID-forward, {len(uid_addrs)} cands) "
                    f"and v2 (HP-reverse) failed. max_hp_candidates="
                    f"{sorted(max_hp_candidates)}, hp_values={sorted(hp_values)}. "
                    "Possibilities: HP encoded as float / compressed 16-bit; "
                    "UID and HP live in unrelated object trees; first-snapshot "
                    "max_hp value never made it into all_seen_max_hp."
                )
            if len(verified) > 1:
                verified = self._va_disambiguate(verified)

            v = verified[0]
            print(f"[value-anchor] SELF: char_serialize=0x{v['char_obj']:X} "
                  f"(UID@+0x{v['uid_off']:X}, Attr@+0x{v['attr_slot_off']:X}) "
                  f"user_fight_attr=0x{v['attr_obj']:X} "
                  f"(CurHp@+0x{v['cur_off']:X}={v['cur_hp']}, "
                  f"MaxHp@+0x{v['max_off']:X}={v['max_hp']})")
            return SelfRefs(
                char_serialize=v["char_obj"],
                user_fight_attr=v["attr_obj"],
                char_id=tcp_uid,
                cur_hp=v["cur_hp"],
                max_hp=v["max_hp"],
                uid_off=v["uid_off"],
                attr_slot_off=v["attr_slot_off"],
                cur_hp_off=v["cur_off"],
                max_hp_off=v["max_off"],
                hp_width=v["hp_width"],
            )

    # ───────── value-anchor internals ─────────

    def _va_parallel_full_heap_scan(self, scan_fn,
                                    *, n_workers: int = 4,
                                    max_region_size: Optional[int] = None) -> list:
        """Generic parallel full-heap RPM + Cython scan.

        scan_fn(bytes) → list[off]  OR  list[(off, *extras)]

        Returns flat list of (region_base, off, *extras) for every hit
        across all readable private regions (skipping > max_region_size).
        Multiple worker threads share the same pymem handle — RPM itself is
        thread-safe, and pymem's read_bytes is a thin ctypes wrapper.
        """
        import threading
        from concurrent.futures import ThreadPoolExecutor

        if max_region_size is None:
            max_region_size = self._VA_MAX_REGION_SIZE
        regions = [r for r in self.pm.iter_regions(
            only_readable=True, only_private=True)
            if r.size <= max_region_size]

        results: list = []
        lock = threading.Lock()

        def scan_one(region):
            buf = self.pm.read_bytes(region.base, region.size)
            if buf is None:
                return
            offs = scan_fn(buf)
            if not offs:
                return
            with lock:
                for item in offs:
                    if isinstance(item, tuple):
                        results.append((region.base, *item))
                    else:
                        results.append((region.base, item))

        if n_workers > 1:
            with ThreadPoolExecutor(max_workers=n_workers) as ex:
                list(ex.map(scan_one, regions))
        else:
            for r in regions:
                scan_one(r)
        return results

    def _va_scan_uid_full_heap(self, tcp_uid: int,
                               *, n_workers: int = 8) -> List[int]:
        """Full-heap i64 == tcp_uid scan with parallel RPM.

        Cython AVX2 is fast (>16 GB/s) but the bottleneck is ReadProcessMemory
        copying 10+ GB across the process boundary (~500 MB/s sequential).
        N threads typically yield ~2-4x — RPM is thread-safe even on the same handle.
        """
        from mem_probe import cy_memscan as _cy
        uid64 = int(tcp_uid) & 0xFFFFFFFFFFFFFFFF
        t0 = time.time()
        results = self._va_parallel_full_heap_scan(
            lambda buf: _cy.find_aligned_u64(buf, uid64),
            n_workers=n_workers,
        )
        hits = [base + off for base, off in results]
        print(f"[value-anchor] uid scan: {len(hits)} hits "
              f"({time.time()-t0:.2f}s, workers={n_workers})")
        return hits

    def _va_verify_with_hp(self, uid_addrs: List[int], max_hp_candidates: set,
                           hp_values: set, src) -> List[dict]:
        """For each UID hit, try to identify the surrounding CharSerialize and
        find a reachable UserFightAttr containing one of `max_hp_candidates`
        adjacent to a `hp_values`-matching cur_hp.

        Returns list of dicts:
            {char_obj, uid_off, attr_slot_off, attr_obj,
             cur_off, max_off, cur_hp, max_hp, hp_width,
             klass_ptr, attr_klass_ptr}
        """
        modules = self.pm.list_modules()
        mod_ranges = sorted([(m.base, m.base + m.size) for m in modules])
        mod_bases = [r[0] for r in mod_ranges]
        import bisect

        def in_module(addr: int) -> bool:
            i = bisect.bisect_right(mod_bases, addr) - 1
            if i < 0:
                return False
            base, end = mod_ranges[i]
            return base <= addr < end

        verified: List[dict] = []
        verbose = (len(uid_addrs) <= 16)
        for uid_addr in uid_addrs:
            for k1 in self._VA_UID_OFF_TRIES:
                obj_base = uid_addr - k1
                klass_blob = self.pm.read_bytes(obj_base, 8)
                if not klass_blob:
                    continue
                klass_ptr = int.from_bytes(klass_blob, "little")
                if not in_module(klass_ptr):
                    continue
                char_blob = self.pm.read_bytes(obj_base, self._VA_CHAR_BODY_RADIUS)
                if not char_blob:
                    continue
                hit = self._va_search_attr_in_char(
                    obj_base, char_blob, k1, max_hp_candidates, hp_values, in_module,
                )
                if hit is not None:
                    verified.append(hit)
                    if verbose:
                        print(f"  [value-anchor 0x{obj_base:X}] UID@+0x{k1:X}, "
                              f"Attr@+0x{hit['attr_slot_off']:X} → "
                              f"UserFightAttr 0x{hit['attr_obj']:X} "
                              f"hp={hit['cur_hp']}/{hit['max_hp']} "
                              f"(CurHp@+0x{hit['cur_off']:X}, "
                              f"MaxHp@+0x{hit['max_off']:X}, "
                              f"width=i{hit['hp_width']*8})")
                    break  # found a working K1 for this uid_addr
        return verified

    def _va_search_attr_in_char(self, obj_base: int, char_blob: bytes,
                                uid_off: int,
                                max_hp_candidates: set, hp_values: set,
                                in_module) -> Optional[dict]:
        """Inside a CharSerialize candidate, find an Attr-ptr slot whose
        deref'd object contains a max_hp ∈ candidates adjacent to a cur_hp
        ∈ hp_values. Tries both i32 (4-byte) and i64 (8-byte) encodings.

        IL2CPP standard layout: CurHp at +0x10 (long), MaxHp at +0x18 (long)
        — i.e. cur_hp PRECEDES max_hp by 8 bytes. We try both orderings.
        """
        for slot_off in range(8, len(char_blob) - 8 + 1, 8):
            if slot_off == uid_off:
                continue  # UID slot, not a ptr
            ptr = int.from_bytes(char_blob[slot_off:slot_off + 8], "little")
            if not (0x10000 <= ptr <= 0x7FFFFFFFFFFF):
                continue
            attr_blob = self.pm.read_bytes(ptr, self._VA_ATTR_BODY_RADIUS)
            if not attr_blob or len(attr_blob) < 16:
                continue
            attr_klass = int.from_bytes(attr_blob[0:8], "little")
            if not in_module(attr_klass):
                continue

            # ── Pass 1: i64 (most likely; dump.cs marks CurHp/MaxHp as `long`) ──
            for off in range(0, len(attr_blob) - 8 + 1, 8):
                v64 = int.from_bytes(attr_blob[off:off + 8], "little")
                if v64 not in max_hp_candidates:
                    continue
                for delta in (-8, 8):
                    co = off + delta
                    if co < 0 or co + 8 > len(attr_blob):
                        continue
                    cv64 = int.from_bytes(attr_blob[co:co + 8], "little")
                    if cv64 in hp_values and 0 <= cv64 <= v64:
                        return self._va_make_hit(
                            obj_base, uid_off, slot_off, ptr,
                            cur_off=co, max_off=off, cur_hp=cv64, max_hp=v64,
                            hp_width=8, attr_klass=attr_klass,
                        )
            # ── Pass 2: i32 (fallback for compressed layouts) ──
            for off in range(0, len(attr_blob) - 4 + 1, 4):
                v32 = int.from_bytes(attr_blob[off:off + 4], "little")
                if v32 not in max_hp_candidates:
                    continue
                # Make sure it's not the lower 32 bits of an unrelated i64
                if off + 8 <= len(attr_blob):
                    v_hi = int.from_bytes(attr_blob[off + 4:off + 8], "little")
                    if v_hi != 0:
                        continue  # high half non-zero → real value is i64
                for delta in self._VA_HP_DELTA_TRIES:
                    co = off + delta
                    if co < 0 or co + 4 > len(attr_blob):
                        continue
                    cv32 = int.from_bytes(attr_blob[co:co + 4], "little")
                    if cv32 in hp_values and 0 <= cv32 <= v32:
                        return self._va_make_hit(
                            obj_base, uid_off, slot_off, ptr,
                            cur_off=co, max_off=off, cur_hp=cv32, max_hp=v32,
                            hp_width=4, attr_klass=attr_klass,
                        )
        return None

    def _va_disambiguate(self, verified: List[dict]) -> List[dict]:
        """When multiple value-anchor candidates pass, score by structural
        richness: real SELF.CharSerialize is a fully-populated client object
        with many child-struct ptrs (CharBase, RoleLevel, ProfessionList,
        EnergyItem, ...). Mirror / cached / proxy objects are sparser.

        Score = count of 8-byte-aligned slots in [obj+0, obj+0x300) whose
        value lies in a typical user-space heap pointer range.
        Highest score wins; ties → first.
        """
        scored: List[Tuple[int, dict]] = []
        for v in verified:
            blob = self.pm.read_bytes(v["char_obj"], 0x300)
            if not blob:
                scored.append((0, v))
                continue
            score = 0
            for off in range(0, len(blob) - 8 + 1, 8):
                p = int.from_bytes(blob[off:off + 8], "little")
                # User-space heap pointer range (above 4 GiB, below sentinel)
                if 0x100000000 <= p <= 0x7FFFFFFFFFFF:
                    score += 1
            scored.append((score, v))
        scored.sort(key=lambda s: -s[0])
        print(f"[value-anchor] {len(scored)} candidates; "
              f"pointer-density scores (heap-shaped slots in 0..0x300):")
        for sc, v in scored:
            print(f"  0x{v['char_obj']:X}: {sc} ptr-shaped slots")
        if len(scored) >= 2 and scored[0][0] == scored[1][0]:
            print(f"[value-anchor] WARN top-2 tied; arbitrary pick — could "
                  f"misidentify SELF if both objects are equally populated.")
        # Return picked candidate first (so the caller's verified[0] is the winner)
        return [scored[0][1]] + [s[1] for s in scored[1:]]

    def _va_make_hit(self, obj_base, uid_off, attr_slot_off, attr_obj,
                     *, cur_off, max_off, cur_hp, max_hp, hp_width, attr_klass):
        return {
            "char_obj": obj_base,
            "uid_off": uid_off,
            "attr_slot_off": attr_slot_off,
            "attr_obj": attr_obj,
            "cur_off": cur_off,
            "max_off": max_off,
            "cur_hp": cur_hp,
            "max_hp": max_hp,
            "hp_width": hp_width,
            "klass_ptr": int.from_bytes(
                self.pm.read_bytes(obj_base, 8) or b"\0" * 8, "little"),
            "attr_klass_ptr": attr_klass,
        }

    # ───── Phase 1: auto-discover substruct offsets (no dump.cs needed) ─────

    def discover_substructs(self, char_obj: int, *,
                            level_base: int = 0,
                            profession_id: int = 0,
                            energy_max: int = 0,
                            fight_point: int = 0,
                            scan_radius: int = 0x300) -> dict:
        """Heuristically locate child-struct ptr slots inside char_serialize.

        Without dump.cs we don't know the offsets of CharBase/RoleLevel/
        ProfessionList/EnergyItem/SeasonMedalInfo. Strategy:
          1. Enumerate all 8-byte aligned ptr slots in char_obj+0..+0x300
          2. Deref each ptr; read 0x40 bytes
          3. Match TCP ground truth values to identify which sub-struct is which:
             - ProfessionList contains profession_id (i32)
             - RoleLevel contains level_base (i32)
             - EnergyItem contains energy_max (i32)
             - CharBase contains fight_point (i32) and a string ptr (Name)
          4. Return discovered mapping {role_name: {slot_off, klass_ptr}}

        Caller passes whatever TCP ground truth values are available; missing
        ground truths just mean we can't identify that particular sub-struct.
        Identifications are best-effort; multiple matches → ambiguous, dropped.
        """
        results: dict = {}
        char_blob = self.pm.read_bytes(char_obj, scan_radius)
        if not char_blob:
            return results
        modules = self.pm.list_modules()
        mod_ranges = sorted([(m.base, m.base + m.size) for m in modules])
        mod_bases = [r[0] for r in mod_ranges]
        import bisect

        def in_module(addr: int) -> bool:
            i = bisect.bisect_right(mod_bases, addr) - 1
            if i < 0:
                return False
            base, end = mod_ranges[i]
            return base <= addr < end

        # Build "wanted needles" — values to look for in deref'd substruct bodies
        targets = {}
        if level_base > 0:
            targets["role_level"] = ("level", int(level_base))
        if profession_id > 0:
            targets["profession_list"] = ("prof_id", int(profession_id))
        if energy_max > 0:
            targets["energy_item"] = ("energy_max", int(energy_max))
        if fight_point > 0:
            targets["char_base"] = ("fight_point", int(fight_point))
        if not targets:
            return results

        # Walk slots
        candidates_per_target: dict = {k: [] for k in targets}
        for slot_off in range(8, len(char_blob) - 8 + 1, 8):
            ptr = int.from_bytes(char_blob[slot_off:slot_off + 8], "little")
            if not (0x10000 <= ptr <= 0x7FFFFFFFFFFF):
                continue
            sub_blob = self.pm.read_bytes(ptr, 0x80)
            if not sub_blob or len(sub_blob) < 16:
                continue
            sub_klass = int.from_bytes(sub_blob[0:8], "little")
            if not in_module(sub_klass):
                continue
            # Probe each target's needle within the substruct body
            for role_name, (field_label, needle) in targets.items():
                # Try i32 (4-byte aligned, common case for profession/level/etc.)
                for off in range(0, len(sub_blob) - 4 + 1, 4):
                    v = int.from_bytes(sub_blob[off:off + 4], "little")
                    if v == needle:
                        candidates_per_target[role_name].append(
                            (slot_off, ptr, sub_klass, off, "i32"))
                        break

        # Pick unambiguous matches only
        for role_name, cands in candidates_per_target.items():
            if len(cands) == 1:
                slot_off, ptr, sub_klass, field_off, width = cands[0]
                results[role_name] = {
                    "slot_off": slot_off,
                    "klass_ptr": _hex(sub_klass),
                    "obj_addr": _hex(ptr),
                    "discovered_field_off": field_off,
                    "discovered_field_width": width,
                }
            elif len(cands) > 1:
                # Take first; warn
                slot_off, ptr, sub_klass, field_off, width = cands[0]
                results[role_name] = {
                    "slot_off": slot_off,
                    "klass_ptr": _hex(sub_klass),
                    "obj_addr": _hex(ptr),
                    "ambiguous": len(cands),
                }
        return results

    # ───── reverse value-anchor (HP-first, falls back when UID-forward fails) ─────

    # Likely positions of the Attr ptr inside CharSerialize and the UID inside it.
    # IL2CPP standard layout puts CurHp/MaxHp at +0x10/+0x18 of UserFightAttr,
    # CharId at +0x10 of CharSerialize, and Attr ptr around +0x80~+0x90.
    _VA_ATTR_SLOT_TRIES = (0x88, 0x80, 0x90, 0x78, 0x70, 0x60,
                           0xA0, 0x98, 0xA8, 0xB0)
    _VA_HP_OFF_IN_UFA_TRIES = (0x10, 0x8, 0x18, 0x20)
    _VA_UID_PTR_RANGE = (0x10000, 0x7FFFFFFFFFFF)
    _VA_MAX_REGION_SIZE = 256 * 1024 * 1024

    def _va_verify_via_hp_reverse(self, tcp_uid: int, max_hp_candidates: set,
                                  hp_values: set) -> List[dict]:
        """Reverse algorithm — when UID-forward finds nothing, scan from the
        rare HP value side and back-resolve to CharSerialize via ptr-to-UFA.

        Stages:
            1. Cython scan i64 == each value in max_hp_candidates → max_hp_addrs.
               (i64 hit count is typically <100 for game-sized values like HP.)
            2. For each max_hp_addr, check ±8 byte adjacency for an i64 == any
               value in hp_values → confirmed (cur_hp_addr, max_hp_addr) pair.
            3. For each pair, propose ufa_base = min(addrs) - K3 for K3 ∈
               {0x10, 0x8, 0x18, 0x20} (CurHp's offset inside UserFightAttr).
            4. Cython multi-target scan for any 8-byte-aligned ptr equal to
               any of the proposed ufa_bases → these are CharSerialize.Attr slots.
            5. For each ptr_addr hit, propose char_obj = ptr_addr - K2 for
               K2 ∈ Attr slot guesses; verify char_obj + uid_off == tcp_uid
               for uid_off ∈ {0x10, 0x8, 0x18, 0x20, 0x40}.

        Returns the same dict shape as _va_make_hit.
        """
        from mem_probe import cy_memscan as _cy
        pm = self.pm

        # Stage 1: scan each max_hp value (i64) — parallel RPM
        max_hp_addrs: dict = {}
        for mhv in sorted(max_hp_candidates):
            if mhv <= 0:
                continue
            mhv_u = int(mhv) & 0xFFFFFFFFFFFFFFFF
            t1 = time.time()
            results = self._va_parallel_full_heap_scan(
                lambda buf, n=mhv_u: _cy.find_aligned_u64(buf, n),
            )
            addrs = [base + off for base, off in results]
            max_hp_addrs[mhv] = addrs
            print(f"[value-anchor v2] i64 == {mhv}: {len(addrs)} hits "
                  f"({time.time()-t1:.2f}s)")

        # Stage 2: find adjacent (cur, max) HP pairs (±8)
        # Each pair → multiple proposed (ufa_base, cur_off, max_off) tuples
        # depending on which CurHp offset we assume inside UserFightAttr.
        ufa_proposals: List[dict] = []
        for max_v, addrs in max_hp_addrs.items():
            for max_addr in addrs:
                for delta in (-8, 8):
                    cur_addr = max_addr + delta
                    blob = pm.read_bytes(cur_addr, 8)
                    if not blob:
                        continue
                    cur_v = int.from_bytes(blob, "little")
                    if cur_v not in hp_values:
                        continue
                    if cur_v < 0 or cur_v > max_v:
                        continue
                    # Standard: CurHp precedes MaxHp by 8 bytes.
                    # cur_in_ufa_off ∈ tries; ufa_base = cur_addr - K3
                    for k3 in self._VA_HP_OFF_IN_UFA_TRIES:
                        ufa_base = cur_addr - k3
                        cur_off = cur_addr - ufa_base   # = k3
                        max_off = max_addr - ufa_base
                        if max_off < 0 or max_off > 0x100:
                            continue
                        ufa_proposals.append({
                            "ufa_base": ufa_base,
                            "cur_off": cur_off,
                            "max_off": max_off,
                            "cur_hp": cur_v,
                            "max_hp": max_v,
                        })
        if not ufa_proposals:
            print("[value-anchor v2] no (cur, max) HP adjacency pairs found")
            return []
        # Dedupe ufa_base
        ufa_bases = sorted({p["ufa_base"] for p in ufa_proposals})
        print(f"[value-anchor v2] {len(ufa_proposals)} HP pairs, "
              f"{len(ufa_bases)} unique candidate UFA bases")

        # Stage 3: cy multi-target scan for ptrs to ufa_bases (parallel RPM)
        t0 = time.time()
        results = self._va_parallel_full_heap_scan(
            lambda buf, bases=ufa_bases: _cy.find_aligned_u64_in_set(buf, bases),
        )
        ptr_hits: List[Tuple[int, int]] = [
            (base + off, val) for base, off, val in results
        ]
        print(f"[value-anchor v2] ptr-to-ufa scan: {len(ptr_hits)} hits "
              f"({time.time()-t0:.2f}s)")

        # Stage 4: for each ptr hit, infer CharSerialize and verify UID
        verified: List[dict] = []
        seen_chars = set()
        for ptr_addr, ufa_base in ptr_hits:
            for attr_off in self._VA_ATTR_SLOT_TRIES:
                char_base = ptr_addr - attr_off
                if char_base in seen_chars:
                    continue
                klass_blob = pm.read_bytes(char_base, 8)
                if not klass_blob:
                    continue
                klass_ptr = int.from_bytes(klass_blob, "little")
                lo, hi = self._VA_UID_PTR_RANGE
                if not (lo <= klass_ptr <= hi):
                    continue
                for uid_off in self._VA_UID_OFF_TRIES:
                    if uid_off == attr_off:
                        continue
                    uid_blob = pm.read_bytes(char_base + uid_off, 8)
                    if not uid_blob:
                        continue
                    if int.from_bytes(uid_blob, "little") != tcp_uid:
                        continue
                    # MATCH — find the proposal that produced this ufa_base
                    proposal = next((p for p in ufa_proposals
                                     if p["ufa_base"] == ufa_base), None)
                    if proposal is None:
                        continue
                    seen_chars.add(char_base)
                    attr_klass = int.from_bytes(
                        pm.read_bytes(ufa_base, 8) or b"\0" * 8, "little")
                    verified.append({
                        "char_obj": char_base,
                        "uid_off": uid_off,
                        "attr_slot_off": attr_off,
                        "attr_obj": ufa_base,
                        "cur_off": proposal["cur_off"],
                        "max_off": proposal["max_off"],
                        "cur_hp": proposal["cur_hp"],
                        "max_hp": proposal["max_hp"],
                        "hp_width": 8,
                        "klass_ptr": klass_ptr,
                        "attr_klass_ptr": attr_klass,
                    })
                    print(f"  [value-anchor v2 0x{char_base:X}] UID@+0x{uid_off:X}, "
                          f"Attr@+0x{attr_off:X} → UFA 0x{ufa_base:X} "
                          f"hp={proposal['cur_hp']}/{proposal['max_hp']} "
                          f"(CurHp@+0x{proposal['cur_off']:X}, "
                          f"MaxHp@+0x{proposal['max_off']:X}, width=i64)")
                    break  # found a UID for this char_base; stop trying uid_offs
                else:
                    continue
                break  # found a char_base for this ptr_addr; stop trying attr_offs
        return verified

    def _va_resolve_self_offline(self, uid_addrs: List[int], known_uid: int,
                                 last_max_hp: int) -> Optional[SelfRefs]:
        """warm path with no TCP: use last_max_hp persisted in anchors as the
        HP target. cur_hp is whatever is adjacent (no value verification —
        we've already verified UID + max_hp + adjacency, which is unique enough).
        """
        modules = self.pm.list_modules()
        mod_ranges = sorted([(m.base, m.base + m.size) for m in modules])
        mod_bases = [r[0] for r in mod_ranges]
        import bisect

        def in_module(addr: int) -> bool:
            i = bisect.bisect_right(mod_bases, addr) - 1
            if i < 0:
                return False
            base, end = mod_ranges[i]
            return base <= addr < end

        # In offline warm without TCP, we use last_max_hp as a *weak* anchor
        # (max_hp can have shifted significantly due to gear/buff). Use ±50%
        # slack — purely to disambiguate the SELF candidate from unrelated
        # i32 values; klass_ptr + UID matching does the real validation.
        max_hp_slack = max(int(last_max_hp * 0.50), 5000)
        for uid_addr in uid_addrs:
            for k1 in self._VA_UID_OFF_TRIES:
                obj_base = uid_addr - k1
                klass_blob = self.pm.read_bytes(obj_base, 8)
                if not klass_blob:
                    continue
                if not in_module(int.from_bytes(klass_blob, "little")):
                    continue
                char_blob = self.pm.read_bytes(obj_base, self._VA_CHAR_BODY_RADIUS)
                if not char_blob:
                    continue
                for slot_off in range(8, len(char_blob) - 8 + 1, 8):
                    if slot_off == k1:
                        continue
                    ptr = int.from_bytes(char_blob[slot_off:slot_off + 8], "little")
                    if not (0x10000 <= ptr <= 0x7FFFFFFFFFFF):
                        continue
                    attr_blob = self.pm.read_bytes(ptr, self._VA_ATTR_BODY_RADIUS)
                    if not attr_blob or len(attr_blob) < 16:
                        continue
                    attr_klass = int.from_bytes(attr_blob[0:8], "little")
                    if not in_module(attr_klass):
                        continue
                    for off in range(0, len(attr_blob) - 4 + 1, 4):
                        v32 = int.from_bytes(attr_blob[off:off + 4], "little")
                        if abs(v32 - last_max_hp) > max_hp_slack:
                            continue
                        for delta in self._VA_HP_DELTA_TRIES:
                            co = off + delta
                            if co < 0 or co + 4 > len(attr_blob):
                                continue
                            cv32 = int.from_bytes(attr_blob[co:co + 4], "little")
                            if 0 <= cv32 <= v32 + 100:  # cur_hp within max_hp
                                return SelfRefs(
                                    char_serialize=obj_base,
                                    user_fight_attr=ptr,
                                    char_id=known_uid,
                                    cur_hp=cv32,
                                    max_hp=v32,
                                )
        return None

    # ───────── construction / persistence ─────────

    def _build_refs(self, obj: int, *, char_id: int = 0, attr: int = 0,
                    cur_hp: int = 0, max_hp: int = 0) -> SelfRefs:
        sr = self.sr
        if not attr:
            attr = sr.read_ptr_field(obj, SELF_CLASS, SENTINEL_FIELD) or 0
        if attr and not max_hp:
            cur_hp = sr.read_field(attr, SENTINEL_CLASS, HP_FIELD) or 0
            max_hp = sr.read_field(attr, SENTINEL_CLASS, MAX_HP_FIELD) or 0
        if not char_id:
            char_id = sr.read_field(obj, SELF_CLASS, UID_FIELD) or 0
        substructs = {}
        for fname in SUBSTRUCT_FIELDS:
            substructs[fname] = sr.read_ptr_field(obj, SELF_CLASS, fname) or 0
        return SelfRefs(
            char_serialize=obj,
            user_fight_attr=attr,
            char_base=substructs.get("CharBase", 0),
            role_level=substructs.get("RoleLevel", 0),
            profession_list=substructs.get("ProfessionList", 0),
            energy_item=substructs.get("EnergyItem", 0),
            season_medal_info=substructs.get("SeasonMedalInfo", 0),
            char_id=int(char_id),
            cur_hp=int(cur_hp),
            max_hp=int(max_hp),
        )

    def _persist(self, refs: SelfRefs, known_uid: int) -> None:
        anchors = _load_anchors(self.anchors_path)
        sl = anchors.get("smart_locator", {})
        # First time we see this UID? Stamp the provenance.
        if int(sl.get("known_uid", 0) or 0) != int(known_uid):
            sl["known_uid"] = int(known_uid)
            sl["known_uid_set_at"] = time.time()
            sl["known_uid_set_via"] = refs.located_via
        sl["last_pid"] = self.pm.pid
        sl["last_self_obj"]          = _hex(refs.char_serialize)
        sl["last_user_fight_attr"]   = _hex(refs.user_fight_attr)
        sl["last_char_base"]         = _hex(refs.char_base)
        sl["last_role_level"]        = _hex(refs.role_level)
        sl["last_profession_list"]   = _hex(refs.profession_list)
        sl["last_energy_item"]       = _hex(refs.energy_item)
        sl["last_season_medal_info"] = _hex(refs.season_medal_info)
        sl["last_cur_hp"] = int(refs.cur_hp)
        sl["last_max_hp"] = int(refs.max_hp)
        # Field offsets — only persist when known (>= 0). Used by
        # value-anchor warm path to skip ptr scanning on subsequent runs.
        if refs.uid_off >= 0:
            sl["last_uid_off"] = int(refs.uid_off)
        if refs.attr_slot_off >= 0:
            sl["last_attr_slot_off"] = int(refs.attr_slot_off)
        if refs.cur_hp_off >= 0:
            sl["last_cur_hp_off"] = int(refs.cur_hp_off)
        if refs.max_hp_off >= 0:
            sl["last_max_hp_off"] = int(refs.max_hp_off)
        if refs.hp_width in (4, 8):
            sl["last_hp_width"] = int(refs.hp_width)
        sl["last_located_at"] = time.time()
        sl["last_located_via"] = refs.located_via
        sl["last_elapsed_s"] = round(refs.elapsed_s, 3)

        # Phase 8: schema_v2 nested anchors block (forward-compatible).
        # We write BOTH legacy flat fields (for backward compat) AND a
        # nested `anchors.self` mirror for future multi-target consumers.
        sl["schema_version"] = 2
        existing_anchors = sl.get("anchors") if isinstance(sl.get("anchors"), dict) else {}
        self_anchor = dict(existing_anchors.get("self") or {})
        self_anchor.update({
            "klass_name": SELF_CLASS,
            "obj_addr": _hex(refs.char_serialize),
            "uid_off": int(refs.uid_off) if refs.uid_off >= 0 else self_anchor.get("uid_off", -1),
            "attr_slot_off": int(refs.attr_slot_off) if refs.attr_slot_off >= 0 else self_anchor.get("attr_slot_off", -1),
            "cur_hp_off": int(refs.cur_hp_off) if refs.cur_hp_off >= 0 else self_anchor.get("cur_hp_off", -1),
            "max_hp_off": int(refs.max_hp_off) if refs.max_hp_off >= 0 else self_anchor.get("max_hp_off", -1),
            "hp_width": int(refs.hp_width) if refs.hp_width in (4, 8) else self_anchor.get("hp_width", 8),
        })
        existing_substructs = self_anchor.get("substructs") if isinstance(self_anchor.get("substructs"), dict) else {}
        substructs_block = dict(existing_substructs)
        substructs_block["user_fight_attr"] = {
            "obj_addr": _hex(refs.user_fight_attr),
        }
        # Persist any non-zero substruct ptrs from refs (Phase 1 enrichment)
        for role, val in (
            ("char_base", refs.char_base),
            ("role_level", refs.role_level),
            ("profession_list", refs.profession_list),
            ("energy_item", refs.energy_item),
            ("season_medal_info", refs.season_medal_info),
        ):
            if val:
                cur = dict(substructs_block.get(role) or {})
                cur["obj_addr"] = _hex(val)
                substructs_block[role] = cur
        self_anchor["substructs"] = substructs_block
        anchors_block = dict(existing_anchors)
        anchors_block["self"] = self_anchor
        sl["anchors"] = anchors_block

        anchors["smart_locator"] = sl
        _save_anchors(self.anchors_path, anchors)

    # Phase 8: write a non-self anchor block (called by Phase 3/4 locators)
    def persist_anchor_block(self, name: str, payload: dict) -> None:
        """Persist a multi-target anchor (scene_manager / entity_collection /
        buff_system / combat_log / ...). `payload` is JSON-able."""
        anchors = _load_anchors(self.anchors_path)
        sl = anchors.get("smart_locator", {})
        sl.setdefault("schema_version", 2)
        anchors_block = dict(sl.get("anchors") or {})
        anchors_block[name] = payload
        sl["anchors"] = anchors_block
        anchors["smart_locator"] = sl
        _save_anchors(self.anchors_path, anchors)


# ───────────────────────── CLI ─────────────────────────

def _print_refs(refs: SelfRefs) -> None:
    print()
    print(f"  via            : {refs.located_via}")
    print(f"  elapsed        : {refs.elapsed_s*1000:.0f} ms")
    print(f"  char_serialize : {_hex(refs.char_serialize)}")
    print(f"  user_fight_attr: {_hex(refs.user_fight_attr)}")
    print(f"  char_base      : {_hex(refs.char_base)}")
    print(f"  role_level     : {_hex(refs.role_level)}")
    print(f"  profession_list: {_hex(refs.profession_list)}")
    print(f"  energy_item    : {_hex(refs.energy_item)}")
    print(f"  season_medal   : {_hex(refs.season_medal_info)}")
    print(f"  char_id        : {refs.char_id}")
    print(f"  hp             : {refs.cur_hp}/{refs.max_hp}")


def cmd_locate(args) -> int:
    if not is_admin():
        print("[warn] not running as admin; OpenProcess may fail.", file=sys.stderr)
    loc = SmartLocator(anchors_path=args.anchors,
                       dump_id=args.dump_id,
                       hp_window_s=args.hp_window,
                       force_value_anchor=getattr(args, "no_dump", False))
    try:
        refs = loc.locate(
            allow_tcp_fallback=(args.mode != "warm"),
            force_first_run=(args.mode == "first"),
        )
    except StarProcessError as e:
        print(f"[fail] attach: {e}", file=sys.stderr)
        return 2
    except SmartLocatorError as e:
        print(f"[fail] locator: {e}", file=sys.stderr)
        return 3
    _print_refs(refs)
    return 0


def cmd_status(args) -> int:
    loc = SmartLocator(anchors_path=args.anchors)
    sl = loc.status()
    if not sl:
        print(f"[empty] {args.anchors} has no smart_locator block")
        return 0
    print(f"[anchors] {args.anchors}")
    for k, v in sl.items():
        if isinstance(v, float) and 1e9 < v < 2e10:
            v = f"{v:.0f} ({time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(v))})"
        print(f"  {k:25s} = {v}")
    return 0


def cmd_reset(args) -> int:
    loc = SmartLocator(anchors_path=args.anchors)
    sl = loc.status()
    if not sl.get("known_uid"):
        print("[noop] no known_uid to reset")
        return 0
    print(f"[reset] clearing known_uid={sl['known_uid']}")
    loc.reset_known_uid()
    print(f"[ok] next `locator` invocation will require TCP first_run")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m mem_probe.locator")
    p.add_argument("--anchors", default=ANCHORS_PATH,
                   help="anchors.json path (default: mem_probe/anchors.json)")
    p.add_argument("--dump-id", default=_DEFAULT_DUMP_ID)
    p.add_argument("--hp-window", type=float, default=0.3,
                   help="HP validation window in seconds (default: 0.3)")
    p.add_argument("--no-dump", action="store_true",
                   help="skip the dump.cs path entirely; use value-anchor "
                        "(TCP UID + HP/MaxHP scan) only — works even when the "
                        "IL2CPP dump is stale or missing")
    sub = p.add_subparsers(dest="cmd")

    p_auto = sub.add_parser("auto", help="cache → warm → first (default)")
    p_auto.set_defaults(func=cmd_locate, mode="auto")

    p_warm = sub.add_parser("warm", help="warm-only; raise if no known_uid")
    p_warm.set_defaults(func=cmd_locate, mode="warm")

    p_first = sub.add_parser("first", help="force first_run with TCP")
    p_first.set_defaults(func=cmd_locate, mode="first")

    p_status = sub.add_parser("status", help="print persisted smart_locator state")
    p_status.set_defaults(func=cmd_status)

    p_reset = sub.add_parser("reset", help="clear known_uid (next run = first_run)")
    p_reset.set_defaults(func=cmd_reset)

    args = p.parse_args(argv)
    if not args.cmd:
        # default to auto
        args.func = cmd_locate
        args.mode = "auto"
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
