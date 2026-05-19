using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Configuration;

/// <summary>
/// S94 — Pin <see cref="WatchedSkillSlotsLoader"/> against the Python
/// <c>normalize_watched_skill_slots</c> Cython implementation
/// (_sao_cy_uihelpers.pyx 78–93) and the Python read-default convention
/// of <c>[1..9]</c> when the key is missing.
/// </summary>
public class Session94WatchedSkillSlotsLoaderTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _settingsPath;

    public Session94WatchedSkillSlotsLoaderTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s94-" + Guid.NewGuid().ToString("N"));
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
        Assert.Throws<ArgumentNullException>(() => WatchedSkillSlotsLoader.Load(null!));
    }

    [Fact]
    public void MissingKey_DefaultsToOneThroughNine()
    {
        var s = NewSettings("{}");
        var slots = WatchedSkillSlotsLoader.Load(s);
        Assert.Equal(new[] { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, slots);
    }

    [Fact]
    public void EmptyArray_DefaultsToOneThroughNine()
    {
        var s = NewSettings("{\"watched_skill_slots\":[]}");
        Assert.Equal(new[] { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void NullValue_DefaultsToOneThroughNine()
    {
        var s = NewSettings("{\"watched_skill_slots\":null}");
        Assert.Equal(new[] { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void ExplicitSubset_PreservedInOrder()
    {
        var s = NewSettings("{\"watched_skill_slots\":[3,1,5]}");
        Assert.Equal(new[] { 3, 1, 5 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void Duplicates_RemovedFirstSeenWins()
    {
        var s = NewSettings("{\"watched_skill_slots\":[2,2,3,2,1]}");
        Assert.Equal(new[] { 2, 3, 1 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void OutOfRange_Skipped()
    {
        var s = NewSettings("{\"watched_skill_slots\":[0,1,9,10,-1,5]}");
        Assert.Equal(new[] { 1, 9, 5 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void StringEntries_CoercedWhenNumeric()
    {
        // Python int(raw) parses numeric strings; non-numeric raises and is skipped.
        var s = NewSettings("{\"watched_skill_slots\":[\"3\",\"x\",\"5\"]}");
        Assert.Equal(new[] { 3, 5 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void FloatEntries_TruncatedThenFiltered()
    {
        // Python int(2.7) == 2.
        var s = NewSettings("{\"watched_skill_slots\":[2.7,4.2,9.99]}");
        Assert.Equal(new[] { 2, 4, 9 }, WatchedSkillSlotsLoader.Load(s));
    }

    [Fact]
    public void Normalize_NullArray_Empty()
    {
        Assert.Empty(WatchedSkillSlotsLoader.Normalize(null));
    }

    [Fact]
    public void Normalize_EmptyArray_Empty()
    {
        Assert.Empty(WatchedSkillSlotsLoader.Normalize(new JsonArray()));
    }

    [Fact]
    public void Normalize_AllValid_PreservesOrderAndContent()
    {
        var arr = new JsonArray { 7, 2, 5 };
        Assert.Equal(new[] { 7, 2, 5 }, WatchedSkillSlotsLoader.Normalize(arr));
    }
}
