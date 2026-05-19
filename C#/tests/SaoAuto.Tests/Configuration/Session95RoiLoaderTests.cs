using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Configuration;

/// <summary>
/// S95 — Pin <see cref="RoiLoader"/> against Python
/// <c>SettingsManager.get_roi</c> (config.py 1463–1469) plus the
/// <c>DEFAULT_ROI</c> percentages (config.py 1191–1201).
/// </summary>
public class Session95RoiLoaderTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _settingsPath;

    public Session95RoiLoaderTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s95-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _settingsPath = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_settingsPath, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private SettingsManager NewSettings(string json)
    {
        File.WriteAllText(_settingsPath, json);
        return new SettingsManager(_settingsPath);
    }

    [Fact]
    public void NullSettings_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => RoiLoader.Load(null!, "stamina_bar"));
    }

    [Fact]
    public void EmptyName_Throws()
    {
        var s = NewSettings("{}");
        Assert.Throws<ArgumentException>(() => RoiLoader.Load(s, ""));
    }

    [Fact]
    public void MissingRoiTable_ReturnsDefault()
    {
        var s = NewSettings("{}");
        Assert.Equal(new Roi(0.330, 0.957, 0.340, 0.036), RoiLoader.Load(s, "stamina_bar"));
    }

    [Fact]
    public void EmptyOverride_FallsThroughToDefault()
    {
        // Python `if custom:` — empty dict is falsy.
        var s = NewSettings("{\"roi\":{\"stamina_bar\":{}}}");
        Assert.Equal(new Roi(0.330, 0.957, 0.340, 0.036), RoiLoader.Load(s, "stamina_bar"));
    }

    [Fact]
    public void FullOverride_Wins()
    {
        var s = NewSettings("{\"roi\":{\"stamina_bar\":{\"x\":0.1,\"y\":0.2,\"w\":0.3,\"h\":0.4}}}");
        Assert.Equal(new Roi(0.1, 0.2, 0.3, 0.4), RoiLoader.Load(s, "stamina_bar"));
    }

    [Fact]
    public void PartialOverride_FallsThroughBecauseInvalid()
    {
        // Missing keys → can't parse → fall through to default. Python's
        // `if custom:` would accept any non-empty dict, but the C# port
        // refuses to invent zeros for missing fields — safer.
        var s = NewSettings("{\"roi\":{\"stamina_bar\":{\"x\":0.1,\"y\":0.2}}}");
        Assert.Equal(new Roi(0.330, 0.957, 0.340, 0.036), RoiLoader.Load(s, "stamina_bar"));
    }

    [Fact]
    public void IntegerValues_Coerced()
    {
        var s = NewSettings("{\"roi\":{\"hp_bar\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}}");
        Assert.Equal(new Roi(0, 0, 1, 1), RoiLoader.Load(s, "hp_bar"));
    }

    [Fact]
    public void UnknownName_ReturnsZeroRoi()
    {
        // Python `dict(DEFAULT_ROI.get(name, {}))` returns {} for unknown.
        var s = NewSettings("{}");
        Assert.Equal(default, RoiLoader.Load(s, "no_such_roi"));
    }

    [Fact]
    public void DefaultsTable_HasEverySupportedName()
    {
        var expected = new[]
        {
            "identity", "level", "name",
            "hp_bar", "hp_text",
            "stamina_bar", "stamina_text",
            "player_id",
        };
        foreach (var k in expected)
        {
            Assert.True(RoiLoader.Defaults.ContainsKey(k), $"missing default ROI: {k}");
        }
    }

    [Fact]
    public void TryParseRoi_NullOrNonObject_False()
    {
        Assert.False(RoiLoader.TryParseRoi(null, out _));
        Assert.False(RoiLoader.TryParseRoi(JsonValue.Create(42), out _));
        Assert.False(RoiLoader.TryParseRoi(new JsonArray(), out _));
    }
}
