using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S98 — Bridges <see cref="GameStateManager"/> → <see cref="BridgeEventBroadcaster"/>.
///
/// Subscribes to state updates and emits
/// <see cref="BridgeEvents.GameStateChanged"/> with the typed
/// <see cref="StateSnapshotPayload.ToDict"/> payload (43-key parity
/// with Python <c>GameState.to_dict</c>, 4dp rounding for hp/stamina,
/// 1dp for boss_enrage_remaining, banker's rounding).
///
/// Dedupe: a payload identical to the previously emitted one is
/// suppressed (compared via canonical JSON string), mirroring the
/// Python emit pattern (mem_probe/entity_watcher.py 286 — `should_emit
/// = sig != state.last_emit_sig`).
///
/// S101 fan-out: after each <c>state.changed</c> emission, the
/// publisher also emits narrow events for fields the JS side commonly
/// wants to subscribe to in isolation —
/// <see cref="BridgeEvents.HealthChanged"/> when
/// <c>HpPct</c>/<c>HpCurrent</c>/<c>HpMax</c> differs from the
/// previous emit, <see cref="BridgeEvents.StaminaChanged"/> for
/// stamina, <see cref="BridgeEvents.BurstReady"/> when the boolean
/// flips. Saves JS subscribers from diffing the 43-key payload by
/// hand. The narrow events fire ONLY when the full <c>state.changed</c>
/// fires (i.e. the payload-dedupe gate is the upstream filter).
///
/// S105 extends the fan-out with two more channels:
/// <see cref="BridgeEvents.BossHpSnapshot"/> when any of
/// BossCurrentHp / BossTotalHp / BossHpEstPct / BossShieldActive /
/// BossShieldPct / BossBreakingStage / BossInOverdrive /
/// BossInvincible / BossHpSource changes, and
/// <see cref="BridgeEvents.DpsSnapshot"/> when BossTotalDamage or
/// BossDps changes. Same first-emit-fires-all contract so JS
/// subscribers that connect after Start can render an initial value.
///
/// S131 adds an optional DPS snapshot provider. When supplied, the
/// full <c>state.changed</c> payload includes the per-attacker
/// <c>dps_entries</c> rollup plus total damage/heal/HPS metadata via
/// <see cref="StateSnapshotPayload.ToDict(GameState, DpsSnapshot?)"/>.
/// The provider is intentionally optional because the App layer does
/// not always own a live <see cref="DpsTracker"/> yet.
///
/// Lifecycle: <see cref="Start"/> hooks the subscription and (by
/// default) emits an initial snapshot so the JS side has a value to
/// render before the first state mutation. <see cref="Dispose"/>
/// drops the subscription. Both are idempotent.
/// </summary>
public sealed class GameStatePublisher : IDisposable
{
    private const string CaptureTsKey = "capture_ts";

    // R8 / DPS-02 idle threshold for the per-tick pump. Mirrors Python's
    // <c>poll_overlay_state(idle_timeout_s=15.0)</c> default at
    // dps_tracker.py:603. After 15s without damage the overlay is told to
    // fade out; on the next damage event has-live flips back true.
    private static readonly TimeSpan DpsIdleTimeout = TimeSpan.FromSeconds(15.0);

    private readonly GameStateManager _states;
    private readonly BridgeEventBroadcaster _broadcaster;
    private readonly Func<DpsSnapshot?>? _dpsSnapshotProvider;
    // R8: optional DpsTracker for the show-live edge + scene-change wiring.
    // Plumbed when the App layer has the live PacketBridge instance.
    // When null the publisher falls back to the snapshot-provider behavior
    // it shipped before R8 (no edge, no fade-out, no toggle).
    private readonly DpsTracker? _dpsTracker;
    private readonly Func<bool>? _dpsEnabledProvider;
    private readonly object _gate = new();
    private IDisposable? _sub;
    private string? _lastSig;
    private bool _disposed;

    // R8 / DPS-07 edge tracker. Latches whether the panel is currently
    // shown so the pump only emits the edge on a true transition.
    private bool _dpsOverlayShown;
    private DateTimeOffset _lastDpsPollAt;

    private bool _hasNarrowSnapshot;
    private double _lastHpPct;
    private int _lastHpCurrent;
    private int _lastHpMax;
    private double _lastStaminaPct;
    private int _lastStaminaCurrent;
    private int _lastStaminaMax;
    private bool _lastBurstReady;
    private int _lastBossCurrentHp;
    private int _lastBossTotalHp;
    private double _lastBossHpEstPct;
    private bool _lastBossShieldActive;
    private double _lastBossShieldPct;
    private int _lastBossBreakingStage;
    private bool _lastBossInOverdrive;
    private bool _lastBossInvincible;
    private BossHpSource _lastBossHpSource;
    private int _lastBossTotalDamage;
    private int _lastBossDps;

