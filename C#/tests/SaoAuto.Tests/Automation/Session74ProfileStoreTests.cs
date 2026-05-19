using System.Collections.Immutable;
using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S74 — auto_key profile CRUD + JSON I/O. Verifies CRUD parity
/// with auto_key_engine.py (Find/Active/Upsert/Delete/Clone/
/// Summarize/Export/Import).
/// </summary>
public class Session74ProfileStoreTests
{
    private static JsonElement Js(string json) => JsonDocument.Parse(json).RootElement;

    private static int _counter;
    private static Func<string> Seq() { _counter = 0; return () => $"r{++_counter:D6}"; }
    private static Func<DateTimeOffset> FixedClock() =>
        () => new DateTimeOffset(2026, 5, 7, 12, 30, 0, TimeSpan.Zero);

    private static AutoKeyProfileSpecRecord Profile(string id, string name = "P", int actions = 1)
    {
        var arr = Enumerable.Range(1, actions)
            .Select(i => AutoKeyProfileSpec.MakeDefaultAction(i, () => $"a{id}{i}"))
            .ToImmutableArray();
        return AutoKeyProfileSpec
            .MakeDefaultProfile(clock: FixedClock(), newId: () => id)
            with { Id = id, ProfileName = name, Actions = arr };
    }

    [Fact]
    public void DefaultConfig_HasDefaultsAndEmptyProfiles()
    {
        var c = AutoKeyProfileStore.DefaultConfig();
        Assert.False(c.Enabled);
        Assert.Equal("", c.ActiveProfileId);
        Assert.Equal(AutoKeyProfileSpec.DefaultServerUrl, c.ServerUrl);
        Assert.Empty(c.Profiles);
    }

    [Fact]
    public void NormalizeConfig_AssignsActiveToFirstWhenMissing()
    {
        var json = """{"profiles":[{"id":"p1"},{"id":"p2"}]}""";
        var c = AutoKeyProfileStore.NormalizeConfig(Js(json), clock: FixedClock(), newId: Seq());
        Assert.Equal(2, c.Profiles.Length);
        Assert.Equal("p1", c.ActiveProfileId);   // first wins when missing
    }

    [Fact]
    public void NormalizeConfig_StrandedActiveResetsToFirst()
    {
        var json = """{"active_profile_id":"ghost","profiles":[{"id":"p1"}]}""";
        var c = AutoKeyProfileStore.NormalizeConfig(Js(json), clock: FixedClock(), newId: Seq());
        Assert.Equal("p1", c.ActiveProfileId);
    }

