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

    // R8 / MAPSWITCH-02: pending combat-reset gate. When a scene change with
    // reset_on_next_damage=true lands (soft restart from wipe buff or
    // same-dungeon-id roll), we DEFER the DpsTracker.Reset() until the FIRST
    // incoming damage event of the next encounter — so the prior encounter
    // window still finalizes via LastReport but the next damage opens a
    // fresh encounter. Mirrors Python's _arm_pending_combat_reset in
    // sao_gui_float_handlers_mixin.py:65-90.
    private long _pendingResetAtSeconds; // 0 == disarmed; otherwise UTC seconds threshold
    private string _pendingResetReason = string.Empty;

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

    /// <summary>
    /// R8: observation surface for SceneChangeEvents that reach the bridge
    /// (after the soft / hard reset wiring runs). Subscribers can fan a
    /// JS-side <c>state.scene_changed</c> event off this without polling
    /// the GameState snapshot. Fired AFTER state mutation + DpsTracker
    /// adjustment so observers see a consistent post-reset world.
    /// </summary>
    public event Action<SceneChangeEvent>? SceneChanged;

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
        var handled = _registry.Dispatch(methodId, body, timestampSeconds, Apply);
        if (!handled)
        {
            // Mirrors packet_parser.py:2173-2178 — known-but-unported method ids
            // surface as named debug lines so we can see which decoders we still
            // owe. Truly unknown ids only get a hex print.
            if (NotifyMethod.Names.TryGetValue(methodId, out var name))
            {
                _log.LogDebug(
                    "[Bridge] Unhandled notify {Name} (0x{Id:X}) len={Length}",
                    name, methodId, body.Length);
            }
            else
            {
                _log.LogDebug(
                    "[Bridge] Unknown notify 0x{Id:X} len={Length}",
                    methodId, body.Length);
            }
        }
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
            // R8: PROTO-01 soft-restart side channel maps to a SceneChangeEvent
            // before falling through to the regular mutator pass — keeps the
            // sequencing identical to dungeon-id / scene-id soft restarts.
            if (ev is SoftSceneRestartEvent ssr)
            {
                HandleSceneChange(new SceneChangeEvent(
                    SceneChangeKind.Restart, ssr.Reason,
                    PreserveCombat: true, ResetOnNextDamage: true,
                    ResetDelaySeconds: 3.0,
                    TimestampSeconds: ssr.TimestampSeconds));
                return;
            }
            if (ev is SceneChangeEvent direct)
            {
                HandleSceneChange(direct);
                return;
            }

            // R8 derived-event capture: scene-aware mutators
            // (ApplyDungeonStart, ApplyEnterScene, ApplyDungeonData) emit
            // synthesised SceneChangeEvents into a thread-local sink during
            // the Apply() call. We collect them then re-feed each through
            // this same Apply() so subscribers see one consistent stream.
            var prior = StateMutators.BeginDerivedCapture(out var sink);
            bool changed;
            try
            {
                changed = StateMutators.Apply(_state, ev);
            }
            finally
            {
                StateMutators.EndDerivedCapture(prior);
            }
            if (changed) Interlocked.Increment(ref _eventsApplied);

            // DPS-08: SkillEffectEvent must drive the DPS pump regardless of
            // whether StateMutators.ApplySkillEffect returned `changed=true`
            // (it can return false when the target uuid has no MonsterDataMap
            // row yet — the very first damage of combat). Routing happens
            // BEFORE the pending-reset gate so the deferred reset arms only
            // when the bridge actually saw post-scene-change damage.
            if (ev is SkillEffectEvent se)
            {
                MaybeApplyPendingReset(se.TimestampSeconds);
                RouteSkillEffectToDps(se);
                _state.PingSubscribers();
            }

            // Re-fan derived events so SceneChangeEvent subscribers see them.
            if (sink.Count > 0)
            {
                foreach (var derived in sink)
                {
                    Apply(derived);
                }
            }
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[PacketBridge] mutator threw for {EventType}", ev.GetType().Name);
        }
    }

    /// <summary>
    /// R8 / MSR-7 / MAPSWITCH-08: per-kind scene transition handler. Hard
    /// resets wipe DpsTracker immediately; restart arms the deferred gate
    /// (first damage in next encounter flushes); transition is observation-only.
    /// </summary>
    private void HandleSceneChange(SceneChangeEvent ev)
    {
        // R8 / MSR-6: stale-monster TTL used by soft scene transitions to
        // purge phantom boss-bar candidates from the prior layer. Mirrors
        // Python's _purge_stale_monsters_locked(8.0) default at
        // packet_parser.py:1693.
        const double TransitionPurgeTtl = 8.0;
        switch (ev.Kind)
        {
            case SceneChangeKind.Hard:
                _state.ResetScene(preserveCombat: false, ev.Reason);
                _dps.Reset();
                _pendingResetAtSeconds = 0;
                _pendingResetReason = string.Empty;
                _log.LogInformation(
                    "[Bridge] Hard scene change: {Reason} — full state reset",
                    ev.Reason);
                break;
            case SceneChangeKind.Restart:
                // Preserve combat HUD across the transition, but drop the
                // monster table + boss summary so the next scene starts with
                // clean candidates. Defer DPS reset to first damage.
                _state.ResetScene(preserveCombat: true, ev.Reason);
                if (ev.ResetOnNextDamage)
                {
                    var delay = ev.ResetDelaySeconds > 0 ? ev.ResetDelaySeconds : 3.0;
                    var nowTicks = DateTimeOffset.UtcNow.UtcTicks;
                    var thresholdTicks = nowTicks + (long)(delay * TimeSpan.TicksPerSecond);
                    Interlocked.Exchange(ref _pendingResetAtSeconds, thresholdTicks);
                    _pendingResetReason = ev.Reason;
                    _log.LogInformation(
                        "[Bridge] Soft scene restart: {Reason} (defer DPS reset {Delay:F1}s)",
                        ev.Reason, delay);
                }
                else
                {
                    _dps.Reset();
                }
                break;
            case SceneChangeKind.Transition:
                // Combat continues seamlessly; only purge stale monster rows
                // so boss-bar candidates from the previous layer don't
                // linger past the TTL. DPS unchanged.
                var dropped = _state.PurgeStaleMonsters(TransitionPurgeTtl, ev.TimestampSeconds);
                _log.LogInformation(
                    "[Bridge] Soft scene transition: {Reason} (combat preserved, purged {Dropped} stale monsters)",
                    ev.Reason, dropped);
                break;
        }
        _state.PingSubscribers();
        // R8: fan the change to bridge subscribers AFTER state mutation +
        // DPS tracker adjustment so JS-side observers see the canonical
        // post-reset world (matches Python's emit order at
        // packet_parser.py:1566 → bridge subscribers).
        try
        {
            SceneChanged?.Invoke(ev);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[Bridge] SceneChanged subscriber threw for {Reason}", ev.Reason);
        }
    }

    /// <summary>
    /// R8 / MAPSWITCH-02: consume the pending-reset gate. When a damage
    /// event arrives after a soft restart was armed, finalize the prior
    /// encounter into LastReport and zero the tracker so the incoming
    /// damage opens a fresh encounter cleanly.
    /// </summary>
    private void MaybeApplyPendingReset(double damageTimestampSeconds)
    {
        var thresholdTicks = Interlocked.Read(ref _pendingResetAtSeconds);
        if (thresholdTicks == 0) return;
        // The threshold is a CAP, not a requirement — first damage event
        // after the arm immediately consumes the gate per Python's
        // _maybe_apply_pending_combat_reset(_pending_combat_reset_after) at
        // sao_gui_float_handlers_mixin.py:92-128. The delay value provides
        // an upper bound for "fall through if no damage yet" cleanup paths
        // (not used in this minimal port — left as future hook).
        _ = thresholdTicks;
        // FinalizeIfIdle already clears entities + encounter on a successful
        // finalize and stashes the report on LastReport. We must NOT call
        // _dps.Reset() afterwards because that would null the just-stored
        // LastReport. If FinalizeIfIdle returns null (no active encounter),
        // there's nothing to flush — fall through and let RouteSkillEffectToDps
        // open the new encounter directly.
        _dps.FinalizeIfIdle(TimeSpan.Zero, reason: _pendingResetReason);
        Interlocked.Exchange(ref _pendingResetAtSeconds, 0);
        _pendingResetReason = string.Empty;
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
        // DPS-04: keep DpsTracker's _selfUid pinned in lock-step with the
        // snapshot so per-row IsSelf classification stays consistent with
        // the tracker's late-upgrade path (FN-009).
        if (selfUid != 0 && _dps.SelfUid != selfUid) _dps.SetSelfUid(selfUid);
        foreach (var row in ev.Damages)
        {
            if (row.Damage <= 0) continue;
            // PROTO-07: re-attribute summon / pet / owner-routed damage so
            // it reaches the owner's DPS row instead of getting dropped at
            // the attackerUid==0 guard below. Mirrors Python's
            // _decode_sync_damage_info four-branch precedence at
            // packet_parser.py:4031-4054:
            //   1. TopSummonerId IS a player → use it as the attacker uuid
            //   2. TopSummonerId is a CharId of self / known player → use as uid
            //   3. otherwise fall back to the raw AttackerUuid
            var attackerUuid = (ulong)row.AttackerUuid;
            var topSummonerId = row.TopSummonerId;
            ulong resolvedUuid = attackerUuid;
            ulong resolvedUid = 0;
            if (topSummonerId != 0)
            {
                var topU = (ulong)topSummonerId;
                if (CyCombat.IsPlayerUuid(topU))
                {
                    resolvedUuid = topU;
                    resolvedUid = CyCombat.UuidToUid(topU);
                }
                else
                {
                    // CharId form: trust as uid only when it matches the
                    // local player (no Players/TeamMembers state ported yet
                    // — that's the CharTeam decoder gap noted in PROTO-07).
                    var ownerUid = topU;
                    if (selfUid != 0 && ownerUid == selfUid)
                    {
                        resolvedUid = ownerUid;
                    }
                    if (attackerUuid != 0 && CyCombat.IsPlayerUuid(attackerUuid))
                    {
                        resolvedUuid = attackerUuid;
                    }
                    else
                    {
                        resolvedUuid = topU;
                    }
                }
            }
            var isSelf = CyCombat.AttackerIsSelf(resolvedUuid, selfUuid, selfUid);
            var attackerUid = resolvedUid != 0
                ? resolvedUid
                : CyCombat.DpsAttackerUid(resolvedUuid, isSelf, selfUid);
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
                    row.SkillId, isCrit: row.IsCrit,
                    targetUuid: ev.TargetUuid);
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
