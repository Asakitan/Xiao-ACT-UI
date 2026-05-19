using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S81c — RecognitionTickHost tests. Exercises the per-tick
/// project + sleep-budget math via the test seam <c>RunOnceAsync</c>,
/// plus start/stop lifecycle on the threaded loop.
/// </summary>
public class Session81cRecognitionTickHostTests
{
    private sealed class StubCapture : IFrameCapture
    {
        public Queue<CapturedFrame?> Frames { get; } = new();
        public CapturedFrame? Capture() => Frames.Count > 0 ? Frames.Dequeue() : null;
        public void Dispose() { }
    }

    private static WindowCandidate MakeWindow(int w = 64, int h = 16) =>
        new(IntPtr.Zero, "stub", "stub.exe", 0, 0, w, h);

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

    private static (RecognitionTickHost host, StubCapture cap, GameStateManager states, Action<double> setClock)
        MakeHost(double fps = 10.0)
    {
        var cap = new StubCapture();
        var clock = 100.0;
        Func<double> readClock = () => clock;
        Action<double> setClock = v => clock = v;
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(), new Roi(0, 0, 1, 1),
            fps: new AdaptiveFpsSelector(activeFps: fps, idleFps: fps, idleAfterSeconds: 999),
            clock: readClock);
        var states = new GameStateManager();
        var host = new RecognitionTickHost(engine, states, clock: readClock);
        return (host, cap, states, setClock);
    }

    [Fact]
    public void RunOnce_ProjectsStaminaIntoState()
    {
        var (host, cap, states, _) = MakeHost();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        host.RunOnceAsync();
        Assert.True(states.Snapshot.RecognitionOk);
        Assert.Equal(1.0, states.Snapshot.StaminaPct, 3);
    }

    [Fact]
    public void RunOnce_SleepBudget_HonoursNextFps()
    {
        var (host, cap, _, _) = MakeHost(fps: 5.0);
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var sleepMs = host.RunOnceAsync();
        // 1/5 fps = 200 ms minus ~0 elapsed (clock doesn't advance).
        Assert.InRange(sleepMs, 195, 205);
    }

    [Fact]
    public void RunOnce_TickThrows_ProjectsErrorWithoutCrash()
    {
        var states = new GameStateManager();
        var engine = new RecognitionTickEngine(
            new ThrowingCapture(),
            () => MakeWindow(),
            new Roi(0, 0, 1, 1),
            clock: () => 100.0);
        var host = new RecognitionTickHost(engine, states, clock: () => 100.0);
        host.RunOnceAsync();
        Assert.False(states.Snapshot.RecognitionOk);
    }

    [Fact]
    public async Task StartStop_LoopRunsAndStopsCleanly()
    {
        var (host, cap, states, _) = MakeHost(fps: 100.0);
        for (var i = 0; i < 5; i++) cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        await host.StartAsync();
        // Give the loop a few ms to tick.
        await Task.Delay(80);
        await host.StopAsync();
        Assert.False(host.IsRunning);
        Assert.True(states.Snapshot.RecognitionOk);
    }

    [Fact]
    public async Task StartStop_DoubleStartIsNoop()
    {
        var (host, cap, _, _) = MakeHost(fps: 100.0);
        for (var i = 0; i < 3; i++) cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        await host.StartAsync();
        await host.StartAsync(); // second call must not throw / spawn dupes
        await host.StopAsync();
        Assert.False(host.IsRunning);
    }

    private sealed class ThrowingCapture : IFrameCapture
    {
        public CapturedFrame? Capture() => throw new InvalidOperationException("boom");
        public void Dispose() { }
    }
}
