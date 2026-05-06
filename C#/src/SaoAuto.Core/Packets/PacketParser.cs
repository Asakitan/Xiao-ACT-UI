using System.Buffers.Binary;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Game-frame envelope parser. Mirrors the dispatch shape of Python's
/// <c>packet_parser.PacketParser.process_packet</c>:
/// <list type="bullet">
///   <item>Decode <c>[4B BE size][2B BE type][payload]</c>.</item>
///   <item>Top bit of type = zstd flag; low 15 bits = <see cref="MessageType"/>.</item>
///   <item><see cref="MessageType.FrameDown"/>: skip 4-byte inner size, recurse on the nested frame.</item>
///   <item><see cref="MessageType.Notify"/>: emit <see cref="RawNotifyEvent"/> with the method id.</item>
///   <item>Other types: emit <see cref="UnknownMessageEvent"/>.</item>
/// </list>
/// Per-method proto decoding (Identity / HP / DPS / BossHP / Buff) is the
/// responsibility of higher-level subscribers and lands in Session 5d as
/// targeted handlers attached to <see cref="Event"/>.
/// </summary>
public sealed class PacketParser : IPacketParser
{
    private readonly ILogger _log;
    private long _rawFrames;
    private long _gameFrames;
    private long _unknownTypes;

    public PacketParser(ILogger<PacketParser>? logger = null)
    {
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public event Action<ParserEvent>? Event;

    public long RawFrames => Interlocked.Read(ref _rawFrames);
    public long GameFrames => Interlocked.Read(ref _gameFrames);
    public long UnknownTypes => Interlocked.Read(ref _unknownTypes);

    public void FeedGameFrame(ReadOnlySpan<byte> frame, double timestampSeconds)
    {
        if (frame.Length < 6) return;
        Interlocked.Increment(ref _rawFrames);
        ProcessFrame(frame, timestampSeconds);
    }

    public void Reset()
    {
        Interlocked.Exchange(ref _rawFrames, 0);
        Interlocked.Exchange(ref _gameFrames, 0);
        Interlocked.Exchange(ref _unknownTypes, 0);
    }

    /// <summary>Recursive frame processor matching Python's <c>process_packet</c>.</summary>
    private void ProcessFrame(ReadOnlySpan<byte> frame, double timestampSeconds)
    {
        // Frame is `[4B BE size][2B BE type][payload]`. Caller already validated size;
        // the 4B prefix is included in size, so total = size and payload starts at +6.
        if (frame.Length < 6) return;
        var rawType = BinaryPrimitives.ReadUInt16BigEndian(frame.Slice(4, 2));
        var isZstd = (rawType & 0x8000) != 0;
        var msgType = rawType & 0x7FFF;
        var payload = frame.Slice(6);

        switch ((MessageType)msgType)
        {
            case MessageType.Notify:
                Interlocked.Increment(ref _gameFrames);
                EmitNotify(payload, isZstd, timestampSeconds);
                break;

            case MessageType.FrameDown:
                Interlocked.Increment(ref _gameFrames);
                HandleFrameDown(payload, isZstd, timestampSeconds);
                break;

            default:
                Interlocked.Increment(ref _unknownTypes);
                Emit(new UnknownMessageEvent(rawType, payload.Length, timestampSeconds));
                break;
        }
    }

    /// <summary>
    /// FrameDown payload starts with a 4-byte inner size, followed by the nested
    /// frame. When zstd is set we cannot recurse (no decoder wired yet) — emit a
    /// placeholder event so callers can record the gap.
    /// </summary>
    private void HandleFrameDown(ReadOnlySpan<byte> payload, bool isZstd, double timestampSeconds)
    {
        if (payload.Length < 4) return;
        var nested = payload.Slice(4);
        if (nested.IsEmpty) return;

        if (isZstd)
        {
            // Zstd-wrapped FrameDown: real decompression lands in Session 5d (ZstdSharp).
            Emit(new CompressedFrameEvent(MessageType.FrameDown, nested.Length, timestampSeconds));
            return;
        }

        ProcessFrame(nested, timestampSeconds);
    }

    /// <summary>
    /// Notify payload begins with the c3SB service UUID (8 BE bytes), 4
    /// reserved bytes, then a 4-byte BE method id, then a length-delimited
    /// inner payload. Mirrors <see cref="CyPacketExtras.ParseNotifyHeader"/>.
    /// Falls back to raw-method emission when the service UUID does not
    /// match — those frames belong to other services we do not parse.
    /// </summary>
    private void EmitNotify(ReadOnlySpan<byte> payload, bool isZstd, double timestampSeconds)
    {
        if (payload.IsEmpty)
        {
            Emit(new RawNotifyEvent(MethodId: -1, IsZstd: isZstd, PayloadLength: 0, timestampSeconds));
            return;
        }

        // Try the strict c3SB header first.
        var bytes = payload.ToArray();
        var parsed = CyPacketExtras.ParseNotifyHeader(bytes, PacketCodes.ServiceUuidC3Sb);
        if (parsed is not null)
        {
            var (methodId, body) = parsed.Value;
            Emit(new RawNotifyEvent(methodId, isZstd, body.Length, timestampSeconds));
            DispatchNotify(methodId, body, timestampSeconds);
            return;
        }

        // Non-c3SB or non-standard header: treat the leading varint as the
        // method id like the legacy Python fallback.
        var fallbackId = (int)Varint.ReadUInt64(payload, out _);
        Emit(new RawNotifyEvent(fallbackId, isZstd, payload.Length, timestampSeconds));
    }

    /// <summary>
    /// Per-method dispatch table. Each handler decodes the inner payload
    /// using <see cref="CyPacketExtras.DecodeFields"/> and emits a
    /// strongly-typed <see cref="ParserEvent"/>. Only the most-used methods
    /// land here; the long tail of methods (parkour / quest / shop / pay)
    /// stays as <see cref="RawNotifyEvent"/> for downstream filters.
    /// </summary>
    private void DispatchNotify(int methodId, byte[] body, double timestampSeconds)
    {
        try
        {
            switch (methodId)
            {
                case NotifyMethod.EnterGame:
                    HandleEnterGame(body, timestampSeconds);
                    break;
                case NotifyMethod.NotifyReviveUser:
                    HandleReviveUser(body, timestampSeconds);
                    break;
                case NotifyMethod.NotifyClientKickOff:
                    Emit(new KickOffEvent(timestampSeconds));
                    break;
                case NotifyMethod.NotifyAllMemberReady:
                    Emit(new AllMemberReadyEvent(timestampSeconds));
                    break;
                case NotifyMethod.NotifyCaptainReady:
                    Emit(new CaptainReadyEvent(timestampSeconds));
                    break;
                case NotifyMethod.NotifyStartPlayingDungeon:
                    HandleDungeonStart(body, timestampSeconds);
                    break;
                case NotifyMethod.SyncServerTime:
                    HandleServerTime(body, timestampSeconds);
                    break;
            }
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[Parser] handler for method 0x{MethodId:X} threw; payload {Length}B",
                methodId, body.Length);
        }
    }

