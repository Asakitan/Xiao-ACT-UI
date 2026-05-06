using System.Collections.Immutable;

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
}
