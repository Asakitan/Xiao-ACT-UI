namespace SaoAuto.Core.Vision;

/// <summary>
/// S79 — Per-slot skill state tracker. Bit-faithful port of
/// <c>skill_recognition.SkillVisualTracker.analyze</c>'s state-machine
/// inner loop (skill_recognition.py lines 304–404), minus the
/// file-system IO (baseline PNG persistence) and ROI lookup which are
/// caller-side concerns.
///
/// <para>Per-slot state:</para>
/// <list type="bullet">
///   <item>Baseline metrics + baseline image (cached on first
///         non-null measurement)</item>
///   <item>Pending state + pending-count for confirm-frames
///         hysteresis (default 2)</item>
///   <item>Stable state — only flips when pending count reaches
///         confirm-frames threshold</item>
///   <item>EMA drift of the baseline metrics while stable=ready
///         (matches Python: 0.88·base + 0.12·current for V/S means,
///         max(0.86·base + 0.14·current, current·0.94) for ratios)</item>
/// </list>
///
/// Composes <see cref="SkillSlotHsvMeasurer"/> (S78) +
/// <see cref="SkillStateClassifier"/> (S39).
/// </summary>
public sealed class SkillSlotTracker
{
    private readonly int _confirmFrames;
    private readonly Dictionary<int, SlotState> _slots = new();

    public SkillSlotTracker(int confirmFrames = 2)
    {
        _confirmFrames = Math.Max(1, confirmFrames);
    }

    public void Reset() => _slots.Clear();

    /// <summary>
    /// Measure + classify a slot frame and update its hysteresis state.
    /// Returns the stable (UI-presentable) result. Pass null/empty bgr
    /// for a missing capture; the slot will hold its previous stable
    /// state and emit raw_state="unknown".
    /// </summary>
    public SkillSlotResult Process(int slotIndex, byte[]? bgr, int width, int height)
    {
        var store = Slot(slotIndex);
        var metrics = bgr is null ? null
            : SkillSlotHsvMeasurer.Measure(bgr, width, height);

        if (metrics is not null && store.BaselineImage is null && bgr is not null)
        {
            store.BaselineImage = (byte[])bgr.Clone();
            store.BaselineWidth = width;
            store.BaselineHeight = height;
            store.Baseline = MetricsToBaseline(metrics);
            store.BaselineState = SkillStateClassifier.GuessBaselineState(metrics);
        }

        SkillBaselineComparison? cmp = null;
        if (bgr is not null && store.BaselineImage is not null
            && store.BaselineWidth == width && store.BaselineHeight == height)
        {
            cmp = SkillSlotHsvMeasurer.Compare(bgr, store.BaselineImage, width, height);
        }

        string rawState;
        double cooldownRatio;
        if (metrics is null)
        {
            rawState = SkillStateClassifier.StateUnknown;
            cooldownRatio = 0.0;
        }
        else
        {
            (rawState, cooldownRatio) = SkillStateClassifier.Classify(
                metrics, store.Baseline, cmp, store.BaselineState);
        }

        if (rawState == store.PendingState) store.PendingCount++;
        else { store.PendingState = rawState; store.PendingCount = 1; }

        var readyEdge = false;
        if (store.PendingCount >= _confirmFrames)
        {
            var previous = store.StableState;
            store.StableState = rawState;
            if (previous != SkillStateClassifier.StateReady &&
                rawState == SkillStateClassifier.StateReady)
                readyEdge = true;
        }

        if (metrics is not null && store.StableState == SkillStateClassifier.StateReady)
            DriftBaseline(store, metrics);

        var stableState = store.StableState;
        if (stableState == SkillStateClassifier.StateReady) cooldownRatio = 0.0;
        cooldownRatio = Math.Clamp(cooldownRatio, 0.0, 1.0);
        var rounded = Math.Round(cooldownRatio, 3);

        return new SkillSlotResult(
            Index: slotIndex,
            State: stableState,
            CooldownRatio: rounded,
            InsufficientEnergy: stableState == SkillStateClassifier.StateInsufficientEnergy,
            Active: stableState == SkillStateClassifier.StateReady,
            ReadyEdge: readyEdge);
    }

    private SlotState Slot(int idx)
    {
        if (!_slots.TryGetValue(idx, out var s))
        {
            s = new SlotState
            {
                Baseline = SkillStateClassifier.DefaultBaseline,
                BaselineState = SkillStateClassifier.StateUnknown,
                StableState = SkillStateClassifier.StateUnknown,
                PendingState = null,
                PendingCount = 0,
            };
            _slots[idx] = s;
        }
        return s;
    }

    private static SkillSlotBaseline MetricsToBaseline(SkillSlotMetrics m)
        => new(InnerVMean: m.InnerVMean, InnerSMean: m.InnerSMean,
               RingRatio: m.RingRatio, BrightRatio: m.BrightRatio,
               IconVMean: m.IconVMean, IconSMean: m.IconSMean);

    private static void DriftBaseline(SlotState store, SkillSlotMetrics m)
    {
        var b = store.Baseline;
        var newInnerV = b.InnerVMean * 0.88 + m.InnerVMean * 0.12;
        var newInnerS = b.InnerSMean * 0.88 + m.InnerSMean * 0.12;
        var newIconV = b.IconVMean * 0.88 + m.IconVMean * 0.12;
        var newIconS = b.IconSMean * 0.88 + m.IconSMean * 0.12;
        var newRing = Math.Max(b.RingRatio * 0.86 + m.RingRatio * 0.14, m.RingRatio * 0.94);
        var newBright = Math.Max(b.BrightRatio * 0.86 + m.BrightRatio * 0.14, m.BrightRatio * 0.94);
        store.Baseline = new SkillSlotBaseline(
            InnerVMean: newInnerV, InnerSMean: newInnerS,
            RingRatio: newRing, BrightRatio: newBright,
            IconVMean: newIconV, IconSMean: newIconS);
    }

    private sealed class SlotState
    {
        public SkillSlotBaseline Baseline;
        public byte[]? BaselineImage;
        public int BaselineWidth;
        public int BaselineHeight;
        public string BaselineState = SkillStateClassifier.StateUnknown;
        public string StableState = SkillStateClassifier.StateUnknown;
        public string? PendingState;
        public int PendingCount;
    }
}

public sealed record SkillSlotResult(
    int Index,
    string State,
    double CooldownRatio,
    bool InsufficientEnergy,
    bool Active,
    bool ReadyEdge);
