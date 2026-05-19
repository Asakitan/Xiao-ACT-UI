using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Configuration;

/// <summary>
/// S100 — Pin <see cref="WindowMatchersLoader"/>. Settings-driven override
/// for <see cref="WindowLocator"/>'s title-keyword + process-name lists.
/// Falls through to <see cref="GameWindowConfig"/> defaults when the
/// settings key is missing/empty/non-array.
/// </summary>
public class Session100WindowMatchersLoaderTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _settingsPath;

    public Session100WindowMatchersLoaderTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s100-" + Guid.NewGuid().ToString("N"));
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
    public void NullSettings_LoadKeywords_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => WindowMatchersLoader.LoadKeywords(null!));
    }

    [Fact]
    public void NullSettings_LoadProcessNames_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => WindowMatchersLoader.LoadProcessNames(null!));
    }

    [Fact]
    public void MissingKey_Keywords_FallsBackToDefaults()
    {
        var s = NewSettings("{}");
        Assert.Same(GameWindowConfig.DefaultTitleKeywords, WindowMatchersLoader.LoadKeywords(s));
    }

    [Fact]
    public void MissingKey_ProcessNames_FallsBackToDefaults()
    {
        var s = NewSettings("{}");
        Assert.Same(GameWindowConfig.DefaultProcessNames, WindowMatchersLoader.LoadProcessNames(s));
    }

    [Fact]
    public void EmptyArray_FallsBackToDefaults()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":[],\"process_names\":[]}}");
        Assert.Same(GameWindowConfig.DefaultTitleKeywords, WindowMatchersLoader.LoadKeywords(s));
        Assert.Same(GameWindowConfig.DefaultProcessNames, WindowMatchersLoader.LoadProcessNames(s));
    }

    [Fact]
    public void NonArrayValue_FallsBackToDefaults()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":\"oops\",\"process_names\":42}}");
        Assert.Same(GameWindowConfig.DefaultTitleKeywords, WindowMatchersLoader.LoadKeywords(s));
        Assert.Same(GameWindowConfig.DefaultProcessNames, WindowMatchersLoader.LoadProcessNames(s));
    }

    [Fact]
    public void FullOverride_Keywords_Wins()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":[\"FooBar\",\"Baz\"]}}");
        Assert.Equal(new[] { "FooBar", "Baz" }, WindowMatchersLoader.LoadKeywords(s));
    }

    [Fact]
    public void FullOverride_ProcessNames_Wins()
    {
        var s = NewSettings("{\"game_window\":{\"process_names\":[\"foo.exe\",\"BAR.EXE\"]}}");
        // process names are not lowercased here — locator does that.
        Assert.Equal(new[] { "foo.exe", "BAR.EXE" }, WindowMatchersLoader.LoadProcessNames(s));
    }

    [Fact]
    public void WhitespaceEntries_Dropped()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":[\"  \",\"\",\"Real\"]}}");
        Assert.Equal(new[] { "Real" }, WindowMatchersLoader.LoadKeywords(s));
    }

    [Fact]
    public void Trim_AppliedToValidEntries()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":[\"  Sword \"]}}");
        Assert.Equal(new[] { "Sword" }, WindowMatchersLoader.LoadKeywords(s));
    }

    [Fact]
    public void NonStringEntries_Skipped()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":[1,true,null,\"OK\"]}}");
        Assert.Equal(new[] { "OK" }, WindowMatchersLoader.LoadKeywords(s));
    }

    [Fact]
    public void AllInvalid_FallsBackToDefaults()
    {
        var s = NewSettings("{\"game_window\":{\"keywords\":[null,\"\",\"   \"]}}");
        Assert.Same(GameWindowConfig.DefaultTitleKeywords, WindowMatchersLoader.LoadKeywords(s));
    }

    [Fact]
    public void ParseStringArray_Null_ReturnsEmpty()
    {
        Assert.Empty(WindowMatchersLoader.ParseStringArray(null));
    }

    [Fact]
    public void ParseStringArray_Empty_ReturnsEmpty()
    {
        Assert.Empty(WindowMatchersLoader.ParseStringArray(new JsonArray()));
    }
}