    public GameStatePublisher(
        GameStateManager states,
        BridgeEventBroadcaster broadcaster,
        Func<DpsSnapshot?>? dpsSnapshotProvider = null,
        DpsTracker? dpsTracker = null,
        Func<bool>? dpsEnabledProvider = null)
    {
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _broadcaster = broadcaster ?? throw new ArgumentNullException(nameof(broadcaster));
        _dpsSnapshotProvider = dpsSnapshotProvider;
        _dpsTracker = dpsTracker;
        _dpsEnabledProvider = dpsEnabledProvider;
    }

    public bool IsActive => _sub is not null;

    /// <summary>S190 — pull a fresh snapshot payload without emitting.
    /// Mirrors what <see cref="OnState"/> builds, minus the dedup and
    /// broadcast steps. Used by the <c>state.snapshot</c> bridge
    /// command so a freshly-opened HUD page can paint immediately.</summary>
    public JsonObject SnapshotPayload()
    {
        var dps = _dpsSnapshotProvider?.Invoke();
        return StateSnapshotPayload.ToDict(_states.Snapshot, dps);
    }

    public void Start(bool emitInitial = true)
    {
        lock (_gate)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(GameStatePublisher));
            if (_sub is not null) return;
            _sub = _states.Subscribe(OnState);
        }
        if (emitInitial) OnState(_states.Snapshot);
    }

    public void Dispose()
    {
        IDisposable? toDispose;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            toDispose = _sub;
            _sub = null;
        }
        toDispose?.Dispose();
    }

    private void OnState(GameState state)
    {
        var dps = _dpsSnapshotProvider?.Invoke();
        var payload = StateSnapshotPayload.ToDict(state, dps);
        // S119 — dedup signature excludes the auto-stamped capture_ts so two
        // back-to-back identical Update calls (which differ only by ms-level
        // CaptureTimestamp) collapse to a single emit, matching Python's
        // last_emit_sig contract (mem_probe/entity_watcher.py 286). The
        // capture_ts key stays on the wire payload for JS consumers.
        var sigNode = payload[CaptureTsKey];
        payload.Remove(CaptureTsKey);
        var sig = payload.ToJsonString();
        payload[CaptureTsKey] = sigNode;

        bool emitHp;
        bool emitStamina;
        bool emitBurst;
        bool emitBossHp;
        bool emitDps;
        lock (_gate)
        {
            if (sig == _lastSig) return;
            _lastSig = sig;

            if (!_hasNarrowSnapshot)
            {
                emitHp = emitStamina = emitBurst = emitBossHp = emitDps = true;
                _hasNarrowSnapshot = true;
            }
            else
            {
                emitHp = state.HpPct != _lastHpPct
                    || state.HpCurrent != _lastHpCurrent
                    || state.HpMax != _lastHpMax;
                emitStamina = state.StaminaPct != _lastStaminaPct
                    || state.StaminaCurrent != _lastStaminaCurrent
                    || state.StaminaMax != _lastStaminaMax;
                emitBurst = state.BurstReady != _lastBurstReady;
                emitBossHp = state.BossCurrentHp != _lastBossCurrentHp
                    || state.BossTotalHp != _lastBossTotalHp
                    || state.BossHpEstPct != _lastBossHpEstPct
                    || state.BossShieldActive != _lastBossShieldActive
                    || state.BossShieldPct != _lastBossShieldPct
                    || state.BossBreakingStage != _lastBossBreakingStage
                    || state.BossInOverdrive != _lastBossInOverdrive
                    || state.BossInvincible != _lastBossInvincible
                    || state.BossHpSource != _lastBossHpSource;
                emitDps = state.BossTotalDamage != _lastBossTotalDamage
                    || state.BossDps != _lastBossDps;
            }

            _lastHpPct = state.HpPct;
            _lastHpCurrent = state.HpCurrent;
            _lastHpMax = state.HpMax;
            _lastStaminaPct = state.StaminaPct;
            _lastStaminaCurrent = state.StaminaCurrent;
            _lastStaminaMax = state.StaminaMax;
            _lastBurstReady = state.BurstReady;
            _lastBossCurrentHp = state.BossCurrentHp;
            _lastBossTotalHp = state.BossTotalHp;
            _lastBossHpEstPct = state.BossHpEstPct;
            _lastBossShieldActive = state.BossShieldActive;
            _lastBossShieldPct = state.BossShieldPct;
            _lastBossBreakingStage = state.BossBreakingStage;
            _lastBossInOverdrive = state.BossInOverdrive;
            _lastBossInvincible = state.BossInvincible;
            _lastBossHpSource = state.BossHpSource;
            _lastBossTotalDamage = state.BossTotalDamage;
            _lastBossDps = state.BossDps;
        }

        _broadcaster.Emit(BridgeEvents.GameStateChanged, payload);

        if (emitHp)
        {
            _broadcaster.Emit(BridgeEvents.HealthChanged, new JsonObject
            {
                ["pct"] = state.HpPct,
                ["current"] = state.HpCurrent,
                ["max"] = state.HpMax,
            });
        }
        if (emitStamina)
        {
            _broadcaster.Emit(BridgeEvents.StaminaChanged, new JsonObject
            {
                ["pct"] = state.StaminaPct,
                ["current"] = state.StaminaCurrent,
                ["max"] = state.StaminaMax,
            });
        }
        if (emitBurst)
        {
            _broadcaster.Emit(BridgeEvents.BurstReady, new JsonObject
            {
                ["ready"] = state.BurstReady,
            });
        }
        if (emitBossHp)
        {
            _broadcaster.Emit(BridgeEvents.BossHpSnapshot, new JsonObject
            {
                ["current"] = state.BossCurrentHp,
                ["max"] = state.BossTotalHp,
                ["pct"] = state.BossHpEstPct,
                ["shield_active"] = state.BossShieldActive,
                ["shield_pct"] = state.BossShieldPct,
                ["breaking_stage"] = state.BossBreakingStage,
                ["in_overdrive"] = state.BossInOverdrive,
                ["invincible"] = state.BossInvincible,
                ["source"] = state.BossHpSource.ToString(),
            });
        }
        if (emitDps)
        {
            _broadcaster.Emit(BridgeEvents.DpsSnapshot, new JsonObject
            {
                ["total_damage"] = state.BossTotalDamage,
                ["dps"] = state.BossDps,
            });
        }

        // R8 / DPS-03: per-tick DPS overlay pump runs after every state
        // mutation that reached OnState (which includes the explicit
        // GameStateManager.PingSubscribers fired by PacketBridge after each
        // SkillEffectEvent). Skip silently when there's no live DpsTracker
        // wired — preserves legacy behaviour for the snapshot-only path.
        PumpDpsOverlay();
    }

    /// <summary>
    /// R8 / DPS-02/03/07: per-tick DPS overlay edge pump. Mirrors
    /// <c>_push_packet_overlays</c>'s DPS branch at
    /// sao_gui_state_mixin.py:246-318: poll the tracker → check enabled +
    /// has-live → emit show / fade-out / hide edges through
    /// <see cref="BridgeEvents.DpsOverlay"/>. JS panels (web/dps.html) own
    /// the visibility animation; we just drive the edges.
    /// </summary>
    public void PumpDpsOverlay()
    {
        if (_dpsTracker is null) return;
        var enabled = _dpsEnabledProvider?.Invoke() ?? true;
        var poll = _dpsTracker.PollOverlayState(DpsIdleTimeout);
        bool emitShow = false;
        bool emitFade = false;
        bool emitHide = false;
        lock (_gate)
        {
            _lastDpsPollAt = DateTimeOffset.UtcNow;
            // Edge cases (in priority order):
            // 1) enabled=false flipped to hidden — make sure JS knows
            // 2) has-live + not yet shown — emit show edge with snapshot
            // 3) should-fade-out + currently shown — emit fade edge
            // No-op when already in steady state.
            if (!enabled && _dpsOverlayShown)
            {
                _dpsOverlayShown = false;
                emitHide = true;
            }
            else if (enabled && poll.HasLive && !_dpsOverlayShown)
            {
                _dpsOverlayShown = true;
                emitShow = true;
            }
            else if (enabled && poll.ShouldFadeOut && _dpsOverlayShown)
            {
                _dpsOverlayShown = false;
                emitFade = true;
            }
            // Steady-shown: still re-emit when dirty so the DPS panel sees
            // updated totals between show/fade edges. Matches Python's
            // `_dps_should_push = dirty or (has_live and ...)` predicate.
            else if (enabled && _dpsOverlayShown && poll.Dirty)
            {
                emitShow = true; // re-use the same edge for live updates
            }
        }
        if (emitShow) EmitDpsOverlay("show", poll.Snapshot, enabled);
        else if (emitFade) EmitDpsOverlay("fade_out", poll.Snapshot, enabled);
        else if (emitHide) EmitDpsOverlay("hide", poll.Snapshot, enabled);
    }

    private void EmitDpsOverlay(string action, DpsSnapshot snap, bool enabled)
    {
        _broadcaster.Emit(BridgeEvents.DpsOverlay, new JsonObject
        {
            ["action"] = action,
            ["enabled"] = enabled,
            ["total_damage"] = snap.TotalDamage,
            ["total_damage_boss"] = snap.TotalDamageBoss,
            ["dps"] = snap.Dps,
            ["total_heal"] = snap.TotalHeal,
            ["hps"] = snap.Hps,
            ["duration_s"] = snap.DurationSeconds,
        });
    }
}
