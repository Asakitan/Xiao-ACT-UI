using System.Collections.Immutable;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S93 — Pin <see cref="RecognitionSkillSlotProjector"/> mapping rules and the
/// <see cref="RecognitionTickHost"/> wiring of projected slots +
/// <see cref="BurstReadyCalculator"/>.
/// </summary>
public class Session93BurstReadyWireUpTests
{
    private static SkillSlotResult Slot(int idx, string state, double cd = 1.0,
        bool ie = false, bool active = false, bool edge = false) =>
        new(idx, state, cd, ie, active, edge);

    [Fact]
    public void Projector_NullOrEmpty_YieldsEmptyArray()
    {
        Assert.Empty(RecognitionSkillSlotProjector.Project(null));
        Assert.Empty(RecognitionSkillSlotProjector.Project(Array.Empty<SkillSlotResult>()));
    }

    [Fact]
    public void Projector_MapsAllFields()
    {
        var arr = RecognitionSkillSlotProjector.Project(new[]
        {
            Slot(2, "ready", cd: 0.0, active: true, edge: true),
            Slot(3, "cooldown", cd: 0.45, ie: true),
        });
        Assert.Equal(2, arr.Length);
        Assert.Equal(2, arr[0].Index);
        Assert.Equal(SkillSlotState.Ready, arr[0].State);
        Assert.True(arr[0].Active);
        Assert.True(arr[0].ReadyEdge);
        Assert.Equal(0.0, arr[0].CooldownPct);
        Assert.Equal(SkillSlotState.Cooldown, arr[1].State);
        Assert.True(arr[1].InsufficientEnergy);
        Assert.Equal(0.45, arr[1].CooldownPct);
    }

    [Theory]
    [InlineData("ready", SkillSlotState.Ready)]
    [InlineData("READY", SkillSlotState.Ready)]
    [InlineData(" cooldown ", SkillSlotState.Cooldown)]
    [InlineData("active", SkillSlotState.Active)]
    [InlineData("insufficient_energy", SkillSlotState.InsufficientEnergy)]
    [InlineData("unknown", SkillSlotState.Unknown)]
    [InlineData("", SkillSlotState.Unknown)]
    [InlineData(null, SkillSlotState.Unknown)]
    public void ParseState_HandlesAllBranches(string? input, SkillSlotState expected)
    {
        Assert.Equal(expected, RecognitionSkillSlotProjector.ParseState(input));
    }

    [Fact]
    public void Projector_ChargeAndRemainingMs_DefaultZero()
    {
        // Documented Python parity: recognition path can't infer charge/remaining,
        // so RemainingMs stays 0 (≤120 → trips the burst-ready branch).
        var arr = RecognitionSkillSlotProjector.Project(new[] { Slot(1, "cooldown", cd: 0.9) });
        Assert.Equal(0, arr[0].ChargeCount);
        Assert.Equal(0, arr[0].RemainingMs);
    }

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

    [Fact]
    public void Host_NoWatchedSlots_BurstReadyFalse()
    {
        var cap = new StubCapture();
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(), new Roi(0, 0, 1, 1), clock: readClock);
        var states = new GameStateManager();
        using var host = new RecognitionTickHost(engine, states, clock: readClock);
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        host.RunOnceAsync();
        Assert.False(states.Snapshot.BurstReady);
        // Recognition tick has no skill ROIs configured → projected to empty.
        Assert.Empty(states.Snapshot.SkillSlots);
    }

    [Fact]
    public void Host_WithWatchedSlots_NoMatchedSlots_BurstReadyFalse()
    {
        var cap = new StubCapture();
        var clock = 100.0;
        Func<double> readClock = () => clock;
        var engine = new RecognitionTickEngine(
            cap, () => MakeWindow(), new Roi(0, 0, 1, 1), clock: readClock);
        var states = new GameStateManager();
        using var host = new RecognitionTickHost(
            engine, states, clock: readClock, watchedSlots: new[] { 2, 3 });
        cap.Frames.Enqueue(MakeGoldFrame(64, 16));
        host.RunOnceAsync();
        // Engine produced no slots → matched=0 → false per BurstReadyCalculator.
        Assert.False(states.Snapshot.BurstReady);
    }
}
