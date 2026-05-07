using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.State;

/// <summary>
/// Thread-safe holder for the current <see cref="GameState"/>.
/// Mirrors the contract of Python <c>game_state.GameStateManager</c>:
/// callers update via lambdas, snapshots are immutable, and subscribers
/// are notified outside the lock with the new snapshot.
/// </summary>
public sealed class GameStateManager
{
    private readonly object _gate = new();
    private readonly List<Action<GameState>> _listeners = new();
    private readonly ILogger _log;
    private readonly Func<DateTimeOffset> _clock;
    private readonly Action<Action> _dispatch;

    private GameState _state = new();

    // S91 — "last known good" non-zero trackers used by the rollback rules
    // (Python game_state.py 262/268/272/278). Zero means "no previous tracked
    // value" (matches Python's `getattr(..., default=current)` + `> 0` guard).
    private int _prevHpCurrent;
    private int _prevLevelBase;
    private int _prevLevelExtra;
    private int _prevStaminaCurrent;

    /// <param name="logger">Optional logger; falls back to NullLogger.</param>
    /// <param name="clock">Override clock (tests).</param>
    /// <param name="dispatch">
    /// Optional dispatcher used to marshal subscriber callbacks (e.g. WPF UI thread).
    /// Defaults to inline invocation. Must not be invoked while holding the manager lock.
    /// </param>
    public GameStateManager(
        ILogger? logger = null,
        Func<DateTimeOffset>? clock = null,
        Action<Action>? dispatch = null)
    {
        _log = logger ?? NullLogger.Instance;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _dispatch = dispatch ?? (action => action());
    }

    public GameState Snapshot
    {
        get
        {
            lock (_gate)
            {
                return _state;
            }
        }
    }

    /// <summary>
    /// Apply a partial mutation to the current snapshot. The mutator runs under the manager lock and
    /// must return a new immutable <see cref="GameState"/> (typically via the <c>with</c> expression).
    /// Subscribers are notified outside the lock with the freshly produced snapshot.
    /// </summary>
    public GameState Update(Func<GameState, GameState> mutator)
    {
        if (mutator is null) throw new ArgumentNullException(nameof(mutator));

        GameState snapshot;
        Action<GameState>[] subscribers;

        lock (_gate)
        {
            var next = mutator(_state) ?? throw new InvalidOperationException("Mutator returned null GameState");
            next = next with
            {
                CaptureTimestamp = _clock().ToUnixTimeMilliseconds() / 1000.0,
            };
            _state = next;
            snapshot = next;
            subscribers = _listeners.ToArray();
        }

        DispatchNotify(subscribers, snapshot);
        return snapshot;
    }

    /// <summary>Replace the snapshot wholesale. Useful when loading from a cache.</summary>
    public GameState Replace(GameState next, bool notify = true)
    {
        if (next is null) throw new ArgumentNullException(nameof(next));

        Action<GameState>[] subscribers;
        lock (_gate)
        {
            _state = next;
            // Re-seed prev trackers so the next ApplyPartial inherits the new
            // baseline rather than the pre-Replace one (cache load case).
            if (next.HpCurrent > 0) _prevHpCurrent = next.HpCurrent;
            if (next.LevelBase > 0) _prevLevelBase = next.LevelBase;
            if (next.LevelExtra > 0) _prevLevelExtra = next.LevelExtra;
            if (next.StaminaCurrent > 0) _prevStaminaCurrent = next.StaminaCurrent;
            subscribers = notify ? _listeners.ToArray() : Array.Empty<Action<GameState>>();
        }

        if (subscribers.Length > 0)
        {
            DispatchNotify(subscribers, next);
        }
        return next;
    }

    /// <summary>
    /// S91 — Typed partial mutation that composes the full Python
    /// <c>game_state.GameStateManager.update</c> rollback + cap pipeline
    /// for the HP / Level / Stamina fields:
    /// <list type="number">
    /// <item><description>Stamina rollback (uses effective stamina_max, the explicit-pct guard).</description></item>
    /// <item><description>HP / level rollbacks against the per-manager <c>_prev*</c> trackers.</description></item>
    /// <item><description>Pct clamp to [0,1] on hp_pct / stamina_pct when supplied.</description></item>
    /// <item><description>Cap hp_current ≤ hp_max and stamina_current ≤ stamina_max.</description></item>
    /// <item><description>Update the <c>_prev*</c> trackers from the *final* values.</description></item>
    /// </list>
    /// Returns the freshly produced snapshot (capture timestamp re-stamped).
    /// Subscribers fire outside the lock.
    /// </summary>
    public GameState ApplyPartial(StatePartial partial)
    {
        if (partial is null) throw new ArgumentNullException(nameof(partial));
        return ApplyPartialCore(partial, extraMutate: null);
    }

    /// <summary>
    /// S102 — <see cref="ApplyPartial(StatePartial)"/> overload that runs the
    /// rollback/clamp/cap pipeline AND applies a free-form mutation to
    /// non-tracked fields (PacketActive, PlayerName, BossHp, etc.) inside
    /// the same atomic snapshot. Lets packet writers move HP/Stamina
    /// through the validation pipeline without splitting one logical
    /// packet into two emitted snapshots (which would double-fire
    /// <c>state.changed</c> + the S101 narrow channels).
    ///
    /// The <paramref name="extraMutate"/> lambda runs AFTER the partial's
    /// fields are composed but BEFORE the cap step. Returning the input
    /// snapshot unchanged is allowed; null is not.
    /// </summary>
    public GameState ApplyPartial(StatePartial partial, Func<GameState, GameState> extraMutate)
    {
        if (partial is null) throw new ArgumentNullException(nameof(partial));
        if (extraMutate is null) throw new ArgumentNullException(nameof(extraMutate));
        return ApplyPartialCore(partial, extraMutate);
    }

