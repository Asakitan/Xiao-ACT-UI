using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;

namespace SaoAuto.Tests.App;

/// <summary>
/// S158 — Pin <see cref="WebViewBridgeBinder"/>: inbound bus messages
/// dispatch through the adapter and the reply lands back on the bus;
/// adapter <see cref="BridgeHostAdapter.PostJson"/> events are
/// forwarded to <c>bus.Post</c>; disposing the binding unhooks both
/// directions and tolerates subsequent traffic.
/// </summary>
public class Session158WebViewBridgeBinderTests
{
    private sealed class FakeBus : IWebMessageBus
    {
        public event Action<string>? Received;
        public List<string> Posted { get; } = new();
        public Action<string>? PostThrows { get; set; }
        public void Post(string json)
        {
            if (PostThrows is { } p) p(json);
            Posted.Add(json);
        }
        public void Receive(string json) => Received?.Invoke(json);
    }

    private static BridgeHostAdapter NewAdapter(out BridgeRouter router, out BridgeEventBroadcaster bc)
    {
        router = new BridgeRouter();
        bc = new BridgeEventBroadcaster();
        return new BridgeHostAdapter(router, bc);
    }

    [Fact]
    public void InboundMessageDispatchesAndRepliesOnBus()
    {
        using var adapter = NewAdapter(out var router, out _);
        router.Register("echo", p => new JsonObject { ["v"] = p?["v"]?.GetValue<string>() });
        var bus = new FakeBus();
        using var binding = WebViewBridgeBinder.Bind(bus, adapter);

        bus.Receive("""{"type":"command","name":"echo","payload":{"v":"hi"}}""");

        Assert.Single(bus.Posted);
        var parsed = JsonNode.Parse(bus.Posted[0])!.AsObject();
        Assert.Equal("reply", parsed["type"]!.GetValue<string>());
        Assert.Equal("hi", parsed["payload"]!["v"]!.GetValue<string>());
    }

    [Fact]
    public void InboundMessageWithNoReplyDoesNotPost()
    {
        using var adapter = NewAdapter(out _, out _);
        var bus = new FakeBus();
        using var binding = WebViewBridgeBinder.Bind(bus, adapter);

        bus.Receive("not json");
        Assert.Empty(bus.Posted);
    }

    [Fact]
    public void BroadcasterEventsAreForwardedToBus()
    {
        using var adapter = NewAdapter(out _, out var bc);
        var bus = new FakeBus();
        using var binding = WebViewBridgeBinder.Bind(bus, adapter);

        bc.Emit("state.test", new JsonObject { ["x"] = 1 });

        Assert.Single(bus.Posted);
        var parsed = JsonNode.Parse(bus.Posted[0])!.AsObject();
        Assert.Equal("event", parsed["type"]!.GetValue<string>());
        Assert.Equal("state.test", parsed["name"]!.GetValue<string>());
    }

    [Fact]
    public void DisposeUnhooksBothDirections()
    {
        using var adapter = NewAdapter(out var router, out var bc);
        router.Register("echo", _ => new JsonObject());
        var bus = new FakeBus();
        var binding = WebViewBridgeBinder.Bind(bus, adapter);
        binding.Dispose();

        bus.Receive("""{"type":"command","name":"echo"}""");
        bc.Emit("state.test", new JsonObject());

        Assert.Empty(bus.Posted);
        // Second dispose must not throw.
        binding.Dispose();
    }

    [Fact]
    public void BusPostThrowingDoesNotKillEventDelivery()
    {
        using var adapter = NewAdapter(out _, out var bc);
        var bus = new FakeBus();
        bus.PostThrows = _ =>
        {
            // First call throws, then disarm so the next emit appends.
            bus.PostThrows = null;
            throw new InvalidOperationException("boom");
        };
        using var binding = WebViewBridgeBinder.Bind(bus, adapter);

        // Should not propagate.
        bc.Emit("state.test", new JsonObject { ["x"] = 1 });
        Assert.Empty(bus.Posted);

        // Subscription not torn down — next event still flows.
        bc.Emit("state.test", new JsonObject { ["x"] = 2 });
        Assert.Single(bus.Posted);
    }

    [Fact]
    public void NullArgsThrow()
    {
        using var adapter = NewAdapter(out _, out _);
        Assert.Throws<ArgumentNullException>(() => WebViewBridgeBinder.Bind(null!, adapter));
        Assert.Throws<ArgumentNullException>(() => WebViewBridgeBinder.Bind(new FakeBus(), null!));
    }
}
