using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.App;

/// <summary>
/// S171 — Pin <see cref="UpdaterBridge"/>: check / download / apply
/// each kick the supplied async delegate, reply with the current
/// <see cref="UpdaterState"/> + <c>queued</c> flag, surface
/// <c>{error:"unsupported"}</c> when the delegate is null, and
/// rebroadcast <see cref="UpdaterStateMachine.StateChanged"/> as
/// <see cref="BridgeEvents.UpdaterStatus"/> events when a broadcaster
/// is supplied.
/// </summary>
public class Session171UpdaterBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void CheckKicksDelegateAndReportsQueued()
    {
        var tcs = new TaskCompletionSource();
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        using var bridge = new UpdaterBridge(router, machine,
            check: async ct => { await tcs.Task.ConfigureAwait(false); });
        var reply = router.Dispatch(Cmd(BridgeCommands.CheckUpdate));
        Assert.True(reply!.Payload!["queued"]!.GetValue<bool>());
        Assert.Equal("Idle", reply.Payload!["status"]!.GetValue<string>());
        tcs.SetResult();
    }

    [Fact]
    public void CheckWithoutDelegateReturnsUnsupported()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        using var bridge = new UpdaterBridge(router, machine);
        var reply = router.Dispatch(Cmd(BridgeCommands.CheckUpdate));
        Assert.False(reply!.Payload!["queued"]!.GetValue<bool>());
        Assert.Equal("unsupported", reply.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void DownloadAndApplyHaveSameShape()
    {
        var dlFired = 0;
        var apFired = 0;
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        var dlGate = new TaskCompletionSource();
        var apGate = new TaskCompletionSource();
        using var bridge = new UpdaterBridge(router, machine,
            download: async ct => { Interlocked.Increment(ref dlFired); await dlGate.Task.ConfigureAwait(false); },
            apply: async ct => { Interlocked.Increment(ref apFired); await apGate.Task.ConfigureAwait(false); });

        var dl = router.Dispatch(Cmd(BridgeCommands.DownloadUpdate));
        Assert.True(dl!.Payload!["queued"]!.GetValue<bool>());
        var ap = router.Dispatch(Cmd(BridgeCommands.ApplyUpdate));
        Assert.True(ap!.Payload!["queued"]!.GetValue<bool>());
        // Allow Task.Run to schedule.
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while ((Volatile.Read(ref dlFired) == 0 || Volatile.Read(ref apFired) == 0) && sw.ElapsedMilliseconds < 1000)
            Thread.Sleep(5);
        Assert.Equal(1, dlFired);
        Assert.Equal(1, apFired);
        dlGate.SetResult();
        apGate.SetResult();
    }

    [Fact]
    public void SkipRunsDelegateSynchronouslyAndReportsResult()
    {
        var skipped = 0;
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        using var bridge = new UpdaterBridge(router, machine,
            skip: ct =>
            {
                Interlocked.Increment(ref skipped);
                machine.CheckCompleted(null, "1.0.0");
                return Task.FromResult(true);
            });

        var reply = router.Dispatch(Cmd(BridgeCommands.SkipUpdate));

        Assert.Equal(1, skipped);
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.True(reply.Payload["skipped"]!.GetValue<bool>());
        Assert.Equal("NoUpdate", reply.Payload["status"]!.GetValue<string>());
    }

    [Fact]
    public void SkipWithoutDelegateReturnsUnsupported()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        using var bridge = new UpdaterBridge(router, machine);

        var reply = router.Dispatch(Cmd(BridgeCommands.SkipUpdate));

        Assert.False(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.False(reply.Payload["skipped"]!.GetValue<bool>());
        Assert.Equal("unsupported", reply.Payload["error"]!.GetValue<string>());
    }

    [Fact]
    public void SnapshotReflectsMachineState()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        using var bridge = new UpdaterBridge(router, machine);
        machine.BeginCheck();
        var reply = router.Dispatch(Cmd(BridgeCommands.CheckUpdate));
        Assert.Equal("Checking", reply!.Payload!["status"]!.GetValue<string>());
    }

    [Fact]
    public void StateChangedBroadcastsUpdaterStatusEvent()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        var broadcaster = new BridgeEventBroadcaster();
        var captured = new List<BridgeMessage>();
        broadcaster.Posted += msg => captured.Add(msg);
        using var bridge = new UpdaterBridge(router, machine, broadcaster);

        machine.BeginCheck();
        machine.CheckCompleted(null, "1.0.0");

        Assert.Contains(captured, m =>
            m.Type == BridgeMessage.TypeEvent && m.Name == BridgeEvents.UpdaterStatus
            && m.Payload!["status"]!.GetValue<string>() == "Checking");
        Assert.Contains(captured, m =>
            m.Type == BridgeMessage.TypeEvent && m.Name == BridgeEvents.UpdaterStatus
            && m.Payload!["status"]!.GetValue<string>() == "NoUpdate");
    }

    [Fact]
    public void NoBroadcasterMeansNoSubscription()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        using var bridge = new UpdaterBridge(router, machine);
        // Just exercising — must not throw or leak.
        machine.BeginCheck();
        machine.CheckCompleted(null, "1.0.0");
    }

    [Fact]
    public void DisposeUnregistersAllAndUnsubscribes()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        var broadcaster = new BridgeEventBroadcaster();
        var captured = new List<BridgeMessage>();
        broadcaster.Posted += msg => captured.Add(msg);
        var bridge = new UpdaterBridge(router, machine, broadcaster);

        Assert.Contains(BridgeCommands.CheckUpdate, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.DownloadUpdate, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ApplyUpdate, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SkipUpdate, router.RegisteredCommands);

        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.CheckUpdate, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.DownloadUpdate, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ApplyUpdate, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.SkipUpdate, router.RegisteredCommands);

        captured.Clear();
        machine.BeginCheck();
        Assert.Empty(captured);
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var machine = new UpdaterStateMachine();
        var router = new BridgeRouter();
        var bridge = new UpdaterBridge(router, machine);
        bridge.Dispose();
        bridge.Dispose();
    }

    [Fact]
    public void NullArgsThrow()
    {
        var router = new BridgeRouter();
        var machine = new UpdaterStateMachine();
        Assert.Throws<ArgumentNullException>(() => new UpdaterBridge(null!, machine));
        Assert.Throws<ArgumentNullException>(() => new UpdaterBridge(router, null!));
    }
}
