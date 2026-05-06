namespace SaoAuto.Core.State;

/// <summary>
/// Pure validation/clamp rules ported from Python <c>game_state.GameStateManager.update</c>.
/// These were intentionally deferred from Session 3 — they belong next to the
/// packet writers (Session 5b parser will compose these into <c>GameStateManager.Update</c>
/// mutators). Keeping them here as static methods means tests can pin the rules
/// before the live writer exists.
/// </summary>
public static class GameStateValidation
{
    public static double ClampPercent(double value) => Math.Clamp(value, 0.0, 1.0);

    public static bool TryAcceptLevelBase(int value) => value >= 0 && value <= 999;

    public static bool TryAcceptLevelExtra(int value) => value >= 0 && value <= 999;

    public static bool TryAcceptNonNegative(int value) => value >= 0;

    public static bool TryAcceptPlayerName(string value) => value.Length <= 20;

    public static bool TryAcceptPlayerId(string value) => value.Length <= 30;

    public static bool TryAcceptIdentityAlertSerial(int value) => value >= 0;

    public static bool TryAcceptIdentityAlertTitle(string value) => value.Length <= 80;

    public static bool TryAcceptIdentityAlertMessage(string value) => value.Length <= 600;

    public static bool TryAcceptBossRaidPhaseName(string value) => value.Length <= 60;

    public static bool TryAcceptBossEnrageRemaining(double value) => value >= 0;

    public static bool TryAcceptBossTimerText(string value) => value.Length <= 40;

    /// <summary>
    /// HP rollback rule from Python: when <c>hp_current</c> arrives as 0 but
    /// <c>hp_pct</c> is &gt; 0.001 and a previous non-zero current was tracked,
    /// reuse the previous value (or fall back to <c>hp_max</c>). The Python
    /// <c>GameStateManager</c> keeps <c>_prev_hp_current</c>; the C# port lets the
    /// caller pass it in so the validator stays a pure function.
    /// </summary>
    public static int RollbackHpCurrent(int incoming, double incomingPct, int hpMax, int previousNonZero)
    {
        if (incoming != 0) return incoming;
        if (incomingPct <= 0.001) return incoming;
        if (hpMax <= 0) return incoming;
        if (previousNonZero > 0) return previousNonZero;
        return hpMax;
    }

    /// <summary>
    /// Cap <c>hp_current</c> to <c>hp_max</c> when the latter is positive.
    /// Mirrors Python's post-update reconciliation step.
    /// </summary>
    public static int CapToMax(int current, int max)
    {
        if (max <= 0) return current;
        return current > max ? max : current;
    }

    /// <summary>Same shape as <see cref="CapToMax(int,int)"/> but for stamina.</summary>
    public static int CapStaminaToMax(int current, int max) => CapToMax(current, max);
}
