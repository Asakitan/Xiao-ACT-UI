using System.Collections.Immutable;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Automation;

public class AutoKeyRuntimeTests
{
    [Fact]
    public void TickFiresMatchingActionOncePerCooldown()
    {
        var (rt, dispatcher, state, advance) = Build();
        rt.SetProfile(new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("hp", new HpBelowTrigger(0.5), VK(0x31), 1000, 1))));
        state.Update(s => s with { HpPct = 0.4 });

        Assert.Equal("hp", rt.Tick());
        Assert.Single(dispatcher.Strokes);

        Assert.Null(rt.Tick());
        Assert.Single(dispatcher.Strokes);

        advance(TimeSpan.FromMilliseconds(1100));
        Assert.Equal("hp", rt.Tick());
        Assert.Equal(2, dispatcher.Strokes.Count);
    }

    [Fact]
    public void DisabledProfileDoesNotFire()
    {
        var (rt, dispatcher, state, _) = Build();
        rt.SetProfile(new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("hp", new HpBelowTrigger(0.5), VK(0x31), 100, 1)),
            Enabled: false));
        state.Update(s => s with { HpPct = 0.1 });
        Assert.Null(rt.Tick());
        Assert.Empty(dispatcher.Strokes);
    }

    [Fact]
    public void EnabledGateGatesEverything()
    {
        var enabled = false;
        var (rt, dispatcher, state, _) = Build();
        rt.SetEnabledGate(() => enabled);
        rt.SetProfile(new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("hp", new HpBelowTrigger(1.0), VK(0x31), 100, 1))));
        state.Update(s => s with { HpPct = 0.1 });

        Assert.Null(rt.Tick());
        Assert.Empty(dispatcher.Strokes);

        enabled = true;
        Assert.Equal("hp", rt.Tick());
        Assert.Single(dispatcher.Strokes);
    }

    [Fact]
    public void HigherPriorityActionFiresFirst()
    {
        var (rt, dispatcher, state, _) = Build();
        rt.SetProfile(new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("low", new HpBelowTrigger(1.0), VK(0x31), 100, 1),
            new AutoKeyAction("high", new HpBelowTrigger(1.0), VK(0x32), 100, 5))));
        state.Update(s => s with { HpPct = 0.5 });

        Assert.Equal("high", rt.Tick());
        Assert.Single(dispatcher.Strokes);
        Assert.Equal(0x32, dispatcher.Strokes[0].VirtualKey);
    }

    [Fact]
    public void DispatcherExceptionIsSwallowedAndFireCounterDoesNotIncrement()
    {
        var state = new GameStateManager();
        using var rt = new AutoKeyRuntime(state, new ThrowingDispatcher());
        rt.SetProfile(new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("hp", new HpBelowTrigger(1.0), VK(0x31), 100, 1))));
        state.Update(s => s with { HpPct = 0.1 });
        Assert.Null(rt.Tick());
        Assert.Equal(0, rt.FireCount);
    }

    private static (AutoKeyRuntime rt, RecordingDispatcher dispatcher, GameStateManager state, Action<TimeSpan> advance) Build()
    {
        var dispatcher = new RecordingDispatcher();
        var state = new GameStateManager();
        var now = DateTimeOffset.UtcNow;
        var rt = new AutoKeyRuntime(state, dispatcher, clock: () => now);
        return (rt, dispatcher, state, dt => now = now.Add(dt));
    }

    private static KeyStroke VK(int vk) => new(vk, AutoKeyModifiers.None);

    private sealed class RecordingDispatcher : IKeyDispatcher
    {
        public List<KeyStroke> Strokes { get; } = new();
        public void Dispatch(KeyStroke stroke) => Strokes.Add(stroke);
    }

    private sealed class ThrowingDispatcher : IKeyDispatcher
    {
        public void Dispatch(KeyStroke stroke) => throw new InvalidOperationException("nope");
    }
}
