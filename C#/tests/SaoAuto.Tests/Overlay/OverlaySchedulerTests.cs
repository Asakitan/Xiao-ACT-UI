using SaoAuto.Overlay.Scheduler;

namespace SaoAuto.Tests.Overlay;

public class OverlaySchedulerTests
{
    [Fact]
    public void RegisteredVisibleRendererIsTickedEachAlignedFrame()
    {
        var scheduler = new OverlayScheduler();
        var renderer = new FakeRenderer { Id = "hp", Visible = true, Animates = true, PhaseOffset = 0 };
        scheduler.Register(renderer);

        for (var i = 0; i < 60; i++) scheduler.Tick(i / 60.0);

        // Animating + phase 0 → ticks every frame.
        Assert.Equal(60, renderer.TickCount);
    }

    [Fact]
    public void InvisibleRendererIsSkipped()
    {
        var scheduler = new OverlayScheduler();
        var renderer = new FakeRenderer { Id = "hp", Visible = false, Animates = true };
        scheduler.Register(renderer);
        scheduler.Tick(0);
        Assert.Equal(0, renderer.TickCount);
    }

    [Fact]
    public void NonAnimatingRendererTicksOncePerSecondByDefault()
    {
        var scheduler = new OverlayScheduler();
        var renderer = new FakeRenderer { Id = "static", Visible = true, Animates = false, PhaseOffset = 0 };
        scheduler.Register(renderer);

        // 120 frames = 2 seconds → 2 phase-0 alignments at frames 60 and 120.
        for (var i = 0; i < 120; i++) scheduler.Tick(i / 60.0);

        Assert.Equal(2, renderer.TickCount);
    }

    [Fact]
    public void PressureSlowsDownStaticPanels()
    {
        var scheduler = new OverlayScheduler { Pressure = 2 };
        var renderer = new FakeRenderer { Id = "static", Visible = true, Animates = false, PhaseOffset = 0 };
        scheduler.Register(renderer);

        // pressure=2 → period=240. 120 frames → 0 ticks (no alignment).
        for (var i = 0; i < 120; i++) scheduler.Tick(i / 60.0);
        Assert.Equal(0, renderer.TickCount);

        // 240 more frames → exactly one tick at frame 240.
        for (var i = 0; i < 240; i++) scheduler.Tick(i / 60.0);
        Assert.Equal(1, renderer.TickCount);
    }

    [Fact]
    public void RendererExceptionDoesNotPoisonOthers()
    {
        var scheduler = new OverlayScheduler();
        var bad = new FakeRenderer { Id = "bad", Visible = true, Animates = true, ThrowOnTick = true };
        var good = new FakeRenderer { Id = "good", Visible = true, Animates = true };
        scheduler.Register(bad);
        scheduler.Register(good);

        scheduler.Tick(0);
        Assert.Equal(1, good.TickCount);
    }

    [Fact]
    public void UnregisterRemovesRenderer()
    {
        var scheduler = new OverlayScheduler();
        var r = new FakeRenderer { Id = "hp", Visible = true, Animates = true };
        scheduler.Register(r);
        Assert.True(scheduler.Unregister("hp"));
        scheduler.Tick(0);
        Assert.Equal(0, r.TickCount);
    }

    private sealed class FakeRenderer : IOverlayRenderer
    {
        public string Id { get; init; } = "test";
        public bool Visible { get; set; }
        public bool Animates { get; set; }
        public int PhaseOffset { get; init; }
        public bool PreferIsolation { get; init; }
        public bool ThrowOnTick { get; init; }
        public int TickCount { get; private set; }

        public void Tick(double monotonicSeconds)
        {
            if (ThrowOnTick) throw new InvalidOperationException("boom");
            TickCount++;
        }
    }
}
