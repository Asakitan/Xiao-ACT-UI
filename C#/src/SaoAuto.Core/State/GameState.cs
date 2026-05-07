using System.Collections.Immutable;
using SaoAuto.Core.Packets;

namespace SaoAuto.Core.State;

/// <summary>
/// Immutable snapshot of the unified game state.
/// Mirrors fields of Python <c>game_state.GameState</c>.
/// UI/overlay code must treat instances as read-only.
/// </summary>
public sealed record GameState
{
    // ── Identity ──
    public string PlayerName { get; init; } = string.Empty;
    public int LevelBase { get; init; }
    public int LevelExtra { get; init; }
    public int SeasonExp { get; init; }
    public string PlayerId { get; init; } = string.Empty;
    public int FightPoint { get; init; }

    // ── HP ──
    public int HpCurrent { get; init; }
    public int HpMax { get; init; }
    public double HpPct { get; init; } = 1.0;

    // ── Stamina ──
    public int StaminaCurrent { get; init; }
    public int StaminaMax { get; init; }
    public double StaminaPct { get; init; } = 1.0;
    public bool StaminaOffline { get; init; }

    // ── Skill bar / burst ──
    public ImmutableArray<SkillSlot> SkillSlots { get; init; } = ImmutableArray<SkillSlot>.Empty;
    public bool BurstReady { get; init; }
    public int ProfessionId { get; init; }
    public string ProfessionName { get; init; } = string.Empty;

    // ── Buff monitor ──
    public ImmutableArray<BuffEntry> SelfBuffs { get; init; } = ImmutableArray<BuffEntry>.Empty;
    public double ServerTimeOffsetMs { get; init; }

    // ── Combat ──
    public bool InCombat { get; init; }

    // ── Self identity (S64: from EnterGame) ──
    public ulong SelfUuid { get; init; }

    // ── Skill cooldown table (S64: from SyncClientUseSkill) ──
    // Keyed by SkillLevelId; value = timestampSeconds of last use.
    public ImmutableDictionary<int, double> SkillLastUseAt { get; init; }
        = ImmutableDictionary<int, double>.Empty;

    // ── Server-reported skill cooldowns (S69: from SyncToMeDeltaInfo) ──
    // Keyed by SkillLevelId; value = full per-CD snapshot (begin/dur/vcd/
    // charge/sub/accel). Populated by AoiSyncToMeDelta.SyncSkillCDs;
    // expired entries are pruned on each delta.
    public ImmutableDictionary<int, SkillCdSnapshot> SkillCdMap { get; init; }
        = ImmutableDictionary<int, SkillCdSnapshot>.Empty;

    // ── Server-reported fight-resource cooldowns (S108: from
    // SyncToMeDeltaInfo.FightResCDs sibling event). Keyed by ResId;
    // value = (begin/dur/vcd) per-resource snapshot. Populated by
    // ApplyToMeFightResCd; expired entries pruned on each delta.
    // Mirrors Python's fight_res_cd_map at packet_parser.py 3967–3973.
    public ImmutableDictionary<int, FightResCdSnapshot> FightResCdMap { get; init; }
        = ImmutableDictionary<int, FightResCdSnapshot>.Empty;

    // ── Buff-driven CD modifier scalars (S122: from
    // SyncToMeDeltaInfo BaseDelta.TempAttrs). Cumulative sums of
    // TempAttr ids 100/101/103 over the collection — pct + accel are
    // /10000 万分比 ratios; fixed is in ms. Self-only; bridge filters
    // by SelfUuid. Consumers that care about server-side CDR
    // (skill HUD timer compression) read these alongside SkillCdMap.
    // Mirrors Python's `player.temp_attr_cd_*` fields written by
    // `_process_temp_attr_collection` at packet_parser.py 4831–4869.
    public int TempAttrCdPct { get; init; }
    public int TempAttrCdFixed { get; init; }
    public int TempAttrCdAccel { get; init; }

    // ── Dungeon (S64: from SyncDungeonData / SyncDungeonDirtyData) ──
    public long DungeonSceneUuid { get; init; }
    public int DungeonDifficulty { get; init; }
    public int? DungeonFlowState { get; init; }
    public ImmutableArray<DungeonTargetProgress> DungeonTargets { get; init; }
        = ImmutableArray<DungeonTargetProgress>.Empty;

