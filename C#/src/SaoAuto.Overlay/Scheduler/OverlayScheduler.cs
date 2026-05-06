namespace SaoAuto.Overlay.Scheduler;

/// <summary>
/// Shared 60 Hz scheduler ported from <c>overlay_scheduler.py</c>. Holds a
/// list of <see cref="IOverlayRenderer"/>s and ticks each one whose
/// <see cref="IOverlayRenderer.PhaseOffset"/> aligns with the current frame
/// counter. Idle (non-animating) renderers can be skipped when
/// <see cref="Pressure"/> is non-zero — mirrors Python's
/// "downsample under heavy combat" rule.
/// </summary>
public sealed class OverlayScheduler
{
    public const int TargetHz = 60;
    public const int FrameModulus = TargetHz; // phase offset wraps every second

    private readonly Dictionary<string, IOverlayRenderer> _renderers = new(StringComparer.Ordinal);
    private readonly object _gate = new();
    private long _frameCounter;
    private int _pressure;

    public IReadOnlyDictionary<string, IOverlayRenderer> Renderers
    {
        get { lock (_gate) { return new Dictionary<string, IOverlayRenderer>(_renderers); } }
    }

    public long FrameCounter
    {
        get { lock (_gate) { return _frameCounter; } }
    }

    /// <summary>0..3. Higher values skip more idle renderers per tick.</summary>
    public int Pressure
    {
        get { lock (_gate) { return _pressure; } }
        set { lock (_gate) { _pressure = Math.Clamp(value, 0, 3); } }
    }

    public void Register(IOverlayRenderer renderer)
    {
        if (renderer is null) throw new ArgumentNullException(nameof(renderer));
        lock (_gate) { _renderers[renderer.Id] = renderer; }
    }

    public bool Unregister(string id)
    {
        lock (_gate) { return _renderers.Remove(id); }
    }

    /// <summary>
    /// Tick the scheduler with the current monotonic time.
    /// Returns the number of renderers that ran.
    /// </summary>
    public int Tick(double monotonicSeconds)
    {
        IOverlayRenderer[] snapshot;
        long frame;
        int pressure;
        lock (_gate)
        {
            frame = ++_frameCounter;
            pressure = _pressure;
            snapshot = _renderers.Values.ToArray();
        }

        var ran = 0;
        foreach (var r in snapshot)
        {
            if (!r.Visible) continue;
            if (!ShouldRunThisFrame(frame, r, pressure)) continue;
            try
            {
                r.Tick(monotonicSeconds);
                ran++;
            }
            catch
            {
                // Swallow — a misbehaving renderer must not poison the loop.
            }
        }
        return ran;
    }

    /// <summary>
    /// Decide whether a renderer should run on the given frame. Animating
    /// renderers always run when their phase aligns; non-animating renderers
    /// honor pressure-based skips.
    /// </summary>
    public static bool ShouldRunThisFrame(long frame, IOverlayRenderer renderer, int pressure)
    {
        var phase = renderer.PhaseOffset % FrameModulus;
        if (phase < 0) phase += FrameModulus;

        if (renderer.Animates)
        {
            // Animating renderers can run every frame; phase is just a hint to
            // spread the visual update across the second. Honor it as a 1-of-N
            // selector when the phase is non-zero, otherwise run every frame.
            return phase == 0 || frame % FrameModulus == phase;
        }

        // Non-animating: run only on phase-aligned frames. Pressure increases
        // the modulus so renderers tick less often under heavy load.
        var period = pressure switch
        {
            0 => FrameModulus,         // ~1 Hz baseline for static panels
            1 => FrameModulus * 2,     // ~0.5 Hz
            2 => FrameModulus * 4,     // ~0.25 Hz
            _ => FrameModulus * 8,     // ~0.125 Hz
        };
        return frame % period == phase;
    }
}
