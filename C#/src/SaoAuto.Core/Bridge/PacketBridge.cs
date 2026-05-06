using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Bridge;

/// <summary>
/// Convergence layer between the packet pipeline and the unified game state.
/// Mirrors <c>sao_auto/packet_bridge.py</c>: subscribe to
/// <see cref="IPacketParser"/> events, route raw notify bodies through a
/// <see cref="MethodDecoderRegistry"/>, then translate every emitted
/// <see cref="ParserEvent"/> into a <see cref="GameStateManager.Update"/>
/// mutation.
///
/// The Python file is large because it carries field-by-field copy logic
/// for every supported event. This C# port keeps the bridge thin: each
/// event type maps to a tiny mutator in <see cref="StateMutators"/>; the
/// bulk lives there, not here.
/// </summary>
public sealed class PacketBridge : IDisposable
{
    private readonly GameStateManager _state;
    private readonly IPacketParser _parser;
    private readonly MethodDecoderRegistry _registry;
    private readonly ILogger _log;
    private long _eventsApplied;
    private long _rawNotifiesDispatched;
    private bool _disposed;

    public PacketBridge(
        GameStateManager state,
        IPacketParser parser,
        MethodDecoderRegistry? registry = null,
        ILogger<PacketBridge>? logger = null)
    {
        _state = state ?? throw new ArgumentNullException(nameof(state));
        _parser = parser ?? throw new ArgumentNullException(nameof(parser));
        _registry = registry ?? MethodDecoderRegistry.BuildDefault();
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _parser.Event += OnParserEvent;
    }

    public long EventsApplied => Interlocked.Read(ref _eventsApplied);
    public long RawNotifiesDispatched => Interlocked.Read(ref _rawNotifiesDispatched);
    public MethodDecoderRegistry Registry => _registry;

    private void OnParserEvent(ParserEvent ev)
    {
        if (ev is RawNotifyEvent raw)
        {
            // Hand off to the per-method registry. The decoder will emit
            // strongly-typed events that we'll see on the next call to
            // OnParserEvent (the registry takes our Apply callback).
            // Since RawNotifyEvent does not carry the body, we only
            // count it; the actual dispatch is done by the parser->bridge
            // contract documented on PacketParserBodyDispatch.
            Interlocked.Increment(ref _rawNotifiesDispatched);
            return;
        }

        Apply(ev);
    }

    /// <summary>
    /// External feed for raw notify bodies — the parser surfaces only
    /// <see cref="RawNotifyEvent"/> on its event channel; the body bytes
    /// flow through this method so the registry can decode them. This
    /// keeps the parser a pure envelope decoder.
    /// </summary>
    public void DispatchRawNotify(int methodId, ReadOnlySpan<byte> body, double timestampSeconds)
    {
        Interlocked.Increment(ref _rawNotifiesDispatched);
        _registry.Dispatch(methodId, body, timestampSeconds, Apply);
    }

    /// <summary>
    /// Apply a strongly-typed parser event to the unified game state.
    /// Public so memory-probe / recognition layers can converge through
    /// the same mutators (mirrors Python's bridge ownership model).
    /// </summary>
    public void Apply(ParserEvent ev)
    {
        try
        {
            var changed = StateMutators.Apply(_state, ev);
            if (changed) Interlocked.Increment(ref _eventsApplied);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[PacketBridge] mutator threw for {EventType}", ev.GetType().Name);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _parser.Event -= OnParserEvent;
    }
}
