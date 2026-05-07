using SaoAuto.Core.State;

namespace SaoAuto.Core.Vision;

/// <summary>
/// S81 — Composition of the recognition tick loop. Bit-faithful port of
/// <c>RecognitionEngine._tick</c> (recognition.py 946–1093) wiring:
/// <see cref="IFrameCapture"/> + window provider +
/// <see cref="StaminaColorDetector"/> + <see cref="StaminaPctFilter"/> +
/// <see cref="StaminaOfflineGate"/> + <see cref="AdaptiveFpsSelector"/> into
/// a single per-tick unit. Skill-slot wiring waits for S82 (ROI table).
///
/// Per-tick:
/// 1. Resolve window via provider; emit error projection if missing.
/// 2. Capture frame; backoff after 5 consecutive failures (every 20 ticks),
///    fall back to last-good frame within <c>frameCacheTtlSeconds</c>.
/// 3. Crop the stamina ROI from BGRA32 to BGR24 and run the color detector.
/// 4. Push result through the filter (drop-lock + large-jump confirm).
/// 5. Push confidence through the offline gate (debounced + warm-up).
/// 6. Compute next-tick FPS via <see cref="AdaptiveFpsSelector"/>.
///
/// Pure single-thread by contract (mirrors Python's vision thread).
/// </summary>
public sealed class RecognitionTickEngine
{
    private readonly IFrameCapture _capture;
    private readonly Func<WindowCandidate?> _windowProvider;
    private readonly Roi _staminaRoi;
    private readonly StaminaPctFilter _filter;
    private readonly StaminaOfflineGate _gate;
    private readonly AdaptiveFpsSelector _fps;
    private readonly SkillSlotTracker? _skillTracker;
    private readonly Func<double> _clock;
    private readonly double _frameCacheTtlSeconds;
    private readonly int _captureBackoffThreshold;
    private readonly int _captureBackoffEveryN;
    private readonly double _warmupSeconds;

    private double _startedAt;
    private bool _started;
    private CapturedFrame? _lastGoodFrame;
    private double _lastGoodFrameAt;
    private int _captureFailCount;

    public RecognitionTickEngine(
        IFrameCapture capture,
        Func<WindowCandidate?> windowProvider,
        Roi staminaRoi,
        StaminaPctFilter? filter = null,
        StaminaOfflineGate? gate = null,
        AdaptiveFpsSelector? fps = null,
        SkillSlotTracker? skillTracker = null,
        Func<double>? clock = null,
        double frameCacheTtlSeconds = 0.5,
        int captureBackoffThreshold = 5,
        int captureBackoffEveryN = 20,
        double warmupSeconds = 0.0)
    {
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
        _windowProvider = windowProvider ?? throw new ArgumentNullException(nameof(windowProvider));
        _staminaRoi = staminaRoi;
        _filter = filter ?? new StaminaPctFilter();
        _gate = gate ?? new StaminaOfflineGate();
        _fps = fps ?? new AdaptiveFpsSelector();
        _skillTracker = skillTracker;
        _clock = clock ?? DefaultClock;
        _frameCacheTtlSeconds = frameCacheTtlSeconds;
        _captureBackoffThreshold = captureBackoffThreshold;
        _captureBackoffEveryN = captureBackoffEveryN;
        _warmupSeconds = warmupSeconds;
    }

    public RecognitionTickResult Tick()
    {
        var now = _clock();
        if (!_started) { _startedAt = now; _started = true; }
        var warmupUntil = _startedAt + _warmupSeconds;

        var window = _windowProvider();
        if (window is null)
        {
            return new RecognitionTickResult(
                RecognitionOk: false,
                ErrorMsg: "game window not found",
                Window: null,
                StaminaPct: _filter.FilteredPct,
                StaminaOffline: _gate.IsOffline,
                StaminaJustWentOffline: false,
                StaminaJustRecovered: false,
                NextFps: _fps.Current(_filter.FilteredPct, now),
                FrameSource: FrameSource.None,
                SkillSlots: Array.Empty<SkillSlotResult>());
        }

        var w = window.Value;

        // Backoff: after N consecutive failures, only retry every M ticks.
        if (_captureFailCount >= _captureBackoffThreshold)
        {
            _captureFailCount++;
            if (_captureFailCount % _captureBackoffEveryN != 0)
            {
                return new RecognitionTickResult(
                    RecognitionOk: true,
                    ErrorMsg: string.Empty,
                    Window: w,
                    StaminaPct: _filter.FilteredPct,
                    StaminaOffline: _gate.IsOffline,
                    StaminaJustWentOffline: false,
                    StaminaJustRecovered: false,
                    NextFps: _fps.Current(_filter.FilteredPct, now),
                    FrameSource: FrameSource.SkippedBackoff,
                    SkillSlots: Array.Empty<SkillSlotResult>());
            }
        }

        var frame = _capture.Capture();
        var source = FrameSource.Live;
        if (frame is null)
        {
            // Try cached frame within TTL — silent bridge across transient gaps.
            if (_lastGoodFrame is not null && (now - _lastGoodFrameAt) < _frameCacheTtlSeconds)
            {
                frame = _lastGoodFrame;
                source = FrameSource.Cached;
            }
            else
            {
                _captureFailCount++;
                return new RecognitionTickResult(
                    RecognitionOk: false,
                    ErrorMsg: "vision capture failed",
                    Window: w,
                    StaminaPct: _filter.FilteredPct,
                    StaminaOffline: _gate.IsOffline,
                    StaminaJustWentOffline: false,
                    StaminaJustRecovered: false,
                    NextFps: _fps.Current(_filter.FilteredPct, now),
                    FrameSource: FrameSource.None,
                    SkillSlots: Array.Empty<SkillSlotResult>());
            }
        }
        else
        {
            _lastGoodFrame = frame;
            _lastGoodFrameAt = now;
            _captureFailCount = 0;
        }

        var roi = _staminaRoi.ToPixels(w.Width, w.Height);
        var bgr = CropBgr24(frame, roi);
        if (bgr is null)
        {
            return new RecognitionTickResult(
                RecognitionOk: true,
                ErrorMsg: string.Empty,
                Window: w,
                StaminaPct: _filter.FilteredPct,
                StaminaOffline: _gate.IsOffline,
                StaminaJustWentOffline: false,
                StaminaJustRecovered: false,
                NextFps: _fps.Current(_filter.FilteredPct, now),
                FrameSource: source,
                SkillSlots: Array.Empty<SkillSlotResult>());
        }

        var det = StaminaColorDetector.Detect(bgr, roi.W, roi.H);
        var gateDecision = _gate.Push(det.Confidence, now, warmupUntil);

        double? staPct = _filter.FilteredPct;
        if (!gateDecision.Offline)
        {
            staPct = _filter.Push(det.Pct, det.Confidence, now);
        }

        var nextFps = _fps.Current(_filter.FilteredPct, now);
        var skillResults = ProcessSkillSlots(frame, w);
        return new RecognitionTickResult(
            RecognitionOk: true,
            ErrorMsg: string.Empty,
            Window: w,
            StaminaPct: staPct,
            StaminaOffline: gateDecision.Offline,
            StaminaJustWentOffline: gateDecision.JustWentOffline,
            StaminaJustRecovered: gateDecision.JustRecovered,
            NextFps: nextFps,
            FrameSource: source,
            SkillSlots: skillResults);
    }

