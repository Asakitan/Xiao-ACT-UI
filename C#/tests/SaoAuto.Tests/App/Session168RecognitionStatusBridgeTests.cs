using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;

namespace SaoAuto.Tests.App;

/// <summary>
/// S168 — Pin <see cref="RecognitionStatusBridge"/>:
/// <c>recognition.status</c> reflects the delegate's current value;
/// <c>recognition.start</c> / <c>recognition.stop</c> invoke the
/// supplied hooks (or report <c>unsupported</c> when null) and
/// echo the post-hook active state; dispose unregisters all three.
/// </summary>
public class Session168RecognitionStatusBridgeTests
{
    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void StatusReflectsDelegate()
    {
        var active = true;
        var router = new BridgeRouter();
        using var bridge = new RecognitionStatusBridge(router, () => active);
        Assert.True(router.Dispatch(Cmd(BridgeCommands.RecognitionStatus))!
            .Payload!["active"]!.GetValue<bool>());
        active = false;
        Assert.False(router.Dispatch(Cmd(BridgeCommands.RecognitionStatus))!
            .Payload!["active"]!.GetValue<bool>());
    }

    [Fact]
    public void StartCallsHookAndReportsPostState()
    {
        var active = false;
        var starts = 0;
        var router = new BridgeRouter();
        using var bridge = new RecognitionStatusBridge(router,
            () => active,
            start: () => { starts++; active = true; });
        var reply = router.Dispatch(Cmd(BridgeCommands.StartRecognition));
        Assert.Equal(1, starts);
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.True(reply.Payload!["active"]!.GetValue<bool>());
    }

    [Fact]
    public void StopCallsHookAndReportsPostState()
    {
        var active = true;
        var stops = 0;
        var router = new BridgeRouter();
        using var bridge = new RecognitionStatusBridge(router,
            () => active,
            stop: () => { stops++; active = false; });
        var reply = router.Dispatch(Cmd(BridgeCommands.StopRecognition));
        Assert.Equal(1, stops);
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());
        Assert.False(reply.Payload!["active"]!.GetValue<bool>());
    }

    [Fact]
    public void StartWithoutHookReturnsUnsupportedAndPreservesState()
    {
        var router = new BridgeRouter();
        using var bridge = new RecognitionStatusBridge(router, () => false);
        var reply = router.Dispatch(Cmd(BridgeCommands.StartRecognition));
        Assert.Equal("unsupported", reply!.Payload!["error"]!.GetValue<string>());
        Assert.False(reply.Payload!["active"]!.GetValue<bool>());
    }

    [Fact]
    public void StopWithoutHookReturnsUnsupportedAndPreservesState()
    {
        var router = new BridgeRouter();
        using var bridge = new RecognitionStatusBridge(router, () => true);
        var reply = router.Dispatch(Cmd(BridgeCommands.StopRecognition));
        Assert.Equal("unsupported", reply!.Payload!["error"]!.GetValue<string>());
        Assert.True(reply.Payload!["active"]!.GetValue<bool>());
    }

    [Fact]
    public void DisposeUnregistersAllThree()
    {
        var router = new BridgeRouter();
        var bridge = new RecognitionStatusBridge(router, () => false);
        Assert.Contains(BridgeCommands.RecognitionStatus, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartRecognition, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StopRecognition, router.RegisteredCommands);
        bridge.Dispose();
        Assert.DoesNotContain(BridgeCommands.RecognitionStatus, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartRecognition, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StopRecognition, router.RegisteredCommands);
    }

    [Fact]
    public void NullArgsThrow()
    {
        var router = new BridgeRouter();
        Assert.Throws<ArgumentNullException>(() => new RecognitionStatusBridge(null!, () => false));
        Assert.Throws<ArgumentNullException>(() => new RecognitionStatusBridge(router, null!));
    }
}
