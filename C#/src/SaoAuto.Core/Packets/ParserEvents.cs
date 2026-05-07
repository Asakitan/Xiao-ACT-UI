namespace SaoAuto.Core.Packets;

/// <summary>
/// Strongly-typed events the packet parser will emit (Session 5b).
/// Defining the events here lets <c>SaoAuto.Core/State</c> and overlay code
/// subscribe to a stable contract while the parser is still being ported.
/// </summary>
public abstract record ParserEvent(double TimestampSeconds);

public sealed record IdentityEvent(
    string PlayerName,
    string PlayerId,
    int LevelBase,
    int LevelExtra,
    int SeasonExp,
    int FightPoint,
    int ProfessionId,
    string ProfessionName,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record HealthEvent(
    int HpCurrent,
    int HpMax,
    double HpPct,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record StaminaEvent(
    double StaminaCurrent,
    long StaminaMax,
    double StaminaPct,
    bool Offline,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record SkillSlotsEvent(
    IReadOnlyList<SkillSlotPayload> Slots,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record SkillSlotPayload(
    int Index,
    string State,
    double CooldownPct,
    bool Active,
    int ChargeCount,
    int RemainingMs);

public sealed record DpsEvent(
    long TotalDamage,
    long Dps,
    long TotalHeal,
    long Hps,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record BossHpEvent(
    long CurrentHp,
    long MaxHp,
    double EstimatedPct,
    bool ShieldActive,
    double ShieldPct,
    int BreakingStage,
    bool InOverdrive,
    bool Invincible,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record BuffSnapshotEvent(
    IReadOnlyList<BuffPayload> Buffs,
    double ServerTimeOffsetMs,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record BuffPayload(
    int Id,
    long Uuid,
    long BeginMs,
    int DurationMs,
    int Layer,
    int Count,
    string Name);

public sealed record IdentityAlertEvent(
    int Serial,
    string Title,
    string Message,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record CombatStateEvent(
    bool InCombat,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record EnterGameEvent(ulong SelfUuid, double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record ReviveEvent(ulong Uuid, double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record KickOffEvent(double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record AllMemberReadyEvent(double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record CaptainReadyEvent(double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record DungeonStartEvent(int DungeonId, double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record EnterSceneEvent(double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record MatchResultEvent(double TimestampSeconds) : ParserEvent(TimestampSeconds);
public sealed record ServerTimeEvent(
    ulong ServerTimeMs,
    double OffsetMs,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record BuffChangeEvent(
    int OldBuffId,
    int NewBuffId,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record SkillUseEvent(
    long TargetUuid,
    int SkillLevelId,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record SkillEndEvent(
    int SkillUuid,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record SkillStageEndEvent(
    int SkillUuid,
    uint StageId,
    uint NewStageId,
    uint ConditionId,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record QteBeginEvent(
    long QteId,
    long QteType,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record DungeonDataEvent(
    long SceneUuid,
    int DungeonDifficulty,
    IReadOnlyList<DungeonTargetProgress> Targets,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record DungeonDirtyDataEvent(
    int? FlowState,
    IReadOnlyList<DungeonTargetProgress> Targets,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

// ── S65 — Near-entity stream skeletons. Decode boundary only;
// full state mutation (entity table, hate/CD tracking) deferred to a
// follow-up bridge session — these events surface enough data for
// downstream consumers to count appearances/disappearances and build
// targeted snapshots without paying the full Entity-shape port cost.
/// <summary>
/// S111/S112/S114/S115: full unpack of <c>AttrCollection</c> for a
/// monster entity. Mirrors the recognised slice of Python's
/// <c>_process_monster_attr_collection</c> (packet_parser.py 4259+).
/// All fields default to zero / empty / false; the matching <c>Has*</c>
/// flag tells the bridge whether the attr was actually on the wire
/// (so a real zero — e.g., HP=0 means death, InOverdrive=false means
/// rage ended — overrides the stored row instead of carrying-forward).
/// Carried in a single struct on <see cref="EntityAppearance.Attrs"/>
/// and <see cref="NearDeltaAttrUpdate.Attrs"/> so future per-attr
/// additions don't keep grafting init slots onto those records.
/// </summary>
// S118 — refactored from positional ctor onto init-only properties so
// future per-attr sessions only add the new field + its presence flag
// here without rippling through every test ctor site. Construct via
// object initializer (`new MonsterCoreAttrs { CurHp = 1, HasCurHp = true }`)
// or `default(MonsterCoreAttrs) with { ... }`.
public readonly record struct MonsterCoreAttrs
{
    public int CurHp { get; init; }
    public int MaxHp { get; init; }
    public int Level { get; init; }
    public bool HasCurHp { get; init; }
    public bool HasMaxHp { get; init; }
    public bool HasLevel { get; init; }

    public string Name { get; init; } = string.Empty;
    public int TemplateId { get; init; }
    public bool HasName { get; init; }
    public bool HasTemplateId { get; init; }

    public int BreakingStage { get; init; }
    public int Extinction { get; init; }
    public int MaxExtinction { get; init; }
    public int Stunned { get; init; }
    public int MaxStunned { get; init; }
    public bool InOverdrive { get; init; }
    public bool HasBreakingStage { get; init; }
    public bool HasExtinction { get; init; }
    public bool HasMaxExtinction { get; init; }
    public bool HasStunned { get; init; }
    public bool HasMaxStunned { get; init; }
    public bool HasInOverdrive { get; init; }

    public int ShieldTotal { get; init; }
    public int ShieldMaxTotal { get; init; }
    public bool HasShield { get; init; }

    // S117 — extended monster flags / attrs (IsLockStunned 445,
    // StopBreakingTicking 453, State 11, DeadType 78, DeadTime 206,
    // FirstAttack 456, HatedCharId 471, HatedCharName 473).
    public bool IsLockStunned { get; init; }
    public bool StopBreakingTicking { get; init; }
    public int State { get; init; }
    public int DeadType { get; init; }
    public int DeadTime { get; init; }
    public bool FirstAttack { get; init; }
    public long HatedCharId { get; init; }
    public string HatedCharName { get; init; } = string.Empty;
    public bool HasIsLockStunned { get; init; }
    public bool HasStopBreakingTicking { get; init; }
    public bool HasState { get; init; }
    public bool HasDeadType { get; init; }
    public bool HasDeadTime { get; init; }
    public bool HasFirstAttack { get; init; }
    public bool HasHatedCharId { get; init; }
    public bool HasHatedCharName { get; init; }

    public MonsterCoreAttrs() { }

    public bool Any => HasCurHp || HasMaxHp || HasLevel
        || HasName || HasTemplateId
        || HasBreakingStage || HasExtinction || HasMaxExtinction
        || HasStunned || HasMaxStunned || HasInOverdrive
        || HasShield
        || HasIsLockStunned || HasStopBreakingTicking
        || HasState || HasDeadType || HasDeadTime
        || HasFirstAttack || HasHatedCharId || HasHatedCharName;
}

public sealed record EntityAppearance(long Uuid, int EntityType)
{
    // S110: per-entity attrs unpacked from Entity.Attrs (AttrCollection)
    // by SyncNearEntitiesDecoder. Mirrors Python's
    // _process_monster_attr_collection at packet_parser.py 4259 — only
    // the HP / MaxHp / Level slice ports here; max-hp estimation,
    // template-id cache, name + breaking-stage land in the follow-up
    // MonsterData session. Defaults to 0 so legacy callers (decoders
    // built before S110) stay positional-compatible.
    public int CurHp { get; init; }
    public int MaxHp { get; init; }
    public int Level { get; init; }

    // S114: monster identity (NAME=1, ID=10). Only populated when the
    // appearance carries the corresponding AttrCollection ids; default
    // empty/zero so non-monster entities and pre-S114 callers stay
    // backward-compatible.
    public string Name { get; init; } = string.Empty;
    public int TemplateId { get; init; }

    // S115: full unpacked AttrCollection. Carries all monster attrs
    // (including break gauge) in a single struct so future per-attr
    // additions don't keep adding init slots to this record. The
    // legacy CurHp/MaxHp/Level/Name/TemplateId props above are kept
    // populated by the decoder so pre-S115 tests still pass; new
    // bridge code reads break-gauge / future fields off Attrs.
    public MonsterCoreAttrs Attrs { get; init; }
}
public sealed record EntityDisappearance(long Uuid, int Reason);

public sealed record NearEntitiesEvent(
    IReadOnlyList<EntityAppearance> Appear,
    IReadOnlyList<EntityDisappearance> Disappear,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

// ── S66 — Container sync skeleton. Surfaces the local player's
// identity + HP top-level fields from CharSerialize. Full attribute
// table mutation (~600 LOC of UserFightAttr / equipment / inventory
// destructuring in Python) is held until a downstream consumer needs
// it. SyncContainerDirtyData (0x16) deferred until a
// ParseContainerDirtyBuffer helper lands in CyPacketExtras.
public sealed record ContainerSyncEvent(
    long CharId,
    string Name,
    int Level,
    int FightPoint,
    long CurHp,
    long MaxHp,
    float Energy,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

// ── S67 — Container dirty stream skeleton. Surfaces one
// (field_index, sub_field, value) tuple per packet using the same
// ContainerDirtyChange shape the parser returns. Bridge maps the
// supported tuples onto GameState slots; unsupported tuples never
// reach this event (the decoder drops them, matching Python's
// silent skip on unrecognised fields).
public sealed record ContainerDirtyEvent(
    ContainerDirtyChange Change,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

// S111: per-uuid attr delta carried inside <see cref="NearDeltaEvent"/>.
// Zero CurHp/MaxHp/Level historically meant "attr not present", but
// S112 disambiguates a real HP=0 (monster died) from "attr absent" by
// adding nullable presence sidecars: when set, the bridge trusts the
// flag (so a real zero overrides); when null (legacy callers), the
// bridge falls back to `value != 0` carry-forward semantics. Init-only
// so the positional ctor stays untouched.
public sealed record NearDeltaAttrUpdate(long Uuid, int CurHp, int MaxHp, int Level)
{
    public bool? HasCurHp { get; init; }
    public bool? HasMaxHp { get; init; }
    public bool? HasLevel { get; init; }

    // S114: monster identity carried on the delta path. Same nullable-
    // bool sidecar pattern as S112: the bridge prefers the explicit
    // flag when set, otherwise falls back to "non-empty/non-zero =
    // present" so legacy callers see no behaviour change.
    public string Name { get; init; } = string.Empty;
    public int TemplateId { get; init; }
    public bool? HasName { get; init; }
    public bool? HasTemplateId { get; init; }

    // S115: full unpacked AttrCollection — see EntityAppearance.Attrs.
    // New attrs (break gauge, shield, …) are read off this struct
    // instead of being grafted on as more init slots.
    public MonsterCoreAttrs Attrs { get; init; }
}

public sealed record NearDeltaEvent(
    IReadOnlyList<long> DeltaUuids,
    double TimestampSeconds) : ParserEvent(TimestampSeconds)
{
    // S111: per-uuid attr deltas unpacked from
    // AoiSyncDelta.Attrs (AttrCollection). Init-only so existing
    // callers (S65 tests) constructing NearDeltaEvent positionally
    // still compile; populated on the decoder side when the delta
    // carries any of the three core monster attr ids.
    public IReadOnlyList<NearDeltaAttrUpdate> AttrUpdates { get; init; }
        = Array.Empty<NearDeltaAttrUpdate>();
}

public sealed record ToMeDeltaEvent(
    long Uuid,
    int HateIdCount,
    IReadOnlyList<SkillCdSnapshot> SkillCds,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// One per-skill cooldown row from <c>AoiSyncToMeDelta.SyncSkillCDs</c>.
/// Mirrors the dict assembled by Python's <c>_on_sync_to_me_delta</c>:
/// <c>BeginMs</c> + <c>DurationMs</c> form the absolute window;
/// <c>ValidCdTimeMs</c> is the elapsed time inside it (legacy field
/// preferred when &gt; 0, falls back to <c>ValidCDTime</c>).
/// Reused as the value-type stored in <c>GameState.SkillCdMap</c>
/// so consumers can read the carry-forward state directly.
/// </summary>
public sealed record SkillCdSnapshot(
    int SkillLevelId,
    long BeginMs,
    int DurationMs,
    int ValidCdTimeMs,
    int ChargeCount,
    int SubCdRatio,
    long SubCdFixed,
    int AccelerateCdRatio);

/// <summary>
/// One per-resource cooldown row from <c>AoiSyncToMeDelta.FightResCDs</c>.
/// Mirrors Python's <c>_on_sync_to_me_delta</c> dict at packet_parser.py
/// 3957–3973. <c>BeginTime</c> + <c>Duration</c> form the absolute
/// window; <c>ValidCdTimeMs</c> is the elapsed time inside it. Rows
/// with <c>ResId &lt;= 0</c> are filtered out at the decoder, matching
/// Python's <c>if res_id &gt; 0</c> guard.
/// </summary>
public sealed record FightResCdSnapshot(
    int ResId,
    long BeginMs,
    int DurationMs,
    int ValidCdTimeMs);

/// <summary>
/// Sibling event surfaced by <see cref="SyncToMeDeltaInfoDecoder"/>
/// when <c>AoiSyncToMeDelta.FightResCDs</c> is non-empty after the
/// <c>ResId &gt; 0</c> filter. Kept separate from
/// <see cref="ToMeDeltaEvent"/> so adding new packet projections does
/// not break positional record consumers (matches the "surface a
/// sibling event" pattern documented on
/// <see cref="ContainerSyncEvent"/>).
/// </summary>
public sealed record ToMeFightResCdEvent(
    long Uuid,
    IReadOnlyList<FightResCdSnapshot> FightResCds,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// S122 — sibling event surfaced from <see cref="SyncToMeDeltaInfoDecoder"/>
/// when the <c>BaseDelta.TempAttrs</c> branch carries CD-modifier
/// temp attrs for the local player. Mirrors Python's
/// <c>_process_temp_attr_collection</c> at <c>packet_parser.py:4831</c>:
/// sums TempAttr ids 100 (cd_pct, /10000), 101 (cd_fixed ms), and 103
/// (cd_accel, /10000) across the collection. Emitted only when
/// <c>BaseDelta.Uuid</c> is a player low-marker — non-player TempAttrs
/// belong to other (unported) entity classes and would not match the
/// per-player CD math.
/// </summary>
public sealed record TempAttrCdEvent(
    long Uuid,
    int CdPct,
    int CdFixed,
    int CdAccel,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// S123 — AOI-delta buff resync sibling event surfaced from
/// <see cref="SyncToMeDeltaInfoDecoder"/> when
/// <c>BaseDelta.BuffInfos</c> carries a <see cref="BuffInfoSync"/>
/// payload. Mirrors Python's <c>_decode_buff_info_sync_pb</c> +
/// <c>_process_aoi_sync_delta</c> branch at packet_parser.py
/// 4032–4041: rows with <c>BaseId == 0</c> are filtered (matches
/// Python's `if not base_id: continue`) and only emitted when at
/// least one valid entry survives. The decoder does NOT classify
/// player vs monster; the bridge uses <see cref="Uuid"/>'s low
/// marker (640) to route into <c>SelfBuffs</c> versus
/// <c>MonsterDataMap[uuid].BuffList</c>.
/// </summary>
public sealed record AoiBuffSyncEvent(
    long Uuid,
    IReadOnlyList<BuffPayload> Buffs,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// S124 — boss-side buff event log surfaced from
/// <see cref="SyncToMeDeltaInfoDecoder"/> when
/// <c>BaseDelta.BuffEffect</c> (<see cref="BuffEffectSync"/>, field 11)
/// carries <see cref="BuffEffect"/> entries whose <c>Type</c> is in the
/// boss-event allow-list. Mirrors Python's
/// <c>_process_buff_effect_sync</c> at packet_parser.py 4789–4825 +
/// the <c>_BOSS_BUFF_EVENTS</c> filter at line 374. Distinct from
/// <see cref="AoiBuffSyncEvent"/>: that event carries sustained-buff
/// state (resync); this event carries one-shot triggers (phase
/// transitions, shield breaks, deaths). Decoder gates on
/// non-player-uuid only — Python invokes this branch via
/// <c>if delta.HasField('BuffEffect') and target_is_monster:</c>.
/// </summary>
public sealed record BuffEffectPayload(
    int Type,
    int BuffUuid,
    long HostUuid);

public sealed record BuffEffectEvent(
    long TargetUuid,
    IReadOnlyList<BuffEffectPayload> Effects,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// S124 — boss-relevant <c>EBuffEventType</c> values mirrored from
/// Python's <c>BuffEventType</c> class at packet_parser.py 364–371
/// + <c>_BOSS_BUFF_EVENTS</c> set at 374. Only these types pass the
/// decoder's filter.
/// </summary>
public static class BuffEventType
{
    public const int HostDeath = 12;
    public const int BodyPartDead = 15;
    public const int BodyPartStateChange = 17;
    public const int ShieldBroken = 47;
    public const int SuperArmorBroken = 51;
    public const int EnterBreaking = 58;
    public const int IntoFractureState = 88;

    internal static readonly HashSet<int> BossEvents = new()
    {
        HostDeath, BodyPartDead, BodyPartStateChange,
        ShieldBroken, SuperArmorBroken,
        EnterBreaking, IntoFractureState,
    };
}

/// <summary>
/// S126 — player-self AttrCollection event surfaced from
/// <see cref="SyncToMeDeltaInfoDecoder"/> when <c>BaseDelta.Uuid</c>
/// is a player low-marker AND <c>BaseDelta.Attrs</c> is non-empty.
/// Mirrors the identity + HP + profession slice of Python's
/// <c>_process_attr_collection</c> at packet_parser.py 4894–5050.
/// Combat-stat fields (Attack/Defense/Crit/etc) deferred to a
/// follow-up session because they need new GameState slots; this
/// session focuses on closing the gap where AOI-delta updates of
/// already-modeled fields (Name/Level/FightPoint/Hp/MaxHp/Profession)
/// were silently dropped at S121's player-uuid skip.
/// Each field uses a nullable presence sidecar so the bridge can
/// tell "field absent" (legacy default) from "field present with
/// zero value" (real packet — though Python's per-field guards
/// skip most zero writes anyway).
/// </summary>
public sealed record PlayerAttrEvent(
    long Uuid,
    double TimestampSeconds) : ParserEvent(TimestampSeconds)
{
    public string Name { get; init; } = string.Empty;
    public int Level { get; init; }
    public int RankLevel { get; init; }
    public int FightPoint { get; init; }
    public int Hp { get; init; }
    public int MaxHp { get; init; }
    public int ProfessionId { get; init; }

    public bool HasName { get; init; }
    public bool HasLevel { get; init; }
    public bool HasRankLevel { get; init; }
    public bool HasFightPoint { get; init; }
    public bool HasHp { get; init; }
    public bool HasMaxHp { get; init; }
    public bool HasProfessionId { get; init; }

    public bool Any => HasName || HasLevel || HasRankLevel
        || HasFightPoint || HasHp || HasMaxHp || HasProfessionId;
}

/// <summary>
/// S127 — one decoded damage row from a <c>SyncDamageInfo</c> within a
/// <c>SkillEffect</c>. Mirrors the fields Python's
/// <c>_decode_sync_damage_info</c> (packet_parser.py 4092–4253) packs
/// into its <c>event</c> dict — minus the per-event attacker resolution
/// (TopSummonerId / team_members lookup) and target-fallback logic
/// which need bridge state and land in S128. <c>Damage</c> is already
/// resolved via <see cref="Automation.CyCombat.CombatDamageAmount"/>;
/// MISS / FALL rows are filtered at the decoder so they never surface.
/// </summary>
public readonly record struct DamagePayload(
    long AttackerUuid,
    long TopSummonerId,
    int SkillId,
    int DamageSource,
    int OwnerLevel,
    int OwnerStage,
    int HitEventId,
    uint PassiveUuid,
    int DamageType,
    int TypeFlag,
    int DamageMode,
    long Damage,
    long HpLessen,
    long ShieldLessen,
    int Element,
    bool IsCrit,
    bool IsDead,
    bool IsNormal,
    bool IsRainbow,
    bool IsHeal,
    bool IsImmune,
    bool IsAbsorbed);

/// <summary>
/// S127 — surfaces an <c>AoiSyncDelta.SkillEffects</c> (proto field 7,
/// singular despite the plural name) carrying one or more
/// <c>SyncDamageInfo</c> rows. Reachable from BOTH 0x2E
/// <c>SyncToMeDeltaInfo.BaseDelta</c> AND each 0x2D
/// <c>SyncNearDeltaInfo.DeltaInfos[]</c> — same per-delta semantics as
/// S121/S125. <see cref="TargetUuid"/> is the AoiSyncDelta uuid (the
/// damage *target*); per-row <c>AttackerUuid</c> / <c>TopSummonerId</c>
/// are on each <see cref="DamagePayload"/>. Decoder-only event:
/// attribution + DPS rollup land in S128.
/// </summary>
public sealed record SkillEffectEvent(
    long TargetUuid,
    IReadOnlyList<DamagePayload> Damages,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// Raw envelope events surfaced by <see cref="PacketParser"/> before the
/// per-method decode logic lands (Session 5d). Lets headless smoke + tests
/// observe the shape of the packet stream without proto parsing.
/// </summary>
public sealed record RawNotifyEvent(
    int MethodId,
    bool IsZstd,
    int PayloadLength,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record CompressedFrameEvent(
    MessageType Kind,
    int PayloadLength,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

public sealed record UnknownMessageEvent(
    int RawType,
    int PayloadLength,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

/// <summary>
/// Contract a <c>PacketParser</c> implements. Concrete implementation lives
/// in <see cref="PacketParser"/>.
/// </summary>
public interface IPacketParser
{
    event Action<ParserEvent>? Event;

    /// <summary>Feed a complete game frame (post TCP reassembly + length-stripping).</summary>
    void FeedGameFrame(ReadOnlySpan<byte> frame, double timestampSeconds);

    /// <summary>Reset transient state on hard scene change / server reconnect.</summary>
    void Reset();
}
