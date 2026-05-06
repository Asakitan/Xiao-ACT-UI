namespace SaoAuto.Core.Automation;

/// <summary>
/// Concurrency guard for SAO sound playback ported from <c>sao_sound</c>.
/// Mirrors the fix for the v2.x crash where rapid recognition callbacks
/// spawned unsafe sound threads — each clip is allowed at most one
/// concurrent play, with rapid repeats coalesced behind a min-interval gate.
/// </summary>
public sealed class SoundConcurrencyGuard
{
    public TimeSpan MinInterval { get; }

    private readonly Dictionary<string, DateTimeOffset> _lastPlay = new();
    private readonly HashSet<string> _inFlight = new();
    private readonly Func<DateTimeOffset> _clock;
    private readonly object _gate = new();

    public SoundConcurrencyGuard(TimeSpan? minInterval = null, Func<DateTimeOffset>? clock = null)
    {
        MinInterval = minInterval ?? TimeSpan.FromMilliseconds(80);
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    /// <summary>
    /// Try to begin playback for <paramref name="clipId"/>. Returns false when:
    /// (a) the same clip is already playing, or
    /// (b) the same clip last started within <see cref="MinInterval"/>.
    /// Caller must invoke <see cref="EndPlayback"/> when the clip finishes.
    /// </summary>
    public bool TryBeginPlayback(string clipId)
    {
        var now = _clock();
        lock (_gate)
        {
            if (_inFlight.Contains(clipId)) return false;
            if (_lastPlay.TryGetValue(clipId, out var last) &&
                (now - last) < MinInterval)
            {
                return false;
            }
            _inFlight.Add(clipId);
            _lastPlay[clipId] = now;
            return true;
        }
    }

    public void EndPlayback(string clipId)
    {
        lock (_gate) _inFlight.Remove(clipId);
    }

    public bool IsPlaying(string clipId)
    {
        lock (_gate) return _inFlight.Contains(clipId);
    }
}
