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
