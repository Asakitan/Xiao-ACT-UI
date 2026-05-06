using System.Collections.Immutable;

namespace SaoAuto.Overlay.Panels;

/// <summary>
/// Render-input record for the HP overlay. Mirrors `web/hp.html` data fields.
/// </summary>
public sealed record HpOverlaySnapshot(
    int HpCurrent,
    int HpMax,
    double HpPct,
    double StaminaPct,
    bool StaminaOffline,
    string PlayerName,
    string LevelText,
    bool RecognitionOk);

/// <summary>Render-input record for the BossHP overlay.</summary>
public sealed record BossHpOverlaySnapshot(
    bool Active,
    long CurrentHp,
    long MaxHp,
    double EstimatedPct,
    int BreakingStage,
    bool ShieldActive,
    double ShieldPct,
    bool InOverdrive,
    bool Invincible,
    string PhaseName,
    double EnrageRemainingSeconds);

/// <summary>Render-input record for the DPS overlay.</summary>
public sealed record DpsOverlaySnapshot(
    bool LiveMode,
    long TotalDamage,
    long Dps,
    long TotalHeal,
    long Hps,
    double DurationSeconds,
    ImmutableArray<DpsRow> Rows);

public sealed record DpsRow(
    string Name,
    long Damage,
    long PerSecond,
    int ProfessionId,
    bool IsSelf);

/// <summary>Render-input record for the BurstReady overlay.</summary>
public sealed record BurstReadySnapshot(
    bool Ready,
    int WatchedSlotCount,
    int ReadySlotCount,
    bool InCombat);

/// <summary>Render-input record for the alert overlay.</summary>
public sealed record AlertSnapshot(
    int Serial,
    AlertSeverity Severity,
    string Title,
    string Message,
    double TimestampSeconds);

public enum AlertSeverity
{
    Info,
    Warning,
    Error,
    Identity,
}

/// <summary>Render-input record for the buff monitor overlay.</summary>
public sealed record BuffMonOverlaySnapshot(
    bool Enabled,
    ImmutableArray<BuffMonEntry> Buffs,
    double ServerTimeOffsetMs);

public sealed record BuffMonEntry(
    int Id,
    long Uuid,
    long BeginMs,
    int DurationMs,
    int Layer,
    int Count,
    string Name);