    // ── Near-entity table (S68: from SyncNearEntities) ──
    // Keyed by entity Uuid; value = (EntityType, FirstSeenSeconds).
    // Removed when SyncNearEntities reports a Disappear for the uuid.
    public ImmutableDictionary<long, EntityTableEntry> NearEntities { get; init; }
        = ImmutableDictionary<long, EntityTableEntry>.Empty;

    // ── Monster data table (S113: skeleton populated from
    // SyncNearEntities + SyncNearDeltaInfo). Keyed by uuid; value =
    // a record with the per-monster fields the entity table can't
    // hold (Name / TemplateId / breaking gauge / shield / IsDead).
    // Mirrors Python's `MonsterData` at packet_parser.py 803. Lazy-
    // create on first appearance for entities with EntityType==1
    // (Entmonster); removed on Disappear; HP slot mirrors the entity
    // table so death (CurHp=0 with HasCurHp=true) flips IsDead. The
    // Name / TemplateId fields stay zero/empty until S114 wires
    // additional AttrCollection ids (NAME=1, ID=10) into the decoder.
    public ImmutableDictionary<long, MonsterData> MonsterDataMap { get; init; }
        = ImmutableDictionary<long, MonsterData>.Empty;

    // ── Window ──
    public WindowRect? WindowRect { get; init; }
    public int WindowWidth { get; init; }
    public int WindowHeight { get; init; }

    // ── Boss raid ──
    public bool BossRaidActive { get; init; }
    public int BossRaidPhase { get; init; }
    public string BossRaidPhaseName { get; init; } = string.Empty;
    public double BossEnrageRemaining { get; init; }
    public string BossTimerText { get; init; } = string.Empty;
    public int BossTotalDamage { get; init; }
    public int BossDps { get; init; }
    public double BossHpEstPct { get; init; } = 1.0;
    public int BossCurrentHp { get; init; }
    public int BossTotalHp { get; init; }
    public BossHpSource BossHpSource { get; init; } = BossHpSource.None;
    // S120 — wall-clock timestamp of the last BossHpEvent application.
    // Used by the MonsterDataMap → Boss* projector to gate "is the packet
    // path still fresh?". When packet-staleness exceeds the projector's
    // window, the projector takes over and overwrites Boss* from the
    // monster table; once a fresh BossHpEvent arrives, it wins again.
    public double BossHpLastPacketSeconds { get; init; }
    public bool BossShieldActive { get; init; }
    public double BossShieldPct { get; init; }
    public int BossBreakingStage { get; init; } = -1;
    public double BossExtinctionPct { get; init; }
    public bool BossInOverdrive { get; init; }
    public bool BossInvincible { get; init; }

    // ── Capture metadata ──
    public double CaptureTimestamp { get; init; }
    public bool RecognitionOk { get; init; }
    public bool PacketActive { get; init; }
    public string ErrorMsg { get; init; } = string.Empty;
    public int IdentityAlertSerial { get; init; }
    public string IdentityAlertTitle { get; init; } = string.Empty;
    public string IdentityAlertMessage { get; init; } = string.Empty;

    public string LevelText => LevelExtra > 0 ? $"{LevelBase}(+{LevelExtra})" : LevelBase.ToString();

    public string HpText => $"{HpCurrent}/{HpMax}";

    public string StaminaText
    {
        get
        {
            var pct = Math.Clamp(StaminaPct, 0.0, 1.0);
            return $"{(int)Math.Round(pct * 100.0)}%";
        }
    }
}

public sealed record SkillSlot
{
    public int Index { get; init; }
    public RectI? Rect { get; init; }
    public SkillSlotState State { get; init; } = SkillSlotState.Unknown;
    public double CooldownPct { get; init; } = 1.0;
    public bool InsufficientEnergy { get; init; }
    public bool Active { get; init; }
    public bool ReadyEdge { get; init; }
    public int ChargeCount { get; init; }
    public int RemainingMs { get; init; }
}

