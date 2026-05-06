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
public sealed record EntityAppearance(long Uuid, int EntityType);
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

public sealed record NearDeltaEvent(
    IReadOnlyList<long> DeltaUuids,
    double TimestampSeconds) : ParserEvent(TimestampSeconds);

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
