using Google.Protobuf;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Bridge;

public class PacketBridgeTests
{
    private static GameStateManager NewState() => new();

    [Fact]
    public void IdentityEventMutatesPlayerFields()
    {
        var state = NewState();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        bridge.Apply(new IdentityEvent("笨猫", "p-001", 60, 5, 1234, 9001, 12, "Plunderer", 1.0));
        var snap = state.Snapshot;
        Assert.Equal("笨猫", snap.PlayerName);
        Assert.Equal(60, snap.LevelBase);
        Assert.Equal(5, snap.LevelExtra);
        Assert.Equal(9001, snap.FightPoint);
        Assert.True(snap.PacketActive);
        Assert.Equal(1, bridge.EventsApplied);
    }

    [Fact]
    public void ServerTimeEventUpdatesOffset()
    {
        var state = NewState();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        bridge.Apply(new ServerTimeEvent(1_500_000_000_000UL, 250.5, 0.0));
        Assert.Equal(250.5, state.Snapshot.ServerTimeOffsetMs);
    }

    [Fact]
    public void KickOffSetsErrorAndClearsPacketActive()
    {
        var state = NewState();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        bridge.Apply(new EnterGameEvent(0, 0.0));
        Assert.True(state.Snapshot.PacketActive);

        bridge.Apply(new KickOffEvent(0.0));
        Assert.False(state.Snapshot.PacketActive);
        Assert.Equal("session ended (kicked)", state.Snapshot.ErrorMsg);
    }

    [Fact]
    public void DispatchRawNotifyRoutesProtoBodyToRegistry()
    {
        var state = NewState();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        var msg = new SyncServerTime { ClientMilliseconds = 1, ServerMilliseconds = 1_700_000_000_000L };
        bridge.DispatchRawNotify(NotifyMethod.SyncServerTime, msg.ToByteArray(), 0.0);

        // Offset = server - local; we just assert it ran end-to-end.
        Assert.Equal(1, bridge.EventsApplied);
        Assert.Equal(1, bridge.RawNotifiesDispatched);
    }

    [Fact]
    public void RawNotifyEventOnParserChannelDoesNotMutate()
    {
        var state = NewState();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        // Fire a RawNotifyEvent directly via the parser's contract; the bridge
        // should count it but not apply state (the body is missing — the
        // proper path is DispatchRawNotify).
        // Simulate by feeding a 6-byte minimum frame via the parser.
        var frame = new byte[] { 0x00, 0x00, 0x00, 0x06, 0x00, 0x02 };
        parser.FeedGameFrame(frame, 0.0);

        Assert.Equal(0, bridge.EventsApplied);
    }

    [Fact]
    public void HealthEventMutatesHpFields()
    {
        var state = NewState();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        bridge.Apply(new HealthEvent(8500, 12000, 8500.0 / 12000.0, 0.0));
        var snap = state.Snapshot;
        Assert.Equal(8500, snap.HpCurrent);
        Assert.Equal(12000, snap.HpMax);
        Assert.InRange(snap.HpPct, 0.7, 0.71);
    }

    [Fact]
    public void DisposeUnsubscribesFromParser()
    {
        var state = NewState();
        var parser = new PacketParser();
        var bridge = new PacketBridge(state, parser);
        bridge.Dispose();

        var frame = new byte[] { 0x00, 0x00, 0x00, 0x06, 0x00, 0x02 };
        parser.FeedGameFrame(frame, 0.0);

        Assert.Equal(0, bridge.RawNotifiesDispatched);
    }
}
