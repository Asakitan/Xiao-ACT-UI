using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S80 — Recognition tick-loop scalar state machines.
/// </summary>
public class Session80RecognitionTickStateTests
{
    // ── StaminaOfflineGate ──────────────────────────────────────────

    [Fact]
    public void Offline_GoodConfidence_StaysOnline()
    {
        var g = new StaminaOfflineGate();
        var d = g.Push(0.8, 100.0, warmupUntil: 0.0);
        Assert.False(d.Offline);
        Assert.False(d.JustWentOffline);
        Assert.False(d.JustRecovered);
    }

    [Fact]
    public void Offline_LowConfidence_NeedsDebounceBeforeOffline()
    {
        var g = new StaminaOfflineGate();
        var first = g.Push(0.05, 100.0, 0.0);
        Assert.False(first.Offline); // 0s elapsed, not yet offline
        var late = g.Push(0.05, 100.11, 0.0);
        Assert.True(late.Offline);
        Assert.True(late.JustWentOffline);
        // Subsequent low-conf does not re-fire JustWentOffline
        var stillOffline = g.Push(0.05, 100.20, 0.0);
        Assert.True(stillOffline.Offline);
        Assert.False(stillOffline.JustWentOffline);
    }

    [Fact]
    public void Offline_WarmupSuppressesOfflineTransition()
    {
        var g = new StaminaOfflineGate();
        g.Push(0.05, 100.0, warmupUntil: 102.0);
        var debounced = g.Push(0.05, 100.15, warmupUntil: 102.0);
        Assert.False(debounced.Offline);
        Assert.False(debounced.JustWentOffline);
        Assert.False(g.IsOffline);
    }

    [Fact]
    public void Offline_RecoversAfterDebounce()
    {
        var g = new StaminaOfflineGate();
        g.Push(0.05, 100.0, 0.0);
        g.Push(0.05, 100.11, 0.0); // offline now
        Assert.True(g.IsOffline);
        var firstGood = g.Push(0.9, 100.20, 0.0);
        Assert.True(firstGood.Offline); // online clock starts but still offline
        Assert.False(firstGood.JustRecovered);
        var recovered = g.Push(0.9, 100.31, 0.0);
        Assert.False(recovered.Offline);
        Assert.True(recovered.JustRecovered);
        Assert.False(g.IsOffline);
    }

    [Fact]
    public void Offline_GoodConfidenceFlickerDoesNotRecover()
    {
        var g = new StaminaOfflineGate();
        g.Push(0.05, 100.0, 0.0);
        g.Push(0.05, 100.11, 0.0); // offline
        // Single good-confidence frame, then back to bad
        g.Push(0.9, 100.15, 0.0);
        var bad = g.Push(0.05, 100.16, 0.0);
        // online clock reset by the bad frame; remains offline
        Assert.True(bad.Offline);
        Assert.Equal(0.0, g.OnlineSince);
    }

    [Fact]
    public void Offline_Reset_ClearsState()
    {
        var g = new StaminaOfflineGate();
        g.Push(0.05, 100.0, 0.0);
        g.Push(0.05, 100.11, 0.0);
        g.Reset();
        Assert.False(g.IsOffline);
        Assert.Equal(0.0, g.OfflineSince);
        Assert.Equal(0.0, g.OnlineSince);
    }

    // ── AdaptiveFpsSelector ─────────────────────────────────────────

    [Fact]
    public void Fps_Disabled_AlwaysActive()
    {
        var sel = new AdaptiveFpsSelector(activeFps: 10, idleFps: 4, idleAfterSeconds: 30, enabled: false);
        Assert.Equal(10.0, sel.Current(0.5, 1000.0));
    }

    [Fact]
    public void Fps_NullStamina_StaysActive()
    {
        var sel = new AdaptiveFpsSelector();
        Assert.Equal(10.0, sel.Current(null, 1000.0));
    }

    [Fact]
    public void Fps_StaminaIdle_DropsToIdleFpsAfterWindow()
    {
        var sel = new AdaptiveFpsSelector(activeFps: 10, idleFps: 4, idleAfterSeconds: 30);
        // First call seeds the value
        Assert.Equal(10.0, sel.Current(0.5, 1000.0));
        // Within window, still active
        Assert.Equal(10.0, sel.Current(0.5, 1020.0));
        // Past window, drops
        Assert.Equal(4.0, sel.Current(0.5, 1031.0));
    }

    [Fact]
    public void Fps_StaminaChange_ResetsToActive()
    {
        var sel = new AdaptiveFpsSelector(activeFps: 10, idleFps: 4, idleAfterSeconds: 30);
        sel.Current(0.5, 1000.0);
        Assert.Equal(4.0, sel.Current(0.5, 1031.0));
        Assert.Equal(10.0, sel.Current(0.6, 1031.5));
    }

    [Fact]
    public void Fps_Reset_ClearsTrackedValue()
    {
        var sel = new AdaptiveFpsSelector();
        sel.Current(0.5, 1000.0);
        sel.Reset();
        Assert.Equal(10.0, sel.Current(0.5, 1100.0));
    }
}
