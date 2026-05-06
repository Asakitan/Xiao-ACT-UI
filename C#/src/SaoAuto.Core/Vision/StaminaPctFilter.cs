namespace SaoAuto.Core.Vision;

/// <summary>
/// S79 — Stamina-percentage offline state machine. Bit-faithful port of
/// <c>recognition.RecognitionEngine._filter_stamina_pct</c> +
/// <c>_accept_stamina_pct</c> (recognition.py lines 818–867).
///
/// Drops noisy or low-confidence readings, locks against false recovery
/// for 0.30 s after a real drop, and only accepts large jumps after
/// they've been confirmed at the same value for ≥0.10 s. Stateful but
/// not thread-safe — designed to be called once per recognition tick
/// from the single vision thread.
///
/// <para>Clock is injected as <c>nowSeconds</c> so the filter is
/// deterministic under unit test (no <c>time.time()</c> coupling).</para>
/// </summary>
public sealed class StaminaPctFilter
{
    public const double LargeDeltaThreshold = 0.20;
    public const double LargeDeltaConfirmS = 0.10;
    public const double LargeDeltaStableEpsilon = 0.08;
    public const double LowConfidence = 0.20;
    public const double DropLockSeconds = 0.30;
    public const double FullSnapThreshold = 0.98;

    public double? FilteredPct { get; private set; }
    public double? PendingPct { get; private set; }
    public double PendingSince { get; private set; }
    public double DropLockUntil { get; private set; }

    /// <summary>
    /// Feed a raw stamina reading + detector confidence. Returns the
    /// filtered (UI-stable) value. Always in [0, 1].
    /// </summary>
    public double Push(double rawPct, double confidence, double nowSeconds)
    {
        rawPct = Math.Clamp(rawPct, 0.0, 1.0);
        var stable = FilteredPct;

        if (stable is null) return Accept(rawPct, nowSeconds);
        if (confidence < LowConfidence) return stable.Value;

        if (rawPct > stable.Value && nowSeconds < DropLockUntil) return stable.Value;

        var delta = rawPct - stable.Value;
        if (Math.Abs(delta) > LargeDeltaThreshold)
        {
            if (PendingPct is null)
            {
                PendingPct = rawPct;
                PendingSince = nowSeconds;
                return stable.Value;
            }

            var pendingDelta = rawPct - PendingPct.Value;
            if (Math.Abs(pendingDelta) <= LargeDeltaStableEpsilon)
            {
                if ((nowSeconds - PendingSince) >= LargeDeltaConfirmS)
                    return Accept(rawPct, nowSeconds);
                return stable.Value;
            }

            if (Math.Abs(rawPct - stable.Value) <= LargeDeltaStableEpsilon)
            {
                PendingPct = null;
                PendingSince = 0.0;
                return stable.Value;
            }

            PendingPct = rawPct;
            PendingSince = nowSeconds;
            return stable.Value;
        }

        return Accept(rawPct, nowSeconds);
    }

    public void Reset()
    {
        FilteredPct = null;
        PendingPct = null;
        PendingSince = 0.0;
        DropLockUntil = 0.0;
    }

    private double Accept(double pct, double nowSeconds)
    {
        pct = Math.Clamp(pct, 0.0, 1.0);
        if (pct >= FullSnapThreshold) pct = 1.0;
        var prev = FilteredPct;
        if (prev is { } p && pct < p - 0.001)
            DropLockUntil = Math.Max(DropLockUntil, nowSeconds + DropLockSeconds);
        FilteredPct = pct;
        PendingPct = null;
        PendingSince = 0.0;
        return pct;
    }
}
