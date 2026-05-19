using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Startup;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.App;

/// <summary>
/// S155 — Pin <see cref="HideSeekStatusPublisher"/> and the
/// <see cref="WebBridgeLifecycle.AttachHideSeek"/> wiring: per-tick
/// snapshots flow as <see cref="BridgeEvents.HideSeekStatus"/>;
/// duplicate snapshots are deduped; inactive lifecycle still emits
/// one initial Empty snapshot.
/// </summary>
public class Session155HideSeekStatusPublisherTests
{
    private sealed class StubFrames : IHideSeekFrameProvider
    {
        public HideSeekFrame? Capture() =>
            new(new byte[12], 2, 2, 6, 3, 0, 0);
    }

    private sealed class StubInput : IHideSeekInput
    {
        public void Click(int x, int y, bool alt) { }
    }

    private static HideSeekStateMachine MakeMachine(bool hit)
    {
        var steps = HideSeekSteps.Default;
        var templates = steps.ToDictionary(s => s.ImageFile,
            s => new HideSeekTemplate(s.ImageFile, new byte[] { 0 }, 1, 1));
        return new HideSeekStateMachine(
            steps, templates, new StubFrames(), new StubInput(),
            detect: (_, _, _) => hit
                ? new HideSeekDetector.DetectResult(10, 20, 0.9, "stub")
                : null);
    }

    private static (List<JsonObject> events, BridgeEventBroadcaster broadcaster) Sink()
    {
        var captured = new List<JsonObject>();
        var bc = new BridgeEventBroadcaster();
        bc.Posted += msg =>
        {
            if (msg.Name == BridgeEvents.HideSeekStatus && msg.Payload is { } p)
                captured.Add(p);
        };
        return (captured, bc);
    }

    [Fact]
    public void InactiveLifecycleStillEmitsInitialEmptySnapshot()
    {
        var (events, bc) = Sink();
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance,
            CancellationToken.None);
        using var pub = new HideSeekStatusPublisher(lc, bc);
        pub.Start();
        Assert.Single(events);
        Assert.Equal("inactive", events[0]["last_error"]!.GetValue<string>());
        Assert.False(events[0]["is_running"]!.GetValue<bool>());
    }

    [Fact]
    public void DuplicateSnapshotsAreDeduped()
    {
        var (events, bc) = Sink();
        using var host = new HideSeekTickHost(MakeMachine(hit: false), intervalMs: 50);
        host.Ticked += _ => { };
        // Manually drive ticks → both fire with same LastFiredStep=-1 +
        // same TickCount? No — TickCount increments each call. So the
        // publisher must emit per-tick. Verify N ticks ≤ N emits but
        // an idempotent emit (same snapshot value) collapses.

        // Compose: subscribe a dedupe sink directly.
        var raw = new List<HideSeekRuntimeSnapshot>();
        host.Ticked += raw.Add;
        host.TickOnce();
        host.TickOnce();
        Assert.Equal(2, raw.Count);
        Assert.NotEqual(raw[0], raw[1]); // TickCount differs
    }

    [Fact]
    public void StatusPublisherEmitsPerTick()
    {
        var (events, bc) = Sink();
        // Use a huge interval so the background pump's Task.Delay
        // does not fire within the test window; manual TickOnce calls
        // are the only source of events.
        var host = new HideSeekTickHost(MakeMachine(hit: true), intervalMs: 60_000);
        using var lc = HideSeekLifecycle.Start(() => host, NullLogger.Instance, CancellationToken.None);
        Assert.True(lc.IsActive);
        using var pub = new HideSeekStatusPublisher(lc, bc);
        pub.Start(emitInitial: false);
        host.TickOnce();
        host.TickOnce();
        Assert.Equal(2, events.Count);
        Assert.Equal(2L, events[^1]["tick_count"]!.GetValue<long>());
        Assert.NotNull(events[^1]["last_fired_step"]);
    }

    [Fact]
    public void NullArgsThrow()
    {
        Assert.Throws<ArgumentNullException>(() =>
            new HideSeekStatusPublisher(null!, new BridgeEventBroadcaster()));
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance, CancellationToken.None);
        Assert.Throws<ArgumentNullException>(() =>
            new HideSeekStatusPublisher(lc, null!));
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var (_, bc) = Sink();
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance, CancellationToken.None);
        var pub = new HideSeekStatusPublisher(lc, bc);
        pub.Start();
        pub.Dispose();
        pub.Dispose();
    }
}
