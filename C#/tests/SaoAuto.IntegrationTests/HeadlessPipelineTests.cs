using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.IntegrationTests;

/// <summary>
/// Lightweight integration tests that exercise more than one
/// subsystem at a time but do not need any live host resource.
/// These actually run in CI; they keep the project honest while the
/// live-only smokes accumulate.
/// </summary>
public class HeadlessPipelineTests
{
    [Fact]
    public void Settings_RoundTripThroughGameStateManager()
    {
        var tmp = Path.Combine(Path.GetTempPath(), $"sao_settings_{Guid.NewGuid():N}.json");
        try
        {
            File.WriteAllText(tmp, "{ \"ui_mode\": \"entity\" }");
            var settings = new SettingsManager(tmp);
            Assert.Equal("entity", settings.Get<string>("ui_mode"));

            var state = new GameStateManager();
            state.Update(s => s with { PlayerName = "笨猫" });
            Assert.Equal("笨猫", state.Snapshot.PlayerName);

            settings.Set("ui_mode", "menu");
            settings.Save();
            var reloaded = new SettingsManager(tmp);
            Assert.Equal("menu", reloaded.Get<string>("ui_mode"));
        }
        finally
        {
            try { File.Delete(tmp); } catch { /* best-effort */ }
        }
    }

    [Fact]
    public void GameState_SubscribeFiresOnUpdate()
    {
        var state = new GameStateManager();
        var hits = 0;
        using (state.Subscribe(_ => hits++))
        {
            state.Update(s => s with { LevelBase = 60 });
            state.Update(s => s with { LevelBase = 61 });
        }
        Assert.Equal(2, hits);
    }
}
