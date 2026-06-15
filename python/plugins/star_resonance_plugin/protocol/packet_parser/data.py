# -*- coding: utf-8 -*-
"""MonsterData / PlayerData state dataclasses extracted from packet_parser.
"""

from typing import Optional, Dict, Any

from packet_parser.helpers import _uuid_to_uid


class MonsterData:
    """Tracks one monster entity's parsed state."""
    __slots__ = ('uuid', 'uid', 'name', 'template_id',
                 'hp', 'max_hp', 'season_level',
                 'breaking_stage', 'extinction', 'max_extinction',
                 'stunned', 'max_stunned', 'in_overdrive',
                 'is_lock_stunned', 'stop_breaking_ticking',
                 'shield_active', 'shield_total', 'shield_max_total',
                 'is_dead', 'last_update',
                 # Extended monster fields (from AttrCollection)
                 'state', 'dead_type', 'dead_time',
                 'first_attack', 'hated_char_id', 'hated_char_name',
                 'buff_list')

    def __init__(self, uuid: int, uid: int = 0):
        self.uuid = uuid
        self.uid = uid or _uuid_to_uid(uuid)
        self.name: str = ''
        self.template_id: int = 0
        self.hp: int = 0
        self.max_hp: int = 0
        self.season_level: int = 0
        self.breaking_stage: int = -1   # -1 = not received; 0 = Breaking (broken), 1 = BreakEnd (normal)
        self.extinction: int = 0
        self.max_extinction: int = 0
        self.stunned: int = 0
        self.max_stunned: int = 0
        self.in_overdrive: bool = False
        self.is_lock_stunned: bool = False
        self.stop_breaking_ticking: bool = False
        self.shield_active: bool = False
        self.shield_total: int = 0
        self.shield_max_total: int = 0
        self.is_dead: bool = False
        self.last_update: float = 0.0
        # Extended monster fields
        self.state: int = 0              # EActorState
        self.dead_type: int = 0          # AttrDeadType
        self.dead_time: int = 0          # AttrDeadTime (timestamp)
        self.first_attack: bool = False  # AttrFirstAttack flag
        self.hated_char_id: int = 0      # Aggro target UID
        self.hated_char_name: str = ''   # Aggro target name
        self.buff_list: list = []        # Active buffs on this monster

    def to_dict(self) -> dict:
        # Break gauge: extinction works like HP — starts at max, depletes to 0.
        # remaining = current / max  →  100% = undamaged, 0% = fully broken.
        # Also consider break data present if we have raw values (even without max)
        # or if breaking_stage has been received (>= 0).
        _has_break = (self.max_extinction > 0 or self.max_stunned > 0
                      or self.extinction > 0 or self.stunned > 0
                      or self.breaking_stage >= 0)
        if self.max_extinction > 0:
            _ext_pct = max(0.0, self.extinction / self.max_extinction)
        elif self.max_stunned > 0:
            _ext_pct = max(0.0, self.stunned / self.max_stunned)
        else:
            _ext_pct = 0.0
        return {
            'uuid': self.uuid,
            'uid': self.uid,
            'name': self.name,
            'template_id': self.template_id,
            'season_level': self.season_level,
            'hp': self.hp,
            'max_hp': self.max_hp,
            'hp_pct': (self.hp / self.max_hp) if self.max_hp > 0 else 0.0,
            'has_break_data': _has_break,
            'breaking_stage': self.breaking_stage,
            'extinction': self.extinction,
            'max_extinction': self.max_extinction,
            'extinction_pct': _ext_pct,
            'stunned': self.stunned,
            'max_stunned': self.max_stunned,
            'in_overdrive': self.in_overdrive,
            'stop_breaking_ticking': self.stop_breaking_ticking,
            'shield_active': self.shield_active and self.shield_total > 0,
            'shield_total': self.shield_total,
            'shield_max_total': self.shield_max_total,
            'shield_pct': (self.shield_total / self.shield_max_total) if self.shield_max_total > 0 else 0.0,
            'is_dead': self.is_dead,
            'buff_list': list(self.buff_list or []),
        }


