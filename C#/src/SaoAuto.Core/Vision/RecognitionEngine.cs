using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Vision;

/// <summary>
/// Recognition pipeline ported from <c>recognition.RecognitionEngine</c>:
/// pulls a frame from <see cref="IFrameCapture"/>, samples HP / STA bars,
/// emits results into <see cref="GameStateManager"/>.
///
/// Skill-slot detection lands in Session 17b (needs the canonical pixel
/// templates from <c>skill_recognition.py</c>); this skeleton covers
/// HP + STA + offline-state propagation so the headless runner can see
/// recognition-driven updates immediately.
/// </summary>
public sealed class RecognitionEngine
{
    private readonly IFrameCapture _capture;
    private readonly Func<WindowCandidate?> _windowProvider;
    private readonly GameStateManager _states;
    private readonly RecognitionConfig _config;
    private readonly ILogger _log;

    public RecognitionEngine(
        IFrameCapture capture,
        Func<WindowCandidate?> windowProvider,
        GameStateManager states,
        RecognitionConfig? config = null,
        ILogger<RecognitionEngine>? logger = null)
    {
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
        _windowProvider = windowProvider ?? throw new ArgumentNullException(nameof(windowProvider));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _config = config ?? RecognitionConfig.Default;
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    /// <summary>
    /// Run one capture+recognition pass. Returns true when a frame was
    /// processed; false when the source is offline or recognition failed.
    /// </summary>
    public bool Tick()
    {
        var window = _windowProvider();
        if (window is null)
        {
            _states.Update(s => s with
            {
                RecognitionOk = false,
                ErrorMsg = "game window not found",
            });
            return false;
        }

        var frame = _capture.Capture();
        if (frame is null)
        {
            _states.Update(s => s with
            {
                RecognitionOk = false,
                ErrorMsg = "capture returned null",
            });
            return false;
        }

        var sample = SampleBars(frame, window.Value);
        _states.Update(s => s with
        {
            RecognitionOk = true,
            ErrorMsg = string.Empty,
            HpPct = sample.HpPct ?? s.HpPct,
            StaminaPct = sample.StaminaPct ?? s.StaminaPct,
            StaminaOffline = sample.StaminaOffline,
        });
        return true;
    }

    /// <summary>Pure-math sampler exposed for tests.</summary>
    public BarSample SampleBars(CapturedFrame frame, WindowCandidate window)
    {
        var hpRoi = _config.HpBarRoi.ToPixels(window.Width, window.Height);
        var staRoi = _config.StaminaBarRoi.ToPixels(window.Width, window.Height);

        var hpRaw = StaminaBarRecognizer.SampleHorizontalFill(frame, hpRoi, _config.HpBarPredicate);
        var staRaw = StaminaBarRecognizer.SampleHorizontalFill(frame, staRoi, _config.StaminaBarPredicate);

        var hpPct = double.IsNaN(hpRaw) ? (double?)null : Math.Clamp(hpRaw, 0.0, 1.0);
        double? staPct = null;
        var staOffline = false;
        if (double.IsNaN(staRaw))
        {
            staOffline = true;
        }
        else
        {
            staPct = StaminaBarRecognizer.NormalizeStaminaPercent(staRaw);
            // STA offline detection: if the bar reads zero for several frames,
            // Python flips offline=true. For the stateless skeleton we treat
            // < 1% as offline.
            staOffline = staPct.Value < 0.01;
        }
        return new BarSample(hpPct, staPct, staOffline);
    }
}

/// <summary>Output of one bar-sample pass.</summary>
public readonly record struct BarSample(double? HpPct, double? StaminaPct, bool StaminaOffline);

/// <summary>
/// Per-engine config: ROI percentages + predicates that classify a pixel as
/// "filled". Defaults match the canonical Star Resonance HP / STA bar
/// positions; users can override via <see cref="GameWindowConfig"/> or a
/// future settings.json field.
/// </summary>
public sealed record RecognitionConfig(
    Roi HpBarRoi,
    Roi StaminaBarRoi,
    Func<byte, byte, byte, bool> HpBarPredicate,
    Func<byte, byte, byte, bool> StaminaBarPredicate)
{
    public static RecognitionConfig Default { get; } = new(
        HpBarRoi: new Roi(0.04, 0.95, 0.20, 0.012),
        StaminaBarRoi: new Roi(0.04, 0.97, 0.20, 0.008),
        HpBarPredicate: (b, g, r) => r > 80 && r > g + 15 && r > b + 15,    // red-ish
        StaminaBarPredicate: (b, g, r) => g > 80 && g > r + 10 && g > b);   // green-ish
}
