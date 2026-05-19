using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S88 — Pins parity for the level / stamina rollback rules added to
/// <see cref="GameStateValidation"/>, mirroring Python
/// <c>game_state.GameStateManager.update</c> pre-filters (lines 267–280).
/// </summary>
public class Session88RollbackRulesTests
{
    [Fact]
    public void RollbackLevelBase_NonZero_PassesThrough()
    {
        Assert.Equal(60, GameStateValidation.RollbackLevelBase(60, previousNonZero: 55));
    }

    [Fact]
    public void RollbackLevelBase_Zero_UsesPrevious()
    {
        Assert.Equal(55, GameStateValidation.RollbackLevelBase(0, previousNonZero: 55));
    }

    [Fact]
    public void RollbackLevelBase_ZeroAndNoPrevious_StaysZero()
    {
        // Cold start with no tracked previous → leave incoming as-is (0).
        Assert.Equal(0, GameStateValidation.RollbackLevelBase(0, previousNonZero: 0));
    }

    [Fact]
    public void RollbackLevelExtra_NonZero_PassesThrough()
    {
        Assert.Equal(12, GameStateValidation.RollbackLevelExtra(12, previousNonZero: 7));
    }

    [Fact]
    public void RollbackLevelExtra_Zero_UsesPrevious()
    {
        Assert.Equal(7, GameStateValidation.RollbackLevelExtra(0, previousNonZero: 7));
    }

    [Fact]
    public void RollbackLevelExtra_ZeroAndNoPrevious_StaysZero()
    {
        Assert.Equal(0, GameStateValidation.RollbackLevelExtra(0, previousNonZero: 0));
    }

    [Fact]
    public void RollbackStaminaCurrent_NonZero_PassesThrough()
    {
        Assert.Equal(85,
            GameStateValidation.RollbackStaminaCurrent(85,
                incomingStaminaPctIsExplicit: false,
                incomingStaminaMax: 0, previousNonZero: 60));
    }

    [Fact]
    public void RollbackStaminaCurrent_ZeroWithExplicitPct_StaysZero()
    {
        // Python: if 'stamina_pct' is in kwargs, skip rollback.
        Assert.Equal(0,
            GameStateValidation.RollbackStaminaCurrent(0,
                incomingStaminaPctIsExplicit: true,
                incomingStaminaMax: 0, previousNonZero: 60));
    }

    [Fact]
    public void RollbackStaminaCurrent_ZeroWithPositiveMax_StaysZero()
    {
        // Python: int(next_sta_max or 0) <= 0 must be true to roll back.
        Assert.Equal(0,
            GameStateValidation.RollbackStaminaCurrent(0,
                incomingStaminaPctIsExplicit: false,
                incomingStaminaMax: 100, previousNonZero: 60));
    }

    [Fact]
    public void RollbackStaminaCurrent_ZeroNoPctNoMax_UsesPrevious()
    {
        Assert.Equal(60,
            GameStateValidation.RollbackStaminaCurrent(0,
                incomingStaminaPctIsExplicit: false,
                incomingStaminaMax: 0, previousNonZero: 60));
    }

    [Fact]
    public void RollbackStaminaCurrent_ZeroNoPctNoMaxNoPrevious_StaysZero()
    {
        Assert.Equal(0,
            GameStateValidation.RollbackStaminaCurrent(0,
                incomingStaminaPctIsExplicit: false,
                incomingStaminaMax: 0, previousNonZero: 0));
    }
}
