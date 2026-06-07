# -*- coding: utf-8 -*-
"""mem_entity_provider - live entity HP snapshots for the app (read-only).

Ties the version-robust ZEntityMgr locator (EntityMgrReader, klass resolved by
name) to the combat-attr reader (EntityCombatReader) and exposes a simple
snapshot of every entity that currently has an HP bar:

    prov = MemEntityProvider(static_dps_source, self_uid=...)
    for e in prov.snapshot():
        # {uuid, config_uuid, kind, cur_hp, max_hp, hp_pct, obj}
        ...
    boss = prov.boss()   # the highest-max-HP combat entity (boss/dummy)

This is the unique value memory adds over TCP: real-time HP + combat state
(breaking_stage / overdrive / stun / cast_skill_id, decoded by attr id from the
ZAttrCacheSlim index) for ALL visible entities, including pre-pull / off-screen
targets the packet stream hasn't reported yet. Display names come from the
entity BaseId via the offline name table (callers resolve).
"""
from __future__ import annotations

from typing import List, Optional

from mem_probe.il2cpp.mem_entity_mgr import (
    EntityMgrReader, ENTITY_DICT_OFF, BOSS_DICT_OFF, MONSTER_DICT_OFF, NPC_DICT_OFF,
    ENT_UUID_OFF, ENT_CONFIG_OFF, ENT_BASEID_OFF,
)
from mem_probe.il2cpp.mem_entity_combat import EntityCombatReader


class MemEntityProvider:
    """Read-only live entity HP snapshots, resolved structurally (no fixed base)."""

    def __init__(self, dps_source, *, self_uid: int = 0):
        self._src = dps_source
        self._emr = EntityMgrReader(dps_source)
        self._pm = dps_source.sr.pm
        self._ecr = EntityCombatReader(self._pm)
        self._self_uid = int(self_uid or 0)

    def set_self_uid(self, uid: int) -> None:
        self._self_uid = int(uid or 0)

    def locate(self, *, force: bool = False) -> int:
        """Return the live ZEntityMgr address (cached, klass+uuid revalidated)."""
        return int(self._emr.locate(self._self_uid, force_rescan=force) or 0)

    def snapshot(self, *, include_monsters: bool = True, include_npcs: bool = False,
                 max_per_dict: int = 128) -> List[dict]:
        """Return one dict per combat entity (entity that has an HP bar)."""
        mgr = self.locate()
        if not mgr:
            return []
        dicts = [("entity", ENTITY_DICT_OFF), ("boss", BOSS_DICT_OFF)]
        if include_monsters:
            dicts.append(("monster", MONSTER_DICT_OFF))
        if include_npcs:
            dicts.append(("npc", NPC_DICT_OFF))
        # collect unique entities from all dicts, then batch every read
        ents: List = []
        seen = set()
        for kind, off in dicts:
            d = self._pm.read_u64(mgr + off)
            if not d:
                continue
            for _key, ent in self._emr._read_dict_entries(d, max_entries=max_per_dict):
                if ent in seen:
                    continue
                seen.add(ent)
                ents.append((kind, ent))
        if not ents:
            return []
        # one batched combat read for all entities (HP + state)
        combat = self._ecr.read_combat_batch([e for _, e in ents])
        live = [(kind, e) for kind, e in ents if combat.get(e)]
        if not live:
            return []
        # batched id reads: Uuid@0xC0 / ConfigUuid@0xC8 / BaseId@0xE0 (one RPM batch)
        id_addrs = []
        for _, e in live:
            id_addrs += [e + ENT_UUID_OFF, e + ENT_CONFIG_OFF, e + ENT_BASEID_OFF]
        idv = self._pm.read_u64_many(id_addrs)
        out: List[dict] = []
        for i, (kind, e) in enumerate(live):
            c = combat[e]
            uuid = idv[3 * i] or 0
            cfg = idv[3 * i + 1] or 0
            base = idv[3 * i + 2]
            base_id = (int(base) & 0xFFFFFFFF) if base is not None else 0
            out.append({
                "uuid": int(uuid), "config_uuid": int(cfg), "base_id": int(base_id),
                "kind": kind, "cur_hp": c["cur_hp"], "max_hp": c["max_hp"],
                "hp_pct": c["hp_pct"], "obj": int(e),
                "breaking_stage": c.get("breaking_stage"),
                "overdrive": c.get("overdrive"),
                "stun": c.get("stun"),
                "cast_skill_id": c.get("cast_skill_id"),
            })
        return out

    def read_name(self, ent_addr: int) -> str:
        """The game's resolved display name for an entity (its NAME attr), or '' if absent
        (many monsters carry only a template id). Authoritative for the JSON self-heal --
        read straight from the game's own memory, independent of our offline tables."""
        try:
            return self._ecr.read_name_attr(int(ent_addr or 0)) if ent_addr else ""
        except Exception:
            return ""

    def boss(self) -> Optional[dict]:
        """Best boss candidate: a bossDict entry, else the highest-max-HP entity."""
        snap = self.snapshot()
        if not snap:
            return None
        bosses = [e for e in snap if e["kind"] == "boss"]
        pool = bosses or snap
        return max(pool, key=lambda e: e["max_hp"])

    def entity_hp(self, uuid: int) -> Optional[dict]:
        """HP snapshot for one uuid (None if not currently shown)."""
        uuid = int(uuid or 0)
        for e in self.snapshot():
            if e["uuid"] == uuid:
                return e
        return None


__all__ = ["MemEntityProvider"]
