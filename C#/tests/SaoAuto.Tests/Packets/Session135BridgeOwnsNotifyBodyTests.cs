using System.Buffers.Binary;
using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// Session 135 — PacketBridge owns NotifyBodyAvailable subscription directly.
/// Previously the lifecycle wrapper forwarded body events from parser to
/// bridge.DispatchRawNotify. After S135 the bridge subscribes itself, so any
/// PacketBridge + IPacketParser pair routes notify bodies end-to-end without
/// a lifecycle in the loop. Also pins the +2 -> +1 RawNotifiesDispatched
/// double-count fix.
/// </summary>
public class Session135BridgeOwnsNotifyBodyTests
{
    [Fact]
    public void BridgeAutoSubscribesAndMutatesStateWithoutExternalForwarder()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);
        // NOTE: no manual parser.NotifyBodyAvailable += bridge.DispatchRawNotify

        var msg = new SyncServerTime
        {
            ClientMilliseconds = 1,
            ServerMilliseconds = 1_700_000_000_000L,
        };
        parser.FeedGameFrame(BuildNotifyFrame(NotifyMethod.SyncServerTime, msg.ToByteArray()), 5.0);

        Assert.NotEqual(0, state.Snapshot.ServerTimeOffsetMs);
    }

    [Fact]
    public void RawNotifiesDispatchedIsCountedExactlyOncePerCSbNotify()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);

        // field 1 varint = some value, valid EnterGame body
        var protoBody = new byte[] { (byte)((1 << 3) | 0), 0xFE, 0x95, 0x03 };
        parser.FeedGameFrame(BuildNotifyFrame(NotifyMethod.EnterGame, protoBody), 1.0);

        // S135 fix: OnParserEvent no longer increments on RawNotifyEvent,
        // only OnNotifyBody -> DispatchRawNotify does. So exactly +1.
        Assert.Equal(1, bridge.RawNotifiesDispatched);
    }

    [Fact]
    public void DisposeUnsubscribesFromBothChannels()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        var bridge = new PacketBridge(state, parser);

        var protoBody = new byte[] { (byte)((1 << 3) | 0), 0x01 };
        parser.FeedGameFrame(BuildNotifyFrame(NotifyMethod.EnterGame, protoBody), 1.0);
        var beforeDispose = bridge.RawNotifiesDispatched;

        bridge.Dispose();

        parser.FeedGameFrame(BuildNotifyFrame(NotifyMethod.EnterGame, protoBody), 2.0);
        Assert.Equal(beforeDispose, bridge.RawNotifiesDispatched);
    }

    [Fact]
    public void LifecycleNoLongerExposesNotifyBodyForwarder()
    {
        // S135 deleted PacketLifecycle's OnNotifyBodyAvailable forwarder.
        // Guard against accidental reintroduction.
        var hostType = typeof(SaoAuto.App.Startup.PacketLifecycle)
            .GetNestedType("PacketRuntimeHost", System.Reflection.BindingFlags.NonPublic);
        Assert.NotNull(hostType);
        var members = hostType!
            .GetMembers(System.Reflection.BindingFlags.Public
                | System.Reflection.BindingFlags.NonPublic
                | System.Reflection.BindingFlags.Instance
                | System.Reflection.BindingFlags.Static)
            .Select(m => m.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("OnNotifyBodyAvailable", members);
    }

    private static byte[] BuildNotifyFrame(int methodId, byte[] body)
    {
        var payload = new byte[16 + body.Length];
        BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(0, 8), PacketCodes.ServiceUuidC3Sb);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(12, 4), (uint)methodId);
        Buffer.BlockCopy(body, 0, payload, 16, body.Length);

        var size = 4 + 2 + payload.Length;
        var frame = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)size);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), (ushort)MessageType.Notify);
        Buffer.BlockCopy(payload, 0, frame, 6, payload.Length);
        return frame;
    }
}
