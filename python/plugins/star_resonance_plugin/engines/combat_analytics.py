# -*- coding: utf-8 -*-
# ACT-facing combat analytics snapshot facade.

from __future__ import annotations

from typing import Any, Dict, List, Optional


def _state_to_dict(state_mgr: Any) -> Dict[str, Any]:
    if state_mgr is None:
        return {}
    try:
        snap = state_mgr.snapshot()
        return snap.to_dict() if snap is not None else {}
    except Exception:
        return {}


def _source_probe_to_dict(source_probe: Any) -> Dict[str, Any]:
    if source_probe is None:
        return {}
    if isinstance(source_probe, dict):
        return dict(source_probe)
    health = getattr(source_probe, "health", None)
    if callable(health):
        try:
            return health() or {}
        except Exception:
            return {}
    out: Dict[str, Any] = {}
    for attr in (
        "mode", "last_error", "last_uid", "last_hp", "last_max_hp",
        "last_profession_id", "last_char_name", "last_skill_cd_count",
        "last_resources", "last_is_dead",
    ):
        if hasattr(source_probe, attr):
            try:
                out[attr] = getattr(source_probe, attr)
            except Exception:
                pass
    return out


def _source_is_active(source: Dict[str, Any]) -> bool:
    if not source:
        return False
    for key in ("running", "alive", "active", "packet_active"):
        if key in source:
            return bool(source.get(key))
    mode = str(source.get("mode") or source.get("data_source") or "").lower()
    return bool(mode and mode not in ("none", "error", "disabled", "off"))


def _normalize_source(source: Dict[str, Any], fallback_data_source: str) -> Dict[str, Any]:
    if not source:
        return {}
    out = dict(source)
    if not str(out.get("data_source") or "").strip():
        out["data_source"] = fallback_data_source
    return out


def _build_sources(source_probe: Any = None,
                   packet_probe: Any = None,
                   memory_probe: Any = None) -> Dict[str, Any]:
    # Build explicit ACT source metadata.
    #
    # ``source_probe`` is the legacy single-source argument.  New runtime callers
    # should pass ``packet_probe`` and ``memory_probe`` separately so the UI can
    # display TCP-primary + memory-fallback instead of collapsing both into the
    # packet slot.
    legacy = packet_probe is None and memory_probe is None and source_probe is not None
    packet = _source_probe_to_dict(packet_probe)
    memory = _source_probe_to_dict(memory_probe)
    if legacy:
        probe = _source_probe_to_dict(source_probe)
        data_source = str(probe.get("data_source") or probe.get("mode") or "").lower()
        if data_source in ("memory", "mem"):
            memory = probe
        else:
            packet = probe
    if packet and isinstance(packet.get("mem"), dict) and not memory:
        memory = dict(packet.get("mem") or {})
    packet = _normalize_source(packet, "tcp")
    memory = _normalize_source(memory, "memory")

    sources: Dict[str, Any] = {}
    if packet:
        sources["packet"] = packet
    if memory:
        sources["memory"] = memory
    if not sources:
        return {}

    packet_active = _source_is_active(packet)
    memory_active = _source_is_active(memory)
    if packet:
        primary = "packet"
    elif memory:
        primary = "memory"
    else:
        primary = "none"

    source_bits: List[str] = []
    if packet:
        source_bits.append(str(packet.get("data_source") or "tcp"))
    if memory:
        mem_label = str(memory.get("data_source") or memory.get("mode") or "memory")
        if mem_label not in source_bits:
            source_bits.append(mem_label)
    sources["summary"] = {
        "data_source": "+".join(source_bits) if source_bits else "none",
        "primary": primary,
        "hybrid": bool(packet and memory),
        "packet_active": packet_active,
        "memory_active": memory_active,
        "fallbacks": ["memory"] if packet and memory else [],
    }
    return sources


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