    private IReadOnlyList<SkillSlotResult> ProcessSkillSlots(CapturedFrame frame, WindowCandidate w)
    {
        if (_skillTracker is null) return Array.Empty<SkillSlotResult>();
        var clientRects = SkillSlotRoiTable.GetSkillSlotClientRects(w.Width, w.Height);
        if (clientRects.Count == 0) return Array.Empty<SkillSlotResult>();
        var results = new List<SkillSlotResult>(clientRects.Count);
        foreach (var slot in clientRects)
        {
            var slotRoi = new RectI(slot.X, slot.Y, slot.W, slot.H);
            var bgr = CropBgr24(frame, slotRoi);
            var (cw, ch) = ClampedSize(frame, slotRoi);
            results.Add(_skillTracker.Process(slot.Index, bgr, cw, ch));
        }
        return results;
    }

    private static (int W, int H) ClampedSize(CapturedFrame frame, RectI roi)
    {
        var x0 = Math.Max(0, roi.X);
        var y0 = Math.Max(0, roi.Y);
        var x1 = Math.Min(frame.Width, roi.X + roi.W);
        var y1 = Math.Min(frame.Height, roi.Y + roi.H);
        if (x1 <= x0 || y1 <= y0) return (0, 0);
        return (x1 - x0, y1 - y0);
    }

    public void Reset()
    {
        _filter.Reset();
        _gate.Reset();
        _fps.Reset();
        _lastGoodFrame = null;
        _lastGoodFrameAt = 0.0;
        _captureFailCount = 0;
        _started = false;
        _startedAt = 0.0;
    }

    /// <summary>
    /// Crop a BGRA32 frame ROI down to a packed BGR24 buffer suitable for
    /// the color detectors. Returns null when the ROI lies fully outside
    /// the frame or has zero area.
    /// </summary>
    private static byte[]? CropBgr24(CapturedFrame frame, RectI roi)
    {
        if (roi.W <= 0 || roi.H <= 0) return null;
        var x0 = Math.Max(0, roi.X);
        var y0 = Math.Max(0, roi.Y);
        var x1 = Math.Min(frame.Width, roi.X + roi.W);
        var y1 = Math.Min(frame.Height, roi.Y + roi.H);
        if (x1 <= x0 || y1 <= y0) return null;
        var w = x1 - x0;
        var h = y1 - y0;
        var dst = new byte[w * h * 3];
        for (var y = 0; y < h; y++)
        {
            var srcOff = (y0 + y) * frame.Stride + x0 * 4;
            var dstOff = y * w * 3;
            for (var x = 0; x < w; x++)
            {
                dst[dstOff + x * 3] = frame.Pixels[srcOff + x * 4];
                dst[dstOff + x * 3 + 1] = frame.Pixels[srcOff + x * 4 + 1];
                dst[dstOff + x * 3 + 2] = frame.Pixels[srcOff + x * 4 + 2];
            }
        }
        return dst;
    }

    private static double DefaultClock() =>
        DateTime.UtcNow.Subtract(DateTime.UnixEpoch).TotalSeconds;
}

public enum FrameSource { None, Live, Cached, SkippedBackoff }

public readonly record struct RecognitionTickResult(
    bool RecognitionOk,
    string ErrorMsg,
    WindowCandidate? Window,
    double? StaminaPct,
    bool StaminaOffline,
    bool StaminaJustWentOffline,
    bool StaminaJustRecovered,
    double NextFps,
    FrameSource FrameSource,
    IReadOnlyList<SkillSlotResult> SkillSlots);
