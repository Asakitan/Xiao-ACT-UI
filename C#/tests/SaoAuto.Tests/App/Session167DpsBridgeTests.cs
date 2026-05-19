using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.App;

/// <summary>
/// S167 — Pin <see cref="DpsBridge"/>: reset invokes the supplied
/// hook, last-report returns a formatted string + numeric metadata,
/// missing snapshot returns <c>{error:"no_report"}</c>, null
/// delegates return <c>{error:"dps_unavailable"}</c> (matching the
/// runner case where the packet runtime never started), dispose
/// unregisters both commands.
/// </summary>
public class Session167DpsBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    private static DpsSnapshot SampleSnapshot() => new(
        Active: true,
        TotalDamage: 12345,
        Dps: 678,
        TotalHeal: 100,
        Hps: 5,
        DurationSeconds: 18.2,
        Rows: ImmutableArray<DpsEntitySnapshot>.Empty);

    [Fact]
    public void ResetCallsHookAndReportsTrue()
    {
        var calls = 0;
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, () => calls++, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ResetCombat));
        Assert.Equal(1, calls);
        Assert.True(reply!.Payload!["reset"]!.GetValue<bool>());
    }

    [Fact]
    public void ResetWithoutHookReturnsUnavailable()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ResetCombat));
        Assert.Equal("dps_unavailable", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void LastReportReturnsFormattedSnapshot()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, () => SampleSnapshot());
        var reply = router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        var payload = reply!.Payload!;
        var report = payload["report"]!.GetValue<string>();
        Assert.Contains("DPS Report", report);
        Assert.Equal(12345, payload["total_damage"]!.GetValue<long>());
        Assert.Equal(18.2, payload["duration_s"]!.GetValue<double>(), 1);
    }

    [Fact]
    public void LastReportWithNullSnapshotReturnsNoReport()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, () => null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        Assert.Equal("no_report", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void LastReportWithoutProviderReturnsUnavailable()
    {
        var router = new BridgeRouter();
        using var bridge = new DpsBridge(router, null, null);
        var reply = router.Dispatch(Cmd(BridgeCommands.ShowLastDpsReport));
        Assert.Equal("dps_unavailable", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeUnregistersBoth()
    {
        var router = new BridgeRouter();
        var bridge = new DpsBridge(router, () => { }, () => null);
        Assert.Contains(BridgeCommands.ResetCombat, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ShowLastDpsReport, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.ResetCombat, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ShowLastDpsReport, router.RegisteredCommands);
    }

    [Fact]
    public void NullRouterThrows()
    {
        Assert.Throws<ArgumentNullException>(() => new DpsBridge(null!, null, null));
    }
}
