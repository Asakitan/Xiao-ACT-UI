namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S149 — Sequential step driver. Mirrors Python <c>HideSeekEngine._tick</c>:
/// always probe the current step; after a post-click cooldown, probe
/// earlier steps as fallbacks (UI from previous steps may linger and
/// cause the engine to loop). On hit: dispatch the click and advance
/// the step pointer modulo the sequence length.
///
/// Decoupled from threading. Hosts (App layer) own the tick cadence,
/// the same shape we use for <see cref="AutoKeyTickHost"/>.
/// </summary>
public sealed class HideSeekStateMachine
{
    public const double StepCooldownSeconds = 5.0;

    private readonly IReadOnlyList<HideSeekStep> _steps;
    private readonly IReadOnlyDictionary<string, HideSeekTemplate> _templates;
    private readonly IHideSeekFrameProvider _frames;
    private readonly IHideSeekInput _input;
    private readonly Action<string, int>? _status;
    private readonly Func<HideSeekStep, HideSeekFrame, HideSeekTemplate, HideSeekDetector.DetectResult?> _detect;

    private readonly object _gate = new();
    private int _currentStep;
    private DateTimeOffset _lastClickAt = DateTimeOffset.MinValue;

    public HideSeekStateMachine(
        IReadOnlyList<HideSeekStep> steps,
        IReadOnlyDictionary<string, HideSeekTemplate> templates,
        IHideSeekFrameProvider frames,
        IHideSeekInput input,
        Action<string, int>? status = null,
        Func<HideSeekStep, HideSeekFrame, HideSeekTemplate, HideSeekDetector.DetectResult?>? detect = null)
    {
        _steps = steps ?? throw new ArgumentNullException(nameof(steps));
        if (_steps.Count == 0) throw new ArgumentException("steps must be non-empty.", nameof(steps));
        _templates = templates ?? throw new ArgumentNullException(nameof(templates));
        _frames = frames ?? throw new ArgumentNullException(nameof(frames));
        _input = input ?? throw new ArgumentNullException(nameof(input));
        _status = status;
        _detect = detect ?? HideSeekDetector.TryDetect;
    }

    public int CurrentStep { get { lock (_gate) return _currentStep; } }

    public void Reset()
    {
        lock (_gate)
        {
            _currentStep = 0;
            _lastClickAt = DateTimeOffset.MinValue;
        }
    }

    /// <summary>
    /// Single iteration of the detection / click / advance loop.
    /// Returns the index of the step that fired, or -1 if no hit.
    /// </summary>
    public int Tick(DateTimeOffset now)
    {
        var frame = _frames.Capture();
        int cur;
        bool fallbackAllowed;
        lock (_gate)
        {
            cur = _currentStep;
            fallbackAllowed = (now - _lastClickAt).TotalSeconds >= StepCooldownSeconds;
        }

        if (frame is null)
        {
            FireStatus("frame capture failed", cur);
            return -1;
        }

        // 1) Always probe current step.
        var hit = TryDetectStep(cur, frame);
        if (hit is { } h0)
        {
            ExecuteAndAdvance(cur, h0, now, leadingStatus: $"step {cur} ({_steps[cur].Name}) MATCH conf={h0.Confidence:F2}");
            return cur;
        }

        // 2) If we're not on step 0 and cooldown has passed, probe earlier steps.
        if (cur > 0 && fallbackAllowed)
        {
            for (int i = 0; i < cur; i++)
            {
                var fbHit = TryDetectStep(i, frame);
                if (fbHit is { } hf)
                {
                    FireStatus($"fallback: step {i} ({_steps[i].Name}) caught while on {cur}", i);
                    lock (_gate) _currentStep = i;
                    ExecuteAndAdvance(i, hf, now, leadingStatus: null);
                    return i;
                }
            }
        }

        FireStatus($"step {cur} ({_steps[cur].Name}): waiting", cur);
        return -1;
    }

    private HideSeekDetector.DetectResult? TryDetectStep(int idx, HideSeekFrame frame)
    {
        var step = _steps[idx];
        if (!_templates.TryGetValue(step.ImageFile, out var tpl)) return null;
        return _detect(step, frame, tpl);
    }

    private void ExecuteAndAdvance(int idx, HideSeekDetector.DetectResult hit, DateTimeOffset now, string? leadingStatus)
    {
        var step = _steps[idx];
        if (leadingStatus is not null) FireStatus(leadingStatus, idx);
        _input.Click(hit.ClickX, hit.ClickY, step.AltClick);
        lock (_gate)
        {
            _lastClickAt = now;
            _currentStep = (idx + 1) % _steps.Count;
        }
    }

    private void FireStatus(string message, int step)
    {
        try { _status?.Invoke(message, step); } catch { /* never let UI callback break the tick */ }
    }
}
