using SaoAuto.App.Menu;

namespace SaoAuto.Tests.Menu;

public class MenuStateMachineTests
{
    [Fact]
    public void OpenTransitionsClosedToOpening()
    {
        var sm = new MenuStateMachine();
        var states = new List<MenuState>();
        sm.StateChanged += states.Add;

        sm.Open();
        Assert.Equal(MenuState.Opening, sm.State);
        Assert.True(sm.Visible);
        Assert.Single(states, MenuState.Opening);
    }

    [Fact]
    public void OpenAnimationCompletedSettlesToOpen()
    {
        var sm = new MenuStateMachine();
        sm.Open();
        sm.OnOpenAnimationCompleted();
        Assert.Equal(MenuState.Open, sm.State);
        Assert.True(sm.Visible);
    }

    [Fact]
    public void CloseTransitionsOpenToClosing()
    {
        var sm = new MenuStateMachine();
        sm.Open();
        sm.OnOpenAnimationCompleted();
        sm.Close();
        Assert.Equal(MenuState.Closing, sm.State);
        // Closing still visible until animation completes.
        Assert.False(sm.Visible);
    }

    [Fact]
    public void ToggleAlternates()
    {
        var sm = new MenuStateMachine();
        sm.Toggle();
        Assert.Equal(MenuState.Opening, sm.State);
        sm.OnOpenAnimationCompleted();
        sm.Toggle();
        Assert.Equal(MenuState.Closing, sm.State);
    }

    [Fact]
    public void OpenIsIdempotentDuringOpening()
    {
        var sm = new MenuStateMachine();
        var openCalls = 0;
        sm.OpenAnimationRequested += () => openCalls++;
        sm.Open();
        sm.Open();
        Assert.Equal(1, openCalls);
    }

    [Fact]
    public void ResetForcesClosed()
    {
        var sm = new MenuStateMachine();
        sm.Open();
        sm.Reset();
        Assert.Equal(MenuState.Closed, sm.State);
    }
}
