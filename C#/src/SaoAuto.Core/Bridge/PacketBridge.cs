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
    }

    public long EventsApplied => Interlocked.Read(ref _eventsApplied);
    public long RawNotifiesDispatched => Interlocked.Read(ref _rawNotifiesDispatched);
    public MethodDecoderRegistry Registry => _registry;
    public DpsTracker DpsTracker => _dps;

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
    }
}
