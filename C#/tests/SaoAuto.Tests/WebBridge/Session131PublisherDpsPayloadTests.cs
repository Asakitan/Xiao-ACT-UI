using System.Collections.Immutable;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

/// <summary>
/// S131 — optional DPS snapshot provider flows through
/// <see cref="GameStatePublisher"/> into the full
/// <see cref="BridgeEvents.GameStateChanged"/> payload without
/// breaking the existing no-provider path.
/// </summary>
public class Session131PublisherDpsPayloadTests
{
    private static DpsSnapshot BuildSnapshot() =>
        new(
            Active: true,
            TotalDamage: 900,
            Dps: 300,
            TotalHeal: 120,
            Hps: 40,
            DurationSeconds: 3.0,
            Rows: ImmutableArray.Create(
                new DpsEntitySnapshot(
                    EntityUuid: 77,
                    EntityName: "Leafa",
                    ProfessionId: 5,
                    IsSelf: false,
                    Damage: 900,
                    Dps: 300,
                    Heal: 120,
                    Hps: 40,
                    Skills: ImmutableArray.Create(
                        new SkillBreakdownRow(
                            SkillId: 9,
                            Name: "Arrow Rain",
                            Total: 900,
                            Hits: 4,
                            CritHits: 2,
                            CritRate: 0.5,
                            MaxHit: 400,
                            HealTotal: 0,
                            HealHits: 0)))));

    [Fact]
    public void ProviderSnapshot_FlowsIntoStateChangedPayload()
    {
        var states = new GameStateManager();
        var br = new BridgeEventBroadcaster();
        var sink = new List<BridgeMessage>();
        br.Posted += sink.Add;
        using var pub = new GameStatePublisher(states, br, () => BuildSnapshot());
        pub.Start(emitInitial: false);

        states.Update(s => s with { PlayerName = "Asuna" });

        var message = Assert.Single(sink, m => m.Name == BridgeEvents.GameStateChanged);
        var payload = message.Payload!;
        Assert.True(payload["dps_active"]!.GetValue<bool>());
        Assert.Equal(900L, payload["dps_total_damage"]!.GetValue<long>());
        Assert.Equal(300L, payload["dps_total"]!.GetValue<long>());
        Assert.Equal(120L, payload["dps_total_heal"]!.GetValue<long>());
        Assert.Equal(40L, payload["dps_hps"]!.GetValue<long>());

        var entries = Assert.IsType<System.Text.Json.Nodes.JsonArray>(payload["dps_entries"]);
        Assert.Single(entries);
        Assert.Equal(77L, entries[0]!["uid"]!.GetValue<long>());
        Assert.Equal("Leafa", entries[0]!["name"]!.GetValue<string>());
    }

    [Fact]
    public void NoProvider_StillEmitsEmptyDpsDefaults()
    {
        var states = new GameStateManager();
        var br = new BridgeEventBroadcaster();
        var sink = new List<BridgeMessage>();
        br.Posted += sink.Add;
        using var pub = new GameStatePublisher(states, br);
        pub.Start(emitInitial: false);

        states.Update(s => s with { PlayerName = "Kirito" });

        var message = Assert.Single(sink, m => m.Name == BridgeEvents.GameStateChanged);
        var payload = message.Payload!;
        Assert.False(payload["dps_active"]!.GetValue<bool>());
        Assert.Equal(0L, payload["dps_total_damage"]!.GetValue<long>());
        var entries = Assert.IsType<System.Text.Json.Nodes.JsonArray>(payload["dps_entries"]);
        Assert.Empty(entries);
    }
}
