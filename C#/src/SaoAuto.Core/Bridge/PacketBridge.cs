using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Automation;
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
    private readonly DpsTracker _dps;
    private long _eventsApplied;
    private long _rawNotifiesDispatched;
    private bool _disposed;

    public PacketBridge(
        GameStateManager state,
        IPacketParser parser,
        MethodDecoderRegistry? registry = null,
        ILogger<PacketBridge>? logger = null,
        DpsTracker? dpsTracker = null)
    {
        _state = state ?? throw new ArgumentNullException(nameof(state));
        _parser = parser ?? throw new ArgumentNullException(nameof(parser));
        _registry = registry ?? MethodDecoderRegistry.BuildDefault();
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _dps = dpsTracker ?? new DpsTracker();
        _parser.Event += OnParserEvent;
        _parser.NotifyBodyAvailable += OnNotifyBody;
    }

    public long EventsApplied => Interlocked.Read(ref _eventsApplied);
    public long RawNotifiesDispatched => Interlocked.Read(ref _rawNotifiesDispatched);
    public MethodDecoderRegistry Registry => _registry;
    public DpsTracker DpsTracker => _dps;

    private void OnParserEvent(ParserEvent ev)
    {
        // S135: raw-notify counting + dispatch both moved to OnNotifyBody so
        // each c3SB notify lands exactly once. Non-c3SB notifies still surface
        // here as RawNotifyEvent but have no decodable body — ignore them.
        if (ev is RawNotifyEvent) return;
        Apply(ev);
    }

    private void OnNotifyBody(NotifyBodyDecoded notify)
    {
        if (notify.IsZstd) return;
        DispatchRawNotify(notify.MethodId, notify.Body.Span, notify.TimestampSeconds);
    }

    /// <summary>
    /// External feed for raw notify bodies. The parser auto-fires this via
    /// <see cref="IPacketParser.NotifyBodyAvailable"/>, but tests and
    /// non-parser sources (replay harness, future zstd inflater) can call
    /// it directly to bypass the parser.
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
            if (ev is SkillEffectEvent se) RouteSkillEffectToDps(se);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[PacketBridge] mutator threw for {EventType}", ev.GetType().Name);
        }
    }

    /// <summary>
    /// S128b — fan out a <see cref="SkillEffectEvent"/> to the
    /// <see cref="DpsTracker"/>. Mirrors Python's <c>_on_damage</c>
    /// callback at packet_parser.py 4172–4253: per-row attacker
    /// resolution via <see cref="CyCombat.DpsAttackerUid"/>, name
    /// resolution from snapshot (self → <c>PlayerName</c>; monster
    /// uuid → <c>MonsterDataMap[uuid].Name</c>; otherwise
    /// <c>Player_{uid}</c>), profession only when self.
    /// </summary>
    private void RouteSkillEffectToDps(SkillEffectEvent ev)
    {
        if (ev.Damages.Count == 0) return;
        var snap = _state.Snapshot;
        var selfUuid = snap.SelfUuid;
        var selfUid = selfUuid >> 16;
        foreach (var row in ev.Damages)
        {
            if (row.Damage <= 0) continue;
            var attackerUuid = (ulong)row.AttackerUuid;
            var isSelf = CyCombat.AttackerIsSelf(attackerUuid, selfUuid, selfUid);
            var attackerUid = CyCombat.DpsAttackerUid(attackerUuid, isSelf, selfUid);
            if (attackerUid == 0) continue;

            string name;
            int profession = 0;
            if (isSelf)
            {
                name = string.IsNullOrEmpty(snap.PlayerName)
                    ? $"Player_{attackerUid}"
                    : snap.PlayerName;
                profession = snap.ProfessionId;
            }
            else if (snap.MonsterDataMap.TryGetValue(row.AttackerUuid, out var md)
                     && !string.IsNullOrEmpty(md.Name))
            {
                name = md.Name;
            }
            else
            {
                name = $"Player_{attackerUid}";
            }

            if (row.IsHeal)
            {
                _dps.RecordHeal((long)attackerUid, name, row.Damage, profession, isSelf, row.SkillId);
            }
            else
            {
                _dps.RecordDamage(
                    (long)attackerUid, name, row.Damage, profession, isSelf,
                    row.SkillId, isCrit: row.IsCrit);
            }
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _parser.Event -= OnParserEvent;
        _parser.NotifyBodyAvailable -= OnNotifyBody;
    }
}
