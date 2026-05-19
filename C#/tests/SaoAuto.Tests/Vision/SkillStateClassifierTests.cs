using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

public class SkillStateClassifierTests
{
    private static SkillSlotMetrics M(
        double innerV = 150, double iconV = 165, double ringRatio = 0.16,
        double bright = 0.20, double dark = 0.10, double grayDark = 0.05,
        double dim = 0.20, double warm = 0.10, double shadow = 0.10,
        double readyScore = 0.40)
        => new(innerV, 150, iconV, 95, ringRatio, bright, dark, grayDark, dim, warm,
               shadow, IconScore: 0.74 * (iconV / 255.0) + 0.26 * (95 / 255.0),
               ReadyScore: readyScore);

    [Fact]
    public void GuessBaselineReadyWhenScoreAndRingMet()
    {
        var m = M(readyScore: 0.45, ringRatio: 0.20);
        Assert.Equal(SkillStateClassifier.StateReady, SkillStateClassifier.GuessBaselineState(m));
    }

    [Fact]
    public void GuessBaselineCooldownWhenIconDim()
    {
        var m = M(iconV: 100);
        Assert.Equal(SkillStateClassifier.StateCooldown, SkillStateClassifier.GuessBaselineState(m));
    }

    [Fact]
    public void GuessBaselineInsufficientWhenWarmDominates()
    {
        var m = M(warm: 0.60, ringRatio: 0.05);
        Assert.Equal(SkillStateClassifier.StateInsufficientEnergy,
            SkillStateClassifier.GuessBaselineState(m));
    }

    [Fact]
    public void ClassifyReadyOnAbsoluteThresholds()
    {
        var m = M(ringRatio: 0.18, iconV: 180, grayDark: 0.05, shadow: 0.05);
        var (state, ratio) = SkillStateClassifier.Classify(m, SkillStateClassifier.DefaultBaseline);
        Assert.Equal(SkillStateClassifier.StateReady, state);
        Assert.Equal(0.0, ratio);
    }

    [Fact]
    public void ClassifyCooldownWhenIconDarkAndShadowy()
    {
        var m = M(iconV: 60, ringRatio: 0.05, shadow: 0.45, grayDark: 0.30, readyScore: 0.10);
        var (state, ratio) = SkillStateClassifier.Classify(m, SkillStateClassifier.DefaultBaseline);
        Assert.Equal(SkillStateClassifier.StateCooldown, state);
        Assert.True(ratio >= 0.05);
    }

    [Fact]
    public void ClassifyInsufficientShortCircuitsCooldown()
    {
        var m = M(warm: 0.55, ringRatio: 0.05, dim: 0.50);
        var (state, _) = SkillStateClassifier.Classify(m, SkillStateClassifier.DefaultBaseline);
        Assert.Equal(SkillStateClassifier.StateInsufficientEnergy, state);
    }

    [Fact]
    public void ClassifyUsesBaselineComparisonForCooldownExit()
    {
        // baseline=cooldown, score_ratio=1.10, ready_absolute=true → ready
        var m = M(ringRatio: 0.20, iconV: 180, grayDark: 0.05, shadow: 0.05);
        var cmp = new SkillBaselineComparison(
            IconVRatio: 1.10, IconSRatio: 1.0, ScoreRatio: 1.10,
            DarkenedRatio: 0.05, RestoredRatio: 0.80, AvgDeltaV: 5);
        var (state, _) = SkillStateClassifier.Classify(
            m, SkillStateClassifier.DefaultBaseline, cmp,
            baselineState: SkillStateClassifier.StateCooldown);
        Assert.Equal(SkillStateClassifier.StateReady, state);
    }

    [Fact]
    public void ClassifyDetectsDarkeningViaCmp()
    {
        // score dropped, darkened ratio high → cooldown via cmp branch
        var m = M(iconV: 130, ringRatio: 0.12, shadow: 0.18);
        var cmp = new SkillBaselineComparison(
            IconVRatio: 0.85, IconSRatio: 0.9, ScoreRatio: 0.80,
            DarkenedRatio: 0.30, RestoredRatio: 0.10, AvgDeltaV: -20);
        var (state, ratio) = SkillStateClassifier.Classify(
            m, SkillStateClassifier.DefaultBaseline, cmp);
        Assert.Equal(SkillStateClassifier.StateCooldown, state);
        Assert.True(ratio >= 0.05);
    }
}
