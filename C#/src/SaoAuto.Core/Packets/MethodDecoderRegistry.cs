using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Per-method dispatch table — the C# port of the giant
/// <c>if method_id == ...</c> chain in
/// <c>sao_auto/packet_parser.py:_on_notify</c>. Higher layers
/// (<c>PacketBridge</c> in S31, the <c>PacketParser</c> as a fallback)
/// query this registry to route a notify body to its proto-aware decoder.
///
/// Decoders are registered up-front with <see cref="Register"/>; the
/// canonical "what we know how to decode today" set is built by
/// <see cref="BuildDefault"/>. Methods we have NOT ported yet are simply
/// absent from the registry — callers see <see cref="TryGet"/> return false
/// and can fall back to a <see cref="RawNotifyEvent"/>.
/// </summary>
public sealed class MethodDecoderRegistry
{
    private readonly Dictionary<int, IMethodDecoder> _decoders = new();
    private readonly ILogger _log;

    public MethodDecoderRegistry(ILogger<MethodDecoderRegistry>? logger = null)
    {
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public int Count => _decoders.Count;

    /// <summary>Snapshot of currently registered method ids (sorted, deterministic).</summary>
    public IReadOnlyList<int> RegisteredMethodIds
    {
        get
        {
            var ids = _decoders.Keys.ToArray();
            Array.Sort(ids);
            return ids;
        }
    }

    public void Register(IMethodDecoder decoder)
    {
        ArgumentNullException.ThrowIfNull(decoder);
        _decoders[decoder.MethodId] = decoder;
    }

    public bool TryGet(int methodId, out IMethodDecoder decoder)
        => _decoders.TryGetValue(methodId, out decoder!);

    /// <summary>
    /// Run the registered decoder for the given method id, if any.
    /// Returns true when a decoder ran (regardless of whether it emitted).
    /// </summary>
    public bool Dispatch(int methodId, ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (!_decoders.TryGetValue(methodId, out var decoder)) return false;
        try
        {
            decoder.Decode(body, timestampSeconds, emit);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex,
                "[MethodDecoderRegistry] decoder for 0x{MethodId:X} threw; payload {Length}B",
                methodId, body.Length);
        }
        return true;
    }

    /// <summary>
    /// Construct the canonical registry with all decoders the C# port
    /// currently ships. Add new decoders here as they are ported across
    /// follow-up sessions.
    /// </summary>
    public static MethodDecoderRegistry BuildDefault(ILoggerFactory? loggerFactory = null)
    {
        var log = (ILogger<MethodDecoderRegistry>?)loggerFactory?.CreateLogger<MethodDecoderRegistry>();
        var reg = new MethodDecoderRegistry(log);
        reg.Register(new SyncServerTimeDecoder());
        reg.Register(new NotifyReviveUserDecoder());
        reg.Register(new NotifyClientKickOffDecoder());
        reg.Register(new NotifyAllMemberReadyDecoder());
        reg.Register(new NotifyCaptainReadyDecoder());
        reg.Register(new NotifyStartPlayingDungeonDecoder());
        reg.Register(new EnterSceneDecoder());
        reg.Register(new EnterMatchResultDecoder());
        reg.Register(new EnterGameDecoder());
        reg.Register(new NotifyBuffChangeDecoder());
        reg.Register(new SyncClientUseSkillDecoder());
        reg.Register(new SyncServerSkillEndDecoder());
        reg.Register(new SyncServerSkillStageEndDecoder());
        reg.Register(new QteBeginDecoder());
        reg.Register(new SyncDungeonDataDecoder());
        reg.Register(new SyncDungeonDirtyDataDecoder());
        reg.Register(new SyncClientUseSkillWorldDecoder());
        reg.Register(new SyncNearEntitiesDecoder());
        reg.Register(new SyncNearDeltaInfoDecoder());
        reg.Register(new SyncToMeDeltaInfoDecoder());
        reg.Register(new SyncContainerDataDecoder());
        reg.Register(new SyncContainerDirtyDataDecoder());
        return reg;
    }
}
