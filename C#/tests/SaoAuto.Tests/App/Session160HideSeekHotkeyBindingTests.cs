using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Hotkeys;
using SaoAuto.App.Menu;
using SaoAuto.App.Startup;
using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.App;

/// <summary>
/// S160 — Pin HideSeek suspend-toggle path: tick host short-circuits
/// when <c>Suspended</c>, lifecycle pass-throughs work, and
/// <see cref="HideSeekHotkeyBinding"/> registers / unregisters with the
/// hotkey service and flips the suspended flag on trigger.
/// </summary>
public class Session160HideSeekHotkeyBindingTests
{
    private sealed class StubFrames : IHideSeekFrameProvider
    {
        public HideSeekFrame? Capture() => new(new byte[12], 2, 2, 6, 3, 0, 0);
    }

    private sealed class StubInput : IHideSeekInput
    {
        public void Click(int x, int y, bool alt) { }
    }

    private static HideSeekStateMachine MakeMachine()
    {
        var steps = HideSeekSteps.Default;
        var templates = steps.ToDictionary(s => s.ImageFile,
            s => new HideSeekTemplate(s.ImageFile, new byte[] { 0 }, 1, 1));
        return new HideSeekStateMachine(steps, templates, new StubFrames(), new StubInput(),
            detect: (_, _, _) => new HideSeekDetector.DetectResult(10, 20, 0.9, "stub"));
    }

    private sealed class FakeHotkeys : IGlobalHotkeyService
    {
        public List<int> Registered { get; } = new();
        public List<int> Unregistered { get; } = new();
        public int NextId { get; set; } = 100;
        public event Action<int>? Triggered;
        public int Register(HotkeyBinding binding) { var id = NextId++; Registered.Add(id); return id; }
        public void Unregister(int id) => Unregistered.Add(id);
        public void Dispose() { }
        public void Fire(int id) => Triggered?.Invoke(id);
    }

    [Fact]
    public void TickHostSuspendedSkipsTick()
    {
        var host = new HideSeekTickHost(MakeMachine(), intervalMs: 10);
        host.TickOnce();
        Assert.Equal(1L, host.TickCount);

        host.Suspended = true;
        Assert.Equal(-1, host.TickOnce());
        Assert.Equal(1L, host.TickCount);

        host.Suspended = false;
        host.TickOnce();
        Assert.Equal(2L, host.TickCount);
    }

    [Fact]
    public void TickHostSuspendedDoesNotRaiseTicked()
    {
        var host = new HideSeekTickHost(MakeMachine(), intervalMs: 10);
        int events = 0;
        host.Ticked += _ => events++;
        host.Suspended = true;
        host.TickOnce();
        Assert.Equal(0, events);
    }

    [Fact]
    public void LifecyclePassthroughsAreNoopWhenInactive()
    {
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("boom"),
            NullLogger.Instance, CancellationToken.None);
        Assert.False(lc.IsSuspended);
        Assert.False(lc.ToggleSuspend());
        lc.Suspend();
        Assert.False(lc.IsSuspended);
        lc.Resume();
        Assert.False(lc.IsSuspended);
    }

    [Fact]
    public void HotkeyBindingRegistersAndUnregisters()
    {
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("noop"),
            NullLogger.Instance, CancellationToken.None);
        var hk = new FakeHotkeys { NextId = 42 };
        var binding = new HideSeekHotkeyBinding(hk, lc,
            new HotkeyBinding(HotkeyModifiers.Ctrl | HotkeyModifiers.Alt, 0x48));
        Assert.Single(hk.Registered);
        binding.Dispose();
        Assert.Single(hk.Unregistered);
        Assert.Equal(42, hk.Unregistered[0]);
        // Idempotent
        binding.Dispose();
        Assert.Single(hk.Unregistered);
    }

    [Fact]
    public void HotkeyTriggerFlipsLifecycleSuspend()
    {
        // Use a real machine so the lifecycle is active.
        using var host = new HideSeekTickHost(MakeMachine(), intervalMs: 60_000);
        // Build a lifecycle that owns this host. Easiest: use a factory.
        using var lc = HideSeekLifecycle.Start(
            () => new HideSeekTickHost(MakeMachine(), intervalMs: 60_000),
            NullLogger.Instance, CancellationToken.None);
        Assert.False(lc.IsSuspended);

        var hk = new FakeHotkeys();
        var toggles = new List<bool>();
        using var binding = new HideSeekHotkeyBinding(hk, lc,
            new HotkeyBinding(HotkeyModifiers.Ctrl, 0x48));
        binding.Toggled += toggles.Add;

        var id = hk.Registered[0];
        hk.Fire(id);
        Assert.True(lc.IsSuspended);
        hk.Fire(id);
        Assert.False(lc.IsSuspended);

        // Different id is ignored.
        hk.Fire(id + 999);
        Assert.False(lc.IsSuspended);

        Assert.Equal(new[] { true, false }, toggles);
    }

    [Fact]
    public void NullArgsThrow()
    {
        using var lc = HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("noop"),
            NullLogger.Instance, CancellationToken.None);
        var hk = new FakeHotkeys();
        var b = new HotkeyBinding(HotkeyModifiers.Ctrl, 0x48);
        Assert.Throws<ArgumentNullException>(() => new HideSeekHotkeyBinding(null!, lc, b));
        Assert.Throws<ArgumentNullException>(() => new HideSeekHotkeyBinding(hk, null!, b));
    }
}
