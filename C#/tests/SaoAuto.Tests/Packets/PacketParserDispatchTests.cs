using System.Buffers.Binary;
using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class PacketParserDispatchTests
{
    [Fact]
    public void EnterGameNotifyEmitsRawAndBodyChannel()
    {
        var parser = new PacketParser();
        var events = new List<ParserEvent>();
        NotifyBodyDecoded? body = null;
        parser.Event += events.Add;
        parser.NotifyBodyAvailable += b => body = b;

        // Inner protobuf body: field 1 varint = self uuid 0xCAFE
        var protoBody = new byte[] { (byte)((1 << 3) | 0), 0xFE, 0x95, 0x03 };
        var notifyPayload = BuildNotifyPayload(NotifyMethod.EnterGame, protoBody);
        var frame = BuildEnvelope(MessageType.Notify, notifyPayload);

        parser.FeedGameFrame(frame, timestampSeconds: 1.0);

        // S134: parser no longer emits typed events. RawNotifyEvent + body channel only.
        Assert.DoesNotContain(events, e => e is EnterGameEvent);
        var raw = events.OfType<RawNotifyEvent>().Single();
        Assert.Equal(NotifyMethod.EnterGame, raw.MethodId);
        Assert.NotNull(body);
        Assert.Equal(NotifyMethod.EnterGame, body!.Value.MethodId);
        Assert.Equal(protoBody, body.Value.Body.ToArray());
    }

    [Fact]
    public void RegistryDecodesEnterGameFromParserBody()
    {
        var parser = new PacketParser();
        var registry = MethodDecoderRegistry.BuildDefault();
        var emitted = new List<ParserEvent>();
        parser.NotifyBodyAvailable += b =>
            registry.Dispatch(b.MethodId, b.Body.Span, b.TimestampSeconds, emitted.Add);

        var protoBody = new byte[] { (byte)((1 << 3) | 0), 0xFE, 0x95, 0x03 };
        var notifyPayload = BuildNotifyPayload(NotifyMethod.EnterGame, protoBody);
        var frame = BuildEnvelope(MessageType.Notify, notifyPayload);

        parser.FeedGameFrame(frame, timestampSeconds: 1.0);

        var enterGame = emitted.OfType<EnterGameEvent>().FirstOrDefault();
        Assert.NotNull(enterGame);
        Assert.True(enterGame!.SelfUuid > 0);
    }

    [Fact]
    public void NotifyClientKickOffDecodedViaRegistry()
    {
        var parser = new PacketParser();
        var registry = MethodDecoderRegistry.BuildDefault();
        var emitted = new List<ParserEvent>();
        parser.NotifyBodyAvailable += b =>
            registry.Dispatch(b.MethodId, b.Body.Span, b.TimestampSeconds, emitted.Add);

        var notifyPayload = BuildNotifyPayload(NotifyMethod.NotifyClientKickOff, Array.Empty<byte>());
        var frame = BuildEnvelope(MessageType.Notify, notifyPayload);

        parser.FeedGameFrame(frame, 5.0);

        Assert.Contains(emitted, e => e is KickOffEvent);
    }

    [Fact]
    public void NotifyAllMemberReadyDecodedViaRegistry()
    {
        var parser = new PacketParser();
        var registry = MethodDecoderRegistry.BuildDefault();
        var emitted = new List<ParserEvent>();
        parser.NotifyBodyAvailable += b =>
            registry.Dispatch(b.MethodId, b.Body.Span, b.TimestampSeconds, emitted.Add);

        var notifyPayload = BuildNotifyPayload(NotifyMethod.NotifyAllMemberReady, Array.Empty<byte>());
        var frame = BuildEnvelope(MessageType.Notify, notifyPayload);

        parser.FeedGameFrame(frame, 1);

        Assert.Contains(emitted, e => e is AllMemberReadyEvent);
    }

    [Fact]
    public void StrictNotifyAlsoEmitsNotifyBodySideChannel()
    {
        var parser = new PacketParser();
        NotifyBodyDecoded? decoded = null;
        parser.NotifyBodyAvailable += x => decoded = x;

        var body = new byte[] { 0x08, 0x2A };
        var notifyPayload = BuildNotifyPayload(NotifyMethod.SyncContainerData, body);
        var frame = BuildEnvelope(MessageType.Notify, notifyPayload);

        parser.FeedGameFrame(frame, 7.5);

        Assert.NotNull(decoded);
        Assert.Equal(NotifyMethod.SyncContainerData, decoded!.Value.MethodId);
        Assert.False(decoded.Value.IsZstd);
        Assert.Equal(7.5, decoded.Value.TimestampSeconds);
        Assert.Equal(body, decoded.Value.Body.ToArray());
    }

    [Fact]
    public void NonC3SbNotifyFallsBackToRawEvent()
    {
        var parser = new PacketParser();
        var events = new List<ParserEvent>();
        parser.Event += events.Add;

        // Notify with a different service uuid → header parse fails → fallback to raw.
        var payload = new byte[20];
        BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(0, 8), 0xDEADBEEFCAFEBABEUL);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(12, 4), 0x42u);
        var frame = BuildEnvelope(MessageType.Notify, payload);

        parser.FeedGameFrame(frame, 1);

        var raws = events.OfType<RawNotifyEvent>().ToList();
        Assert.Single(raws);
    }

    private static byte[] BuildNotifyPayload(int methodId, byte[] body)
    {
        // 16-byte header: 8B service uuid + 4B reserved + 4B method id (BE)
        var payload = new byte[16 + body.Length];
        BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(0, 8), PacketCodes.ServiceUuidC3Sb);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(12, 4), (uint)methodId);
        Buffer.BlockCopy(body, 0, payload, 16, body.Length);
        return payload;
    }

    private static byte[] BuildEnvelope(MessageType type, byte[] payload)
    {
        var size = 4 + 2 + payload.Length;
        var frame = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)size);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), (ushort)type);
        Buffer.BlockCopy(payload, 0, frame, 6, payload.Length);
        return frame;
    }
}

public class SharpPcapPacketSourceTests
{
    [Fact]
    public void ListDevicesIsSafeWhenNpcapMissing()
    {
        // Always succeeds: returns either a list of devices (Npcap installed)
        // or an empty list (Npcap missing). Should not throw.
        var devices = SharpPcapPacketSource.ListDevices();
        Assert.NotNull(devices);
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var source = new SharpPcapPacketSource();
        source.Dispose();
        source.Dispose();  // should not throw
    }
}
