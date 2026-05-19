using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S79 — Stamina filter + skill slot tracker glue. Verifies the
/// offline state machine semantics of
/// <c>recognition._filter_stamina_pct</c> +
/// <c>SkillVisualTracker.analyze</c>.
/// </summary>
public class Session79RecognitionGlueTests
{
    // ── StaminaPctFilter ────────────────────────────────────────────

    [Fact]
    public void Filter_FirstReading_AcceptedImmediately()
    {
        var f = new StaminaPctFilter();
        Assert.Equal(0.5, f.Push(0.5, 0.9, 100.0));
        Assert.Equal(0.5, f.FilteredPct);
    }

    [Fact]
    public void Filter_LowConfidence_HoldsStable()
    {
        var f = new StaminaPctFilter();
        f.Push(0.5, 0.9, 100.0);
        Assert.Equal(0.5, f.Push(0.99, 0.10, 100.5));
        Assert.Equal(0.5, f.FilteredPct);
    }

    [Fact]
    public void Filter_SmallChange_AcceptedDirectly()
    {
        var f = new StaminaPctFilter();
        f.Push(0.50, 0.9, 100.0);
        Assert.Equal(0.55, f.Push(0.55, 0.9, 100.1));
    }

    [Fact]
    public void Filter_DropLockBlocksRecoveryFor300ms()
    {
        var f = new StaminaPctFilter();
        f.Push(0.80, 0.9, 100.0);
        f.Push(0.50, 0.9, 100.1); // drop accepted (delta 0.30 > 0.20 → pending)
        // Confirm the drop after 0.10s pending interval
        var afterDrop = f.Push(0.50, 0.9, 100.21);
        Assert.Equal(0.50, afterDrop);
        // Recovery suppressed during lock
        Assert.Equal(0.50, f.Push(0.80, 0.9, 100.30));
        // After lock expires, small recovery accepted
        Assert.Equal(0.55, f.Push(0.55, 0.9, 100.60));
    }

    [Fact]
    public void Filter_LargeJumpRequiresConfirm()
    {
        var f = new StaminaPctFilter();
        f.Push(0.30, 0.9, 100.0);
        // First large jump: pending only, returns stable
        Assert.Equal(0.30, f.Push(0.80, 0.9, 100.05));
        Assert.NotNull(f.PendingPct);
        // Same value within stable epsilon, but only 0.04s after → still pending
        Assert.Equal(0.30, f.Push(0.81, 0.9, 100.09));
        // Now ≥0.10s confirm window elapsed → accepted
        Assert.Equal(0.81, f.Push(0.81, 0.9, 100.20));
    }

    [Fact]
    public void Filter_LargeJumpThatRetreatsToStable_ResetsPending()
    {
        var f = new StaminaPctFilter();
        f.Push(0.30, 0.9, 100.0);
        f.Push(0.80, 0.9, 100.05);
        Assert.NotNull(f.PendingPct);
        // Reading drops back near stable: delta 0.02 ≤ threshold so the
        // small-change branch triggers and accepts 0.32 directly. The
        // pending-clear path is only reachable when the new reading is
        // still a *large* jump (>0.20) but happens to fall within
        // epsilon of stable — practically rare. Mirrors Python flow.
        Assert.Equal(0.32, f.Push(0.32, 0.9, 100.07));
        Assert.Null(f.PendingPct);
    }

    [Fact]
    public void Filter_HighReading_SnapsToOne()
    {
        var f = new StaminaPctFilter();
        Assert.Equal(1.0, f.Push(0.985, 0.9, 100.0));
    }

    [Fact]
    public void Filter_Reset_ClearsState()
    {
        var f = new StaminaPctFilter();
        f.Push(0.5, 0.9, 100.0);
        f.Reset();
        Assert.Null(f.FilteredPct);
        Assert.Null(f.PendingPct);
        Assert.Equal(0.0, f.DropLockUntil);
    }

    // ── SkillSlotTracker ────────────────────────────────────────────

    private static byte[] MakeBgr(int w, int h, byte b, byte g, byte r)
    {
        var buf = new byte[w * h * 3];
        for (var i = 0; i < buf.Length; i += 3) { buf[i] = b; buf[i + 1] = g; buf[i + 2] = r; }
        return buf;
    }

    [Fact]
    public void Tracker_NoCapture_HoldsUnknown()
    {
        var t = new SkillSlotTracker();
        var r = t.Process(0, null, 0, 0);
        Assert.Equal(SkillStateClassifier.StateUnknown, r.State);
        Assert.False(r.ReadyEdge);
    }

    [Fact]
    public void Tracker_BlackSlot_BecomesCooldownAfterConfirmFrames()
    {
        var t = new SkillSlotTracker(confirmFrames: 2);
        var black = MakeBgr(32, 32, 0, 0, 0);
        var first = t.Process(0, black, 32, 32);
        // First frame: pending only, stable still default unknown
        Assert.Equal(SkillStateClassifier.StateUnknown, first.State);
        var second = t.Process(0, black, 32, 32);
        Assert.Equal(SkillStateClassifier.StateCooldown, second.State);
        Assert.True(second.CooldownRatio > 0.0);
    }

    [Fact]
    public void Tracker_CyanSlot_BecomesReadyAndFiresEdge()
    {
        // Cyan flood passes readyAbsolute: RingRatio≈1, IconVMean=255,
        // GrayDark=0, Shadow=0 — the canonical "ready" synthetic input.
        var t = new SkillSlotTracker(confirmFrames: 1);
        var cyan = MakeBgr(32, 32, 255, 255, 0);
        var ready = t.Process(0, cyan, 32, 32);
        Assert.Equal(SkillStateClassifier.StateReady, ready.State);
        Assert.True(ready.ReadyEdge);
        Assert.Equal(0.0, ready.CooldownRatio);
        Assert.True(ready.Active);
        // Subsequent ready frame does not re-fire the edge.
        var stillReady = t.Process(0, cyan, 32, 32);
        Assert.False(stillReady.ReadyEdge);
    }

    [Fact]
    public void Tracker_SlotsAreIndependent()
    {
        var t = new SkillSlotTracker(confirmFrames: 1);
        var black = MakeBgr(32, 32, 0, 0, 0);
        var cyan = MakeBgr(32, 32, 255, 255, 0);
        var a = t.Process(0, black, 32, 32);
        var b = t.Process(1, cyan, 32, 32);
        Assert.Equal(SkillStateClassifier.StateCooldown, a.State);
        Assert.Equal(SkillStateClassifier.StateReady, b.State);
    }

    [Fact]
    public void Tracker_Reset_ClearsAllSlots()
    {
        var t = new SkillSlotTracker(confirmFrames: 1);
        var cyan = MakeBgr(32, 32, 255, 255, 0);
        t.Process(0, cyan, 32, 32);
        t.Reset();
        var post = t.Process(0, cyan, 32, 32);
        Assert.Equal(SkillStateClassifier.StateReady, post.State);
        Assert.True(post.ReadyEdge);
    }
}