    [Fact]
    public void Find_And_ActiveProfile()
    {
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "p2",
            Profiles = ImmutableArray.Create(Profile("p1"), Profile("p2", "Burst")),
        };
        Assert.Null(AutoKeyProfileStore.FindProfile(c, ""));
        Assert.Null(AutoKeyProfileStore.FindProfile(c, "missing"));
        Assert.Equal("Burst", AutoKeyProfileStore.FindProfile(c, "p2")!.ProfileName);
        Assert.Equal("p2", AutoKeyProfileStore.ActiveProfile(c)!.Id);
    }

    [Fact]
    public void Upsert_AddsNewAndPromotesActiveWhenEmpty()
    {
        var c = AutoKeyProfileStore.DefaultConfig();
        var c2 = AutoKeyProfileStore.UpsertProfile(c, Profile("p1"));
        Assert.Single(c2.Profiles);
        Assert.Equal("p1", c2.ActiveProfileId);  // promoted because was empty
    }

    [Fact]
    public void Upsert_ReplacesByIdWithoutChangingActive()
    {
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "p2",
            Profiles = ImmutableArray.Create(Profile("p1"), Profile("p2")),
        };
        var renamed = Profile("p2", "Renamed");
        var c2 = AutoKeyProfileStore.UpsertProfile(c, renamed);
        Assert.Equal(2, c2.Profiles.Length);
        Assert.Equal("Renamed", AutoKeyProfileStore.FindProfile(c2, "p2")!.ProfileName);
        Assert.Equal("p2", c2.ActiveProfileId);
    }

    [Fact]
    public void Upsert_ActivateOverridesActive()
    {
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "p1",
            Profiles = ImmutableArray.Create(Profile("p1")),
        };
        var c2 = AutoKeyProfileStore.UpsertProfile(c, Profile("p2"), activate: true);
        Assert.Equal("p2", c2.ActiveProfileId);
    }

    [Fact]
    public void Delete_RemovesAndReassignsActiveIfDeleted()
    {
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "p1",
            Profiles = ImmutableArray.Create(Profile("p1"), Profile("p2")),
        };
        var c2 = AutoKeyProfileStore.DeleteProfile(c, "p1");
        Assert.Single(c2.Profiles);
        Assert.Equal("p2", c2.ActiveProfileId);
    }

    [Fact]
    public void Delete_LastProfileClearsActive()
    {
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "only",
            Profiles = ImmutableArray.Create(Profile("only")),
        };
        var c2 = AutoKeyProfileStore.DeleteProfile(c, "only");
        Assert.Empty(c2.Profiles);
        Assert.Equal("", c2.ActiveProfileId);
    }

    [Fact]
    public void Delete_NonActiveLeavesActiveUntouched()
    {
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "p1",
            Profiles = ImmutableArray.Create(Profile("p1"), Profile("p2")),
        };
        var c2 = AutoKeyProfileStore.DeleteProfile(c, "p2");
        Assert.Equal("p1", c2.ActiveProfileId);
    }

    [Fact]
    public void Clone_RegeneratesIdsAndAddsCopySuffix()
    {
        var src = Profile("p1", "Combat", actions: 2);
        var c = AutoKeyProfileStore.DefaultConfig() with
        {
            ActiveProfileId = "p1",
            Profiles = ImmutableArray.Create(src),
        };
        var newId = Seq();
        var (c2, cloned) = AutoKeyProfileStore.CloneProfile(c, "p1", newId: newId, clock: FixedClock());
        Assert.NotNull(cloned);
        Assert.NotEqual("p1", cloned!.Id);
        Assert.StartsWith("profile_", cloned.Id);
        Assert.Equal("Combat Copy", cloned.ProfileName);
        Assert.Equal("local", cloned.Source);
        Assert.Null(cloned.RemoteId);
        Assert.Equal(2, cloned.Actions.Length);
        Assert.All(cloned.Actions, a => Assert.NotEqual("aaa", a.Id));      // ids regenerated
        Assert.NotEqual(src.Actions[0].Id, cloned.Actions[0].Id);
        Assert.Equal(2, c2.Profiles.Length);
        Assert.Equal("p1", c2.ActiveProfileId);                              // not auto-activated
    }

    [Fact]
    public void Clone_MissingIdReturnsNullAndKeepsConfig()
    {
        var c = AutoKeyProfileStore.DefaultConfig();
        var (c2, cloned) = AutoKeyProfileStore.CloneProfile(c, "nope", newId: Seq(), clock: FixedClock());
        Assert.Null(cloned);
        Assert.Equal(c, c2);
    }

    [Fact]
    public void Summarize_CountsActions()
    {
        var p = Profile("p1", actions: 3) with
        {
            Actions = ImmutableArray.Create(
                AutoKeyProfileSpec.MakeDefaultAction(1, () => "a1") with { Enabled = true },
                AutoKeyProfileSpec.MakeDefaultAction(2, () => "a2") with { Enabled = false },
                AutoKeyProfileSpec.MakeDefaultAction(3, () => "a3") with { Enabled = true }),
        };
        var s = AutoKeyProfileStore.SummarizeProfile(p);
        Assert.Equal(3, s.ActionCount);
        Assert.Equal(2, s.EnabledActionCount);
        Assert.Equal("p1", s.Id);
    }

    [Fact]
    public void ExportProfileJson_HasSchemaWrapperAndSnakeCaseKeys()
    {
        var p = Profile("p1");
        var json = AutoKeyProfileStore.ExportProfileJson(p);
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        Assert.Equal(1, root.GetProperty("schema_version").GetInt32());
        var prof = root.GetProperty("profile");
        Assert.Equal("p1", prof.GetProperty("id").GetString());
        Assert.Equal("local", prof.GetProperty("source").GetString());
        Assert.True(prof.GetProperty("actions").GetArrayLength() >= 1);
        Assert.True(prof.TryGetProperty("author_snapshot", out _));
        Assert.True(prof.TryGetProperty("engine", out var engine));
        Assert.Equal(50, engine.GetProperty("tick_ms").GetInt32());
    }

    [Fact]
    public void ImportProfile_RegeneratesIdsAndForcesLocalSource()
    {
        var src = Profile("downloaded_id") with { Source = "downloaded", RemoteId = "r99" };
        var json = AutoKeyProfileStore.ExportProfileJson(src);
        var imported = AutoKeyProfileStore.ImportProfile(Js(json), newId: Seq(), clock: FixedClock());
        Assert.NotEqual("downloaded_id", imported.Id);
        Assert.Equal("local", imported.Source);
        Assert.Null(imported.RemoteId);
        Assert.NotEqual(src.Actions[0].Id, imported.Actions[0].Id);
        Assert.Equal("2026-05-07T12:30:00Z", imported.CreatedAt);
        Assert.Equal("2026-05-07T12:30:00Z", imported.UpdatedAt);
    }

    [Fact]
    public void ImportProfile_AcceptsBareProfile_NotJustWrapped()
    {
        var p = Profile("direct");
        var json = AutoKeyProfileStore.ExportProfileJson(p);
        using var doc = JsonDocument.Parse(json);
        var bareProfile = doc.RootElement.GetProperty("profile");
        var imported = AutoKeyProfileStore.ImportProfile(bareProfile, newId: Seq(), clock: FixedClock());
        Assert.NotEqual("direct", imported.Id);
        Assert.Equal("local", imported.Source);
    }

    [Fact]
    public void ExportToPath_WritesFile_AndImportRoundTrips()
    {
        var dir = Path.Combine(Path.GetTempPath(), "saoauto_s74_" + Guid.NewGuid().ToString("N"));
        try
        {
            var p = Profile("p1", "Burst Build") with { ProfileName = "Burst Build" };
            var path = AutoKeyProfileStore.ExportProfileToPath(p, dir, localClock: FixedClock());
            Assert.True(File.Exists(path));
            Assert.StartsWith("Burst_Build_", Path.GetFileName(path));
            var roundTripped = AutoKeyProfileStore.ImportProfileFromPath(path, newId: Seq(), clock: FixedClock());
            Assert.Equal("Burst Build", roundTripped.ProfileName);
            Assert.Equal("local", roundTripped.Source);
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
    }
}
