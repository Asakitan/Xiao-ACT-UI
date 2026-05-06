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
            subscribers = notify ? _listeners.ToArray() : Array.Empty<Action<GameState>>();
        }

        if (subscribers.Length > 0)
        {
            DispatchNotify(subscribers, next);
        }
        return next;
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
