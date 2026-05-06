namespace SaoAuto.Core.Vision;

/// <summary>
/// Skill slot ready/cooldown/insufficient state classifier ported from
/// the pure-math tail of <c>skill_recognition.py</c> (<c>_classify_state</c>
/// and <c>_guess_baseline_state</c>). Operates on pre-computed slot
/// metrics — no HSV / OpenCV dependency — so it unit-tests deterministically.
///
/// The HSV + slot-mask measurement front-end (`_measure_slot`,
/// `_compare_to_baseline`) is deferred until a vision pipeline lands;
/// callers can already exercise the classifier with synthetic metrics
/// or with metrics produced from a future C# HSV path.
/// </summary>
public static class SkillStateClassifier
{
    public const string StateReady = "ready";
    public const string StateCooldown = "cooldown";
    public const string StateInsufficientEnergy = "insufficient_energy";
    public const string StateUnknown = "unknown";

    public static readonly SkillSlotBaseline DefaultBaseline = new(
        InnerVMean: 150.0,
        InnerSMean: 150.0,
        RingRatio: 0.16,
        BrightRatio: 0.20,
        IconVMean: 165.0,
        IconSMean: 95.0);

    /// <summary>Heuristic guess of which state a baseline frame represents.</summary>
    public static string GuessBaselineState(SkillSlotMetrics m)
    {
        if (m.WarmRatio >= 0.45 && m.RingRatio <= 0.08) return StateInsufficientEnergy;
        if (m.IconVMean <= 125.0 || m.ShadowRatio >= 0.30) return StateCooldown;
        if (m.ReadyScore >= 0.34 && m.RingRatio >= 0.08) return StateReady;
        return StateUnknown;
    }

    /// <summary>
    /// Classify a frame's slot state given live metrics, the per-slot
    /// baseline metrics, and an optional baseline-comparison block.
    /// Returns (state, intensityRatio) — for cooldown the ratio is the
    /// estimated remaining-cooldown weight in [0,1]; for ready it's 0;
    /// for insufficient it's the icon dimness; for unknown the residual.
    /// </summary>
    public static (string State, double Ratio) Classify(
        SkillSlotMetrics m,
        SkillSlotBaseline baseline,
        SkillBaselineComparison? cmp = null,
        string baselineState = StateUnknown)
    {
        var refV = Math.Max(1.0, baseline.InnerVMean);
        var refRing = Math.Max(0.06, baseline.RingRatio);
        var refIconV = Math.Max(1.0, baseline.IconVMean);

        var vRatio = Math.Min(1.5, m.InnerVMean / refV);
        var ringRel = Math.Min(2.0, m.RingRatio / refRing);
        var iconVRatio = Math.Min(1.5, m.IconVMean / refIconV);

        var insufficientLike =
            m.WarmRatio >= 0.42 && m.RingRatio <= 0.09 && m.DimRatio >= 0.38;
        var readyAbsolute =
            m.RingRatio >= 0.10 && m.IconVMean >= 128.0 &&
            m.GrayDarkRatio <= 0.20 && m.ShadowRatio <= 0.20;

        if (insufficientLike)
            return (StateInsufficientEnergy, Clamp01(Math.Max(0.12, 1.0 - m.IconScore)));

        if (cmp is { } c)
        {
            if (baselineState == StateCooldown)
            {
                if (readyAbsolute && c.ScoreRatio >= 1.06) return (StateReady, 0.0);
                var cd = Clamp01(Math.Max(0.12,
                    (1.0 - Math.Min(1.0, c.ScoreRatio)) * 0.50
                    + m.ShadowRatio * 0.30
                    + m.GrayDarkRatio * 0.20));
                return (StateCooldown, cd);
            }

            if ((c.ScoreRatio <= 0.90 && c.DarkenedRatio >= 0.14) ||
                (c.IconVRatio <= 0.90 && c.DarkenedRatio >= 0.18))
            {
                var cd = Clamp01(
                    (1.0 - Math.Min(1.0, c.ScoreRatio)) * 0.56
                    + c.DarkenedRatio * 0.28
                    + m.ShadowRatio * 0.16);
                return (StateCooldown, Math.Max(0.05, cd));
            }

            if (readyAbsolute ||
                (c.RestoredRatio >= 0.48 && c.ScoreRatio >= 0.93 && m.ShadowRatio <= 0.24))
                return (StateReady, 0.0);
        }

        if (readyAbsolute ||
            (m.ReadyScore >= 0.34 && vRatio >= 0.78 && ringRel >= 0.72 &&
             iconVRatio >= 0.78 && m.GrayDarkRatio <= 0.28))
            return (StateReady, 0.0);

        var cooldownRatio = Clamp01(
            (1.0 - Math.Min(1.0, iconVRatio)) * 0.42
            + m.ShadowRatio * 0.30
            + m.GrayDarkRatio * 0.18
            + (1.0 - Math.Min(1.0, ringRel)) * 0.10);
        if (cooldownRatio >= 0.08) return (StateCooldown, Math.Max(0.05, cooldownRatio));
        return (StateUnknown, cooldownRatio);
    }

    private static double Clamp01(double v) => Math.Clamp(v, 0.0, 1.0);
}

public sealed record SkillSlotMetrics(
    double InnerVMean,
    double InnerSMean,
    double IconVMean,
    double IconSMean,
    double RingRatio,
    double BrightRatio,
    double DarkRatio,
    double GrayDarkRatio,
    double DimRatio,
    double WarmRatio,
    double ShadowRatio,
    double IconScore,
    double ReadyScore);

public sealed record SkillSlotBaseline(
    double InnerVMean,
    double InnerSMean,
    double RingRatio,
    double BrightRatio,
    double IconVMean,
    double IconSMean);

public sealed record SkillBaselineComparison(
    double IconVRatio,
    double IconSRatio,
    double ScoreRatio,
    double DarkenedRatio,
    double RestoredRatio,
    double AvgDeltaV);
