using System.Buffers.Binary;
using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// Session 134 — collapse parser inline fallback into registry-only dispatch.
/// Verifies the parser is now a pure envelope decoder and that every method
/// id (including the seven that used to be inline) flows through the
/// MethodDecoderRegistry.
/// </summary>
public class Session134RegistryOnlyDispatchTests
{
    [Fact]
    public void ParserHasNoInlineDispatchSurfaceArea()
    {
        // S134 deleted the static IsInlineDispatchMethod helper — make sure no
        // future change reintroduces it under a different name. We also assert
        // the parser type no longer exposes per-method state.
        var members = typeof(PacketParser).GetMembers()
            .Select(m => m.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("IsInlineDispatchMethod", members);
        Assert.DoesNotContain("InlineDispatchMethodIds", members);
    }

    [Fact]
    public void ServerTimeRoutesThroughRegistryEndToEnd()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);
        parser.NotifyBodyAvailable += b =>
            bridge.DispatchRawNotify(b.MethodId, b.Body.Span, b.TimestampSeconds);

        var msg = new SyncServerTime
        {
            ClientMilliseconds = 1,
            ServerMilliseconds = 1_700_000_000_000L,
        };
        var frame = BuildNotifyFrame(NotifyMethod.SyncServerTime, msg.ToByteArray());

        parser.FeedGameFrame(frame, timestampSeconds: 5.0);

        // ServerTime mutator stamps ServerTimeOffsetMs onto state.
        Assert.NotEqual(0, state.Snapshot.ServerTimeOffsetMs);
        Assert.True(bridge.RawNotifiesDispatched >= 1);
    }

    [Fact]
    public void EnterGameStillEmittedViaRegistryAfterInlinePathRemoved()
    {
        var parser = new PacketParser();
        var registry = MethodDecoderRegistry.BuildDefault();
        var emitted = new List<ParserEvent>();
        parser.NotifyBodyAvailable += b =>
            registry.Dispatch(b.MethodId, b.Body.Span, b.TimestampSeconds, emitted.Add);

        // field 1 varint = 0xCAFE
        var protoBody = new byte[] { (byte)((1 << 3) | 0), 0xFE, 0x95, 0x03 };
        var frame = BuildNotifyFrame(NotifyMethod.EnterGame, protoBody);

        parser.FeedGameFrame(frame, timestampSeconds: 1.0);

        var enterGame = emitted.OfType<EnterGameEvent>().FirstOrDefault();
        Assert.NotNull(enterGame);
        Assert.True(enterGame!.SelfUuid > 0);
    }

    [Fact]
    public void ParserChannelOnlyEmitsRawNotifyForFormerlyInlineMethod()
    {
        var parser = new PacketParser();
        var events = new List<ParserEvent>();
        parser.Event += events.Add;

        var frame = BuildNotifyFrame(NotifyMethod.NotifyClientKickOff, Array.Empty<byte>());
        parser.FeedGameFrame(frame, 9.0);

        // Parser no longer materialises KickOffEvent on its own channel —
        // only the RawNotifyEvent envelope record.
        Assert.DoesNotContain(events, e => e is KickOffEvent);
        Assert.Single(events.OfType<RawNotifyEvent>());
    }

    [Fact]
    public void RegistryCoversEveryFormerlyInlineMethod()
    {
        // The 7 method ids the parser used to dispatch inline must all be in
        // the default registry so the cleanup is a true no-op for callers.
        var registry = MethodDecoderRegistry.BuildDefault();
        var inlineIds = new[]
        {
            NotifyMethod.EnterGame,
            NotifyMethod.NotifyReviveUser,
            NotifyMethod.NotifyClientKickOff,
            NotifyMethod.NotifyAllMemberReady,
            NotifyMethod.NotifyCaptainReady,
            NotifyMethod.NotifyStartPlayingDungeon,
            NotifyMethod.SyncServerTime,
        };
        foreach (var id in inlineIds)
        {
            Assert.True(registry.TryGet(id, out _), $"registry missing decoder for 0x{id:X}");
        }
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
