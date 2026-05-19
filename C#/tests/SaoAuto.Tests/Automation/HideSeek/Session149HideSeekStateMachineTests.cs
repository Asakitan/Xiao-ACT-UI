using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S149 — Pin <see cref="HideSeekStateMachine"/>: sequential advance,
/// post-click cooldown blocks fallback to earlier steps, missing
/// template / missing frame returns gracefully, status callback
/// errors do not crash the tick. Uses an injected detector stub so
/// the tests pin the state-machine behaviour without depending on
/// CV details (which already have their own S147/S146 tests).
/// </summary>
public class Session149HideSeekStateMachineTests
{
    private sealed class FixedFrameProvider : IHideSeekFrameProvider
    {
        public HideSeekFrame? Next;
        public HideSeekFrame? Capture() => Next;
    }

    private sealed class RecordingInput : IHideSeekInput
    {
        public List<(int X, int Y, bool Alt)> Clicks { get; } = new();
        public void Click(int x, int y, bool alt) => Clicks.Add((x, y, alt));
    }

    private static HideSeekFrame Frame()
        => new(Pixels: new byte[12], Width: 2, Height: 2, Stride: 6, Channels: 3,
               ClientLeft: 0, ClientTop: 0);

    private static IReadOnlyDictionary<string, HideSeekTemplate> StubTemplates(IReadOnlyList<HideSeekStep> steps)
    {
        var map = new Dictionary<string, HideSeekTemplate>();
        foreach (var s in steps)
            map[s.ImageFile] = new HideSeekTemplate(s.ImageFile, new byte[] { 0 }, 1, 1);
        return map;
    }

    private static Func<HideSeekStep, HideSeekFrame, HideSeekTemplate, HideSeekDetector.DetectResult?>
        HitWhen(Func<HideSeekStep, bool> predicate)
        => (step, _, _) =>
            predicate(step)
                ? new HideSeekDetector.DetectResult(100, 200, 0.95, "stub")
                : null;

    [Fact]
    public void TickAdvancesStepOnHit()
    {
        var steps = HideSeekSteps.Default;
        var input = new RecordingInput();
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = Frame() }, input,
            detect: HitWhen(s => s.Name == "Accept"));

        int fired = sm.Tick(DateTimeOffset.UtcNow);

        Assert.Equal(0, fired);
        Assert.Single(input.Clicks);
        Assert.Equal((100, 200, true), input.Clicks[0]); // Accept uses alt-click
        Assert.Equal(1, sm.CurrentStep);
    }

    [Fact]
    public void TickReturnsMinusOneWhenFrameNull()
    {
        var steps = HideSeekSteps.Default;
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = null }, new RecordingInput(),
            detect: HitWhen(_ => true));
        Assert.Equal(-1, sm.Tick(DateTimeOffset.UtcNow));
        Assert.Equal(0, sm.CurrentStep);
    }

    [Fact]
    public void TickReturnsMinusOneWhenTemplateMissing()
    {
        var steps = HideSeekSteps.Default;
        var sm = new HideSeekStateMachine(steps,
            templates: new Dictionary<string, HideSeekTemplate>(),
            new FixedFrameProvider { Next = Frame() },
            new RecordingInput(),
            detect: HitWhen(_ => true));
        Assert.Equal(-1, sm.Tick(DateTimeOffset.UtcNow));
    }

    [Fact]
    public void NoFallbackBeforeCooldownExpires()
    {
        var steps = HideSeekSteps.Default;
        var input = new RecordingInput();
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = Frame() }, input,
            detect: HitWhen(s => s.Name == "Accept"));

        var t0 = DateTimeOffset.UtcNow;
        sm.Tick(t0); // step 0 fires, advance to 1
        Assert.Equal(1, sm.CurrentStep);

        // 1s later: detector still only matches Accept (step 0); current
        // step 1 doesn't match. Cooldown is 5s → fallback NOT allowed.
        int second = sm.Tick(t0.AddSeconds(1));
        Assert.Equal(-1, second);
        Assert.Equal(1, sm.CurrentStep);
        Assert.Single(input.Clicks);
    }

    [Fact]
    public void FallbackFiresAfterCooldown()
    {
        var steps = HideSeekSteps.Default;
        var input = new RecordingInput();
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = Frame() }, input,
            detect: HitWhen(s => s.Name == "Accept"));

        var t0 = DateTimeOffset.UtcNow;
        sm.Tick(t0); // fires step 0 → advances to 1
        Assert.Equal(1, sm.CurrentStep);

        // 6s later: step 0 still detected, cooldown expired → fallback
        // snaps cur back to 0, fires, advances to 1 again.
        int second = sm.Tick(t0.AddSeconds(6));
        Assert.Equal(0, second);
        Assert.Equal(1, sm.CurrentStep);
        Assert.Equal(2, input.Clicks.Count);
    }

    [Fact]
    public void WrapsFromLastStepBackToZero()
    {
        var steps = HideSeekSteps.Default;
        var input = new RecordingInput();
        // Detector always hits — every tick fires whichever step is current.
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = Frame() }, input,
            detect: HitWhen(_ => true));
        var t = DateTimeOffset.UtcNow;
        for (int i = 0; i < steps.Count; i++) sm.Tick(t.AddSeconds(i * 10));
        Assert.Equal(0, sm.CurrentStep); // wrapped
        Assert.Equal(steps.Count, input.Clicks.Count);
    }

    [Fact]
    public void ResetClearsStateAndCooldown()
    {
        var steps = HideSeekSteps.Default;
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = Frame() }, new RecordingInput(),
            detect: HitWhen(s => s.Name == "Accept"));
        sm.Tick(DateTimeOffset.UtcNow);
        Assert.Equal(1, sm.CurrentStep);
        sm.Reset();
        Assert.Equal(0, sm.CurrentStep);
    }

    [Fact]
    public void StatusCallbackErrorsAreSwallowed()
    {
        var steps = HideSeekSteps.Default;
        var sm = new HideSeekStateMachine(steps, StubTemplates(steps),
            new FixedFrameProvider { Next = Frame() }, new RecordingInput(),
            status: (msg, step) => throw new InvalidOperationException("boom"),
            detect: HitWhen(_ => true));
        sm.Tick(DateTimeOffset.UtcNow); // should not throw
    }

    [Fact]
    public void RejectsEmptyStepList()
    {
        Assert.Throws<ArgumentException>(() =>
            new HideSeekStateMachine(
                Array.Empty<HideSeekStep>(),
                new Dictionary<string, HideSeekTemplate>(),
                new FixedFrameProvider(),
                new RecordingInput()));
    }
}