def boss_state_from_monster_update(monster_data: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    # Convert parser ``MonsterData.to_dict()`` payloads into ACT boss fields.
    #
    # Runtime BossHP can render a directly attacked monster from packet data even
    # when the BossRaid engine is inactive.  ACT snapshots are GameState-based,
    # so keep the same packet HP/break/shield facts in the shared state without
    # marking the raid engine active.
    data = monster_data or {}
    uuid = _safe_int(data.get("uuid"), 0)
    hp = max(0, _safe_int(data.get("hp"), 0))
    max_hp = max(0, _safe_int(data.get("max_hp"), 0))
    if uuid <= 0 or (hp <= 0 and max_hp <= 0):
        return {}
    if bool(data.get("is_dead", False)) and hp <= 0:
        return {}
    total = max_hp if max_hp > 0 else hp
    pct = max(0.0, min(1.0, (hp / total) if total > 0 else 1.0))
    breaking_stage = _safe_int(data.get("breaking_stage"), -1)
    if not data.get("has_break_data", False) and breaking_stage < 0:
        breaking_stage = -1
    return {
        "boss_current_hp": hp,
        "boss_total_hp": total,
        "boss_hp_est_pct": pct,
        "boss_hp_source": "packet",
        "boss_shield_active": bool(data.get("shield_active", False)),
        "boss_shield_pct": max(0.0, min(1.0, _safe_float(data.get("shield_pct"), 0.0))),
        "boss_breaking_stage": breaking_stage,
        "boss_extinction_pct": max(0.0, min(1.0, _safe_float(data.get("extinction_pct"), 0.0))),
        "boss_in_overdrive": bool(data.get("in_overdrive", False)),
        "boss_invincible": bool(data.get("invincible", False)),
    }


def mem_boss_break_override(bridge: Any):
    # Resolve the MEM boss-break override for the boss-HP feeders.
    #
    # Returns ``(breaking_stage, has_break_data, extinction_pct, stop_breaking_ticking)``
    # when MEM currently owns the boss break signal (hybrid/auto/memory + correct base
    # acquired) AND has a live boss, else ``None``. Callers keep TCP shield untouched —
    # shield is never sourced from MEM. A ``None`` result means "leave the existing TCP
    # break fields as-is" (TCP mode, before base acquisition, or no live MEM boss).
    if not bridge:
        return None
    try:
        if bridge.boss_break_source() != 'mem':
            return None
        mb = bridge.get_boss_break_mem()
    except Exception:
        return None
    if not mb:
        return None
    return (
        _safe_int(mb.get('breaking_stage'), -1),
        bool(mb.get('has_break_data', False)),
        max(0.0, min(1.0, _safe_float(mb.get('extinction_pct'), 0.0))),
        bool(mb.get('stop_breaking_ticking', False)),
    )


def _entity_rows(live: Optional[Dict[str, Any]]) -> List[Dict[str, Any]]:
    rows = []
    for index, entity in enumerate((live or {}).get("entities") or [], start=1):
        if not isinstance(entity, dict):
            continue
        rows.append({
            "rank": index,
            "uid": _safe_int(entity.get("uid"), 0),
            "name": str(entity.get("name") or entity.get("display_name") or "Unknown"),
            "profession": str(entity.get("profession") or entity.get("profession_name") or ""),
            "damage": _safe_int(entity.get("damage_total") or entity.get("damage"), 0),
            "heal": _safe_int(entity.get("heal_total") or entity.get("heal"), 0),
            "dps": _safe_int(entity.get("dps"), 0),
            "hps": _safe_int(entity.get("hps"), 0),
            "damage_pct": _safe_float(entity.get("damage_pct"), 0.0),
            "crit_rate": _safe_float(entity.get("crit_rate"), 0.0),
            "is_self": bool(entity.get("is_self", False)),
            # MEM cross-check (DamageDataMgr); per-encounter DPS is the displayed figure,
            # cumulative total kept for reference. 0 when absent.
            "mem_damage_total": _safe_int(entity.get("mem_damage_total"), 0),
            "mem_dps": _safe_int(entity.get("mem_dps"), 0),
        })
    return rows


def _boss_context(context: Optional[Dict[str, Any]],
                  encounter: Optional[Dict[str, Any]] = None,
                  live: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    context = context or {}
    encounter = encounter or {}
    live = live or {}
    hp_source = str(context.get("boss_hp_source") or "none")
    has_packet_boss = bool(
        hp_source != "none"
        and (_safe_int(context.get("boss_current_hp"), 0)
             or _safe_int(context.get("boss_total_hp"), 0))
    )
    encounter_active = bool(
        live.get("encounter_active")
        or str(encounter.get("status") or "") in ("active", "pending_reset")
    )
    return {
        "active": bool(context.get("boss_raid_active", False) or (has_packet_boss and encounter_active)),
        "current_hp": _safe_int(context.get("boss_current_hp"), 0),
        "total_hp": _safe_int(context.get("boss_total_hp"), 0),
        "hp_pct": max(0.0, min(1.0, _safe_float(context.get("boss_hp_est_pct"), 1.0))),
        "hp_source": hp_source,
        "shield_active": bool(context.get("boss_shield_active", False)),
        "shield_pct": max(0.0, min(1.0, _safe_float(context.get("boss_shield_pct"), 0.0))),
        "breaking_stage": _safe_int(context.get("boss_breaking_stage"), -1),
        "extinction_pct": max(0.0, min(1.0, _safe_float(context.get("boss_extinction_pct"), 0.0))),
        "in_overdrive": bool(context.get("boss_in_overdrive", False)),
        "invincible": bool(context.get("boss_invincible", False)),
        "raid_phase": _safe_int(context.get("boss_raid_phase"), 0),
        "raid_phase_name": str(context.get("boss_raid_phase_name") or ""),
    }


def build_act_render_spec(live: Optional[Dict[str, Any]] = None,
                          last_report: Optional[Dict[str, Any]] = None,
                          history: Optional[List[Dict[str, Any]]] = None,
                          context: Optional[Dict[str, Any]] = None,
                          encounter: Optional[Dict[str, Any]] = None,
                          sources: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    # Build the shared WebView/Entity ACT render contract.
    live = live or {}
    last_report = last_report or {}
    history = history or []
    context = context or {}
    encounter = encounter or {}
    sources = sources or {}
    is_live = bool(live.get("encounter_active") or encounter.get("status") in ("active", "pending_reset"))
    source = live if is_live or not last_report else last_report
    dungeon_name = str(context.get("dungeon_name") or encounter.get("dungeon_name") or "")
    last_skill = context.get("last_skill_event") or encounter.get("last_skill_event") or {}
    last_boss_event = context.get("last_boss_event") or encounter.get("last_boss_event") or {}
    last_skill_fact = last_skill.get("combat_fact") if isinstance(last_skill, dict) else {}
    if not isinstance(last_skill_fact, dict):
        last_skill_fact = {}
    last_boss_fact = last_boss_event.get("combat_fact") if isinstance(last_boss_event, dict) else {}
    if not isinstance(last_boss_fact, dict):
        last_boss_fact = {}
    boss = _boss_context(context, encounter, live)
    return {
        "version": 1,
        "mode": "live" if is_live else ("report" if last_report else "empty"),
        "title": dungeon_name or ("Live Encounter" if is_live else "Last Encounter"),
        "parity_targets": ["webview", "entity"],
        "context": {
            "dungeon_id": _safe_int(context.get("dungeon_id") or encounter.get("dungeon_id"), 0),
            "dungeon_scene_id": _safe_int(
                context.get("dungeon_scene_id") or encounter.get("dungeon_scene_id"), 0),
            "dungeon_difficulty": _safe_int(
                context.get("dungeon_difficulty") or encounter.get("dungeon_difficulty"), 0),
            "dungeon_name": dungeon_name,
            "last_skill_event_kind": str(last_skill.get("kind") or ""),
            "last_skill_id": _safe_int(last_skill.get("skill_id") or last_skill_fact.get("skill_id"), 0),
            "last_skill_name": str(last_skill.get("skill_name") or last_skill_fact.get("skill_name") or ""),
            "last_skill_role": str(last_skill.get("skill_role") or last_skill_fact.get("skill_role") or ""),
            "last_skill_category": str(last_skill.get("skill_category") or last_skill_fact.get("skill_category") or ""),
            "last_skill_kind": str(last_skill.get("skill_kind") or last_skill_fact.get("skill_kind") or last_skill.get("skill_category") or last_skill_fact.get("skill_category") or last_skill.get("kind") or ""),
            "last_skill_is_ultimate": bool(last_skill.get("is_ultimate") or last_skill_fact.get("is_ultimate")),
            "last_skill_is_player": bool(last_skill.get("is_player_skill") or last_skill_fact.get("is_player_skill")),
            "last_skill_is_monster": bool(last_skill.get("is_monster_skill") or last_skill_fact.get("is_monster_skill")),
            "last_skill_is_environment": bool(last_skill.get("is_environment_skill") or last_skill_fact.get("is_environment_skill")),
            "last_skill_is_boss_skill": bool(last_skill.get("is_boss_skill") or last_skill_fact.get("is_boss_skill")),
            "last_skill_is_boss_mechanic": bool(last_skill.get("is_boss_mechanic_skill") or last_skill_fact.get("is_boss_mechanic_skill")),
            "last_boss_event_type": _safe_int(last_boss_event.get("event_type"), 0),
            "last_buff_category": str(last_boss_event.get("buff_category") or last_boss_fact.get("buff_category") or ""),
            "last_boss_mechanic_key": str(
                last_boss_event.get("boss_mechanic_key") or last_boss_fact.get("boss_mechanic_key") or ""),
            "last_boss_mechanic_label": str(
                last_boss_event.get("boss_mechanic_label") or last_boss_fact.get("boss_mechanic_label") or ""),
            "last_boss_trigger_family": str(
                last_boss_event.get("trigger_family") or last_boss_fact.get("trigger_family") or ""),
            "last_boss_host_uuid": _safe_int(last_boss_event.get("host_uuid"), 0),
        },
        "name_resolution": {
            "dungeon_name_source": "resolver" if dungeon_name else "pending_runtime_ztable",
            "pending_runtime_ztable": not bool(dungeon_name),
        },
        "sources": sources,
        "boss": boss,
        "encounter": {
            "id": str(encounter.get("encounter_id") or ""),
            "status": str(encounter.get("status") or "idle"),
            "kind": str(encounter.get("kind") or "unknown"),
            "duration_s": _safe_float(encounter.get("duration_s") or source.get("elapsed_s"), 0.0),
            "boss_uuid": _safe_int(encounter.get("boss_uuid"), 0),
            "target_uuid": _safe_int(encounter.get("target_uuid"), 0),
        },
        "totals": {
            "damage": _safe_int(source.get("total_damage"), 0),
            "damage_all": _safe_int(source.get("total_damage_all") or source.get("total_damage"), 0),
            "heal": _safe_int(source.get("total_heal"), 0),
            "dps": _safe_int(source.get("total_dps"), 0),
            "hps": _safe_int(source.get("total_hps"), 0),
            "elapsed_s": _safe_float(source.get("elapsed_s") or encounter.get("duration_s"), 0.0),
        },
        "rows": _entity_rows(live if is_live or live else last_report),
        "history_count": len(history),
    }


def build_act_snapshot(dps_tracker: Any = None,
                       history_store: Any = None,
                       state_mgr: Any = None,
                       encounter_mgr: Any = None,
                       trigger_engine: Any = None,
                       source_probe: Any = None,
                       packet_probe: Any = None,
                       memory_probe: Any = None,
                       history_limit: int = 20,
                       lite: bool = False,
                       live_max_age_ms: float = 150.0) -> Dict[str, Any]:
    # Build a single ACT snapshot for UI/API consumers.
    #
    # This function deliberately delegates locking to the source objects so it can
    # be called from pywebview workers without taking unrelated locks together.
    #
    # ``lite=True`` is the high-rate live-push path (Entity overlay set_act_snapshot
    # + WebView DpsMeter.showActSnapshot + act_snapshot plugin subscribers). Those
    # consumers only read ``render_spec`` / ``sources`` / ``triggers`` — never the
    # per-entity skill breakdown, the full ``last_report``, or ``history`` — so in
    # lite mode we use the 150ms-cached, skill-free snapshot and skip the per-event
    # deepcopy of the last report + 20-report history while a fight is live. The
    # default ``lite=False`` path is byte-for-byte the original behaviour (used by
    # the offline replay harness and any consumer that wants the full payload).
    live: Optional[Dict[str, Any]] = None
    last_report: Optional[Dict[str, Any]] = None
    history = []
    encounter: Dict[str, Any] = {}

    if encounter_mgr is not None:
        try:
            encounter = encounter_mgr.snapshot() or {}
        except Exception:
            encounter = {}

    if dps_tracker is not None:
        try:
            if lite:
                getfast = getattr(dps_tracker, 'get_snapshot_fast', None)
                live = (getfast(live_max_age_ms) if callable(getfast)
                        else dps_tracker.get_snapshot(include_skills=False))
            else:
                live = dps_tracker.get_snapshot(include_skills=True)
        except Exception:
            live = None

    # The render spec is built from `live` during an active encounter; the last
    # report + history are only needed for the idle "report" view. In lite mode,
    # skip their deepcopy entirely while a fight is live (that is exactly when
    # the per-event push fires the most).
    _is_live = bool(
        (live or {}).get('encounter_active')
        or str(encounter.get('status') or '') in ('active', 'pending_reset')
    )
    _skip_report = bool(lite and _is_live)

    if dps_tracker is not None and not _skip_report:
        try:
            last_report = dps_tracker.get_last_report()
        except Exception:
            last_report = None

    if history_store is not None:
        if not last_report and not _skip_report:
            try:
                last_report = history_store.latest_report()
            except Exception:
                last_report = None
        if not lite and history_limit and int(history_limit) > 0:
            try:
                history = history_store.list_reports(history_limit)
            except Exception:
                history = []

    state = _state_to_dict(state_mgr)
    context = {
        "dungeon_id": state.get("dungeon_id", 0),
        "dungeon_scene_id": state.get("dungeon_scene_id", 0),
        "dungeon_difficulty": state.get("dungeon_difficulty", 0),
        "dungeon_name": state.get("dungeon_name", ""),
        "last_dungeon_event": state.get("last_dungeon_event") or {},
        "last_skill_event": state.get("last_skill_event") or {},
        "last_boss_event": state.get("last_boss_event") or {},
        "boss_raid_active": state.get("boss_raid_active", False),
        "boss_raid_phase": state.get("boss_raid_phase", 0),
        "boss_raid_phase_name": state.get("boss_raid_phase_name", ""),
        "boss_hp_est_pct": state.get("boss_hp_est_pct", 1.0),
        "boss_current_hp": state.get("boss_current_hp", 0),
        "boss_total_hp": state.get("boss_total_hp", 0),
        "boss_hp_source": state.get("boss_hp_source", "none"),
        "boss_shield_active": state.get("boss_shield_active", False),
        "boss_shield_pct": state.get("boss_shield_pct", 0.0),
        "boss_breaking_stage": state.get("boss_breaking_stage", -1),
        "boss_extinction_pct": state.get("boss_extinction_pct", 0.0),
        "boss_in_overdrive": state.get("boss_in_overdrive", False),
        "boss_invincible": state.get("boss_invincible", False),
    }
    sources = _build_sources(source_probe=source_probe,
                             packet_probe=packet_probe,
                             memory_probe=memory_probe)
    render_spec = build_act_render_spec(live, last_report, history, context, encounter, sources)
    triggers: Dict[str, Any] = {}
    if trigger_engine is not None:
        try:
            emitted = trigger_engine.evaluate({"render_spec": render_spec}) or []
        except Exception:
            emitted = []
        try:
            triggers = trigger_engine.snapshot() or {}
        except Exception:
            triggers = {}
        if emitted:
            triggers = dict(triggers)
            triggers["emitted"] = emitted
    return {
        "live": live,
        "last_report": last_report,
        "history": history,
        "context": context,
        "encounter": encounter,
        "sources": sources,
        "render_spec": render_spec,
        "triggers": triggers,
    }


__all__ = ["boss_state_from_monster_update", "build_act_render_spec", "build_act_snapshot"]
