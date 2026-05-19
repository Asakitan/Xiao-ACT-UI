using SaoAuto.App.Menu;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Menu;

public class MenuHitTestTests
{
    private static MenuLayout BuildLayout() => new(
        LeftWidget: new RectI(0, 0, 100, 200),
        MenuButton: new RectI(900, 50, 80, 40),
        ChildRows: new[]
        {
            new RectI(900, 100, 200, 30),
            new RectI(900, 132, 200, 30),
            new RectI(900, 164, 200, 30),
        },
        Backdrop: new RectI(0, 0, 1920, 1080));

    [Fact]
    public void HitInsideLeftWidget()
    {
        var hit = MenuHitTest.Test(50, 50, BuildLayout());
        Assert.Equal(MenuHitKind.LeftWidget, hit.Kind);
    }

    [Fact]
    public void HitInsideMenuButton()
    {
        var hit = MenuHitTest.Test(940, 60, BuildLayout());
        Assert.Equal(MenuHitKind.MenuButton, hit.Kind);
    }

    [Fact]
    public void HitInsideChildRowReturnsIndex()
    {
        var hit = MenuHitTest.Test(950, 145, BuildLayout()); // second row (y=132..162)
        Assert.Equal(MenuHitKind.ChildRow, hit.Kind);
        Assert.Equal(1, hit.Index);
    }

    [Fact]
    public void HitOutsideAllReturnsBackdropOrOutside()
    {
        // Inside backdrop but not in any sub-region → Backdrop.
        var hit = MenuHitTest.Test(500, 800, BuildLayout());
        Assert.Equal(MenuHitKind.Backdrop, hit.Kind);
    }
}

public class OutsideClickGateTests
{
    [Fact]
    public void NoOpWhenNotArmed()
    {
        var gate = new OutsideClickGate();
        Assert.False(gate.ShouldClose());
    }

    [Fact]
    public void GraceBlocksImmediateClose()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var gate = new OutsideClickGate(grace: TimeSpan.FromMilliseconds(300), clock: () => clock.Now);
        gate.NoteOpened();
        Assert.False(gate.ShouldClose());
        clock.Advance(TimeSpan.FromMilliseconds(100));
        Assert.False(gate.ShouldClose());
    }

    [Fact]
    public void ClosePassesAfterGrace()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var gate = new OutsideClickGate(grace: TimeSpan.FromMilliseconds(300), clock: () => clock.Now);
        gate.NoteOpened();
        clock.Advance(TimeSpan.FromMilliseconds(350));
        Assert.True(gate.ShouldClose());
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; }
        public TestClock(DateTimeOffset s) => Now = s;
        public void Advance(TimeSpan d) => Now = Now.Add(d);
    }
}
