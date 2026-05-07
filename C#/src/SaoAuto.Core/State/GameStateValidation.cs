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

    /// <summary>
    /// S88 — Level-base rollback (Python game_state.py 267–270).
    /// When the new <c>level_base</c> arrives as 0 but a previous non-zero
    /// value was tracked, reuse the previous value (treat zero as "stale
    /// reading", not "level is actually zero").
    /// </summary>
    public static int RollbackLevelBase(int incoming, int previousNonZero)
    {
        if (incoming != 0) return incoming;
        return previousNonZero > 0 ? previousNonZero : incoming;
    }

    /// <summary>
    /// S88 — Level-extra rollback (Python game_state.py 271–274). Same
    /// shape as <see cref="RollbackLevelBase"/> but for the bracketed
    /// "+XX" extra-level field.
    /// </summary>
    public static int RollbackLevelExtra(int incoming, int previousNonZero)
    {
        if (incoming != 0) return incoming;
        return previousNonZero > 0 ? previousNonZero : incoming;
    }

    /// <summary>
    /// S88 — Stamina-current rollback (Python game_state.py 275–280).
    /// When <c>stamina_current</c> arrives as 0 *and* no <c>stamina_pct</c>
    /// is being written *and* <c>stamina_max</c> is non-positive, reuse the
    /// previously-tracked non-zero value. The Python guard exists because
    /// stamina_max==0 means we have no anchor for the % conversion, so the
    /// 0 is almost always a misread rather than an actual empty bar.
    /// </summary>
    /// <param name="incoming">Newly arrived stamina_current.</param>
    /// <param name="incomingStaminaPctIsExplicit">
    /// True when the same update batch also writes stamina_pct — in that
    /// case Python skips the rollback (the explicit pct already pins truth).
    /// </param>
    /// <param name="incomingStaminaMax">
    /// Stamina max effective for this update (either the new incoming value
    /// or the existing snapshot's value when not being mutated).
    /// </param>
    /// <param name="previousNonZero">Tracked previous non-zero stamina_current.</param>
    public static int RollbackStaminaCurrent(
        int incoming,
        bool incomingStaminaPctIsExplicit,
        int incomingStaminaMax,
        int previousNonZero)
    {
        if (incoming != 0) return incoming;
        if (incomingStaminaPctIsExplicit) return incoming;
        if (incomingStaminaMax > 0) return incoming;
        return previousNonZero > 0 ? previousNonZero : incoming;
    }
}
