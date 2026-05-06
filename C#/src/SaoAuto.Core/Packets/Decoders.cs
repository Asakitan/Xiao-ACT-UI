using Google.Protobuf;
using Star;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Decoder for <c>NotifyMethod.SyncServerTime (0x2B)</c>. Mirrors
/// <c>packet_parser._on_sync_server_time</c>: parses the proto, computes
/// the offset between server-reported and local wall clock, and emits a
/// <see cref="ServerTimeEvent"/>. Used by cooldown / DPS / boss-HP code
/// to project packet timestamps onto game time.
/// </summary>
public sealed class SyncServerTimeDecoder : ProtoMethodDecoder<SyncServerTime>
{
    public override int MethodId => NotifyMethod.SyncServerTime;
    protected override MessageParser<SyncServerTime> Parser => SyncServerTime.Parser;

    protected override void Project(SyncServerTime msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (msg.ClientMilliseconds <= 0 && msg.ServerMilliseconds <= 0) return;
        var localMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var offset = (double)msg.ServerMilliseconds - localMs;
        emit(new ServerTimeEvent((ulong)msg.ServerMilliseconds, offset, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.NotifyReviveUser (0x27)</c>. Emits a
/// <see cref="ReviveEvent"/> with the resurrected actor's uuid.
/// </summary>
public sealed class NotifyReviveUserDecoder : ProtoMethodDecoder<NotifyReviveUser>
{
    public override int MethodId => NotifyMethod.NotifyReviveUser;
    protected override MessageParser<NotifyReviveUser> Parser => NotifyReviveUser.Parser;

    protected override void Project(NotifyReviveUser msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new ReviveEvent(unchecked((ulong)msg.VActorUuid), timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.NotifyClientKickOff (0x31)</c>.
/// Server-side session-end notifications carry a small empty message —
/// we surface a <see cref="KickOffEvent"/> so subscribers can tear down
/// state without having to walk the proto.
/// </summary>
public sealed class NotifyClientKickOffDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyClientKickOff;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new KickOffEvent(timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.NotifyAllMemberReady (0x46)</c>.
/// </summary>
public sealed class NotifyAllMemberReadyDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyAllMemberReady;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new AllMemberReadyEvent(timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.NotifyCaptainReady (0x47)</c>.
/// </summary>
public sealed class NotifyCaptainReadyDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyCaptainReady;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new CaptainReadyEvent(timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.NotifyStartPlayingDungeon (0x37)</c>.
/// The proto class is not generated as a named message (the body is a
/// single varint dungeon id), so we read it manually with
/// <see cref="CyPacketExtras.DecodeFields"/>.
/// </summary>
public sealed class NotifyStartPlayingDungeonDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyStartPlayingDungeon;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (body.IsEmpty) { emit(new DungeonStartEvent(0, timestampSeconds)); return; }
        var fields = CyPacketExtras.DecodeFields(body.ToArray());
        var dungeonId = 0;
        if (fields.TryGetValue(1, out var list) && list.Count > 0)
        {
            dungeonId = list[0] switch
            {
                ulong u => Varint.ToInt32(u),
                long l => unchecked((int)l),
                int i => i,
                _ => 0,
            };
        }
        emit(new DungeonStartEvent(dungeonId, timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.EnterScene (0x03)</c>. Surfaces a
/// <see cref="EnterSceneEvent"/> so subscribers can refresh per-scene
/// state (DPS reset, recognition prime, etc.) without inspecting the
/// proto body.
/// </summary>
public sealed class EnterSceneDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.EnterScene;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new EnterSceneEvent(timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.EnterMatchResultNtf (0x48001)</c>.
/// Fires after a dungeon clear — useful to drive the DPS finalize gate
/// and to flip the auto-key engine into post-combat mode.
/// </summary>
public sealed class EnterMatchResultDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.EnterMatchResultNtf;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new MatchResultEvent(timestampSeconds));
    }
}

