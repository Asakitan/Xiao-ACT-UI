using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S148 — Pin <see cref="HideSeekClickOrchestrator"/>'s call sequence
/// against Python <c>_send_mouse_click</c> + <c>_alt_click</c>: the
/// alt bracket wraps a move → down → sleep → up cycle, with timings
/// matching Python (30/50/50/50 ms).
/// </summary>
public class Session148HideSeekClickTests
{
    private sealed class RecordingRaw : IHideSeekRawInput
    {
        public List<string> Calls { get; } = new();
        public void MouseMoveAbsolute(int x, int y) => Calls.Add($"move:{x},{y}");
        public void MouseLeftDown() => Calls.Add("down");
        public void MouseLeftUp() => Calls.Add("up");
        public void KeyDown(ushort vk) => Calls.Add($"kd:0x{vk:X}");
        public void KeyUp(ushort vk) => Calls.Add($"ku:0x{vk:X}");
    }

    [Fact]
    public void PlainClickSequence()
    {
        var raw = new RecordingRaw();
        var sleeps = new List<int>();
        var orch = new HideSeekClickOrchestrator(raw, sleeps.Add);

        orch.Click(100, 200, altModifier: false);

        Assert.Equal(new[] { "move:100,200", "down", "up" }, raw.Calls);
        Assert.Equal(new[] { 30, 50 }, sleeps); // move-dwell, click-hold
    }

    [Fact]
    public void AltClickWrapsWithVkMenu()
    {
        var raw = new RecordingRaw();
        var sleeps = new List<int>();
        var orch = new HideSeekClickOrchestrator(raw, sleeps.Add);

        orch.Click(50, 75, altModifier: true);

        Assert.Equal(new[]
        {
            "kd:0x12", "move:50,75", "down", "up", "ku:0x12",
        }, raw.Calls);
        Assert.Equal(new[] { 50, 30, 50, 50 }, sleeps); // alt-bracket, move, hold, alt-bracket
    }

    [Fact]
    public void OrchestratorWithoutSleepStillCallsRaw()
    {
        var raw = new RecordingRaw();
        var orch = new HideSeekClickOrchestrator(raw); // no sleep injected

        orch.Click(1, 2, altModifier: false);

        Assert.Equal(new[] { "move:1,2", "down", "up" }, raw.Calls);
    }

    [Fact]
    public void RejectsNullRawInput()
    {
        Assert.Throws<ArgumentNullException>(() =>
            new HideSeekClickOrchestrator(null!));
    }

    [Fact]
    public void TimingConstantsMatchPython()
    {
        Assert.Equal(30, HideSeekClickOrchestrator.MoveDwellMs);
        Assert.Equal(50, HideSeekClickOrchestrator.ClickHoldMs);
        Assert.Equal(50, HideSeekClickOrchestrator.AltBracketMs);
        Assert.Equal((ushort)0x12, HideSeekClickOrchestrator.VK_MENU);
    }

    [Fact]
    public void Win32HideSeekInputCanBeConstructed()
    {
        // Just proves the production wiring compiles. We do not call
        // Click() because that would issue real SendInput packets.
        var input = new Win32HideSeekInput();
        Assert.NotNull(input);
    }
}
