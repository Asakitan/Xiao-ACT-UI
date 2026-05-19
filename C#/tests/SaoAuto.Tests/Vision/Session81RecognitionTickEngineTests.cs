using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S81 — RecognitionTickEngine composition tests. Drives the engine
/// with a stub IFrameCapture + window provider + injected clock,
/// verifies the per-tick projection record matches the Python flow.
/// </summary>
public class Session81RecognitionTickEngineTests
{
    private sealed class StubCapture : IFrameCapture
    {
        public Queue<CapturedFrame?> Frames { get; } = new();
        public int CallCount { get; private set; }
        public CapturedFrame? Capture()
        {
            CallCount++;
            return Frames.Count > 0 ? Frames.Dequeue() : null;
        }
        public void Dispose() { }
    }

    private static WindowCandidate MakeWindow(int w = 100, int h = 100) =>
        new(IntPtr.Zero, "stub", "stub.exe", 0, 0, w, h);

    /// <summary>Solid gold-colour BGRA32 frame (255,174,53,255) at width×height.</summary>
    private static CapturedFrame MakeGoldFrame(int width, int height)
    {
        var stride = width * 4;
        var px = new byte[stride * height];
        for (var i = 0; i < px.Length; i += 4)
        {
            px[i] = 53; px[i + 1] = 174; px[i + 2] = 255; px[i + 3] = 255;
        }
        return new CapturedFrame(width, height, stride, px);
    }

    /// <summary>Black BGRA32 frame.</summary>
    private static CapturedFrame MakeBlackFrame(int width, int height)
    {
        var stride = width * 4;
        return new CapturedFrame(width, height, stride, new byte[stride * height]);
    }

    private static (RecognitionTickEngine engine, StubCapture cap, Func<double> setNow)
        MakeEngine(WindowCandidate? window, double initialClock = 100.0,
                   double warmupSeconds = 0.0)
    {
        var cap = new StubCapture();
        var clock = initialClock;
        Func<double> readClock = () => clock;
        Action<double> setClock = v => clock = v;
        var roi = new Roi(0.0, 0.0, 1.0, 1.0);
        var engine = new RecognitionTickEngine(
            cap,
            () => window,
            roi,
            warmupSeconds: warmupSeconds,
            clock: readClock);
        return (engine, cap, () => clock);
    }

    [Fact]
    public void Tick_NoWindow_ReturnsErrorProjection()
    {
        var cap = new StubCapture();
        var engine = new RecognitionTickEngine(
            cap, () => null, new Roi(0, 0, 1, 1), clock: () => 100.0);
        var r = engine.Tick();
        Assert.False(r.RecognitionOk);
        Assert.Equal("game window not found", r.ErrorMsg);
        Assert.Null(r.Window);
        Assert.Equal(FrameSource.None, r.FrameSource);
        Assert.Equal(0, cap.CallCount);
    }

    [Fact]
    public void Tick_CaptureNull_ReportsCaptureFailure()
    {
        var cap = new StubCapture();
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16), new Roi(0, 0, 1, 1), clock: () => 100.0);
        var r = engine.Tick();
        Assert.False(r.RecognitionOk);
        Assert.Equal("vision capture failed", r.ErrorMsg);
        Assert.Equal(FrameSource.None, r.FrameSource);
    }

    [Fact]
    public void Tick_GoldFrame_StaminaPctApproachesOne()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock);
        var r = engine.Tick();
        Assert.True(r.RecognitionOk);
        Assert.Equal(FrameSource.Live, r.FrameSource);
        Assert.False(r.StaminaOffline);
        Assert.NotNull(r.StaminaPct);
        Assert.Equal(1.0, r.StaminaPct!.Value, 3);
    }

    [Fact]
    public void Tick_BlackFrame_GoesOfflineAfterDebounce()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeBlackFrame(64, 16));
        cap.Frames.Enqueue(MakeBlackFrame(64, 16));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock);
        var first = engine.Tick();
        Assert.False(first.StaminaOffline); // debounce not elapsed
        clock = 100.20;
        var second = engine.Tick();
        Assert.True(second.StaminaOffline);
        Assert.True(second.JustWentOfflineOrFalse());
    }

    [Fact]
    public void Tick_WarmupSuppressesOffline()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeBlackFrame(64, 16));
        cap.Frames.Enqueue(MakeBlackFrame(64, 16));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock,
            warmupSeconds: 5.0);
        engine.Tick();
        clock = 100.20;
        var second = engine.Tick();
        Assert.False(second.StaminaOffline); // warm-up suppresses
    }

    [Fact]
    public void Tick_CachedFrame_BridgesTransientGap()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        // Second call returns null (transient capture failure)
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock,
            frameCacheTtlSeconds: 1.0);
        var first = engine.Tick();
        Assert.Equal(FrameSource.Live, first.FrameSource);
        clock = 100.10;
        var second = engine.Tick();
        Assert.Equal(FrameSource.Cached, second.FrameSource);
        Assert.True(second.RecognitionOk);
    }

    [Fact]
    public void Tick_CacheExpired_ReportsFailure()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock,
            frameCacheTtlSeconds: 0.5);
        engine.Tick();
        clock = 101.0; // > TTL
        var second = engine.Tick();
        Assert.False(second.RecognitionOk);
        Assert.Equal(FrameSource.None, second.FrameSource);
    }

    [Fact]
    public void Tick_BackoffSkipsCaptureAfterRepeatedFailures()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        // No frames enqueued — every Capture() returns null.
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock,
            frameCacheTtlSeconds: 0.0,
            captureBackoffThreshold: 3,
            captureBackoffEveryN: 5);
        for (var i = 0; i < 3; i++) engine.Tick();
        Assert.Equal(3, cap.CallCount);
        // Next ticks should be skipped (backoff active)
        var skipped = engine.Tick();
        Assert.Equal(FrameSource.SkippedBackoff, skipped.FrameSource);
        Assert.True(skipped.RecognitionOk);
        Assert.Equal(3, cap.CallCount); // capture not invoked
    }

    [Fact]
    public void Tick_NextFpsDropsAfterIdleWindow()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        for (var i = 0; i < 3; i++) cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1),
            fps: new AdaptiveFpsSelector(activeFps: 10, idleFps: 4, idleAfterSeconds: 5),
            clock: readClock);
        var r1 = engine.Tick();
        Assert.Equal(10.0, r1.NextFps);
        clock = 102.0;
        var r2 = engine.Tick();
        Assert.Equal(10.0, r2.NextFps);
        clock = 110.0;
        var r3 = engine.Tick();
        Assert.Equal(4.0, r3.NextFps);
    }

    [Fact]
    public void Tick_Reset_ClearsAllState()
    {
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(64, 16),
            new Roi(0, 0, 1, 1), clock: readClock);
        engine.Tick();
        engine.Reset();
        // After reset, capture still null but cache cleared, fail count zero.
        var post = engine.Tick();
        Assert.False(post.RecognitionOk);
        Assert.Equal("vision capture failed", post.ErrorMsg);
    }
}

internal static class RecognitionTickResultExtensions
{
    // Convenience for older test phrasing — kept terse.
    public static bool JustWentOfflineOrFalse(this RecognitionTickResult r)
        => r.StaminaJustWentOffline;
}