public sealed record BuffEntry
{
    public int Id { get; init; }
    public long Uuid { get; init; }
    public long BeginMs { get; init; }
    public int DurationMs { get; init; }
    public int Layer { get; init; }
    public int Count { get; init; }
    public string Name { get; init; } = string.Empty;
}

public readonly record struct RectI(int X, int Y, int W, int H);

public readonly record struct WindowRect(int Left, int Top, int Right, int Bottom);

public enum SkillSlotState
{
    Unknown = 0,
    Ready = 1,
    Cooldown = 2,
    InsufficientEnergy = 3,
    Active = 4,
}

public enum BossHpSource
{
    None = 0,
    Packet = 1,
    Estimate = 2,
    // S120 — projected from the MonsterDataMap (selected by aggro on
    // SelfUuid, fallback to highest MaxHp Entmonster). Lower precedence
    // than Packet within the freshness window; takes over when the
    // packet path goes stale.
    MonsterData = 3,
}

/// <summary>
/// One row of the near-entity table populated from SyncNearEntities
/// appearances. Holds the entity's wire-format type + the timestamp
/// of first observation, plus the S110 core monster attrs (CurHp,
/// MaxHp, Level) decoded from <c>Entity.Attrs</c> at appearance time.
/// Richer per-entity attributes (TempAttrs, buff list, breaking-stage)
/// land in a follow-up session when the deeper SyncNearDeltaInfo
/// destructuring is ported.
/// </summary>
public readonly record struct EntityTableEntry(
    int EntityType,
    double FirstSeenSeconds,
    int CurHp = 0,
    int MaxHp = 0,
    int Level = 0);

/// <summary>
/// S113 — skeleton port of Python's <c>MonsterData</c> (packet_parser.py
/// 803). Carries the per-monster fields the entity table can't hold:
/// the public-facing identity (Name / TemplateId), the break gauge
/// (BreakingStage / Extinction / MaxExtinction), the shield (
/// ShieldTotal / ShieldMaxTotal / ShieldActive), and the death flag.
/// HP fields mirror the entity table so a delta with HP=0 (HasCurHp=true)
/// can flip IsDead on this row in the same dispatch.
/// Defaults follow Python: BreakingStage=-1 ("not received"), all other
/// numeric/bool slots zero/false. The Name / TemplateId fields stay at
/// their defaults until S114 extends the AttrCollection unpack with
/// NAME (id=1) + ID (id=10).
/// </summary>
public sealed record MonsterData
{
    public long Uuid { get; init; }
    public long Uid { get; init; }
    public string Name { get; init; } = string.Empty;
    public int TemplateId { get; init; }
    public int Hp { get; init; }
    public int MaxHp { get; init; }
    public int Level { get; init; }
    public int BreakingStage { get; init; } = -1;
    public int Extinction { get; init; }
    public int MaxExtinction { get; init; }
    public int Stunned { get; init; }
    public int MaxStunned { get; init; }
    public bool InOverdrive { get; init; }
    public bool ShieldActive { get; init; }
    public int ShieldTotal { get; init; }
    public int ShieldMaxTotal { get; init; }
    public bool IsDead { get; init; }
    // S117 — extended monster flags / attrs.
    public bool IsLockStunned { get; init; }
    public bool StopBreakingTicking { get; init; }
    public int State { get; init; }
    public int DeadType { get; init; }
    public int DeadTime { get; init; }
    public bool FirstAttack { get; init; }
    public long HatedCharId { get; init; }
    public string HatedCharName { get; init; } = string.Empty;
    // S123 — sustained buff list mirrored from
    // AoiSyncDelta.BuffInfos (BuffInfoSync). Authoritative for the
    // monster's currently-active buffs; periodic resync replaces
    // the row wholesale (matches Python's
    // `self._monsters[uuid].buff_list = buffs`). Empty by default
    // so legacy callers (entries created before S123) see no
    // BuffList until a real sync arrives.
    public ImmutableArray<BuffEntry> BuffList { get; init; } = ImmutableArray<BuffEntry>.Empty;
    public double LastUpdateSeconds { get; init; }
}
