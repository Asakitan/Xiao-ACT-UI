using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S151 — Pin <see cref="HideSeekTickHost"/>: TickOnce delegates to
/// the state machine, swallows exceptions into LastError, the
/// background pump fires repeatedly and stops cleanly.
/// </summary>
public class Session151HideSeekTickHostTests
{
    private sealed class StubFrames : IHideSeekFrameProvider
    {
        public HideSeekFrame? Capture() =>
            new(new byte[12], 2, 2, 6, 3, 0, 0);
    }

    private sealed class StubInput : IHideSeekInput
    {
        public int Count;
        public void Click(int x, int y, bool alt) => Interlocked.Increment(ref Count);
    }

    private static HideSeekStateMachine MakeMachine(bool throwOnTick = false, bool hit = true)
    {
        var steps = HideSeekSteps.Default;
        var templates = steps.ToDictionary(s => s.ImageFile,
            s => new HideSeekTemplate(s.ImageFile, new byte[] { 0 }, 1, 1));
        return new HideSeekStateMachine(
            steps, templates, new StubFrames(), new StubInput(),
            detect: (step, _, _) =>
            {
                if (throwOnTick) throw new InvalidOperationException("boom");
                return hit
                    ? new HideSeekDetector.DetectResult(10, 20, 0.9, "stub")
                    : null;
            });
    }

    [Fact]
    public void TickOnceDelegatesToStateMachine()
    {
        var sm = MakeMachine(hit: true);
        var host = new HideSeekTickHost(sm, intervalMs: 10);
        int fired = host.TickOnce();
        Assert.Equal(0, fired);
        Assert.Equal(1, host.TickCount);
        Assert.Equal(0, host.LastFiredStep);
        Assert.Equal("", host.LastError);
    }

    [Fact]
    public void TickOnceSwallowsExceptionsIntoLastError()
    {
        var sm = MakeMachine(throwOnTick: true);
        var host = new HideSeekTickHost(sm, intervalMs: 10);
        int fired = host.TickOnce();
        Assert.Equal(-1, fired);
        Assert.Contains("InvalidOperationException", host.LastError);
        Assert.Contains("boom", host.LastError);
    }

    [Fact]
    public void IntervalMustBePositive()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            new HideSeekTickHost(MakeMachine(), intervalMs: 0));
    }

    [Fact]
    public async Task PumpFiresTicksAndStopsCleanly()
    {
        var sm = MakeMachine(hit: true);
        var host = new HideSeekTickHost(sm, intervalMs: 20);
        await host.StartAsync(CancellationToken.None);
        await Task.Delay(120);
        await host.StopAsync();
        Assert.False(host.IsRunning);
        Assert.True(host.TickCount >= 2, $"expected ≥2 ticks, saw {host.TickCount}");
    }

    [Fact]
    public async Task DisposeStopsThePump()
    {
        var sm = MakeMachine(hit: false);
        var host = new HideSeekTickHost(sm, intervalMs: 10);
        await host.StartAsync(CancellationToken.None);
        await Task.Delay(40);
        await host.DisposeAsync();
        Assert.False(host.IsRunning);
        // TickOnce after dispose returns -1 without touching the machine
        Assert.Equal(-1, host.TickOnce());
    }

    [Fact]
    public async Task StartAsyncIsIdempotent()
    {
        var sm = MakeMachine(hit: false);
        var host = new HideSeekTickHost(sm, intervalMs: 50);
        await host.StartAsync(CancellationToken.None);
        // Second call must not start a second pump.
        await host.StartAsync(CancellationToken.None);
        await host.StopAsync();
    }
}
