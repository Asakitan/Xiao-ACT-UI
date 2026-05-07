using System.Text.Json.Nodes;
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
/// Lifecycle: <see cref="Start"/> hooks the subscription and (by
/// default) emits an initial snapshot so the JS side has a value to
/// render before the first state mutation. <see cref="Dispose"/>
/// drops the subscription. Both are idempotent.
/// </summary>
public sealed class GameStatePublisher : IDisposable
{
    private const string CaptureTsKey = "capture_ts";

    private readonly GameStateManager _states;
    private readonly BridgeEventBroadcaster _broadcaster;
    private readonly object _gate = new();
    private IDisposable? _sub;
    private string? _lastSig;
    private bool _disposed;

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

    public GameStatePublisher(GameStateManager states, BridgeEventBroadcaster broadcaster)
    {
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _broadcaster = broadcaster ?? throw new ArgumentNullException(nameof(broadcaster));
    }

    public bool IsActive => _sub is not null;

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
        var payload = StateSnapshotPayload.ToDict(state);
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
    }
}
