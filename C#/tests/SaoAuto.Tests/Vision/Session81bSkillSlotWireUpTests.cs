using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S81b — Verifies <see cref="RecognitionTickEngine"/> wires
/// <see cref="SkillSlotTracker"/> + <see cref="SkillSlotRoiTable"/>
/// into <see cref="RecognitionTickResult.SkillSlots"/>.
/// </summary>
public class Session81bSkillSlotWireUpTests
{
    private sealed class StubCapture : IFrameCapture
    {
        public Queue<CapturedFrame?> Frames { get; } = new();
        public CapturedFrame? Capture() => Frames.Count > 0 ? Frames.Dequeue() : null;
        public void Dispose() { }
    }

    private static WindowCandidate MakeWindow(int w, int h) =>
        new(IntPtr.Zero, "stub", "stub.exe", 0, 0, w, h);

    private static CapturedFrame MakeFlatFrame(int width, int height, byte b, byte g, byte r)
    {
        var stride = width * 4;
        var px = new byte[stride * height];
        for (var i = 0; i < px.Length; i += 4)
        {
            px[i] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = 255;
        }
        return new CapturedFrame(width, height, stride, px);
    }

    [Fact]
    public void Tick_NoTracker_EmptySkillSlots()
    {
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeFlatFrame(1920, 1080, 53, 174, 255));
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(1920, 1080),
            new Roi(0, 0, 1, 1), clock: () => 100.0);
        var r = engine.Tick();
        Assert.True(r.RecognitionOk);
        Assert.NotNull(r.SkillSlots);
        Assert.Empty(r.SkillSlots);
    }

    [Fact]
    public void Tick_WithTracker_NineSlotResults()
    {
        var cap = new StubCapture();
        cap.Frames.Enqueue(MakeFlatFrame(1920, 1080, 80, 80, 80));
        var tracker = new SkillSlotTracker();
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(1920, 1080),
            new Roi(0, 0, 1, 1),
            skillTracker: tracker,
            clock: () => 100.0);
        var r = engine.Tick();
        Assert.True(r.RecognitionOk);
        Assert.Equal(9, r.SkillSlots.Count);
        for (var i = 0; i < 9; i++)
            Assert.Equal(i + 1, r.SkillSlots[i].Index);
    }

    [Fact]
    public void Tick_NoWindow_EmptySkillSlots()
    {
        var engine = new RecognitionTickEngine(
            new StubCapture(), () => null, new Roi(0, 0, 1, 1),
            skillTracker: new SkillSlotTracker(), clock: () => 100.0);
        var r = engine.Tick();
        Assert.False(r.RecognitionOk);
        Assert.Empty(r.SkillSlots);
    }

    [Fact]
    public void Tick_CaptureFail_EmptySkillSlots()
    {
        var engine = new RecognitionTickEngine(
            new StubCapture(), () => MakeWindow(1920, 1080),
            new Roi(0, 0, 1, 1),
            skillTracker: new SkillSlotTracker(), clock: () => 100.0);
        var r = engine.Tick();
        Assert.False(r.RecognitionOk);
        Assert.Empty(r.SkillSlots);
    }
}
