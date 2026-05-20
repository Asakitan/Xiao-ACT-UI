using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S196 — Pin the <c>hud_offset_x</c> settings round-trip + key
/// shape. The HUD geometry itself is derived from the monitor
/// dimensions at every launch (1:1 with <c>sao_webview.py</c>
/// lines 3282–3318); only the horizontal offset is persisted.
/// </summary>
public class Session196HudGeometrySettingsTests : IDisposable
{
    private readonly string _path;

    public Session196HudGeometrySettingsTests()
    {
        _path = Path.Combine(Path.GetTempPath(), $"saoauto-s196-{Guid.NewGuid():N}.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { File.Delete(_path); } catch { /* ignore */ }
    }

    [Fact]
    public void HudOffsetKey_IsCanonicalPythonName()
    {
        Assert.Equal("hud_offset_x", SettingsKeys.HudOffsetX);
    }

    [Fact]
    public void MissingKey_ReturnsNull()
    {
        var settings = new SettingsManager(_path);
        Assert.Null(settings.Get<double?>(SettingsKeys.HudOffsetX));
    }

    [Fact]
    public void RoundTripPreservesOffset()
    {
        var settings = new SettingsManager(_path);
        settings.Set(SettingsKeys.HudOffsetX, 0.12);
        settings.Save();

        var reloaded = new SettingsManager(_path);
        Assert.Equal(0.12, reloaded.Get<double?>(SettingsKeys.HudOffsetX));
    }

    [Fact]
    public void RoundTripPreservesSiblings()
    {
        var settings = new SettingsManager(_path);
        settings.Set("unrelated", 42);
        settings.Set(SettingsKeys.HudOffsetX, 0.05);
        settings.Save();

        var reloaded = new SettingsManager(_path);
        Assert.Equal(0.05, reloaded.Get<double?>(SettingsKeys.HudOffsetX));
        Assert.Equal(42, reloaded.Get<int>("unrelated"));
    }
}
