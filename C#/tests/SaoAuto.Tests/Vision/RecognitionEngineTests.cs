using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

public class RecognitionEngineTests
{
    [Fact]
    public void TickWithoutWindowMarksRecognitionFailed()
    {
        var states = new GameStateManager();
        var engine = new RecognitionEngine(
            capture: new FixtureFrameCapture(Array.Empty<CapturedFrame>()),
            windowProvider: () => null,
            states: states);

        Assert.False(engine.Tick());
        Assert.False(states.Snapshot.RecognitionOk);
        Assert.Equal("game window not found", states.Snapshot.ErrorMsg);
    }

    [Fact]
    public void TickWithWindowButNoCaptureMarksFailed()
    {
        var states = new GameStateManager();
        var engine = new RecognitionEngine(
            capture: new FixtureFrameCapture(Array.Empty<CapturedFrame>()),
            windowProvider: () => DefaultWindow(),
            states: states);

        Assert.False(engine.Tick());
        Assert.False(states.Snapshot.RecognitionOk);
        Assert.Contains("capture", states.Snapshot.ErrorMsg);
    }

    [Fact]
    public void TickWithFullHpBarReadsOnePct()
    {
        var window = DefaultWindow();
        var frame = BuildFrameAllRed(window.Width, window.Height);
        var states = new GameStateManager();
        var engine = new RecognitionEngine(
            capture: FixtureFrameCapture.Single(frame),
            windowProvider: () => window,
            states: states);

        Assert.True(engine.Tick());
        Assert.True(states.Snapshot.RecognitionOk);
        Assert.Equal(1.0, states.Snapshot.HpPct, 4);
    }

    [Fact]
    public void TickWithEmptyHpBarReadsZeroPct()
    {
        var window = DefaultWindow();
        var frame = BuildFrameSolid(window.Width, window.Height, b: 0, g: 0, r: 0);
        var states = new GameStateManager
            ();
        // Pre-set snapshot HP > 0 so we can confirm the recognition writer changes it.
        states.Update(s => s with { HpPct = 0.5 });

        var engine = new RecognitionEngine(
            capture: FixtureFrameCapture.Single(frame),
            windowProvider: () => window,
            states: states);

        Assert.True(engine.Tick());
        // Black frame fails the red predicate → HpPct=0.
        Assert.Equal(0.0, states.Snapshot.HpPct);
    }

    [Fact]
    public void StaminaOfflineSetWhenBarReadsZero()
    {
        var window = DefaultWindow();
        var frame = BuildFrameSolid(window.Width, window.Height, b: 0, g: 0, r: 0);
        var states = new GameStateManager();
        var engine = new RecognitionEngine(
            capture: FixtureFrameCapture.Single(frame),
            windowProvider: () => window,
            states: states);

        engine.Tick();
        Assert.True(states.Snapshot.StaminaOffline);
    }

    private static WindowCandidate DefaultWindow() =>
        new(IntPtr.Zero, "Test", null, 0, 0, 1920, 1080);

    private static CapturedFrame BuildFrameSolid(int width, int height, byte b, byte g, byte r, byte a = 255)
    {
        var stride = width * 4;
        var pixels = new byte[stride * height];
        for (var i = 0; i < pixels.Length; i += 4)
        {
            pixels[i] = b;
            pixels[i + 1] = g;
            pixels[i + 2] = r;
            pixels[i + 3] = a;
        }
        return new CapturedFrame(width, height, stride, pixels);
    }

    private static CapturedFrame BuildFrameAllRed(int width, int height) =>
        BuildFrameSolid(width, height, b: 0, g: 0, r: 200);
}
