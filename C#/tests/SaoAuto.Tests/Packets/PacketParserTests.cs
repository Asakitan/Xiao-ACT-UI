using System.Buffers.Binary;
using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class PacketParserTests
{
    [Fact]
    public void NotifyFrameEmitsRawNotifyEvent()
    {
        var parser = new PacketParser();
        ParserEvent? received = null;
        parser.Event += e => received = e;

        // Notify (msg type=2), method id varint = 0x14 (EnterGame)
        var frame = BuildFrame(MessageType.Notify, isZstd: false,
            payload: new byte[] { (byte)NotifyMethod.EnterGame, 0x00, 0x00 });

        parser.FeedGameFrame(frame, timestampSeconds: 1.0);

        var notify = Assert.IsType<RawNotifyEvent>(received);
        Assert.Equal(NotifyMethod.EnterGame, notify.MethodId);
        Assert.False(notify.IsZstd);
        Assert.Equal(3, notify.PayloadLength);
        Assert.Equal(1, parser.GameFrames);
    }

    [Fact]
    public void FrameDownRecursesIntoNestedFrame()
    {
        var parser = new PacketParser();
        var events = new List<ParserEvent>();
        parser.Event += events.Add;

        // Inner Notify with method=0x06 (SyncNearEntities)
        var inner = BuildFrame(MessageType.Notify, false, new byte[] { (byte)NotifyMethod.SyncNearEntities });
        // FrameDown payload: [4B inner-size][inner-frame]
        var frameDownPayload = new byte[4 + inner.Length];
        BinaryPrimitives.WriteUInt32BigEndian(frameDownPayload.AsSpan(0, 4), (uint)inner.Length);
        Buffer.BlockCopy(inner, 0, frameDownPayload, 4, inner.Length);
        var outer = BuildFrame(MessageType.FrameDown, false, frameDownPayload);

        parser.FeedGameFrame(outer, timestampSeconds: 2.0);

        Assert.Single(events);
        var notify = Assert.IsType<RawNotifyEvent>(events[0]);
        Assert.Equal(NotifyMethod.SyncNearEntities, notify.MethodId);
        // Both outer FrameDown and inner Notify count as game frames.
        Assert.Equal(2, parser.GameFrames);
    }

    [Fact]
    public void ZstdFrameDownEmitsCompressedPlaceholder()
    {
        var parser = new PacketParser();
        ParserEvent? received = null;
        parser.Event += e => received = e;

        var frameDownPayload = new byte[4 + 8];
        BinaryPrimitives.WriteUInt32BigEndian(frameDownPayload.AsSpan(0, 4), 8);
        var outer = BuildFrame(MessageType.FrameDown, isZstd: true, payload: frameDownPayload);

        parser.FeedGameFrame(outer, timestampSeconds: 3.0);

        var compressed = Assert.IsType<CompressedFrameEvent>(received);
        Assert.Equal(MessageType.FrameDown, compressed.Kind);
        Assert.Equal(8, compressed.PayloadLength);
    }

    [Fact]
    public void UnknownMessageTypeIsCounted()
    {
        var parser = new PacketParser();
        ParserEvent? received = null;
        parser.Event += e => received = e;

        var frame = BuildFrame((MessageType)42, false, new byte[] { 0xDE, 0xAD });
        parser.FeedGameFrame(frame, 0);

        var unk = Assert.IsType<UnknownMessageEvent>(received);
        Assert.Equal(42, unk.RawType);
        Assert.Equal(1, parser.UnknownTypes);
    }

    [Fact]
    public void RejectsTooShortFrames()
    {
        var parser = new PacketParser();
        var events = new List<ParserEvent>();
        parser.Event += events.Add;

        parser.FeedGameFrame(new byte[] { 0x00 }, 0);
        parser.FeedGameFrame(new byte[] { 0x00, 0x00, 0x00, 0x06, 0x00 }, 0); // 5 bytes (< 6 required)

        Assert.Empty(events);
        Assert.Equal(0, parser.RawFrames);
    }

    [Fact]
    public void ResetClearsCounters()
    {
        var parser = new PacketParser();
        parser.FeedGameFrame(BuildFrame(MessageType.Notify, false, new byte[] { 0x14 }), 0);
        Assert.Equal(1, parser.RawFrames);
        parser.Reset();
        Assert.Equal(0, parser.RawFrames);
        Assert.Equal(0, parser.GameFrames);
    }

    private static byte[] BuildFrame(MessageType type, bool isZstd, byte[] payload)
    {
        var size = 4 + 2 + payload.Length;
        var frame = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)size);
        var rawType = (ushort)((isZstd ? 0x8000 : 0) | (ushort)type);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), rawType);
        Buffer.BlockCopy(payload, 0, frame, 6, payload.Length);
        return frame;
    }
}
