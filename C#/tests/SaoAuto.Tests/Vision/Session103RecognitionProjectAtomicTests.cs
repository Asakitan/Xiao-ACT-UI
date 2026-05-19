using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S103 — Pin the migrated <see cref="RecognitionTickHost"/> projection.
/// The host now routes <c>StaminaPct</c> through
/// <see cref="GameStateManager.ApplyPartial(StatePartial, Func{GameState, GameState})"/>
/// so the [0,1] clamp runs, while the other recognition fields
/// (RecognitionOk / ErrorMsg / StaminaOffline / SkillSlots / BurstReady)
/// ride along via the extraMutate hook in a SINGLE atomic emission.
/// </summary>
public class Session103RecognitionProjectAtomicTests
{
    private sealed class StubCapture : IFrameCapture
    {
        public Queue<CapturedFrame?> Frames { get; } = new();
        public CapturedFrame? Capture() => Frames.Count > 0 ? Frames.Dequeue() : null;
        public void Dispose() { }
    }

    private static WindowCandidate MakeWindow(int w = 64, int h = 16) =>
        new(IntPtr.Zero, "stub", "stub.exe", 0, 0, w, h);

    private static CapturedFrame MakeGoldFrame(int width, int height)
    {
        var stride = width * 4;
        var px = new byte[stride * height];
        for (var i = 0; i < px.Length; i += 4)
        {
            px[i] = 53; px[i + 1] = 174; px[i + 2] = 255; px[i + 3] = 255;
        }
        return new CapturedFrame(width, height, stride, px);
    }

    private static (RecognitionTickHost host, StubCapture cap, GameStateManager states)
        MakeHost(double fps = 10.0)
    {
        var cap = new StubCapture();
        Func<double> clock = () => 100.0;
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(), new Roi(0, 0, 1, 1),
            fps: new AdaptiveFpsSelector(activeFps: fps, idleFps: fps, idleAfterSeconds: 999),
            clock: clock);
        var states = new GameStateManager();
        var host = new RecognitionTickHost(engine, states, clock: clock);
        return (host, cap, states);
    }

    [Fact]
    public void RunOnce_FiresExactlyOneEmission()
    {
        var (host, cap, states) = MakeHost();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var emissions = new List<GameState>();
        using var sub = states.Subscribe(emissions.Add);

        host.RunOnceAsync();

        // S103 contract: one tick → one emission, even though the
        // projection now goes through ApplyPartial + extraMutate.
        Assert.Single(emissions);
    }

    [Fact]
    public void RunOnce_StaminaAndRecognitionInSameSnapshot()
    {
        var (host, cap, states) = MakeHost();
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        var emissions = new List<GameState>();
        using var sub = states.Subscribe(emissions.Add);

        host.RunOnceAsync();

        var snap = emissions[0];
        Assert.True(snap.RecognitionOk);
        Assert.Equal(1.0, snap.StaminaPct, 3);
    }

    [Fact]
    public void RunOnce_NullStaminaPct_KeepsExistingValue()
    {
        // Throwing engine → SafeTick returns StaminaPct=null result.
        // ApplyPartial with StaminaPct=null must not overwrite the
        // existing snapshot's StaminaPct (matches the pre-S103
        // `r.StaminaPct ?? s.StaminaPct` semantics).
        var states = new GameStateManager();
        states.Update(s => s with { StaminaPct = 0.42 });
        var engine = new RecognitionTickEngine(
            new ThrowingCapture(),
            () => MakeWindow(),
            new Roi(0, 0, 1, 1),
            clock: () => 100.0);
        var host = new RecognitionTickHost(engine, states, clock: () => 100.0);

        host.RunOnceAsync();

        Assert.Equal(0.42, states.Snapshot.StaminaPct);
        Assert.False(states.Snapshot.RecognitionOk);
    }

    private sealed class ThrowingCapture : IFrameCapture
    {
        public CapturedFrame? Capture() => throw new InvalidOperationException("boom");
        public void Dispose() { }
    }
}
