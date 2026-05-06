namespace SaoAuto.App.Menu;

/// <summary>
/// Outside-click acceptance gate ported from Python's
/// <c>_outside_click_grace</c>: when the menu just opened, ignore
/// outside-clicks for a short grace period so the same click that
/// opened the menu doesn't immediately close it.
/// </summary>
public sealed class OutsideClickGate
{
    public const double DefaultGraceSeconds = 0.30;

    private readonly Func<DateTimeOffset> _clock;
    private DateTimeOffset _openedAt;
    private bool _armed;

    public TimeSpan Grace { get; }

    public OutsideClickGate(TimeSpan? grace = null, Func<DateTimeOffset>? clock = null)
    {
        Grace = grace ?? TimeSpan.FromSeconds(DefaultGraceSeconds);
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    public void NoteOpened()
    {
        _openedAt = _clock();
        _armed = true;
    }

    /// <summary>True when an outside click should propagate to "close menu".</summary>
    public bool ShouldClose()
    {
        if (!_armed) return false;
        var now = _clock();
        return now - _openedAt >= Grace;
    }

    public void Reset() => _armed = false;
}
