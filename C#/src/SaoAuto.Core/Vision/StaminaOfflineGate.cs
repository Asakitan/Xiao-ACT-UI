namespace SaoAuto.Core.Vision;

/// <summary>
/// S80 — Stamina-bar offline/online debounce gate. Bit-faithful port
/// of the OFFLINE-tracking-with-hysteresis block in
/// <c>RecognitionEngine._tick</c> (recognition.py lines 1029–1074).
///
/// Goes OFFLINE after 0.10 s of sustained <c>confidence &lt; 0.12</c>;
/// goes back ONLINE after 0.10 s of sustained good confidence (prevents
/// rapid OFFLINE/ONLINE oscillation when the bar flickers).
///
/// Suppresses going-offline transitions during a warm-up window
/// (PrintWindow returns blank frames for the first few seconds after
/// process start). Pure scalar + injected clock seam — no thread state.
/// </summary>
public sealed class StaminaOfflineGate
{
    public const double LowConfidence = 0.12;
    public const double DebounceSeconds = 0.10;

    public bool IsOffline { get; private set; }
    public double OfflineSince { get; private set; }
    public double OnlineSince { get; private set; }

    /// <summary>
    /// Feed one detector reading. Returns (offline, justWentOffline,
    /// justRecovered) — caller emits log lines + state mutations on
    /// the transition flags.
    /// </summary>
    public OfflineDecision Push(double confidence, double nowSeconds, double warmupUntil)
    {
        double offlineElapsed;
        if (confidence < LowConfidence)
        {
            OnlineSince = 0.0;
            if (OfflineSince == 0.0) OfflineSince = nowSeconds;
            offlineElapsed = nowSeconds - OfflineSince;
        }
        else
        {
            OfflineSince = 0.0;
            offlineElapsed = 0.0;
            if (IsOffline)
            {
                if (OnlineSince == 0.0) OnlineSince = nowSeconds;
            }
            else OnlineSince = 0.0;
        }

        var justWentOffline = false;
        var justRecovered = false;

        if (offlineElapsed >= DebounceSeconds)
        {
            if (nowSeconds < warmupUntil)
            {
                // Suppress offline during warm-up — caller still treats as online.
                return new OfflineDecision(IsOffline, false, false);
            }
            if (!IsOffline)
            {
                IsOffline = true;
                justWentOffline = true;
            }
            return new OfflineDecision(true, justWentOffline, false);
        }

        if (IsOffline)
        {
            var onlineElapsed = OnlineSince > 0 ? (nowSeconds - OnlineSince) : 0.0;
            if (onlineElapsed >= DebounceSeconds)
            {
                IsOffline = false;
                OnlineSince = 0.0;
                justRecovered = true;
                return new OfflineDecision(false, false, true);
            }
            return new OfflineDecision(true, false, false);
        }

        return new OfflineDecision(false, false, false);
    }

    public void Reset()
    {
        IsOffline = false;
        OfflineSince = 0.0;
        OnlineSince = 0.0;
    }
}

public readonly record struct OfflineDecision(
    bool Offline,
    bool JustWentOffline,
    bool JustRecovered);

/// <summary>
/// S80 — Adaptive FPS selector. Bit-faithful port of
/// <c>RecognitionEngine._current_fps</c> (recognition.py lines 925–944).
///
/// Drops from <c>activeFps</c> to <c>idleFps</c> after the stamina
/// reading hasn't changed for <paramref name="idleAfterSeconds"/> s.
/// Any change immediately resets to <c>activeFps</c> (zero-latency
/// ramp-up).
/// </summary>
public sealed class AdaptiveFpsSelector
{
    private readonly double _activeFps;
    private readonly double _idleFps;
    private readonly double _idleAfterSeconds;
    private readonly bool _enabled;
    private double? _lastValue;
    private double _lastChangeAt;

    public AdaptiveFpsSelector(
        double activeFps = 10.0,
        double idleFps = 4.0,
        double idleAfterSeconds = 30.0,
        bool enabled = true)
    {
        _activeFps = activeFps;
        _idleFps = idleFps;
        _idleAfterSeconds = idleAfterSeconds;
        _enabled = enabled;
    }

    public double Current(double? filteredStaminaPct, double nowSeconds)
    {
        if (!_enabled) return _activeFps;
        if (filteredStaminaPct is null) return _activeFps;
        if (filteredStaminaPct != _lastValue)
        {
            _lastValue = filteredStaminaPct;
            _lastChangeAt = nowSeconds;
            return _activeFps;
        }
        if (nowSeconds - _lastChangeAt >= _idleAfterSeconds) return _idleFps;
        return _activeFps;
    }

    public void Reset()
    {
        _lastValue = null;
        _lastChangeAt = 0.0;
    }
}
