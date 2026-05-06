using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// One sample from <see cref="TcpProbeSource"/> — the C# analog of
/// <c>mem_probe.tcp_source.TcpSnapshot</c>. Carries the player-identity
/// fields a value-anchor scan needs as ground truth.
/// </summary>
public readonly record struct TcpSnapshot(
    double Timestamp,
    long Hp,
    long MaxHp,
    int LevelBase,
    int LevelExtra,
    int FightPoint,
    int ProfessionId,
    bool InCombat,
    bool PacketActive);

/// <summary>
/// Background-sampled TCP probe. Owns a <see cref="GameStateManager"/> +
/// <see cref="PacketBridge"/>, surfaces the latest snapshot, and keeps a
/// short sliding history so memory-side observations can be correlated
/// with recent TCP values (mirrors <c>tcp_source.TcpSnapshotSource</c>).
///
/// The Python implementation runs a background thread polling at 50ms.
/// The C# version subscribes to <see cref="GameStateManager"/> directly so
/// every state mutation produces a sample — the registry → bridge already
/// runs on the parser thread, so we get equivalent freshness without an
/// extra thread.
/// </summary>
public sealed class TcpProbeSource : IDisposable
{
    /// <summary>Default rolling-window length (matches Python).</summary>
    public static readonly TimeSpan DefaultWindow = TimeSpan.FromMilliseconds(300);

    private readonly GameStateManager _state;
    private readonly PacketBridge _bridge;
    private readonly bool _ownsBridge;
    private readonly TimeSpan _historyCapacity;
    private readonly object _gate = new();
    private readonly LinkedList<TcpSnapshot> _history = new();
    private readonly HashSet<long> _seenHp = new();
    private readonly HashSet<long> _seenMaxHp = new();
    private TcpSnapshot? _latest;
    private IDisposable? _subscription;
    private readonly Func<DateTimeOffset> _clock;
    private bool _disposed;

    public TcpProbeSource(
        GameStateManager state,
        PacketBridge bridge,
        TimeSpan? historyCapacity = null,
        Func<DateTimeOffset>? clock = null,
        bool ownsBridge = false)
    {
        _state = state ?? throw new ArgumentNullException(nameof(state));
        _bridge = bridge ?? throw new ArgumentNullException(nameof(bridge));
        _historyCapacity = historyCapacity ?? TimeSpan.FromSeconds(1.0);
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _ownsBridge = ownsBridge;
    }

    /// <summary>Most recent snapshot observed, or null until one arrives.</summary>
    public TcpSnapshot? Latest
    {
        get { lock (_gate) return _latest; }
    }

    /// <summary>
    /// Set of every distinct HP value ever observed (cleared by <see cref="Reset"/>).
    /// Used by SmartLocator-style code that needs a memory-vs-TCP cross-check
    /// against the very first reported value, which the rolling history
    /// would already have evicted.
    /// </summary>
    public IReadOnlyCollection<long> SeenHp
    {
        get { lock (_gate) return _seenHp.ToArray(); }
    }

    public IReadOnlyCollection<long> SeenMaxHp
    {
        get { lock (_gate) return _seenMaxHp.ToArray(); }
    }

    /// <summary>
    /// Begin sampling. Subscribes to the underlying state manager; every
    /// state mutation produces a fresh snapshot in the rolling history.
    /// </summary>
    public void Start()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(TcpProbeSource));
        _subscription ??= _state.Subscribe(OnStateChanged);
    }

    public void Stop()
    {
        _subscription?.Dispose();
        _subscription = null;
    }

    public void Reset()
    {
        lock (_gate)
        {
            _latest = null;
            _history.Clear();
            _seenHp.Clear();
            _seenMaxHp.Clear();
        }
    }

    /// <summary>
    /// Returns true when the supplied HP value matches any TCP-observed HP
    /// within <paramref name="window"/> of now. Mirrors
    /// <c>tcp_source.hp_in_window</c>.
    /// </summary>
    public bool HpInWindow(long hp, TimeSpan? window = null)
    {
        var w = (window ?? DefaultWindow).TotalSeconds;
        var nowSec = _clock().ToUnixTimeMilliseconds() / 1000.0;
        lock (_gate)
        {
            foreach (var sample in _history)
            {
                if (nowSec - sample.Timestamp > w) continue;
                if (sample.Hp == hp) return true;
            }
        }
        return false;
    }

    private void OnStateChanged(GameState s)
    {
        var snap = new TcpSnapshot(
            Timestamp: _clock().ToUnixTimeMilliseconds() / 1000.0,
            Hp: s.HpCurrent,
            MaxHp: s.HpMax,
            LevelBase: s.LevelBase,
            LevelExtra: s.LevelExtra,
            FightPoint: s.FightPoint,
            ProfessionId: s.ProfessionId,
            InCombat: s.InCombat,
            PacketActive: s.PacketActive);

        lock (_gate)
        {
            _latest = snap;
            _history.AddLast(snap);
            if (snap.Hp > 0) _seenHp.Add(snap.Hp);
            if (snap.MaxHp > 0) _seenMaxHp.Add(snap.MaxHp);
            // Evict samples older than the history capacity.
            var cutoff = snap.Timestamp - _historyCapacity.TotalSeconds;
            while (_history.First is { } first && first.Value.Timestamp < cutoff)
            {
                _history.RemoveFirst();
            }
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Stop();
        if (_ownsBridge) _bridge.Dispose();
    }
}