    private GameState ApplyPartialCore(StatePartial partial, Func<GameState, GameState>? extraMutate)
    {
        GameState snapshot;
        Action<GameState>[] subscribers;

        lock (_gate)
        {
            var s = _state;

            // ── 1. Rollback stamina_current first because Python checks the
            //      effective stamina_max (incoming or current) + the
            //      "is stamina_pct in this batch" guard before deciding. ──
            int? staminaCurrent = partial.StaminaCurrent;
            if (staminaCurrent.HasValue)
            {
                var effStaMax = partial.StaminaMax ?? s.StaminaMax;
                staminaCurrent = GameStateValidation.RollbackStaminaCurrent(
                    incoming: staminaCurrent.Value,
                    incomingStaminaPctIsExplicit: partial.StaminaPct.HasValue,
                    incomingStaminaMax: effStaMax,
                    previousNonZero: _prevStaminaCurrent);
            }

            // ── 2. HP rollback (uses incoming pct + effective hp_max). Python
            //      reads kwargs['hp_pct'] when present, otherwise leaves it
            //      to the original snapshot's pct — but the rollback only
            //      *fires* when current==0 and pct is treated as "not zero",
            //      so the conservative read uses incoming when present and
            //      hp_max in the cap (which matches Python's flow). ──
            int? hpCurrent = partial.HpCurrent;
            if (hpCurrent.HasValue)
            {
                var effHpMax = partial.HpMax ?? s.HpMax;
                var effHpPct = partial.HpPct ?? s.HpPct;
                hpCurrent = GameStateValidation.RollbackHpCurrent(
                    incoming: hpCurrent.Value,
                    incomingPct: effHpPct,
                    hpMax: effHpMax,
                    previousNonZero: _prevHpCurrent);
            }

            // ── 3. Level rollbacks (no extra guards). ──
            int? levelBase = partial.LevelBase;
            if (levelBase.HasValue)
            {
                levelBase = GameStateValidation.RollbackLevelBase(
                    levelBase.Value, _prevLevelBase);
            }
            int? levelExtra = partial.LevelExtra;
            if (levelExtra.HasValue)
            {
                levelExtra = GameStateValidation.RollbackLevelExtra(
                    levelExtra.Value, _prevLevelExtra);
            }

            // ── 4. Pct clamp [0,1]. ──
            double? hpPct = partial.HpPct.HasValue
                ? GameStateValidation.ClampPercent(partial.HpPct.Value)
                : null;
            double? staminaPct = partial.StaminaPct.HasValue
                ? GameStateValidation.ClampPercent(partial.StaminaPct.Value)
                : null;

            // ── 5. Compose next snapshot (only the fields the partial set). ──
            var next = s with
            {
                HpCurrent = hpCurrent ?? s.HpCurrent,
                HpMax = partial.HpMax ?? s.HpMax,
                HpPct = hpPct ?? s.HpPct,
                StaminaCurrent = staminaCurrent ?? s.StaminaCurrent,
                StaminaMax = partial.StaminaMax ?? s.StaminaMax,
                StaminaPct = staminaPct ?? s.StaminaPct,
                LevelBase = levelBase ?? s.LevelBase,
                LevelExtra = levelExtra ?? s.LevelExtra,
            };

            // ── 5b. S102: apply caller's extra mutation for non-tracked fields. ──
            if (extraMutate is not null)
            {
                next = extraMutate(next)
                    ?? throw new InvalidOperationException("extraMutate must not return null");
            }

            // ── 6. Cap current ≤ max for HP / stamina. ──
            next = next with
            {
                HpCurrent = GameStateValidation.CapToMax(next.HpCurrent, next.HpMax),
                StaminaCurrent = GameStateValidation.CapStaminaToMax(next.StaminaCurrent, next.StaminaMax),
                CaptureTimestamp = _clock().ToUnixTimeMilliseconds() / 1000.0,
            };

            // ── 7. Refresh _prev* trackers from FINAL values (Python 337–344). ──
            if (next.HpCurrent > 0) _prevHpCurrent = next.HpCurrent;
            if (next.LevelBase > 0) _prevLevelBase = next.LevelBase;
            if (next.LevelExtra > 0) _prevLevelExtra = next.LevelExtra;
            if (next.StaminaCurrent > 0) _prevStaminaCurrent = next.StaminaCurrent;

            _state = next;
            snapshot = next;
            subscribers = _listeners.ToArray();
        }

        DispatchNotify(subscribers, snapshot);
        return snapshot;
    }

    public IDisposable Subscribe(Action<GameState> callback)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));
        lock (_gate)
        {
            _listeners.Add(callback);
        }
        return new Subscription(this, callback);
    }

    private void Unsubscribe(Action<GameState> callback)
    {
        lock (_gate)
        {
            _listeners.Remove(callback);
        }
    }

    private void DispatchNotify(IReadOnlyList<Action<GameState>> subscribers, GameState snapshot)
    {
        if (subscribers.Count == 0) return;
        _dispatch(() =>
        {
            foreach (var cb in subscribers)
            {
                try
                {
                    cb(snapshot);
                }
                catch (Exception ex)
                {
                    _log.LogError(ex, "[GameState] subscriber threw");
                }
            }
        });
    }

    private sealed class Subscription : IDisposable
    {
        private GameStateManager? _owner;
        private readonly Action<GameState> _callback;

        public Subscription(GameStateManager owner, Action<GameState> callback)
        {
            _owner = owner;
            _callback = callback;
        }

        public void Dispose()
        {
            var owner = Interlocked.Exchange(ref _owner, null);
            owner?.Unsubscribe(_callback);
        }
    }
}