class PlayerData:
    """Tracks one player's parsed data."""
    __slots__ = ('uid', 'name', 'level', 'rank_level', 'season_level',
                 'level_extra', 'level_extra_source',
                 'level_extra_pending_source', 'level_extra_pending_value',
                 'level_extra_pending_hits',
                 'fight_point', 'season_strength',
                 'in_combat', 'combat_state_time',
                 'hp', 'max_hp', 'energy', 'energy_limit', 'extra_energy_limit',
                 'energy_info_value', 'energy_valid', 'energy_source_priority',
                 'resource_values', 'energy_info_map', 'stamina_resource_id',
                 'stamina_ratio', 'stamina_ratio_observed_at',
                 'season_medal_level', 'monster_hunt_level',
                 'battlepass_level', 'battlepass_data_level',
                 'profession', 'profession_id', 'sub_profession',
                 'hp_from_full_sync',
                 'skill_slot_map', 'skill_level_info_map',
                 'slot_bar_map',
                 'skill_cd_map', 'skill_last_use_at', 'skill_seen_ids',
                 '_inferred_skill_count',
                 'fight_res_cd_map', 'fight_resource_values', 'attr_skill_id',
                 'server_time_offset_ms',
                 'attr_skill_cd', 'attr_skill_cd_pct', 'attr_cd_accelerate_pct',
                 'temp_attr_cd_pct', 'temp_attr_cd_fixed', 'temp_attr_cd_accel',
                 'attr_fight_res_cd_speed',
                 'cd_speed_ratio',
                 # ── Extended combat stats (from AttrCollection) ──
                 'attack', 'magic_attack', 'defense', 'magic_defense',
                 'crit_rate', 'crit_damage',
                 'attack_speed_pct', 'cast_speed_pct', 'charge_speed_pct',
                 'heal_power',
                 'dam_inc', 'mdam_inc', 'boss_dam_inc',
                 # ── Buff tracking ──
                 'buff_list',
                 # ── Scene / dungeon ──
                 'scene_id', 'dungeon_id', 'dungeon_difficulty',
                 # ── Self identity guard ──
                 'self_uid_confirmed', 'self_uid_source',
                 # ── Extended CharSerialize data (decoded generic) ──
                 'extended_data')

    def __init__(self, uid: int):
        self.uid = uid
        self.name: str = ''
        self.level: int = 0          # Visible character level from AttrLevel 0x2710
        self.rank_level: int = 0     # Rank/star level from AttrRankLevel 0x274C
        self.season_level: int = 0   # Seasonal extra level shown as (+XX)
        self.level_extra: int = 0
        self.level_extra_source: str = ''
        self.level_extra_pending_source: str = ''
        self.level_extra_pending_value: int = 0
        self.level_extra_pending_hits: int = 0
        self.fight_point: int = 0
        self.season_strength: int = 0     # AttrSeasonStrength (11440) — 梦境强度
        self.in_combat: bool = False      # AttrCombatState (104)
        self.combat_state_time: int = 0   # AttrCombatStateTime (114) — timestamp ms
        self.hp: int = 0
        self.max_hp: int = 0
        self.energy: float = 0.0
        self.energy_limit: int = 0
        self.extra_energy_limit: int = 0
        self.energy_info_value: int = 0
        self.energy_valid: bool = False
        self.energy_source_priority: int = 0
        self.resource_values: Dict[int, int] = {}
        self.energy_info_map: Dict[int, Dict[str, int]] = {}
        self.stamina_resource_id: int = 0
        self.stamina_ratio: float = -1.0
        self.stamina_ratio_observed_at: float = 0.0
        self.season_medal_level: int = 0
        self.monster_hunt_level: int = 0
        self.battlepass_level: int = 0
        self.battlepass_data_level: int = 0
        self.profession: str = ''
        self.profession_id: int = 0
        self.sub_profession: str = ''     # 子职业分支名 (e.g. '防盾', '光盾')
        self.hp_from_full_sync: bool = False  # Whether HP came from a trusted full sync
        self.skill_slot_map: Dict[int, int] = {}
        self.skill_level_info_map: Dict[int, Dict[str, int]] = {}
        self.slot_bar_map: Dict[int, int] = {}  # CharSerialize.Slots (field 55): slot_id → skill_id
        self.skill_cd_map: Dict[int, Dict[str, Any]] = {}
        self.skill_last_use_at: Dict[int, float] = {}
        self.skill_seen_ids = []
        self._inferred_skill_count: int = 0
        self.fight_res_cd_map: Dict[int, Dict[str, Any]] = {}  # res_id → FightResCD state
        self.fight_resource_values: list = []
        self.attr_skill_id: int = 0
        self.server_time_offset_ms: Optional[float] = None
        # Entity-level CD modifiers (from AttrCollection + TempAttr)
        self.attr_skill_cd: int = 0           # AttrSkillCD 11750 — flat CD reduction ms
        self.attr_skill_cd_pct: int = 0       # AttrSkillCDPCT 11760 — pct /10000
        self.attr_cd_accelerate_pct: int = 0  # AttrCdAcceleratePct 11960 — accel /10000
        self.temp_attr_cd_pct: int = 0        # TempAttr type 100 — buff pct CD reduce /10000
        self.temp_attr_cd_fixed: int = 0      # TempAttr type 101 — buff flat CD reduce ms
        self.temp_attr_cd_accel: int = 0      # TempAttr type 103 — buff CD accelerate /10000
        # FightResCdSpeedPct (11980) — CD speed/duration modifier /10000
        # 10000 = base (1x), values below 10000 → shorter CDs
        self.attr_fight_res_cd_speed: int = 0
        # Observed VCD speed ratio: how fast valid_cd_time ticks vs real time
        # e.g. 2.3 means VCD advances 2.3ms per 1ms real time (CD acceleration)
        self.cd_speed_ratio: float = 1.0
        # Extended combat stats (from AttrCollection — Total variants preferred)
        self.attack: int = 0             # AttrAttack/Total (11330/11331)
        self.magic_attack: int = 0       # AttrMAttack/Total (11340/11341)
        self.defense: int = 0            # AttrDefense/Total (11350/11351)
        self.magic_defense: int = 0      # AttrMDefense/Total (11360/11361)
        self.crit_rate: int = 0          # AttrCri/Total (11110/11111) 万分比
        self.crit_damage: int = 0        # AttrCritDamage/Total (12510/12511) 万分比
        self.attack_speed_pct: int = 0   # AttrAttackSpeedPCT (11720) 万分比
        self.cast_speed_pct: int = 0     # AttrCastSpeedPCT (11730) 万分比
        self.charge_speed_pct: int = 0   # AttrChargeSpeedPCT (11740) 万分比
        self.heal_power: int = 0         # AttrHeal (11790)
        self.dam_inc: int = 0            # AttrDamInc (12550) physical dmg inc 万分比
        self.mdam_inc: int = 0           # AttrMdamInc (12570) magic dmg inc 万分比
        self.boss_dam_inc: int = 0       # AttrBossDamInc (12630) boss dmg inc 万分比
        # Buff tracking from BuffInfoSync / NotifyBuffChange
        self.buff_list: list = []        # [{buff_id, begin_time, duration, layer, ...}]
        # Scene / dungeon context
        self.scene_id: int = 0           # from CharSerialize.SceneData or SyncDungeonData
        self.dungeon_id: int = 0         # current dungeon ID
        self.dungeon_difficulty: int = 0
        self.self_uid_confirmed: bool = False
        self.self_uid_source: str = ''
        # Extended CharSerialize data — decoded but not actively used by overlay
        # Keys are CharField names, values are decoded field dicts
        self.extended_data: Dict[str, Any] = {}