    private void HandleEnterGame(byte[] body, double timestampSeconds)
    {
        var fields = CyPacketExtras.DecodeFields(body);
        var selfUuid = ReadFirstUInt64(fields, fieldNumber: 1);
        Emit(new EnterGameEvent(selfUuid, timestampSeconds));
    }

    private void HandleReviveUser(byte[] body, double timestampSeconds)
    {
        var fields = CyPacketExtras.DecodeFields(body);
        var uuid = ReadFirstUInt64(fields, fieldNumber: 1);
        Emit(new ReviveEvent(uuid, timestampSeconds));
    }

    private void HandleDungeonStart(byte[] body, double timestampSeconds)
    {
        var fields = CyPacketExtras.DecodeFields(body);
        var dungeonId = ReadFirstInt32(fields, fieldNumber: 1);
        Emit(new DungeonStartEvent(dungeonId, timestampSeconds));
    }

    private void HandleServerTime(byte[] body, double timestampSeconds)
    {
        var fields = CyPacketExtras.DecodeFields(body);
        var serverTimeMs = ReadFirstUInt64(fields, fieldNumber: 1);
        // Compute offset from local wall clock (matches Python behaviour).
        var localMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var offset = (double)serverTimeMs - localMs;
        Emit(new ServerTimeEvent(serverTimeMs, offset, timestampSeconds));
    }

    private static ulong ReadFirstUInt64(Dictionary<int, List<object>> fields, int fieldNumber)
    {
        if (!fields.TryGetValue(fieldNumber, out var list) || list.Count == 0) return 0;
        var first = list[0];
        return first switch
        {
            ulong u => u,
            long l => unchecked((ulong)l),
            byte[] b when b.Length >= 8 => System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(b),
            _ => 0,
        };
    }

    private static int ReadFirstInt32(Dictionary<int, List<object>> fields, int fieldNumber)
    {
        if (!fields.TryGetValue(fieldNumber, out var list) || list.Count == 0) return 0;
        var first = list[0];
        return first switch
        {
            ulong u => Varint.ToInt32(u),
            long l => unchecked((int)l),
            int i => i,
            _ => 0,
        };
    }

    private void Emit(ParserEvent ev)
    {
        var handlers = Event;
        if (handlers is null) return;
        try
        {
            handlers.Invoke(ev);
        }
        catch (Exception ex)
        {
            _log.LogError(ex, "[Parser] subscriber threw");
        }
    }
}
