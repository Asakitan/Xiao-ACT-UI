using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;

namespace SaoAuto.Tests.WebBridge;

public class BridgeMessageTests
{
    [Fact]
    public void RoundTripsThroughWireJson()
    {
        var payload = new JsonObject { ["delay"] = 100, ["enabled"] = true };
        var msg = new BridgeMessage(BridgeMessage.TypeCommand, BridgeCommands.StartAutoKey, payload);
        var json = msg.ToWireJson();

        var parsed = BridgeMessage.TryParse(json);
        Assert.NotNull(parsed);
        Assert.Equal(BridgeMessage.TypeCommand, parsed!.Type);
        Assert.Equal(BridgeCommands.StartAutoKey, parsed.Name);
        Assert.Equal(100, parsed.Payload!["delay"]!.GetValue<int>());
        Assert.True(parsed.Payload["enabled"]!.GetValue<bool>());
    }

    [Fact]
    public void TryParseRejectsMissingFields()
    {
        Assert.Null(BridgeMessage.TryParse("{}"));
        Assert.Null(BridgeMessage.TryParse("{\"type\":\"command\"}"));
        Assert.Null(BridgeMessage.TryParse("not json"));
    }
}

public class BridgeRouterTests
{
    [Fact]
    public void DispatchInvokesRegisteredHandler()
    {
        var router = new BridgeRouter();
        router.Register(BridgeCommands.StartRecognition, _ => new JsonObject { ["ok"] = true });

        var reply = router.Dispatch(new BridgeMessage(
            BridgeMessage.TypeCommand, BridgeCommands.StartRecognition, null));

        Assert.NotNull(reply);
        Assert.Equal(BridgeMessage.TypeReply, reply!.Type);
        Assert.True(reply.Payload!["ok"]!.GetValue<bool>());
    }

    [Fact]
    public void UnknownCommandRepliesWithError()
    {
        var router = new BridgeRouter();
        var reply = router.Dispatch(new BridgeMessage(
            BridgeMessage.TypeCommand, "unknown.thing", null));

        Assert.NotNull(reply);
        Assert.Equal("unknown_command", reply!.Payload!["error"]!.GetValue<string>());
    }

    [Fact]
    public void HandlerExceptionRepliesWithError()
    {
        var router = new BridgeRouter();
        router.Register("boom", _ => throw new InvalidOperationException("nope"));

        var reply = router.Dispatch(new BridgeMessage(
            BridgeMessage.TypeCommand, "boom", null));

        Assert.NotNull(reply);
        Assert.Equal("handler_exception", reply!.Payload!["error"]!.GetValue<string>());
        Assert.Equal("nope", reply.Payload["message"]!.GetValue<string>());
    }

    [Fact]
    public void EventMessagesAreNotDispatched()
    {
        var router = new BridgeRouter();
        var reply = router.Dispatch(new BridgeMessage(BridgeMessage.TypeEvent, "x.y", null));
        Assert.Null(reply);
    }
}

public class BridgeEventBroadcasterTests
{
    [Fact]
    public void EmitDeliversToSubscribers()
    {
        var broadcaster = new BridgeEventBroadcaster();
        BridgeMessage? received = null;
        broadcaster.Posted += msg => received = msg;

        broadcaster.Emit(BridgeEvents.GameStateChanged, new JsonObject { ["hp"] = 100 });

        Assert.NotNull(received);
        Assert.Equal(BridgeMessage.TypeEvent, received!.Type);
        Assert.Equal(BridgeEvents.GameStateChanged, received.Name);
        Assert.Equal(100, received.Payload!["hp"]!.GetValue<int>());
    }

    [Fact]
    public void GenericEmitSerializesPayload()
    {
        var broadcaster = new BridgeEventBroadcaster();
        BridgeMessage? received = null;
        broadcaster.Posted += msg => received = msg;

        broadcaster.Emit(BridgeEvents.HealthChanged, new { hp_current = 800, hp_max = 1000 });

        Assert.NotNull(received);
        Assert.Equal(800, received!.Payload!["hp_current"]!.GetValue<int>());
        Assert.Equal(1000, received.Payload["hp_max"]!.GetValue<int>());
    }
}
