using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;

namespace SaoAuto.Tests.App;

/// <summary>
/// S157 — Pin <see cref="BridgeHostAdapter"/>: inbound JSON parses
/// + dispatches through the router and returns wire-format reply
/// JSON; broadcaster events are re-emitted on PostJson; Dispose
/// unhooks the broadcaster subscription.
/// </summary>
public class Session157BridgeHostAdapterTests
{
    [Fact]
    public void HandleMessageJsonDispatchesAndReturnsReply()
    {
        var router = new BridgeRouter();
        router.Register("echo", payload => payload is null
            ? new JsonObject { ["echo"] = "(empty)" }
            : new JsonObject { ["echo"] = payload["v"]?.GetValue<string>() });
        var bc = new BridgeEventBroadcaster();
        using var adapter = new BridgeHostAdapter(router, bc);

        var wire = adapter.HandleMessageJson("""{"type":"command","name":"echo","payload":{"v":"hi"}}""");

        Assert.NotNull(wire);
        var parsed = JsonNode.Parse(wire!)!.AsObject();
        Assert.Equal("reply", parsed["type"]!.GetValue<string>());
        Assert.Equal("echo", parsed["name"]!.GetValue<string>());
        Assert.Equal("hi", parsed["payload"]!["echo"]!.GetValue<string>());
    }

    [Fact]
    public void HandleMessageJsonNullOrUnparseableReturnsNull()
    {
        var router = new BridgeRouter();
        using var adapter = new BridgeHostAdapter(router, new BridgeEventBroadcaster());
        Assert.Null(adapter.HandleMessageJson(null));
        Assert.Null(adapter.HandleMessageJson(""));
        Assert.Null(adapter.HandleMessageJson("not json"));
        Assert.Null(adapter.HandleMessageJson("[1,2,3]"));
    }

    [Fact]
    public void UnknownCommandStillProducesReplyWithError()
    {
        var router = new BridgeRouter();
        using var adapter = new BridgeHostAdapter(router, new BridgeEventBroadcaster());
        var wire = adapter.HandleMessageJson("""{"type":"command","name":"nope"}""");
        Assert.NotNull(wire);
        var parsed = JsonNode.Parse(wire!)!.AsObject();
        Assert.Equal("reply", parsed["type"]!.GetValue<string>());
        Assert.Equal("unknown_command", parsed["payload"]!["error"]!.GetValue<string>());
    }

    [Fact]
    public void PostJsonFiresForBroadcasterEvents()
    {
        var bc = new BridgeEventBroadcaster();
        using var adapter = new BridgeHostAdapter(new BridgeRouter(), bc);
        var captured = new List<string>();
        adapter.PostJson += captured.Add;

        bc.Emit("state.test", new JsonObject { ["x"] = 1 });

        Assert.Single(captured);
        var parsed = JsonNode.Parse(captured[0])!.AsObject();
        Assert.Equal("event", parsed["type"]!.GetValue<string>());
        Assert.Equal("state.test", parsed["name"]!.GetValue<string>());
        Assert.Equal(1, parsed["payload"]!["x"]!.GetValue<int>());
    }

    [Fact]
    public void DisposeStopsRelayingBroadcasterEvents()
    {
        var bc = new BridgeEventBroadcaster();
        var adapter = new BridgeHostAdapter(new BridgeRouter(), bc);
        var captured = new List<string>();
        adapter.PostJson += captured.Add;

        adapter.Dispose();
        bc.Emit("state.test", new JsonObject { ["x"] = 1 });

        Assert.Empty(captured);
        // Second dispose must not throw
        adapter.Dispose();
    }

    [Fact]
    public void DisposedAdapterIgnoresMessages()
    {
        var router = new BridgeRouter();
        router.Register("echo", _ => new JsonObject());
        var adapter = new BridgeHostAdapter(router, new BridgeEventBroadcaster());
        adapter.Dispose();
        Assert.Null(adapter.HandleMessageJson("""{"type":"command","name":"echo"}"""));
    }

    [Fact]
    public void NullArgsThrow()
    {
        Assert.Throws<ArgumentNullException>(() =>
            new BridgeHostAdapter(null!, new BridgeEventBroadcaster()));
        Assert.Throws<ArgumentNullException>(() =>
            new BridgeHostAdapter(new BridgeRouter(), null!));
    }
}
