namespace SaoAuto.Overlay.Scheduler;

/// <summary>
/// Renderer registered with <see cref="OverlayScheduler"/>. Every visible
/// renderer is ticked at most once per scheduler frame; <see cref="Animates"/>
/// determines whether idle ticks can be skipped under pressure.
/// </summary>
public interface IOverlayRenderer
{
    /// <summary>Stable identifier for logging / lane assignment.</summary>
    string Id { get; }

    /// <summary>True when this renderer wants to be ticked at all.</summary>
    bool Visible { get; }

    /// <summary>
    /// True when the renderer's output changes per-frame even with no input
    /// (e.g. burst-ready beam, fisheye distort). False = only redraw when
    /// data version increments.
    /// </summary>
    bool Animates { get; }

    /// <summary>Phase offset in scheduler frames (0..59); spreads load across the 60 Hz tick.</summary>
    int PhaseOffset { get; }

    /// <summary>Marker for heavy panels that prefer their own worker lane.</summary>
    bool PreferIsolation { get; }

    /// <summary>Tick the renderer at the given monotonic time. Caller marshals any output.</summary>
    void Tick(double monotonicSeconds);
}
