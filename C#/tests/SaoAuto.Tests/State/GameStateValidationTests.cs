using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

public class GameStateValidationTests
{
    [Theory]
    [InlineData(-0.5, 0.0)]
    [InlineData(0.0, 0.0)]
    [InlineData(0.5, 0.5)]
    [InlineData(1.0, 1.0)]
    [InlineData(1.5, 1.0)]
    public void ClampPercentMatchesPythonRule(double input, double expected)
    {
        Assert.Equal(expected, GameStateValidation.ClampPercent(input));
    }

    [Theory]
    [InlineData(0, true)]
    [InlineData(60, true)]
    [InlineData(999, true)]
    [InlineData(1000, false)]
    [InlineData(-1, false)]
    public void LevelBaseRange(int input, bool expected)
    {
        Assert.Equal(expected, GameStateValidation.TryAcceptLevelBase(input));
    }

    [Theory]
    [InlineData("A", true)]
    [InlineData("ABCDEFGHIJKLMNOPQRST", true)] // 20 chars
    [InlineData("ABCDEFGHIJKLMNOPQRSTU", false)] // 21 chars
    public void PlayerNameLength(string input, bool expected)
    {
        Assert.Equal(expected, GameStateValidation.TryAcceptPlayerName(input));
    }

    [Fact]
    public void RollbackHpCurrentRestoresPreviousWhenZeroAndNotDead()
    {
        // HP arrives as 0, but pct is 0.5 (not death) and we had a previous non-zero current.
        Assert.Equal(800, GameStateValidation.RollbackHpCurrent(0, 0.5, hpMax: 1000, previousNonZero: 800));
    }

    [Fact]
    public void RollbackHpCurrentFallsBackToHpMaxIfNoPreviousValue()
    {
        Assert.Equal(1000, GameStateValidation.RollbackHpCurrent(0, 0.5, hpMax: 1000, previousNonZero: 0));
    }

    [Fact]
    public void RollbackHpCurrentDoesNotRestoreWhenIncomingIsActualDeath()
    {
        // pct ≤ 0.001 → genuine death event, leave the 0
        Assert.Equal(0, GameStateValidation.RollbackHpCurrent(0, 0.0, hpMax: 1000, previousNonZero: 800));
    }

    [Fact]
    public void RollbackHpCurrentNoOpWhenHpMaxUnknown()
    {
        // hp_max == 0 means we have no anchor; never rewrite.
        Assert.Equal(0, GameStateValidation.RollbackHpCurrent(0, 0.5, hpMax: 0, previousNonZero: 800));
    }

    [Theory]
    [InlineData(800, 1000, 800)]
    [InlineData(1500, 1000, 1000)]
    [InlineData(500, 0, 500)] // unknown max → leave as-is
    public void CapToMaxClampsAboveMax(int current, int max, int expected)
    {
        Assert.Equal(expected, GameStateValidation.CapToMax(current, max));
    }
}
