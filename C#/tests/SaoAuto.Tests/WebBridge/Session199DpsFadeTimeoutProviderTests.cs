using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public class Session199DpsFadeTimeoutProviderTests
{
    [Fact]
    public void PumpDpsOverlayUsesProvidedIdleTimeout()
    {
        var now = DateTimeOffset.UtcNow;
        var tracker = new DpsTracker(() => now);
        var states = new GameStateManager();
        var broadcaster = new BridgeEventBroadcaster();
        var events = new List<JsonObject>();
        broadcaster.Posted += msg =>
        {
            if (msg.Name == BridgeEvents.DpsOverlay && msg.Payload is not null)
                events.Add(msg.Payload);
        };

        using var publisher = new GameStatePublisher(
            states,
            broadcaster,
            dpsTracker: tracker,
            dpsEnabledProvider: () => true,
            dpsIdleTimeoutProvider: () => TimeSpan.FromSeconds(1));

        tracker.RecordDamage(1, "self", 500, 7, true);
        publisher.PumpDpsOverlay();
        now = now.AddSeconds(2);
        publisher.PumpDpsOverlay();

        Assert.Equal("show", events[0]["action"]!.GetValue<string>());
        Assert.Equal("fade_out", events[1]["action"]!.GetValue<string>());
        Assert.Equal(500, events[1]["total_damage"]!.GetValue<long>());
    }
}
