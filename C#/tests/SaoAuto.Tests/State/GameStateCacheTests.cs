using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

public class GameStateCacheTests : IDisposable
{
    private static readonly string FixturePath = Path.Combine(
        AppContext.BaseDirectory, "Fixtures", "settings.json");

    private readonly string _workDir;
    private readonly string _settingsPath;

    public GameStateCacheTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-cache-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _settingsPath = Path.Combine(_workDir, "settings.json");
        File.Copy(FixturePath, _settingsPath);
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    [Fact]
    public void LoadFromFixtureRehydratesIdentityAndHpFields()
    {
        var settings = new SettingsManager(_settingsPath);
        var states = new GameStateManager();

        GameStateCache.Load(settings, states);
        var s = states.Snapshot;

        // Match fixture sao_auto/settings.json
        Assert.Equal("咲", s.PlayerName);
        Assert.Equal(60, s.LevelBase);
        Assert.Equal(74, s.LevelExtra);
        Assert.Equal("36668136", s.PlayerId);
        Assert.Equal(54452, s.FightPoint);
        Assert.Equal(863343, s.HpCurrent);
        Assert.Equal(863343, s.HpMax);
        Assert.Equal(1.0, s.HpPct);
        Assert.Equal(0.892, s.StaminaPct, 3);
        Assert.Equal(12, s.ProfessionId);
        Assert.Equal("神盾骑士", s.ProfessionName);
    }

    [Fact]
    public void SaveSkipsZeroIdentityFieldsToAvoidOverwritingPriorCache()
    {
        var settings = new SettingsManager(_settingsPath);
        var states = new GameStateManager();
        GameStateCache.Load(settings, states); // populate from fixture

        // Reset identity fields (simulate tool starting before any packets arrive)
        states.Update(s => s with
        {
            PlayerName = string.Empty,
            LevelBase = 0,
            LevelExtra = 0,
            FightPoint = 0,
            ProfessionId = 0,
            ProfessionName = string.Empty,
        });

        GameStateCache.Save(settings, states);

        var reloaded = new SettingsManager(_settingsPath);
        var states2 = new GameStateManager();
        GameStateCache.Load(reloaded, states2);
        var s = states2.Snapshot;

        // Identity preserved from the original fixture.
        Assert.Equal("咲", s.PlayerName);
        Assert.Equal(60, s.LevelBase);
        Assert.Equal(74, s.LevelExtra);
        Assert.Equal(54452, s.FightPoint);
        Assert.Equal(12, s.ProfessionId);
        Assert.Equal("神盾骑士", s.ProfessionName);
    }
}
